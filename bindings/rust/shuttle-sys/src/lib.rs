//! Raw FFI declarations for the frozen Shuttle C ABI.
//!
//! This crate is the complete surface as of ABI v1.4 and nothing else: the ten
//! frozen v1 functions `shuttle_create`..`shuttle_keepalive`, plus the additive
//! `shuttle_create_ex` (v1.1) and `shuttle_get_stats` (v1.2). Every declaration
//! is transcribed by hand from `include/shuttle/shuttle_c.h`, which is the
//! single source of truth. No symbol appears here that the header does not
//! declare.
//!
//! `SHUTTLE_ABI_VERSION` is still 1: everything since v1.1 has been a new
//! symbol or a new constant, never a changed signature.
//!
//! **bindgen is deliberately not used.** It would pull libclang into every
//! build of a crate whose entire job is eleven stable signatures that cannot
//! change without an ABI break. The same decision is recorded for the
//! reference bindings in `tests/ffi/rust/shuttle.rs`. The cost is that a drift
//! between this file and the header would not be caught by the compiler — it
//! is caught by the repo's byte-exact FFI integration tests instead, and by
//! this crate's own layout assertions on [`shuttle_stats`].
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
/// `shuttle_create_ex` with a hugetlb bit: the explicit huge pages could not be
/// delivered. Never a silent downgrade to normal pages.
pub const SHUTTLE_ERR_NO_HUGEPAGES: c_int = -14;
/// `shuttle_get_stats` on a segment created without [`SHUTTLE_CREATE_STATS`].
pub const SHUTTLE_ERR_NO_STATS: c_int = -15;

/// NOT an error, and the only POSITIVE return in this ABI (v1.3): a
/// [`SHUTTLE_DROP_NEWEST`] write found no room and discarded the message.
/// Only a call that opted into the flag can receive it, which is why error
/// tests here are `rc < 0` and never `rc != SHUTTLE_OK`.
pub const SHUTTLE_DROPPED: c_int = 1;

/// Per-op flag: try-semantics instead of parking. Passed to the read/write
/// entry points only.
pub const SHUTTLE_NONBLOCK: c_int = 0x1;

/// Per-op flag (v1.3), `shuttle_write` only: opt into the lossy drop-newest
/// policy. Implies try-semantics; a message that does not fit is discarded and
/// [`SHUTTLE_DROPPED`] returned. Rejected with `INVALID_ARGS` on the read and
/// acquire paths, where there is nothing to drop.
pub const SHUTTLE_DROP_NEWEST: c_int = 0x2;

/// Create-flag (v1.1): advise transparent huge pages for the mapping. A
/// **separate namespace** from [`SHUTTLE_NONBLOCK`] — it is only ever passed as
/// `shuttle_create_ex`'s `create_flags`, never as a per-op `flags`.
pub const SHUTTLE_CREATE_HUGEPAGES: u32 = 0x1;
/// Create-flags (v1.2): back the segment with EXPLICIT reserved huge pages of
/// the named size. Guarantee-or-error — [`SHUTTLE_ERR_NO_HUGEPAGES`] if they
/// cannot be obtained, never a fallback to normal pages. Setting both is
/// `INVALID_ARGS`.
pub const SHUTTLE_CREATE_HUGETLB_2MB: u32 = 0x2;
pub const SHUTTLE_CREATE_HUGETLB_1GB: u32 = 0x4;
/// Create-flag (v1.2): allocate the statistics counters (segment layout
/// version 2). Changes the LAYOUT VERSION, so a peer built before v1.2 reports
/// [`SHUTTLE_ERR_BAD_VERSION`] rather than ignoring the bit.
pub const SHUTTLE_CREATE_STATS: u32 = 0x8;
/// Create-flag (v1.4): every payload span starts on a system page, so a
/// borrowed pointer can go straight to an API that requires page-aligned host
/// memory (`cudaHostRegister`, Metal's `newBufferWithBytesNoCopy`).
///
/// Two costs, both real: padding of up to `2*page - 9` bytes per message, and —
/// like [`SHUTTLE_CREATE_STATS`] — a restriction on who can attach. The
/// segment's `data_offset` is page-rounded, which a peer built before v1.4
/// rejects with [`SHUTTLE_ERR_CORRUPT`]. That is deliberate: such a peer would
/// otherwise misparse every frame.
pub const SHUTTLE_CREATE_ALIGNED_SPANS: u32 = 0x10;

/// Counter snapshot (v1.2). A plain five-`u64` record, in exactly this order —
/// the C struct is the ABI shape, and the caller allocates it.
///
/// Byte counts are PAYLOAD bytes: the 8-byte frame header, and any page padding
/// added by [`SHUTTLE_CREATE_ALIGNED_SPANS`], are transport overhead and are
/// excluded on both sides.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct shuttle_stats {
    pub msgs_written: u64,
    pub bytes_written: u64,
    pub msgs_dropped: u64,
    pub msgs_read: u64,
    pub bytes_read: u64,
}

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

    // --- statistics (v1.2) -------------------------------------------------

    /// Copy the segment's counters into `*out`. Either side may call it, and
    /// so may a third process that merely opened the segment. Returns
    /// `SHUTTLE_OK`, `SHUTTLE_ERR_INVALID_ARGS` (NULL argument), or
    /// [`SHUTTLE_ERR_NO_STATS`] (segment created without
    /// [`SHUTTLE_CREATE_STATS`]).
    pub fn shuttle_get_stats(ch: *mut shuttle_channel, out: *mut shuttle_stats) -> c_int;
}
