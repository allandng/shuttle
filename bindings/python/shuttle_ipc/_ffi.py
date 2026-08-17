"""cffi (ABI mode) binding of the frozen Shuttle C ABI, plus library discovery.

ABI mode, not API mode: nothing here is compiled at install time, so the wheel
is pure Python and binds whatever ``libshuttle_c`` the host provides. The cdef
below is the complete surface as of ABI v1.4 — the eleven frozen v1 functions,
``shuttle_create_ex`` (v1.1), ``shuttle_get_stats`` + ``shuttle_stats`` (v1.2),
the file-backed trio ``shuttle_create_file`` / ``shuttle_open_file`` /
``shuttle_unlink_file`` and the lookahead ``shuttle_peek_next`` (v1.4) —
transcribed from include/shuttle/shuttle_c.h. No symbol is declared that the
header does not declare.

``SHUTTLE_ABI_VERSION`` is still 1: every addition since has been a new symbol
or a new constant, never a changed signature, so a library built at any of these
levels links and runs. What the *constants* below gate is which segments a peer
can attach to — see ``CREATE_STATS`` and ``CREATE_ALIGNED_SPANS``.
"""
import ctypes.util
import os
import threading

import cffi

from .errors import LibraryNotFoundError

__all__ = ["CDEF", "Library", "load_library", "resolve_library_path"]

CDEF = """
typedef struct shuttle_channel shuttle_channel;

typedef struct shuttle_stats {
    uint64_t msgs_written;
    uint64_t bytes_written;
    uint64_t msgs_dropped;
    uint64_t msgs_read;
    uint64_t bytes_read;
} shuttle_stats;

shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
shuttle_channel* shuttle_create_ex(const char* name, size_t capacity_bytes,
                                   size_t max_payload_bytes,
                                   uint32_t create_flags, int* err);
shuttle_channel* shuttle_open(const char* name, int* err);
void shuttle_close(shuttle_channel* ch);
int shuttle_unlink(const char* name);

shuttle_channel* shuttle_create_file(const char* path, size_t capacity_bytes,
                                     size_t max_payload_bytes,
                                     uint32_t create_flags, int* err);
shuttle_channel* shuttle_open_file(const char* path, int* err);
int shuttle_unlink_file(const char* path);

int shuttle_write(shuttle_channel* ch, const void* data, size_t len,
                  int flags);
long shuttle_read(shuttle_channel* ch, void* out, size_t cap, int flags);

int shuttle_acquire_write(shuttle_channel* ch, void** ptr, size_t len,
                          int flags);
int shuttle_commit_write(shuttle_channel* ch, size_t actual_len);
int shuttle_acquire_read(shuttle_channel* ch, const void** ptr, size_t* len,
                         int flags);
int shuttle_release_read(shuttle_channel* ch);
int shuttle_peek_next(shuttle_channel* ch, size_t* len_out);

void shuttle_keepalive(shuttle_channel* ch);

int shuttle_get_stats(shuttle_channel* ch, shuttle_stats* out);
"""

# --- per-op flags: the `flags` argument of the read/write entry points -------

#: SHUTTLE_NONBLOCK — try-semantics instead of parking. A DISTINCT namespace
#: from the create-flags below; the two are never passed to the same argument.
NONBLOCK = 0x1
#: SHUTTLE_DROP_NEWEST (v1.3) — write-only, and the only way this library ever
#: drops a message. Implies try-semantics; a message that does not fit now is
#: discarded and ``DROPPED`` is returned instead of ``ERR_WOULD_BLOCK``.
#: Rejected with ``ERR_INVALID_ARGS`` on the read/acquire paths.
DROP_NEWEST = 0x2

#: SHUTTLE_DROPPED (v1.3) — the one POSITIVE return in the ABI, and not an
#: error: a drop-newest write found no room. Only a call that opted in can
#: receive it, which is why ``errors.check`` tests ``rc < 0``, not ``rc != 0``.
DROPPED = 1

# --- create-flags: the `create_flags` word of shuttle_create_ex --------------

#: SHUTTLE_CREATE_HUGEPAGES (v1.1) — advise transparent huge pages. Advisory:
#: a no-op wherever the kernel policy does not permit it, never an error.
CREATE_HUGEPAGES = 0x1
#: SHUTTLE_CREATE_HUGETLB_2MB / _1GB (v1.2) — back the segment with EXPLICIT
#: reserved huge pages. Guarantee-or-error: ``NoHugePages`` if they cannot be
#: delivered, never a silent downgrade. Setting both is ``InvalidArgs``.
CREATE_HUGETLB_2MB = 0x2
CREATE_HUGETLB_1GB = 0x4
#: SHUTTLE_CREATE_STATS (v1.2) — allocate the counters (segment layout v2).
#: Changes the layout VERSION, so a peer built before v1.2 gets ``BadVersion``.
CREATE_STATS = 0x8
#: SHUTTLE_CREATE_ALIGNED_SPANS (v1.4) — every payload span starts on a system
#: page, so a borrowed ``memoryview`` can be handed to an API that requires
#: page-aligned host memory (``cudaHostRegister``, Metal's
#: ``newBufferWithBytesNoCopy``) with no copy. Costs padding — up to
#: ``2*page - 9`` bytes per message — and, like ``CREATE_STATS``, restricts who
#: can attach: the segment's page-rounded ``data_offset`` makes a peer built
#: before v1.4 report ``Corrupt``. See docs/API.md.
CREATE_ALIGNED_SPANS = 0x10
#: SHUTTLE_CREATE_FILE_BACKED (v1.4) — the segment lives in a FILE rather than a
#: POSIX shm object, so capacity is bounded by the filesystem and the page cache
#: decides residency. The one create-flag you never pass to ``Channel.create``:
#: choosing this backing means supplying a PATH, so it is selected by calling
#: ``Channel.create_file`` instead, which sets the bit itself. Passed to
#: ``create`` it is simply masked off (not an error), like any bit that entry
#: point cannot implement. Persisted and informational — read it back off a
#: segment to see what the creator asked for.
CREATE_FILE_BACKED = 0x20

ABI_VERSION = 1

_lock = threading.Lock()
_default = None


def resolve_library_path(path=None):
    """Locate ``libshuttle_c`` in the documented order.

    1. ``path``, if given (an explicit path always wins);
    2. the ``SHUTTLE_C_LIB`` environment variable;
    3. ``ctypes.util.find_library("shuttle_c")`` — the platform's own search.

    Returns the string handed to ``dlopen``. Raises ``LibraryNotFoundError``
    if all three come up empty.
    """
    if path is not None:
        return os.fspath(path)
    env = os.environ.get("SHUTTLE_C_LIB")
    if env:
        return env
    found = ctypes.util.find_library("shuttle_c")
    if found:
        return found
    raise LibraryNotFoundError(
        "libshuttle_c not found: pass an explicit path, set SHUTTLE_C_LIB, "
        "or install the library where the dynamic loader can find it"
    )


class Library:
    """A loaded ``libshuttle_c`` and the cffi context that talks to it.

    Holding the ``FFI`` alongside the handle matters: every cdata object
    (``int*`` out-params, ``ffi.buffer`` views) belongs to the FFI that made it.
    """

    __slots__ = ("ffi", "lib", "path")

    def __init__(self, path=None):
        self.path = resolve_library_path(path)
        self.ffi = cffi.FFI()
        self.ffi.cdef(CDEF)
        try:
            self.lib = self.ffi.dlopen(self.path)
        except OSError as exc:
            raise LibraryNotFoundError(
                "could not dlopen {!r}: {}".format(self.path, exc)
            ) from exc

    def __repr__(self):
        return "<shuttle_ipc.Library {!r}>".format(self.path)


def load_library(path=None):
    """Return a ``Library``, caching the default (no explicit path) one.

    An explicit ``path`` always produces a fresh ``Library`` — two different
    builds of the library can coexist in one process, which is occasionally
    what you want in a test.
    """
    global _default
    if path is not None:
        return Library(path)
    with _lock:
        if _default is None:
            _default = Library(None)
        return _default
