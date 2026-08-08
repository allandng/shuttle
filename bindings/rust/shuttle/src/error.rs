//! The integer error codes of the C ABI, as a Rust enum.
//!
//! No exception ever crosses the boundary and nothing here ever panics: every
//! failure the C layer reports arrives as a negative integer and is mapped to
//! exactly one variant. The mapping is total — a code this crate does not know
//! becomes [`Error::Unknown`] rather than a panic or a silent success.

use core::fmt;

use shuttle_sys as sys;

/// A failure reported by the C ABI.
///
/// `#[non_exhaustive]`: the ABI is additive, so a future code may become a
/// named variant. Match with a `_` arm.
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Error {
    /// NULL handle/pointer, malformed name, zero size, or misuse (a second
    /// reservation, an operation on the wrong role).
    InvalidArgs,
    /// Name exceeds the platform shm-name limit (macOS 30, Linux 254).
    NameTooLong,
    /// `create`: a segment with that name already exists.
    Exists,
    /// `open` / `unlink`: no such segment.
    NotFound,
    /// Unexpected syscall failure (inspect `errno`), or an exception caught at
    /// the C boundary.
    Sys,
    /// `open`: segment magic word mismatch.
    BadMagic,
    /// `open`: layout version mismatch.
    BadVersion,
    /// `create`: `capacity < max_payload + 8`.
    CapacityTooSmall,
    /// `open`: the creator never published readiness within 5 s.
    InitTimeout,
    /// Header geometry failed validation, or a framed length is impossible.
    Corrupt,
    /// `SHUTTLE_ERR_MSG_TOO_LARGE`. On write: over `max_payload`. On a copy
    /// read: the destination is smaller than the queued message — **and the
    /// message stays queued**, so a larger-buffered retry is well-defined.
    TooBig,
    /// A non-blocking op cannot proceed right now (full on write, empty on
    /// read). Only ever returned by the `try_*` methods.
    WouldBlock,
    /// A blocking wait aborted: the peer's heartbeat went stale. An idle but
    /// live peer looks the same as a dead one — see [`keepalive`].
    ///
    /// [`keepalive`]: crate::Producer::keepalive
    PeerDead,
    /// A code this crate does not name. Carries the raw integer.
    Unknown(i32),
}

impl Error {
    /// Map a raw ABI code. `code` is expected to be negative; a non-negative
    /// one is not an error and yields [`Error::Unknown`] rather than a guess.
    pub fn from_code(code: i32) -> Error {
        match code {
            sys::SHUTTLE_ERR_INVALID_ARGS => Error::InvalidArgs,
            sys::SHUTTLE_ERR_NAME_TOO_LONG => Error::NameTooLong,
            sys::SHUTTLE_ERR_EXISTS => Error::Exists,
            sys::SHUTTLE_ERR_NOT_FOUND => Error::NotFound,
            sys::SHUTTLE_ERR_SYS => Error::Sys,
            sys::SHUTTLE_ERR_BAD_MAGIC => Error::BadMagic,
            sys::SHUTTLE_ERR_BAD_VERSION => Error::BadVersion,
            sys::SHUTTLE_ERR_CAPACITY_TOO_SMALL => Error::CapacityTooSmall,
            sys::SHUTTLE_ERR_INIT_TIMEOUT => Error::InitTimeout,
            sys::SHUTTLE_ERR_CORRUPT => Error::Corrupt,
            sys::SHUTTLE_ERR_MSG_TOO_LARGE => Error::TooBig,
            sys::SHUTTLE_ERR_WOULD_BLOCK => Error::WouldBlock,
            sys::SHUTTLE_ERR_PEER_DEAD => Error::PeerDead,
            other => Error::Unknown(other),
        }
    }

    /// The raw ABI code this error came from.
    pub fn code(self) -> i32 {
        match self {
            Error::InvalidArgs => sys::SHUTTLE_ERR_INVALID_ARGS,
            Error::NameTooLong => sys::SHUTTLE_ERR_NAME_TOO_LONG,
            Error::Exists => sys::SHUTTLE_ERR_EXISTS,
            Error::NotFound => sys::SHUTTLE_ERR_NOT_FOUND,
            Error::Sys => sys::SHUTTLE_ERR_SYS,
            Error::BadMagic => sys::SHUTTLE_ERR_BAD_MAGIC,
            Error::BadVersion => sys::SHUTTLE_ERR_BAD_VERSION,
            Error::CapacityTooSmall => sys::SHUTTLE_ERR_CAPACITY_TOO_SMALL,
            Error::InitTimeout => sys::SHUTTLE_ERR_INIT_TIMEOUT,
            Error::Corrupt => sys::SHUTTLE_ERR_CORRUPT,
            Error::TooBig => sys::SHUTTLE_ERR_MSG_TOO_LARGE,
            Error::WouldBlock => sys::SHUTTLE_ERR_WOULD_BLOCK,
            Error::PeerDead => sys::SHUTTLE_ERR_PEER_DEAD,
            Error::Unknown(code) => code,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = match self {
            Error::InvalidArgs => "invalid arguments",
            Error::NameTooLong => "name too long",
            Error::Exists => "channel already exists",
            Error::NotFound => "channel not found",
            Error::Sys => "system call failed",
            Error::BadMagic => "bad segment magic",
            Error::BadVersion => "bad layout version",
            Error::CapacityTooSmall => "capacity too small (need max_payload + 8)",
            Error::InitTimeout => "timed out waiting for the creator to publish readiness",
            Error::Corrupt => "segment corrupt",
            Error::TooBig => "message too large",
            Error::WouldBlock => "would block",
            Error::PeerDead => "peer dead",
            Error::Unknown(_) => "unknown error",
        };
        write!(f, "{} (code {})", text, self.code())
    }
}

impl std::error::Error for Error {}

/// `Result` with this crate's [`Error`].
pub type Result<T> = core::result::Result<T, Error>;

/// Turn an `int`-returning ABI call into a `Result`.
pub(crate) fn check(rc: i32) -> Result<()> {
    if rc == sys::SHUTTLE_OK {
        Ok(())
    } else {
        Err(Error::from_code(rc))
    }
}
