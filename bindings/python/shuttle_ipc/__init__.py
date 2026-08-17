"""shuttle-ipc — Python bindings for Shuttle, a zero-copy shared-memory SPSC
IPC library.

The whole package sits on the frozen C ABI (``include/shuttle/shuttle_c.h``,
tracked through v1.4) via cffi in ABI mode: nothing is compiled at install time, no C++ header
is involved, and no symbol is bound that the header does not declare.

    from shuttle_ipc import Channel

    producer = Channel.create("/demo", capacity=1 << 20, max_payload=1 << 16)
    producer.write(b"hello")

    consumer = Channel.open("/demo")
    with consumer.acquire_read() as payload:   # memoryview, zero copy
        assert bytes(payload) == b"hello"

    consumer.close()
    producer.close()
    Channel.unlink("/demo")

Two paths are exposed on each side, exactly as the ABI has them:

- copy: ``write`` / ``read``,
- zero-copy: ``acquire_write`` + ``commit_write``, ``acquire_read`` + release.

Blocking is the default; ``nonblock=True`` gives try-semantics and raises
``WouldBlock``. Every failure the C layer reports as a negative integer is
raised here as a ``ShuttleError`` subclass carrying that integer as ``.code``.
The one POSITIVE return, ``DROPPED``, is not an error and never raises: it
reaches you as ``write(..., drop_newest=True) is False``.

Opt-in create-time behavior arrives through ``Channel.create(..., flags=...)``
— ``CREATE_STATS`` for the counters (read them with ``Channel.get_stats``),
``CREATE_ALIGNED_SPANS`` for page-aligned payload spans (check one with
``borrow.address % PAGESIZE``), the hugetlb bits for explicitly reserved huge
pages.

Finding the library, in order: an explicit path argument, then the
``SHUTTLE_C_LIB`` environment variable, then
``ctypes.util.find_library("shuttle_c")``. See README.md.
"""

from ._ffi import (ABI_VERSION, CREATE_ALIGNED_SPANS, CREATE_HUGEPAGES,
                   CREATE_HUGETLB_1GB, CREATE_HUGETLB_2MB, CREATE_STATS,
                   DROP_NEWEST, NONBLOCK, Library, load_library,
                   resolve_library_path)
from .channel import (BorrowedMessage, Channel, Reservation, Stats, unlink)
from .errors import (DROPPED, ERR_BAD_MAGIC, ERR_BAD_VERSION,
                     ERR_CAPACITY_TOO_SMALL, ERR_CORRUPT, ERR_EXISTS,
                     ERR_INIT_TIMEOUT, ERR_INVALID_ARGS, ERR_MSG_TOO_LARGE,
                     ERR_NAME_TOO_LONG, ERR_NO_HUGEPAGES, ERR_NO_STATS,
                     ERR_NOT_FOUND, ERR_PEER_DEAD, ERR_SYS, ERR_WOULD_BLOCK,
                     OK, BadMagic, BadVersion, CapacityTooSmall, Corrupt,
                     Exists, InitTimeout, InvalidArgs, LibraryNotFoundError,
                     MsgTooLarge, NameTooLong, NoHugePages, NoStats, NotFound,
                     PeerDead, ShuttleError, SysError, TooBig, WouldBlock,
                     error_for_code)

__version__ = "1.4.0"

__all__ = [
    "__version__",
    "ABI_VERSION",
    # handle + guards
    "Channel",
    "BorrowedMessage",
    "Reservation",
    "Stats",
    "unlink",
    # library discovery
    "Library",
    "load_library",
    "resolve_library_path",
    # flags
    "NONBLOCK",
    "DROP_NEWEST",
    "DROPPED",
    "CREATE_HUGEPAGES",
    "CREATE_HUGETLB_2MB",
    "CREATE_HUGETLB_1GB",
    "CREATE_STATS",
    "CREATE_ALIGNED_SPANS",
    # errors
    "ShuttleError",
    "LibraryNotFoundError",
    "InvalidArgs",
    "NameTooLong",
    "Exists",
    "NotFound",
    "SysError",
    "BadMagic",
    "BadVersion",
    "CapacityTooSmall",
    "InitTimeout",
    "Corrupt",
    "TooBig",
    "MsgTooLarge",
    "WouldBlock",
    "PeerDead",
    "NoHugePages",
    "NoStats",
    "error_for_code",
    # raw codes
    "OK",
    "ERR_INVALID_ARGS",
    "ERR_NAME_TOO_LONG",
    "ERR_EXISTS",
    "ERR_NOT_FOUND",
    "ERR_SYS",
    "ERR_BAD_MAGIC",
    "ERR_BAD_VERSION",
    "ERR_CAPACITY_TOO_SMALL",
    "ERR_INIT_TIMEOUT",
    "ERR_CORRUPT",
    "ERR_MSG_TOO_LARGE",
    "ERR_WOULD_BLOCK",
    "ERR_PEER_DEAD",
    "ERR_NO_HUGEPAGES",
    "ERR_NO_STATS",
]
