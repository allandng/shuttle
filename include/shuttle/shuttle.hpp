#pragma once

// Internal C++ lifecycle API (Phase 1). The extern "C" ABI that mirrors
// these signatures is frozen in Phase 6; error codes are plain ints from
// day one so that freeze is mechanical.

#include <cstddef>

#include "shuttle/header.hpp"

namespace shuttle {

enum Err : int {
    kOk = 0,
    kErrInvalidArgs = -1,
    kErrNameTooLong = -2,       // platform shm name limit (macOS ~31 chars)
    kErrExists = -3,            // create: name already exists
    kErrNotFound = -4,          // open/unlink: no such segment
    kErrSys = -5,               // unexpected syscall failure (see errno)
    kErrBadMagic = -6,          // FR-3
    kErrBadVersion = -7,        // FR-3 (distinct from magic)
    kErrCapacityTooSmall = -8,  // FR-4: capacity < max_payload + framing
    kErrInitTimeout = -9,       // opener: creator never published init
    kErrCorrupt = -10,          // NFR-S2: header fields fail validation
    kErrMsgTooLarge = -11,      // payload > max_payload: fail fast (§2.2)
    kErrWouldBlock = -12,       // non-blocking op cannot proceed (IF-3)
    kErrPeerDead = -13,         // blocked wait aborted: peer heartbeat stale
    // create with kFlagHugeTLB2M/1G: the explicit huge pages could not be
    // delivered — no hugetlbfs mount of that page size, no free reserved pages
    // (hugetlbfs accounts them at mmap time), or a platform without hugetlbfs
    // at all. Never a silent downgrade to normal pages: that is the whole
    // difference from the advisory kFlagHugePages.
    kErrNoHugePages = -14,
    kErrNoStats = -15,  // get_stats: segment has no stats block (v1 layout)
};

// NOT an error, and the first POSITIVE code in the surface: an opt-in
// drop-newest write found no room and threw the message away instead of
// blocking (C ABI: SHUTTLE_DROPPED, from shuttle_write with kOpDropNewest).
// Callers that test `rc != kOk` therefore see a successful drop as a failure;
// testing `rc < 0` for errors is the form that stays correct. Nothing returns
// this unless the caller asked for the policy on that very call.
constexpr int kDropped = 1;

// Per-op flag bits (the `flags` argument of the read/write entry points) — a
// SEPARATE namespace from the kFlag* create-flags in header.hpp. Blocking is
// the default; these are the opt-outs.
//   kOpNonBlock   — try-semantics: kErrWouldBlock instead of parking.
//   kOpDropNewest — write only: try-semantics PLUS a lossy policy; a message
//                   that does not fit now is dropped and kDropped returned.
//                   Meaningless on the read/acquire paths, which reject it
//                   with kErrInvalidArgs rather than ignore it.
constexpr int kOpNonBlock = 0x1;
constexpr int kOpDropNewest = 0x2;

// Snapshot of the opt-in counters (kVersionStats segments only). Plain
// uint64s, not atomics: get_stats copies them out. Byte counts are PAYLOAD
// bytes; the 8-byte frame header is excluded on both sides.
struct Stats {
    uint64_t msgs_written;
    uint64_t bytes_written;
    // Messages thrown away by an opt-in drop-newest write (kOpDropNewest is
    // never implied): 0 on any channel that never asks for a lossy policy,
    // which is every channel by default.
    uint64_t msgs_dropped;
    uint64_t msgs_read;
    uint64_t bytes_read;
};

// Local (per-process) handle; never stored in the segment.
struct Channel {
    void* base;
    size_t map_len;
    ChannelHeader* hdr;
    // Segment handle retained past mapping. On POSIX this is always kSegInvalid
    // — the fd is dropped once the mapping keeps the object alive — so close()
    // is a no-op on it. On Windows it is the live section HANDLE the creator/
    // opener must hold for the channel's life (a named section vanishes with
    // its last handle); close() releases it. See seg_keep_after_map().
    SegHandle seg;
};

// FR-1: shm_open(O_CREAT|O_EXCL) + one-shot ftruncate + mmap + header init,
// publishing init last with a release store. Owner-only permissions (NFR-S1).
// create_flags carries opt-in create-time bits (kFlagHugePages, kFlagStats,
// kFlagHugeTLB2M, kFlagHugeTLB1G); unknown bits are masked off. Defaulted for
// source-compatibility with pre-flags callers. kFlagStats additionally selects
// the kVersionStats layout; without it the segment written is byte-for-byte the
// v1 format as before. A hugetlb bit instead selects the segment's BACKING (a
// hugetlbfs file) and can fail with kErrNoHugePages; setting both is
// kErrInvalidArgs.
Channel* create(const char* name, size_t capacity_bytes,
                size_t max_payload_bytes, int* err, uint32_t create_flags = 0);

// FR-3 / NFR-S2: the pure, post-map half of open()'s validation — magic,
// version, and the version-selected geometry — over an already-mapped byte
// range [base, base + map_len). No syscalls, no I/O, no global state: it only
// reads the cold identity block, which the creator writes once before it
// release-stores init_state. open() calls this after its init-state spin, and
// is the only reason the function exists as a separate symbol: it makes the
// validation testable and fuzzable (fuzz/header_fuzz.cpp) against arbitrary
// bytes without a real segment behind them.
//
// Returns kOk, kErrBadMagic, kErrBadVersion, or kErrCorrupt — the same codes,
// in the same precedence, that open() reports. A null base or a range shorter
// than the smallest known header (kDataOffsetV1) is kErrCorrupt, matching the
// verdict open() already reaches before mapping such an object; the function
// never reads outside the range it was given.
//
// NOT included, because neither is pure: the acquire-spin on init_state and
// the peer-liveness/THP work. Both stay in open().
int validate_header(const void* base, size_t map_len) noexcept;

// FR-2/FR-3: attach without re-init; waits for init publication, then
// validates magic, version, and header sanity (NFR-S2) via validate_header.
Channel* open(const char* name, int* err);

// FR-5: unmap and free the local handle; the named object survives.
void close(Channel* ch);

// FR-5: remove the named object. Returns kOk / kErrNotFound / kErrSys.
int unlink(const char* name);

// Copy out the segment's counters. kOk, kErrInvalidArgs (null channel), or
// kErrNoStats if the segment was not created with kFlagStats (v1 layout: the
// bytes those fields would occupy are data, and are never touched). The
// snapshot is per-field relaxed — a producer/consumer running concurrently may
// have advanced one counter and not yet another; each field is individually
// exact and monotonic, the five together are not an atomic tuple.
int get_stats(Channel* ch, Stats& out);

}  // namespace shuttle
