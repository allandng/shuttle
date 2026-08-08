"""cffi (ABI mode) binding of the frozen Shuttle C ABI v1.1, plus library
discovery.

ABI mode, not API mode: nothing here is compiled at install time, so the wheel
is pure Python and binds whatever ``libshuttle_c`` the host provides. The cdef
below is the complete v1.1 surface — the ten frozen v1 functions plus
``shuttle_create_ex`` — transcribed from include/shuttle/shuttle_c.h. No symbol
is declared that the header does not declare.
"""
import ctypes.util
import os
import threading

import cffi

from .errors import LibraryNotFoundError

__all__ = ["CDEF", "Library", "load_library", "resolve_library_path"]

CDEF = """
typedef struct shuttle_channel shuttle_channel;

shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
shuttle_channel* shuttle_create_ex(const char* name, size_t capacity_bytes,
                                   size_t max_payload_bytes,
                                   uint32_t create_flags, int* err);
shuttle_channel* shuttle_open(const char* name, int* err);
void shuttle_close(shuttle_channel* ch);
int shuttle_unlink(const char* name);

int shuttle_write(shuttle_channel* ch, const void* data, size_t len,
                  int flags);
long shuttle_read(shuttle_channel* ch, void* out, size_t cap, int flags);

int shuttle_acquire_write(shuttle_channel* ch, void** ptr, size_t len,
                          int flags);
int shuttle_commit_write(shuttle_channel* ch, size_t actual_len);
int shuttle_acquire_read(shuttle_channel* ch, const void** ptr, size_t* len,
                         int flags);
int shuttle_release_read(shuttle_channel* ch);

void shuttle_keepalive(shuttle_channel* ch);
"""

#: Per-op flag word (SHUTTLE_NONBLOCK). Distinct namespace from the create-flags.
NONBLOCK = 0x1
#: Create-flag word (SHUTTLE_CREATE_HUGEPAGES), v1.1. Never mixed with NONBLOCK.
CREATE_HUGEPAGES = 0x1

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
