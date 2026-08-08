"""The ``Channel`` handle and its two borrow guards.

One ``Channel`` wraps one ``shuttle_channel*``. Roles are bound lazily by the C
layer — a handle becomes the producer on its first write/acquire_write and the
consumer on its first read/acquire_read — so this class exposes both sides and
leaves the binding where the ABI puts it.

Blocking control mirrors the ABI: blocking is the default, ``nonblock=True``
sets ``SHUTTLE_NONBLOCK`` (try-semantics). ``timeout=`` is a convenience layered
on top, not an ABI feature — see ``_attempt``.
"""
import collections
import time

from . import _ffi
from .errors import (DROPPED, ERR_WOULD_BLOCK, InvalidArgs, ShuttleError,
                     TooBig, WouldBlock, check, error_for_code)

__all__ = ["Channel", "BorrowedMessage", "Reservation", "Stats", "unlink"]

#: Snapshot of a segment's counters (``shuttle_get_stats``, v1.2). Byte counts
#: are PAYLOAD bytes: the 8-byte frame header — and, on a channel created with
#: ``CREATE_ALIGNED_SPANS``, the page padding — are transport overhead and are
#: excluded, so these are directly comparable to the lengths you passed in.
#: Each field is individually exact and monotonic; the five are not sampled
#: atomically, so a snapshot taken while traffic flows may show ``msgs_read``
#: trailing ``msgs_written``.
Stats = collections.namedtuple(
    "Stats", "msgs_written bytes_written msgs_dropped msgs_read bytes_read")

# Polling schedule for the emulated `timeout=` (see _attempt): start tight,
# back off to a millisecond so a long timeout does not burn a core.
_POLL_MIN = 50e-6
_POLL_MAX = 1e-3

# Starting buffer for a copy-read whose size we cannot know (an opened channel
# does not learn max_payload — the ABI has no getter). Doubles on TooBig, which
# is safe because an oversized copy-read leaves the message queued.
_DEFAULT_READ_CAP = 1 << 16
_MAX_READ_CAP = 1 << 31


class _Guard:
    """Shared machinery for the two borrow guards.

    Python cannot enforce a borrow lifetime the way Rust's ``Borrowed<'a>``
    does. What it can do is refuse to serve a stale view: on release the
    memoryview is invalidated and every later access raises.
    """

    __slots__ = ("_channel", "_base", "_view", "_len", "_released", "_address")

    def __init__(self, channel, view, base, length, address=0):
        self._channel = channel
        self._base = base
        self._view = view
        self._len = length
        self._address = address
        self._released = False

    @property
    def view(self):
        if self._released:
            raise ShuttleError(self._stale_message)
        return self._view

    @property
    def address(self):
        """The span's address in this process, as an integer.

        The reason it is exposed: the APIs a zero-copy handoff targets take a
        raw pointer, not a buffer object — ``cudaHostRegister``, Metal's
        ``newBufferWithBytesNoCopy``, an ``mmap``-style ioctl. Several of them
        also require it to be page-aligned, which is what
        ``CREATE_ALIGNED_SPANS`` guarantees:

            assert borrow.address % mmap.PAGESIZE == 0

        Valid only until release, exactly like ``view``.
        """
        if self._released:
            raise ShuttleError(self._stale_message)
        return self._address

    def __len__(self):
        return self._len

    @property
    def released(self):
        return self._released

    def _invalidate(self):
        """Drop our references to segment memory. Never raises.

        ``memoryview.release()`` raises ``BufferError`` if the caller exported
        the view (``np.frombuffer(view)`` and friends). We still mark the guard
        released — the C-side release has already happened by then — and the
        exported object is documented as aliasing memory the peer may reuse.
        """
        self._released = True
        for mv in (self._view, self._base):
            try:
                mv.release()
            except BufferError:
                pass

    def __enter__(self):
        return self.view

    def __exit__(self, exc_type, exc, tb):
        self.release()
        return False


class BorrowedMessage(_Guard):
    """A zero-copy view of one message, borrowed in place from the segment.

    Yielded by ``Channel.acquire_read``. The bytes live in shared memory and
    stay valid only until ``release`` — after that the producer may overwrite
    them, so the view is invalidated rather than left pointing at reusable
    memory. The view is read-only: a consumer has no business writing into the
    ring.

        with channel.acquire_read() as payload:   # memoryview, no copy
            total = sum(payload)                  # indexed in place
    """

    __slots__ = ()

    _stale_message = "borrowed payload used after release_read"

    def release(self):
        """Return the bytes to the producer (``shuttle_release_read``).

        Idempotent. The C release runs first, then the view is invalidated —
        the same order as the reference binding in tests/ffi/py_consumer.py.
        """
        if self._released:
            return
        channel = self._channel
        rc = channel._lib.shuttle_release_read(channel._handle)
        self._invalidate()
        channel._borrow = None
        check(rc, "release_read")

    def tobytes(self):
        """An owned copy of the payload. Defeats zero-copy — be deliberate."""
        return bytes(self.view)


class Reservation(_Guard):
    """A writable span reserved in the segment, not yet published.

    Yielded by ``Channel.acquire_write``. Fill ``view``, then ``commit(n)`` to
    publish the first ``n`` bytes (``n`` defaults to the full reservation).
    Nothing is visible to the consumer until the commit.

    The C ABI has no cancel: a reservation ends only at ``shuttle_commit_write``
    (docs/API.md). This wrapper does not invent one — a reservation dropped
    without a commit publishes nothing and stays outstanding, and the next
    ``acquire_write`` on that channel fails with ``InvalidArgs``. Used as a
    context manager, ``Reservation`` therefore commits the full span on a clean
    exit; on an exception it leaves the span uncommitted, because publishing
    half-written bytes would be worse than a stuck handle.
    """

    __slots__ = ()

    _stale_message = "write reservation used after commit_write"

    def commit(self, actual_len=None):
        """Publish ``actual_len`` bytes (``<=`` the reserved length)."""
        if self._released:
            raise ShuttleError(self._stale_message)
        if actual_len is None:
            actual_len = self._len
        if actual_len > self._len:
            raise ValueError(
                "commit of {} bytes exceeds the {}-byte reservation".format(
                    actual_len, self._len
                )
            )
        channel = self._channel
        rc = channel._lib.shuttle_commit_write(channel._handle, actual_len)
        self._invalidate()
        channel._reservation = None
        check(rc, "commit_write")

    def __exit__(self, exc_type, exc, tb):
        if exc_type is None and not self._released:
            self.commit()
        return False


class Channel:
    """A handle on one Shuttle channel.

    Construct with ``Channel.create`` or ``Channel.open`` — the constructor is
    not part of the public API. Usable as a context manager; ``close`` is
    idempotent and never fails.
    """

    __slots__ = (
        "_library",
        "_ffi",
        "_lib",
        "_handle",
        "_name",
        "_max_payload",
        "_borrow",
        "_reservation",
        "__weakref__",
    )

    def __init__(self, library, handle, name, max_payload=None):
        self._library = library
        self._ffi = library.ffi
        self._lib = library.lib
        self._handle = handle
        self._name = name
        self._max_payload = max_payload
        self._borrow = None
        self._reservation = None

    # --- lifecycle -----------------------------------------------------

    @classmethod
    def create(cls, name, capacity, max_payload, huge_pages=False,
               flags=0, library=None):
        """Create a new channel and return a handle on it.

        ``name`` must start with ``/`` (max 30 chars on macOS, 254 on Linux)
        and must not already exist. ``capacity`` must be at least
        ``max_payload + 8`` — the 8 bytes are the frame header, and the rule is
        what makes a permanently-unsatisfiable write impossible. (With
        ``CREATE_ALIGNED_SPANS`` the floor rises to
        ``page + round_up(max_payload, page)``, because that is what one frame
        costs there; too small is ``CapacityTooSmall`` either way.)

        ``huge_pages=True`` sets ``SHUTTLE_CREATE_HUGEPAGES`` and routes through
        ``shuttle_create_ex`` (v1.1). It is advisory: it takes effect only where
        the kernel THP policy permits, and is a no-op elsewhere.

        ``flags`` carries any other create-time bits, OR'd together::

            from shuttle_ipc import CREATE_STATS, CREATE_ALIGNED_SPANS
            Channel.create(name, cap, maxp,
                           flags=CREATE_STATS | CREATE_ALIGNED_SPANS)

        Two of them decide **which peers can attach**, so choose deliberately:
        ``CREATE_STATS`` bumps the segment layout version (pre-v1.2 openers get
        ``BadVersion``) and ``CREATE_ALIGNED_SPANS`` page-rounds the segment's
        geometry (pre-v1.4 openers get ``Corrupt``). Unknown bits are masked off
        by the C layer and never persisted, so passing a flag an older library
        does not implement is not an error — it simply does nothing.
        """
        lib_ = _resolve_library(library)
        ffi = lib_.ffi
        errp = ffi.new("int*")
        cname = _encode(name)
        if huge_pages:
            flags |= _ffi.CREATE_HUGEPAGES
        if flags:
            handle = lib_.lib.shuttle_create_ex(cname, capacity, max_payload,
                                                flags, errp)
        else:
            # Plain shuttle_create, not create_ex(..., 0, ...): identical in
            # behavior, but it keeps the frozen v1 entry point on the default
            # path, where the binding's own coverage is thickest.
            handle = lib_.lib.shuttle_create(cname, capacity, max_payload,
                                             errp)
        if handle == ffi.NULL:
            raise error_for_code(errp[0], "create {!r}".format(name))
        return cls(lib_, handle, name, max_payload)

    @classmethod
    def open(cls, name, library=None):
        """Attach to an existing channel.

        Waits (bounded, 5 s) for the creator's readiness publication, then
        validates the header before trusting any field. ``max_payload`` is not
        knowable through this path — the ABI exposes no getter — so the copy
        read sizes its buffer adaptively.
        """
        lib_ = _resolve_library(library)
        ffi = lib_.ffi
        errp = ffi.new("int*")
        handle = lib_.lib.shuttle_open(_encode(name), errp)
        if handle == ffi.NULL:
            raise error_for_code(errp[0], "open {!r}".format(name))
        return cls(lib_, handle, name)

    @staticmethod
    def unlink(name, library=None):
        """Remove the named shm object. Live mappings stay valid until closed."""
        lib_ = _resolve_library(library)
        check(lib_.lib.shuttle_unlink(_encode(name)),
              "unlink {!r}".format(name))

    def close(self):
        """Unmap and free the handle. The named object survives — see unlink.

        Idempotent, and never fails. Any outstanding borrow or reservation is
        invalidated first: after the handle is gone, its views point at memory
        this process no longer maps.
        """
        if self._handle is None:
            return
        for guard in (self._borrow, self._reservation):
            if guard is not None and not guard.released:
                guard._invalidate()
        self._borrow = None
        self._reservation = None
        self._lib.shuttle_close(self._handle)
        self._handle = None

    @property
    def name(self):
        return self._name

    @property
    def max_payload(self):
        """The channel's max payload, or ``None`` on an opened handle."""
        return self._max_payload

    @property
    def closed(self):
        return self._handle is None

    def keepalive(self):
        """Bump this side's heartbeat without moving data.

        Sparse-traffic peers must call this, or the other side eventually
        declares them dead and a blocking wait aborts with ``PeerDead``.
        """
        self._lib.shuttle_keepalive(self._require_handle())

    # --- producer side -------------------------------------------------

    def write(self, data, nonblock=False, timeout=None, drop_newest=False):
        """Copy path: frame and enqueue ``data`` as one message.

        Blocking by default (the C layer spins briefly, then parks).
        ``nonblock=True`` raises ``WouldBlock`` instead of parking.
        ``timeout=`` gives up after that many seconds, raising ``WouldBlock``.

        ``drop_newest=True`` (v1.3) opts into the LOSSY policy for this one
        call: it never parks, and a message that does not fit right now is
        thrown away rather than reported as an error. Returns ``True`` if the
        message was written and ``False`` if it was dropped — so a caller has to
        look, which is the point. Nothing already queued is disturbed by a drop,
        and the drop is counted in ``msgs_dropped`` on a ``CREATE_STATS``
        segment. An oversized payload is still ``TooBig``: a message that could
        never fit in any ring state is a bug, not backpressure.

        Without ``drop_newest`` the return is always ``True`` — the call either
        succeeded or raised.
        """
        handle = self._require_handle()
        # from_buffer keeps this zero-copy on the way in and gets the byte
        # length right for any buffer (a memoryview's len() counts items).
        buf = self._ffi.from_buffer(data)
        length = len(buf)
        if length == 0:
            buf = self._ffi.NULL
        if drop_newest:
            if nonblock or timeout is not None:
                raise ValueError(
                    "drop_newest already implies try-semantics; do not combine "
                    "it with nonblock= or timeout=")
            rc = check(
                self._lib.shuttle_write(handle, buf, length,
                                        _ffi.DROP_NEWEST), "write")
            return rc != DROPPED
        self._attempt(
            lambda flags: self._lib.shuttle_write(handle, buf, length, flags),
            nonblock, timeout, "write")
        return True

    def acquire_write(self, length, nonblock=False, timeout=None):
        """Zero-copy path: reserve ``length`` contiguous writable bytes.

        Returns a ``Reservation`` whose ``view`` is a writable memoryview over
        the segment. Nothing is published until ``commit``. At most one
        reservation may be outstanding per handle.
        """
        handle = self._require_handle()
        self._reject_outstanding(self._reservation, "write reservation")
        ptrp = self._ffi.new("void**")
        self._attempt(
            lambda flags: self._lib.shuttle_acquire_write(
                handle, ptrp, length, flags),
            nonblock, timeout, "acquire_write")
        base = memoryview(self._ffi.buffer(ptrp[0], length))
        res = Reservation(self, base, base, length, self._address_of(ptrp[0]))
        self._reservation = res
        return res

    def commit_write(self, actual_len):
        """Publish ``actual_len`` bytes of the outstanding reservation.

        The ABI-mirroring form; ``Reservation.commit`` is the same call with the
        guard invalidation attached, and is what you normally want.
        """
        rc = self._lib.shuttle_commit_write(self._require_handle(), actual_len)
        res = self._reservation
        if rc == 0 and res is not None and not res.released:
            res._invalidate()
            self._reservation = None
        check(rc, "commit_write")

    # --- consumer side -------------------------------------------------

    def read(self, max_len=None, nonblock=False, timeout=None):
        """Copy path: dequeue the next message and return it as ``bytes``.

        ``max_len`` bounds the destination buffer. Leave it ``None`` to let the
        binding size the buffer itself: it starts from the channel's
        ``max_payload`` when known, otherwise 64 KiB, and doubles on
        ``MSG_TOO_LARGE``. That retry is well-defined because an oversized
        copy-read leaves the message queued.
        """
        handle = self._require_handle()
        grow = max_len is None
        cap = max_len
        if cap is None:
            cap = self._max_payload or _DEFAULT_READ_CAP
        while True:
            out = self._ffi.new("char[]", cap) if cap else self._ffi.NULL
            try:
                n = self._attempt(
                    lambda flags: self._lib.shuttle_read(handle, out, cap,
                                                         flags),
                    nonblock, timeout, "read")
            except TooBig:
                if not grow or cap >= _MAX_READ_CAP:
                    raise
                cap *= 2
                continue
            return bytes(self._ffi.buffer(out, n)) if n else b""

    def acquire_read(self, nonblock=False, timeout=None):
        """Zero-copy path: borrow the next message in place.

        Returns a ``BorrowedMessage`` context manager yielding a read-only
        memoryview over the segment — no copy is taken anywhere on this path.
        The view is invalidated on release; touching it afterward raises.
        """
        handle = self._require_handle()
        self._reject_outstanding(self._borrow, "read borrow")
        ptrp = self._ffi.new("const void**")
        lenp = self._ffi.new("size_t*")
        self._attempt(
            lambda flags: self._lib.shuttle_acquire_read(
                handle, ptrp, lenp, flags),
            nonblock, timeout, "acquire_read")
        length = lenp[0]
        if length:
            base = memoryview(self._ffi.buffer(ptrp[0], length))
        else:
            base = memoryview(b"")
        # The address comes from the pointer the ABI returned, not from the
        # memoryview: an empty message has no buffer to take an address from,
        # and its span is still a real (page-aligned, under
        # CREATE_ALIGNED_SPANS) location in the segment.
        msg = BorrowedMessage(self, base.toreadonly(), base, length,
                              self._address_of(ptrp[0]))
        self._borrow = msg
        return msg

    def release_read(self):
        """Release the outstanding borrow (the ABI-mirroring form)."""
        msg = self._borrow
        if msg is not None and not msg.released:
            msg.release()
            return
        check(self._lib.shuttle_release_read(self._require_handle()),
              "release_read")

    # --- statistics ----------------------------------------------------

    def get_stats(self):
        """Copy out the segment's counters as a ``Stats`` tuple.

        Either side may call it, and so may any third process that merely
        opened the segment — the counters live in the segment, not the handle.
        Raises ``NoStats`` if the channel was not created with ``CREATE_STATS``
        (on such a segment those bytes are payload, and nothing reads them).
        """
        out = self._ffi.new("shuttle_stats*")
        check(self._lib.shuttle_get_stats(self._require_handle(), out),
              "get_stats")
        return Stats(out.msgs_written, out.bytes_written, out.msgs_dropped,
                     out.msgs_read, out.bytes_read)

    # --- internals -----------------------------------------------------

    def _address_of(self, ptr):
        """The integer address of a cdata pointer into the segment."""
        return int(self._ffi.cast("uintptr_t", ptr))

    def _require_handle(self):
        if self._handle is None:
            raise ShuttleError("channel {!r} is closed".format(self._name))
        return self._handle

    @staticmethod
    def _reject_outstanding(guard, what):
        """Enforce "at most one borrow/reservation per handle" (docs/API.md).

        The C layer enforces this itself on the write side; on the read side a
        repeat acquire is idempotent there, which would silently orphan the
        first guard and its view. The binding holds the documented contract on
        both sides so the guards stay in step with the segment.
        """
        if guard is not None and not guard.released:
            raise InvalidArgs(
                "a {} is already outstanding on this channel".format(what))

    def _attempt(self, op, nonblock, timeout, context):
        """Run ``op(flags)`` under the requested blocking policy.

        The ABI has exactly two modes: park, or fail with ``WOULD_BLOCK``.
        There is no deadline variant, so a ``timeout`` is emulated by repeating
        the non-blocking call with a backing-off sleep. That is polling, not
        parking — pass ``timeout=None`` on any hot path and let the C layer
        park instead.
        """
        if nonblock and timeout is not None:
            raise ValueError("pass nonblock=True or timeout=, not both")
        if nonblock:
            return check(op(_ffi.NONBLOCK), context)
        if timeout is None:
            return check(op(0), context)
        if timeout <= 0:
            return check(op(_ffi.NONBLOCK), context)
        deadline = time.monotonic() + timeout
        delay = _POLL_MIN
        while True:
            rc = op(_ffi.NONBLOCK)
            if rc != ERR_WOULD_BLOCK:
                return check(rc, context)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise WouldBlock(context=context)
            time.sleep(min(delay, remaining))
            delay = min(delay * 2, _POLL_MAX)

    # --- sugar ---------------------------------------------------------

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self):
        state = "closed" if self.closed else "open"
        return "<shuttle_ipc.Channel {!r} {}>".format(self._name, state)


def unlink(name, library=None):
    """Module-level alias for ``Channel.unlink``."""
    Channel.unlink(name, library=library)


def _encode(name):
    if isinstance(name, bytes):
        return name
    return name.encode("utf-8")


def _resolve_library(library):
    """Accept an already-loaded ``Library``, a path, or ``None`` (the default)."""
    if isinstance(library, _ffi.Library):
        return library
    return _ffi.load_library(library)
