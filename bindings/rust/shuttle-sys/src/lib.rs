//! Raw FFI declarations for the frozen Shuttle C ABI.
//!
//! This crate is the complete v1.1 surface and nothing else: the ten frozen v1
//! functions `shuttle_create`..`shuttle_keepalive`, plus the additive v1.1
//! `shuttle_create_ex`. Every declaration is transcribed by hand from
//! `include/shuttle/shuttle_c.h`, which is the single source of truth. No
//! symbol appears here that the header does not declare.
//!
//! **bindgen is deliberately not used.** It would pull libclang into every
//! build of a crate whose entire job is eleven stable signatures that cannot
//! change without an ABI break. The same decision is recorded for the
//! reference bindings in `tests/ffi/rust/shuttle.rs`. The cost is that a drift
//! between this file and the header would not be caught by the compiler — it
//! is caught by the repo's byte-exact FFI integration tests instead.
//!
//! Everything here is `unsafe` to call. Use the `shuttle` crate for a safe
//! interface; this one exists so that a caller who needs the raw ABI can have
//! it without reimplementing the declarations.

#![no_std]
#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_long, c_void};

/// `SHUTTLE_ABI_VERSION`. Bumped only by a breaking change to this surface;
/// the v1.1 additions are new symbols only, so it stays 1.
pub const SHUTTLE_ABI_VERSION: c_int = 1;

// --- error codes (mirror shuttle::Err; see docs/API.md) ---------------------

pub const SHUTTLE_OK: c_int = 0;
pub const SHUTTLE_ERR_INVALID_ARGS: c_int = -1;
pub const SHUTTLE_ERR_NAME_TOO_LONG: c_int = -2;
pub const SHUTTLE_ERR_EXISTS: c_int = -3;
pub const SHUTTLE_ERR_NOT_FOUND: c_int = -4;
pub const SHUTTLE_ERR_SYS: c_int = -5;
pub const SHUTTLE_ERR_BAD_MAGIC: c_int = -6;
pub const SHUTTLE_ERR_BAD_VERSION: c_int = -7;
pub const SHUTTLE_ERR_CAPACITY_TOO_SMALL: c_int = -8;
pub const SHUTTLE_ERR_INIT_TIMEOUT: c_int = -9;
pub const SHUTTLE_ERR_CORRUPT: c_int = -10;
pub const SHUTTLE_ERR_MSG_TOO_LARGE: c_int = -11;
pub const SHUTTLE_ERR_WOULD_BLOCK: c_int = -12;
pub const SHUTTLE_ERR_PEER_DEAD: c_int = -13;

/// Per-op flag: try-semantics instead of parking. Passed to the read/write
/// entry points only.
pub const SHUTTLE_NONBLOCK: c_int = 0x1;

/// Create-flag (v1.1): advise transparent huge pages for the mapping. A
/// **separate namespace** from [`SHUTTLE_NONBLOCK`] — it is only ever passed as
/// `shuttle_create_ex`'s `create_flags`, never as a per-op `flags`.
pub const SHUTTLE_CREATE_HUGEPAGES: u32 = 0x1;

/// Opaque channel handle. Never constructed on this side; only ever held
/// behind the pointer the C layer returns.
#[repr(C)]
pub struct shuttle_channel {
    _opaque: [u8; 0],
}

extern "C" {
    // --- lifecycle ---------------------------------------------------------

    /// Create and initialize a new channel. Returns null on failure with the
    /// code in `*err`. `capacity_bytes >= max_payload_bytes + 8`.
    pub fn shuttle_create(
        name: *const c_char,
        capacity_bytes: usize,
        max_payload_bytes: usize,
        err: *mut c_int,
    ) -> *mut shuttle_channel;

    /// v1.1: as [`shuttle_create`], plus a `create_flags` word. Unknown bits
    /// are masked off by the implementation.
    /// `shuttle_create(n, c, m, e)` == `shuttle_create_ex(n, c, m, 0, e)`.
    pub fn shuttle_create_ex(
        name: *const c_char,
        capacity_bytes: usize,
        max_payload_bytes: usize,
        create_flags: u32,
        err: *mut c_int,
    ) -> *mut shuttle_channel;

    /// Attach to an existing channel. Bounded wait (5 s) for the creator's
    /// readiness publication. Null on failure with the code in `*err`.
    pub fn shuttle_open(name: *const c_char, err: *mut c_int) -> *mut shuttle_channel;

    /// Unmap and free the handle. The named object survives. Null-safe,
    /// never fails.
    pub fn shuttle_close(ch: *mut shuttle_channel);

    /// Remove the named shm object. Live mappings stay valid until closed.
    pub fn shuttle_unlink(name: *const c_char) -> c_int;

    // --- copy path ---------------------------------------------------------

    /// Frame and enqueue one message. `SHUTTLE_OK` or a negative code.
    pub fn shuttle_write(
        ch: *mut shuttle_channel,
        data: *const c_void,
        len: usize,
        flags: c_int,
    ) -> c_int;

    /// Dequeue the next message into `out`. Returns the payload length
    /// (`>= 0`) on success. A message larger than `cap` yields
    /// `SHUTTLE_ERR_MSG_TOO_LARGE` **and stays queued**.
    pub fn shuttle_read(
        ch: *mut shuttle_channel,
        out: *mut c_void,
        cap: usize,
        flags: c_int,
    ) -> c_long;

    // --- zero-copy borrow path ---------------------------------------------

    /// Reserve a contiguous writable span of `len` bytes at `*ptr`. Nothing is
    /// published until [`shuttle_commit_write`].
    pub fn shuttle_acquire_write(
        ch: *mut shuttle_channel,
        ptr: *mut *mut c_void,
        len: usize,
        flags: c_int,
    ) -> c_int;

    /// Publish `actual_len` (`<=` reserved) bytes of the outstanding
    /// reservation. The release edge on the producer side.
    pub fn shuttle_commit_write(ch: *mut shuttle_channel, actual_len: usize) -> c_int;

    /// Borrow the next message in place: `*ptr` points into the shared
    /// segment, `*len` is the payload length. Must be paired with
    /// [`shuttle_release_read`].
    pub fn shuttle_acquire_read(
        ch: *mut shuttle_channel,
        ptr: *mut *const c_void,
        len: *mut usize,
        flags: c_int,
    ) -> c_int;

    /// Release the outstanding borrow. The borrowed pointer is invalid
    /// afterward — the bytes may be reused by the producer immediately.
    pub fn shuttle_release_read(ch: *mut shuttle_channel) -> c_int;

    // --- liveness ----------------------------------------------------------

    /// Bump this side's heartbeat without transferring data. Null-safe,
    /// never fails.
    pub fn shuttle_keepalive(ch: *mut shuttle_channel);
}
