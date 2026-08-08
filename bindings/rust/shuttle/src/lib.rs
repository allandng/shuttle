//! Safe Rust bindings for [Shuttle], a zero-copy shared-memory SPSC IPC
//! library.
//!
//! Everything here sits on the frozen C ABI (`include/shuttle/shuttle_c.h`,
//! tracked through v1.4) through the [`shuttle_sys`] crate. No C++ header is
//! involved and no symbol is bound that the header does not declare. The
//! reference bindings under `tests/ffi/rust/` remain the ABI conformance
//! tests; this crate is the distributable layer over the same surface.
//!
//! # The shape of the API
//!
//! A [`Channel`] is a handle on a segment. The C layer binds a handle's role
//! lazily — producer on its first write, consumer on its first read — so this
//! crate makes that binding explicit and irreversible at the type level:
//! [`Channel::into_producer`] and [`Channel::into_consumer`] consume the
//! handle and hand back a [`Producer`] or a [`Consumer`]. A handle can then
//! only be used one way, which is what the SPSC contract wants anyway.
//!
//! ```no_run
//! use shuttle::Channel;
//!
//! // Process A
//! let mut producer = Channel::create("/demo", 1 << 20, 1 << 16)?
//!     .into_producer();
//! producer.write(b"hello")?;
//!
//! // Process B
//! let mut consumer = Channel::open("/demo")?.into_consumer();
//! {
//!     let msg = consumer.acquire_read()?;   // zero-copy borrow
//!     assert_eq!(msg.as_slice(), b"hello"); // read in place, no copy
//! }                                         // release happens here
//!
//! Channel::unlink("/demo")?;
//! # Ok::<(), shuttle::Error>(())
//! ```
//!
//! # The borrow contract
//!
//! This is the point of the wrapper. [`Consumer::acquire_read`] borrows the
//! consumer mutably and returns a [`Borrowed<'_>`]; the slice from
//! [`Borrowed::as_slice`] is tied to that `Borrowed`'s lifetime, and its `Drop`
//! performs `shuttle_release_read`. Two consequences are enforced **at compile
//! time**, not at runtime:
//!
//! - a payload slice cannot outlive the release — use-after-release is a
//!   borrow-check error, not a dangling read into memory the producer has
//!   already reused;
//! - no second acquire while a borrow is outstanding — the `Consumer` stays
//!   mutably borrowed until the `Borrowed` drops.
//!
//! The same shape covers the producer: [`Reservation<'_>`] borrows the
//! [`Producer`] mutably for as long as the reservation is outstanding.
//!
//! # Blocking
//!
//! Blocking is the default: the C layer spins briefly, then parks (it does not
//! poll). The `try_*` methods set `SHUTTLE_NONBLOCK` and return
//! [`Error::WouldBlock`] instead of parking. The ABI has no deadline variant,
//! so this crate does not offer one — a timeout would have to be polling, and
//! polling is what the parking design exists to avoid.
//!
//! [Shuttle]: https://github.com/allandng/shuttle

#![deny(missing_docs)]
#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::{c_void, CString};
use std::fmt;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;

use shuttle_sys as sys;

mod error;

pub use error::{Error, Result};
use error::check;

/// Starting buffer for [`Consumer::read_owned`]. The ABI has no way to query
/// `max_payload` on an opened handle, so the copy read sizes itself.
const DEFAULT_READ_CAP: usize = 1 << 16;
/// Ceiling for that growth, so a corrupt length cannot drive an unbounded
/// allocation loop.
const MAX_READ_CAP: usize = 1 << 31;

/// Create-time flags for [`Channel::create_with`] (`SHUTTLE_CREATE_*`, v1.1).
///
/// A **separate namespace** from the per-op flags used by the read/write
/// calls. This crate never lets the two mix: per-op blocking is chosen by
/// method (`write` vs `try_write`), and these bits reach only
/// `shuttle_create_ex`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash)]
pub struct CreateFlags(u32);

impl CreateFlags {
    /// No create-time options. Equivalent to plain `shuttle_create`.
    pub const NONE: CreateFlags = CreateFlags(0);

    /// `SHUTTLE_CREATE_HUGEPAGES`: advise transparent huge pages for the
    /// mapping.
    ///
    /// Purely advisory — effective only where the kernel THP policy permits, a
    /// silent no-op on macOS and on kernels that refuse. Never a correctness
    /// dependency. The choice is recorded in the segment header, so an opener's
    /// independent mapping is advised too.
    pub const HUGEPAGES: CreateFlags = CreateFlags(sys::SHUTTLE_CREATE_HUGEPAGES);

    /// `SHUTTLE_CREATE_HUGETLB_2MB`: back the segment with **explicit reserved
    /// 2 MB huge pages** (a file on a hugetlbfs mount), not with normal pages
    /// the kernel may or may not promote.
    ///
    /// Guarantee or error: [`Error::NoHugePages`] if the pages cannot be
    /// obtained, never a silent downgrade — that is the whole difference from
    /// [`CreateFlags::HUGEPAGES`]. Linux only; requires an operator to have
    /// reserved pages and mounted hugetlbfs.
    pub const HUGETLB_2MB: CreateFlags = CreateFlags(sys::SHUTTLE_CREATE_HUGETLB_2MB);

    /// `SHUTTLE_CREATE_HUGETLB_1GB`: as [`CreateFlags::HUGETLB_2MB`], with 1 GB
    /// pages. Setting both sizes is [`Error::InvalidArgs`] — they name two
    /// different segments.
    pub const HUGETLB_1GB: CreateFlags = CreateFlags(sys::SHUTTLE_CREATE_HUGETLB_1GB);

    /// `SHUTTLE_CREATE_STATS`: allocate the counters in the segment and keep
    /// them updated; read them with [`Producer::stats`] / [`Consumer::stats`].
    ///
    /// This is the one create-flag that changes the segment's **layout
    /// version** (1 → 2), so it also decides who can attach: a peer built
    /// before v1.2 reports [`Error::BadVersion`].
    pub const STATS: CreateFlags = CreateFlags(sys::SHUTTLE_CREATE_STATS);

    /// `SHUTTLE_CREATE_ALIGNED_SPANS`: every payload span starts on a system
    /// page, so [`Borrowed::as_ptr`] / [`Reservation::as_mut_slice`] can be
    /// handed to an API that requires page-aligned host memory —
    /// `cudaHostRegister`, Metal's `newBufferWithBytesNoCopy` — with no copy.
    ///
    /// Two costs, stated plainly. Padding: up to `2 * page - 9` bytes per
    /// message (8183 at a 4 KiB page), so size the channel for the padded
    /// stride — `capacity` must be at least `page + round_up(max_payload,
    /// page)` or [`Error::CapacityTooSmall`]. And attachment: the segment's
    /// `data_offset` is page-rounded, which a peer built before v1.4 rejects
    /// with [`Error::Corrupt`]. That rejection is deliberate — the flag changes
    /// the FRAMING, and a peer that merely ignored the bit would misparse every
    /// message.
    ///
    /// Composes with the rest: `STATS | ALIGNED_SPANS` keeps counting payload
    /// bytes rather than the padded stride, and a hugetlb backing leaves the
    /// alignment unit at the *system* page.
    pub const ALIGNED_SPANS: CreateFlags = CreateFlags(sys::SHUTTLE_CREATE_ALIGNED_SPANS);

    /// The raw `create_flags` word.
    pub const fn bits(self) -> u32 {
        self.0
    }

    /// Wrap a raw `create_flags` word.
    ///
    /// The forward-compatibility escape hatch: it lets a caller pass a bit this
    /// version of the crate has no constant for, against a library that does
    /// implement it. It is safe in both directions because the C layer MASKS
    /// `create_flags` down to the bits it actually implements — a bit the
    /// library does not know is dropped, never persisted and never an error.
    pub const fn from_bits(bits: u32) -> CreateFlags {
        CreateFlags(bits)
    }
}

/// A snapshot of a segment's counters ([`CreateFlags::STATS`] channels only).
///
/// Byte counts are **payload** bytes: the 8-byte frame header, and any page
/// padding added by [`CreateFlags::ALIGNED_SPANS`], are transport overhead and
/// are excluded on both sides — so these numbers are directly comparable to the
/// lengths you passed in, and to each other.
///
/// Each field is individually exact and monotonic; the five are *not* sampled
/// atomically, so a snapshot taken while traffic flows may show `msgs_read`
/// trailing `msgs_written`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Stats {
    /// Messages published (counted at the commit that makes one visible).
    pub msgs_written: u64,
    /// Payload bytes published.
    pub bytes_written: u64,
    /// Messages discarded by a [`Producer::write_or_drop`] call, and nothing
    /// else. Stays `0` on a channel that never opts into the lossy policy.
    pub msgs_dropped: u64,
    /// Messages released by the consumer.
    pub msgs_read: u64,
    /// Payload bytes released.
    pub bytes_read: u64,
}

impl From<sys::shuttle_stats> for Stats {
    fn from(s: sys::shuttle_stats) -> Stats {
        Stats {
            msgs_written: s.msgs_written,
            bytes_written: s.bytes_written,
            msgs_dropped: s.msgs_dropped,
            msgs_read: s.msgs_read,
            bytes_read: s.bytes_read,
        }
    }
}

/// The outcome of a [`Producer::write_or_drop`] call.
///
/// A separate type rather than a `bool` because the two outcomes are equally
/// ordinary under that policy, and a caller has to look at which one happened —
/// `SHUTTLE_DROPPED` is not an error, and code that treats "not OK" as failure
/// gets it wrong.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Written {
    /// The message was written and is visible to the consumer.
    Yes,
    /// The ring could not take it, so it was discarded. Nothing was written and
    /// nothing already queued was disturbed.
    Dropped,
}

/// Read the counters out of a live handle. Shared by every role wrapper.
fn stats_of(ch: *mut sys::shuttle_channel) -> Result<Stats> {
    let mut raw = sys::shuttle_stats::default();
    // SAFETY: the handle is live for as long as its owner exists, and `raw` is
    // a valid, correctly-sized out-param.
    check(unsafe { sys::shuttle_get_stats(ch, &mut raw) })?;
    Ok(Stats::from(raw))
}

impl std::ops::BitOr for CreateFlags {
    type Output = CreateFlags;

    fn bitor(self, rhs: CreateFlags) -> CreateFlags {
        CreateFlags(self.0 | rhs.0)
    }
}

/// A handle on a Shuttle channel, before a role is chosen.
///
/// Dropping closes the handle (`shuttle_close`): the mapping goes away, the
/// named object does not. Use [`Channel::unlink`] to remove the name.
pub struct Channel {
    ch: *mut sys::shuttle_channel,
}

impl Channel {
    /// Create a new channel.
    ///
    /// `name` must start with `/` (max 30 chars on macOS, 254 on Linux) and
    /// must not already exist. `capacity` must be at least `max_payload + 8` —
    /// the 8 bytes are the frame header, and that rule is what makes a
    /// permanently-unsatisfiable write impossible by construction.
    ///
    /// # Errors
    ///
    /// [`Error::InvalidArgs`], [`Error::NameTooLong`],
    /// [`Error::CapacityTooSmall`], [`Error::Exists`], [`Error::Sys`].
    pub fn create(name: &str, capacity: usize, max_payload: usize) -> Result<Channel> {
        Channel::create_with(name, capacity, max_payload, CreateFlags::NONE)
    }

    /// Create a new channel with create-time flags (v1.1 `shuttle_create_ex`).
    ///
    /// Unknown bits are masked off by the implementation, so passing a flag a
    /// given build does not know is not an error.
    pub fn create_with(
        name: &str,
        capacity: usize,
        max_payload: usize,
        flags: CreateFlags,
    ) -> Result<Channel> {
        let cname = c_name(name)?;
        let mut err: i32 = 0;
        // SAFETY: cname is a valid NUL-terminated string alive across the
        // call; err is a valid out-param. The C layer never throws.
        let ch = unsafe {
            sys::shuttle_create_ex(cname.as_ptr(), capacity, max_payload, flags.bits(), &mut err)
        };
        if ch.is_null() {
            return Err(Error::from_code(err));
        }
        Ok(Channel { ch })
    }

    /// Attach to an existing channel.
    ///
    /// Waits (bounded, 5 s) for the creator's readiness publication, then
    /// validates magic, version and header geometry before trusting any field.
    ///
    /// # Errors
    ///
    /// [`Error::InvalidArgs`], [`Error::NotFound`], [`Error::Corrupt`],
    /// [`Error::Sys`], [`Error::InitTimeout`], [`Error::BadMagic`],
    /// [`Error::BadVersion`].
    pub fn open(name: &str) -> Result<Channel> {
        let cname = c_name(name)?;
        let mut err: i32 = 0;
        // SAFETY: as in create_with.
        let ch = unsafe { sys::shuttle_open(cname.as_ptr(), &mut err) };
        if ch.is_null() {
            return Err(Error::from_code(err));
        }
        Ok(Channel { ch })
    }

    /// Remove the named shm object. Existing mappings stay valid until closed.
    pub fn unlink(name: &str) -> Result<()> {
        let cname = c_name(name)?;
        // SAFETY: cname is a valid NUL-terminated string alive across the call.
        check(unsafe { sys::shuttle_unlink(cname.as_ptr()) })
    }

    /// Bind this handle as the producer.
    pub fn into_producer(self) -> Producer {
        let me = ManuallyDrop::new(self);
        Producer { ch: me.ch }
    }

    /// Bind this handle as the consumer.
    pub fn into_consumer(self) -> Consumer {
        let me = ManuallyDrop::new(self);
        Consumer { ch: me.ch }
    }

    /// Bump this side's heartbeat without transferring data.
    pub fn keepalive(&self) {
        // SAFETY: self.ch is a live handle for as long as `self` exists.
        unsafe { sys::shuttle_keepalive(self.ch) };
    }

    /// Copy out the segment's counters.
    ///
    /// # Errors
    ///
    /// [`Error::NoStats`] if the channel was not created with
    /// [`CreateFlags::STATS`].
    pub fn stats(&self) -> Result<Stats> {
        stats_of(self.ch)
    }
}

// The handle is an opaque pointer; there is nothing else to show, and the ABI
// offers no getter for the name or the geometry.
impl fmt::Debug for Channel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Channel").field("handle", &self.ch).finish()
    }
}

impl Drop for Channel {
    fn drop(&mut self) {
        // SAFETY: the handle is live and owned; shuttle_close is NULL-safe and
        // never fails.
        unsafe { sys::shuttle_close(self.ch) };
    }
}

/// The producing end of a channel.
///
/// Obtained from [`Channel::into_producer`]. Dropping closes the handle.
pub struct Producer {
    ch: *mut sys::shuttle_channel,
}

impl Producer {
    /// Copy path: frame and enqueue `data` as one message. Blocks (parks)
    /// until there is room.
    ///
    /// # Errors
    ///
    /// [`Error::TooBig`] if `data` is longer than the channel's `max_payload`
    /// (checked fail-fast — this never parks), [`Error::PeerDead`] if the wait
    /// aborted because the consumer's heartbeat went stale, [`Error::Sys`].
    pub fn write(&mut self, data: &[u8]) -> Result<()> {
        self.write_flags(data, 0)
    }

    /// Copy path, try-semantics: [`Error::WouldBlock`] instead of parking.
    pub fn try_write(&mut self, data: &[u8]) -> Result<()> {
        self.write_flags(data, sys::SHUTTLE_NONBLOCK)
    }

    /// Copy path with the opt-in **lossy** policy (`SHUTTLE_DROP_NEWEST`): if
    /// the message does not fit right now it is thrown away instead of
    /// blocking, and [`Written::Dropped`] is returned.
    ///
    /// This is the only way this library ever drops a message — "backpressure,
    /// never drops" remains the default, and there is no channel-wide or
    /// fallback form of the policy. A drop moves no cursor and disturbs nothing
    /// already queued, so the consumer cannot observe it; on a
    /// [`CreateFlags::STATS`] channel it is counted in [`Stats::msgs_dropped`].
    ///
    /// It never parks, so it never yields [`Error::PeerDead`] either: a parked
    /// or dead consumer simply means the ring stays full and writes drop.
    ///
    /// # Errors
    ///
    /// [`Error::TooBig`] still applies — a payload over `max_payload` could
    /// never fit in any ring state, which is a bug rather than backpressure and
    /// is not swallowed as a drop.
    pub fn write_or_drop(&mut self, data: &[u8]) -> Result<Written> {
        // SAFETY: as in write_flags.
        let rc = unsafe {
            sys::shuttle_write(
                self.ch,
                data.as_ptr() as *const c_void,
                data.len(),
                sys::SHUTTLE_DROP_NEWEST,
            )
        };
        if rc < 0 {
            return Err(Error::from_code(rc));
        }
        Ok(if rc == sys::SHUTTLE_DROPPED {
            Written::Dropped
        } else {
            Written::Yes
        })
    }

    /// Zero-copy path: reserve `len` contiguous writable bytes in the segment.
    ///
    /// Nothing is published until [`Reservation::commit`]. The returned
    /// `Reservation` borrows this producer mutably, so a second reservation
    /// while one is outstanding is a compile error rather than an
    /// [`Error::InvalidArgs`] at runtime.
    pub fn acquire_write(&mut self, len: usize) -> Result<Reservation<'_>> {
        self.acquire_write_flags(len, 0)
    }

    /// Zero-copy path, try-semantics: [`Error::WouldBlock`] if no contiguous
    /// span is free right now.
    pub fn try_acquire_write(&mut self, len: usize) -> Result<Reservation<'_>> {
        self.acquire_write_flags(len, sys::SHUTTLE_NONBLOCK)
    }

    /// Bump this side's heartbeat without transferring data.
    ///
    /// A peer that is alive but makes no calls is indistinguishable from a
    /// dead one. Sparse-traffic producers must call this, or the consumer's
    /// blocking waits eventually abort with [`Error::PeerDead`].
    pub fn keepalive(&self) {
        // SAFETY: the handle is live for as long as `self` exists.
        unsafe { sys::shuttle_keepalive(self.ch) };
    }

    /// Copy out the segment's counters. See [`Stats`].
    ///
    /// # Errors
    ///
    /// [`Error::NoStats`] if the channel was not created with
    /// [`CreateFlags::STATS`].
    pub fn stats(&self) -> Result<Stats> {
        stats_of(self.ch)
    }

    /// The raw handle, for calling [`shuttle_sys`] directly. The handle stays
    /// owned by this `Producer`; do not close it.
    pub fn as_raw(&self) -> *mut sys::shuttle_channel {
        self.ch
    }

    fn write_flags(&mut self, data: &[u8], flags: i32) -> Result<()> {
        // SAFETY: data.as_ptr() is valid for data.len() bytes; a zero-length
        // slice yields a dangling-but-nonnull pointer, which the C layer only
        // dereferences when len != 0.
        check(unsafe {
            sys::shuttle_write(self.ch, data.as_ptr() as *const c_void, data.len(), flags)
        })
    }

    fn acquire_write_flags(&mut self, len: usize, flags: i32) -> Result<Reservation<'_>> {
        let mut ptr: *mut c_void = std::ptr::null_mut();
        // SAFETY: ptr is a valid out-param; the handle is live.
        check(unsafe { sys::shuttle_acquire_write(self.ch, &mut ptr, len, flags) })?;
        Ok(Reservation {
            ptr: ptr as *mut u8,
            len,
            ch: self.ch,
            _producer: PhantomData,
        })
    }
}

impl fmt::Debug for Producer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Producer").field("handle", &self.ch).finish()
    }
}

impl Drop for Producer {
    fn drop(&mut self) {
        // SAFETY: the handle is live and owned.
        unsafe { sys::shuttle_close(self.ch) };
    }
}

/// The consuming end of a channel.
///
/// Obtained from [`Channel::into_consumer`]. Dropping closes the handle.
pub struct Consumer {
    ch: *mut sys::shuttle_channel,
}

impl Consumer {
    /// Copy path: dequeue the next message into `out`, returning its length.
    /// Blocks (parks) until a message arrives.
    ///
    /// # Errors
    ///
    /// [`Error::TooBig`] if the queued message is larger than `out` — **the
    /// message stays queued**, so retrying with a larger buffer is
    /// well-defined. Also [`Error::PeerDead`], [`Error::Corrupt`],
    /// [`Error::Sys`].
    pub fn read_into(&mut self, out: &mut [u8]) -> Result<usize> {
        self.read_into_flags(out, 0)
    }

    /// Copy path, try-semantics: [`Error::WouldBlock`] on an empty channel.
    pub fn try_read_into(&mut self, out: &mut [u8]) -> Result<usize> {
        self.read_into_flags(out, sys::SHUTTLE_NONBLOCK)
    }

    /// Copy path into an owned `Vec`.
    ///
    /// The ABI exposes no way to query `max_payload` on an opened handle, so
    /// this starts from a 64 KiB buffer and doubles on [`Error::TooBig`]. That
    /// retry is well-defined precisely because an oversized copy read leaves
    /// the message queued. Use [`Consumer::read_into`] when you own a buffer,
    /// or [`Consumer::acquire_read`] when you want no copy at all.
    pub fn read_owned(&mut self) -> Result<Vec<u8>> {
        self.read_owned_flags(0)
    }

    /// [`Consumer::read_owned`] with try-semantics.
    pub fn try_read_owned(&mut self) -> Result<Vec<u8>> {
        self.read_owned_flags(sys::SHUTTLE_NONBLOCK)
    }

    /// Zero-copy path: borrow the next message in place. Blocks (parks) until
    /// a message arrives.
    ///
    /// The returned [`Borrowed`] borrows this consumer mutably and releases on
    /// drop. Nothing is copied anywhere on this path.
    pub fn acquire_read(&mut self) -> Result<Borrowed<'_>> {
        self.acquire_read_flags(0)
    }

    /// Zero-copy path, try-semantics: [`Error::WouldBlock`] on an empty
    /// channel.
    pub fn try_acquire_read(&mut self) -> Result<Borrowed<'_>> {
        self.acquire_read_flags(sys::SHUTTLE_NONBLOCK)
    }

    /// Bump this side's heartbeat without transferring data. See
    /// [`Producer::keepalive`].
    pub fn keepalive(&self) {
        // SAFETY: the handle is live for as long as `self` exists.
        unsafe { sys::shuttle_keepalive(self.ch) };
    }

    /// Copy out the segment's counters. See [`Stats`].
    ///
    /// # Errors
    ///
    /// [`Error::NoStats`] if the channel was not created with
    /// [`CreateFlags::STATS`].
    pub fn stats(&self) -> Result<Stats> {
        stats_of(self.ch)
    }

    /// The raw handle, for calling [`shuttle_sys`] directly. The handle stays
    /// owned by this `Consumer`; do not close it.
    pub fn as_raw(&self) -> *mut sys::shuttle_channel {
        self.ch
    }

    fn read_into_flags(&mut self, out: &mut [u8], flags: i32) -> Result<usize> {
        // SAFETY: out is valid for out.len() bytes; the handle is live.
        let rc = unsafe {
            sys::shuttle_read(self.ch, out.as_mut_ptr() as *mut c_void, out.len(), flags)
        };
        if rc < 0 {
            return Err(Error::from_code(rc as i32));
        }
        Ok(rc as usize)
    }

    fn read_owned_flags(&mut self, flags: i32) -> Result<Vec<u8>> {
        let mut cap = DEFAULT_READ_CAP;
        loop {
            let mut buf = vec![0u8; cap];
            match self.read_into_flags(&mut buf, flags) {
                Ok(n) => {
                    buf.truncate(n);
                    return Ok(buf);
                }
                // The message is still queued; grow and ask again.
                Err(Error::TooBig) if cap < MAX_READ_CAP => cap *= 2,
                Err(e) => return Err(e),
            }
        }
    }

    fn acquire_read_flags(&mut self, flags: i32) -> Result<Borrowed<'_>> {
        let mut ptr: *const c_void = std::ptr::null();
        let mut len: usize = 0;
        // SAFETY: ptr and len are valid out-params; the handle is live.
        check(unsafe { sys::shuttle_acquire_read(self.ch, &mut ptr, &mut len, flags) })?;
        Ok(Borrowed {
            ptr: ptr as *const u8,
            len,
            ch: self.ch,
            _consumer: PhantomData,
        })
    }
}

impl fmt::Debug for Consumer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Consumer").field("handle", &self.ch).finish()
    }
}

impl Drop for Consumer {
    fn drop(&mut self) {
        // SAFETY: the handle is live and owned.
        unsafe { sys::shuttle_close(self.ch) };
    }
}

/// A writable span reserved in the segment, not yet published.
///
/// Fill it, then [`commit`](Reservation::commit) the number of bytes you
/// actually wrote. Nothing is visible to the consumer until then.
///
/// The C ABI has no cancel — a reservation ends only at
/// `shuttle_commit_write` — so this type does not invent one, and does not
/// implement `Drop`. Dropping a `Reservation` without committing publishes
/// nothing and leaves the reservation outstanding; the next `acquire_write` on
/// that producer then returns [`Error::InvalidArgs`]. Committing zero bytes is
/// the way to release a span you decided not to use, at the cost of an empty
/// message on the wire.
pub struct Reservation<'a> {
    ptr: *mut u8,
    len: usize,
    ch: *mut sys::shuttle_channel,
    _producer: PhantomData<&'a mut Producer>,
}

impl<'a> Reservation<'a> {
    /// The reserved bytes, writable in place. Never outlives this
    /// `Reservation`.
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        if self.len == 0 {
            return &mut [];
        }
        // SAFETY: the C layer guarantees the span is contiguous, mapped, and
        // exclusively ours until commit; the lifetime ties it to `self`.
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
    }

    /// The reserved bytes, read-only. The segment is mapped memory, so reading
    /// a not-yet-written span is defined — it is simply whatever the peer left
    /// behind.
    pub fn as_slice(&self) -> &[u8] {
        if self.len == 0 {
            return &[];
        }
        // SAFETY: as in as_mut_slice.
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }

    /// The reserved span's address, for the APIs that take a raw pointer
    /// rather than a slice (`cudaHostRegister`, a driver ioctl, an
    /// `mmap`-style handoff).
    ///
    /// On a [`CreateFlags::ALIGNED_SPANS`] channel this is page-aligned, which
    /// is what several of those APIs require:
    ///
    /// ```no_run
    /// # use shuttle::{Channel, CreateFlags};
    /// # let mut producer = Channel::create_with("/demo", 1 << 20, 1 << 16,
    /// #     CreateFlags::ALIGNED_SPANS)?.into_producer();
    /// let res = producer.acquire_write(4096)?;
    /// assert_eq!(res.as_ptr() as usize % 4096, 0);
    /// # Ok::<(), shuttle::Error>(())
    /// ```
    ///
    /// Valid only until the commit, exactly like the slices above.
    pub fn as_ptr(&self) -> *mut u8 {
        self.ptr
    }

    /// The reserved length.
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether the reservation is zero bytes long.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Publish the first `actual_len` bytes (`<=` the reserved length). This
    /// is the release edge that makes the payload visible to the consumer.
    ///
    /// Consumes the reservation, so a double commit cannot happen.
    pub fn commit(self, actual_len: usize) -> Result<()> {
        // SAFETY: the handle is live (the producer is borrowed for 'a) and a
        // reservation is outstanding.
        check(unsafe { sys::shuttle_commit_write(self.ch, actual_len) })
    }

    /// Publish the whole reserved span.
    pub fn commit_all(self) -> Result<()> {
        let len = self.len;
        self.commit(len)
    }
}

/// A message borrowed in place from the segment — the zero-copy read path.
///
/// The bytes live in shared memory and stay valid only until this value drops,
/// which performs `shuttle_release_read`; afterwards the producer may reuse
/// them. That window is expressed as a lifetime, so the compiler enforces it.
///
/// A slice cannot outlive the release:
///
/// ```compile_fail,E0597
/// use shuttle::Channel;
///
/// let mut consumer = Channel::open("/demo").unwrap().into_consumer();
/// let stale;
/// {
///     let msg = consumer.acquire_read().unwrap();
///     stale = msg.as_slice();
/// } // msg drops here: shuttle_release_read runs
/// println!("{}", stale[0]); // E0597: `msg` does not live long enough
/// ```
///
/// And no second borrow may be outstanding:
///
/// ```compile_fail,E0499
/// use shuttle::Channel;
///
/// let mut consumer = Channel::open("/demo").unwrap().into_consumer();
/// let first = consumer.acquire_read().unwrap();
/// let second = consumer.acquire_read().unwrap(); // E0499: already borrowed
/// println!("{} {}", first.len(), second.len());
/// ```
///
/// Both are compile errors, not runtime faults — that is the whole reason this
/// crate hand-writes a wrapper instead of exposing the raw calls.
pub struct Borrowed<'a> {
    ptr: *const u8,
    len: usize,
    ch: *mut sys::shuttle_channel,
    _consumer: PhantomData<&'a mut Consumer>,
}

impl<'a> Borrowed<'a> {
    /// The payload, viewed in place. No copy is taken; the slice cannot
    /// outlive this `Borrowed`.
    pub fn as_slice(&self) -> &[u8] {
        if self.len == 0 {
            return &[];
        }
        // SAFETY: the C layer guarantees the payload is contiguous and mapped
        // until release, and release is this value's Drop; the lifetime ties
        // the slice to `self`.
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }

    /// The payload's address, for the APIs that take a raw pointer rather than
    /// a slice — `cudaHostRegister`, Metal's `newBufferWithBytesNoCopy`, a
    /// driver ioctl. On a [`CreateFlags::ALIGNED_SPANS`] channel it is
    /// page-aligned, which is what those APIs require in order to map the
    /// bytes in place instead of copying them.
    ///
    /// The pointer is valid only while this `Borrowed` lives, and the borrow
    /// checker cannot follow it out of the type the way it follows
    /// [`Borrowed::as_slice`] — anything you hand it to must be finished with
    /// it before the borrow drops.
    pub fn as_ptr(&self) -> *const u8 {
        self.ptr
    }

    /// The payload length in bytes.
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether the message is zero bytes long. Empty messages are legal.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// An owned copy of the payload. Defeats zero-copy — be deliberate.
    pub fn to_vec(&self) -> Vec<u8> {
        self.as_slice().to_vec()
    }

    /// Release explicitly and surface the result.
    ///
    /// `Drop` already releases and has nowhere to report a failure; use this
    /// when you want the code. Consumes the borrow, so no double release.
    pub fn release(self) -> Result<()> {
        let me = ManuallyDrop::new(self);
        // SAFETY: the handle is live (the consumer is borrowed for 'a) and a
        // borrow is outstanding; ManuallyDrop keeps Drop from releasing again.
        check(unsafe { sys::shuttle_release_read(me.ch) })
    }
}

impl<'a> Drop for Borrowed<'a> {
    fn drop(&mut self) {
        // SAFETY: the handle is live and a borrow is outstanding. The result
        // is discarded — a Drop has nowhere to report it; call
        // Borrowed::release when the code matters.
        unsafe { sys::shuttle_release_read(self.ch) };
    }
}

/// A channel name as a C string. Rejects an interior NUL the same way the C
/// layer rejects a malformed name.
fn c_name(name: &str) -> Result<CString> {
    CString::new(name).map_err(|_| Error::InvalidArgs)
}
