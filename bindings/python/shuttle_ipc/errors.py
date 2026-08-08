"""Exception hierarchy for the Shuttle C ABI's integer error codes.

No exception ever crosses the C boundary; every failure arrives as a negative
integer (docs/API.md, "Error codes"). This module is the one place those codes
are turned into Python exceptions, so a caller can catch either the whole
family (``ShuttleError``) or one specific failure (``NotFound``), and can always
read the raw integer back off the exception as ``.code``.
"""

__all__ = [
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
    "DROPPED",
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
    "check",
]

# Mirrors the SHUTTLE_* defines in include/shuttle/shuttle_c.h. The C header is
# the single source of truth; these are transcribed, never derived at runtime.
OK = 0
ERR_INVALID_ARGS = -1
ERR_NAME_TOO_LONG = -2
ERR_EXISTS = -3
ERR_NOT_FOUND = -4
ERR_SYS = -5
ERR_BAD_MAGIC = -6
ERR_BAD_VERSION = -7
ERR_CAPACITY_TOO_SMALL = -8
ERR_INIT_TIMEOUT = -9
ERR_CORRUPT = -10
ERR_MSG_TOO_LARGE = -11
ERR_WOULD_BLOCK = -12
ERR_PEER_DEAD = -13
ERR_NO_HUGEPAGES = -14
ERR_NO_STATS = -15

#: SHUTTLE_DROPPED. NOT an error and not OK: the one positive return in the ABI
#: (v1.3), produced only by a write that opted into ``DROP_NEWEST``. It is
#: listed here beside the codes because it shares their integer space, and it is
#: why ``check`` below tests ``rc < 0`` rather than ``rc != 0``.
DROPPED = 1


class ShuttleError(Exception):
    """Base of every error this package raises.

    ``code`` is the integer the C ABI returned, or ``None`` for failures that
    never reached the ABI (``LibraryNotFoundError``).
    """

    code = None

    def __init__(self, message=None, code=None, context=None):
        if code is None:
            code = type(self).code
        self.code = code
        if message is None:
            message = _MESSAGES.get(code, "unknown error")
            if code is not None:
                message = "{} (code {})".format(message, code)
        if context:
            message = "{}: {}".format(context, message)
        super().__init__(message)


class LibraryNotFoundError(ShuttleError):
    """libshuttle_c could not be located or loaded. Not an ABI error code."""


class InvalidArgs(ShuttleError):
    """NULL handle/pointer, malformed name, zero size, or API misuse."""

    code = ERR_INVALID_ARGS


class NameTooLong(ShuttleError):
    """Name exceeds the platform shm-name limit (macOS 30, Linux 254)."""

    code = ERR_NAME_TOO_LONG


class Exists(ShuttleError):
    """create: a segment with that name already exists."""

    code = ERR_EXISTS


class NotFound(ShuttleError):
    """open/unlink: no such segment."""

    code = ERR_NOT_FOUND


class SysError(ShuttleError):
    """Unexpected syscall failure, or an exception caught at the C boundary."""

    code = ERR_SYS


class BadMagic(ShuttleError):
    """open: segment magic word mismatch."""

    code = ERR_BAD_MAGIC


class BadVersion(ShuttleError):
    """open: layout version mismatch."""

    code = ERR_BAD_VERSION


class CapacityTooSmall(ShuttleError):
    """create: capacity_bytes < max_payload_bytes + 8."""

    code = ERR_CAPACITY_TOO_SMALL


class InitTimeout(ShuttleError):
    """open: the creator never published readiness within 5 s."""

    code = ERR_INIT_TIMEOUT


class Corrupt(ShuttleError):
    """Header geometry failed validation, or a framed length is impossible."""

    code = ERR_CORRUPT


class TooBig(ShuttleError):
    """SHUTTLE_ERR_MSG_TOO_LARGE.

    On write: the payload exceeds ``max_payload``. On the copy read: the
    destination buffer is smaller than the queued message — the message stays
    queued, so retrying with a larger buffer is well-defined.
    """

    code = ERR_MSG_TOO_LARGE


#: Alias under the C spelling, for readers coming from the header.
MsgTooLarge = TooBig


class WouldBlock(ShuttleError):
    """A non-blocking op cannot proceed now (full on write, empty on read)."""

    code = ERR_WOULD_BLOCK


class PeerDead(ShuttleError):
    """A blocking wait aborted: the peer's heartbeat went stale."""

    code = ERR_PEER_DEAD


class NoHugePages(ShuttleError):
    """create: explicit huge pages were requested and cannot be delivered.

    No hugetlbfs mount of that page size, no free reserved pages, no permission
    on the mount, or a platform without hugetlbfs. There is deliberately no
    silent fallback to normal pages — that is what ``CREATE_HUGEPAGES``
    (advisory THP) is for.
    """

    code = ERR_NO_HUGEPAGES


class NoStats(ShuttleError):
    """get_stats: the segment has no counters (created without CREATE_STATS).

    On such a segment those bytes are payload, so nothing may read them.
    """

    code = ERR_NO_STATS


_MESSAGES = {
    ERR_INVALID_ARGS: "invalid arguments",
    ERR_NAME_TOO_LONG: "name too long",
    ERR_EXISTS: "channel already exists",
    ERR_NOT_FOUND: "channel not found",
    ERR_SYS: "system call failed",
    ERR_BAD_MAGIC: "bad segment magic",
    ERR_BAD_VERSION: "bad layout version",
    ERR_CAPACITY_TOO_SMALL: "capacity too small (need max_payload + 8)",
    ERR_INIT_TIMEOUT: "timed out waiting for the creator to publish readiness",
    ERR_CORRUPT: "segment corrupt",
    ERR_MSG_TOO_LARGE: "message too large",
    ERR_WOULD_BLOCK: "would block",
    ERR_PEER_DEAD: "peer dead",
    ERR_NO_HUGEPAGES: "explicit huge pages unavailable",
    ERR_NO_STATS: "segment has no statistics block",
}

_BY_CODE = {
    ERR_INVALID_ARGS: InvalidArgs,
    ERR_NAME_TOO_LONG: NameTooLong,
    ERR_EXISTS: Exists,
    ERR_NOT_FOUND: NotFound,
    ERR_SYS: SysError,
    ERR_BAD_MAGIC: BadMagic,
    ERR_BAD_VERSION: BadVersion,
    ERR_CAPACITY_TOO_SMALL: CapacityTooSmall,
    ERR_INIT_TIMEOUT: InitTimeout,
    ERR_CORRUPT: Corrupt,
    ERR_MSG_TOO_LARGE: TooBig,
    ERR_WOULD_BLOCK: WouldBlock,
    ERR_PEER_DEAD: PeerDead,
    ERR_NO_HUGEPAGES: NoHugePages,
    ERR_NO_STATS: NoStats,
}


def error_for_code(code, context=None):
    """Build (do not raise) the exception for an ABI error code.

    An unrecognized code yields a plain ``ShuttleError`` carrying it, so a
    future additive code can never turn into a ``KeyError`` here.
    """
    cls = _BY_CODE.get(code, ShuttleError)
    return cls(code=code, context=context)


def check(rc, context=None):
    """Return ``rc`` if it is not an error code, else raise.

    Used for every int-returning entry point. ``shuttle_read`` returns a
    non-negative length on success, and a drop-newest write returns the
    positive ``DROPPED``, so the test is strictly ``rc < 0`` — never
    ``rc != OK``, which would count a successful drop as a failure.
    """
    if rc < 0:
        raise error_for_code(rc, context=context)
    return rc
