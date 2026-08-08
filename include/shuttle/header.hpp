#pragma once

// Segment layout: control header followed by the BipBuffer data region.
// Everything in the segment is fixed-width and referenced by byte OFFSET
// from the segment base — never by pointer; each process maps the segment
// at a different address (App. B #1).

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "shuttle/bipbuffer.hpp"  // kFrameHeader (transport framing)
#include "shuttle/platform.hpp"

namespace shuttle {

constexpr uint64_t kMagic = 0x53485554544C4531ull;  // "SHUTTLE1"

// Layout versions. kVersion is the DEFAULT layout and the only one a plain
// create() ever writes — the on-disk default format is unchanged. A version
// is NOT a flag: it gates the header's physical size, so an opener that does
// not know a version must refuse the segment (kErrBadVersion) rather than
// ignore the difference.
constexpr uint32_t kVersion = 1;
// v2 = v1 plus the two trailing statistics cache lines (opt-in, kFlagStats).
// Everything at and below data_offset in v1 keeps its exact offset, so a v2
// segment differs from a v1 segment only by a larger header.
constexpr uint32_t kVersionStats = 2;

// init_state values: 0 (zero-filled segment) = uninitialized.
constexpr uint32_t kInitReady = 0x52454459;  // "REDY"

// Create-flag bits for ChannelHeader::flags. Contract: the creator writes the
// full flags word ONCE, in the cold identity block, before the release-store
// that publishes init_state; it is immutable thereafter. Openers must IGNORE
// unknown bits — flags is an additive extension point, so new bits carry no
// kVersion bump (an old opener simply doesn't act on a bit it doesn't know).
constexpr uint32_t kFlagHugePages = 0x1;  // creator advised MADV_HUGEPAGE

// IMPLEMENTED: the segment is backed by EXPLICIT, reserved huge pages — a file
// on a hugetlbfs mount of the matching page size (SegBacking::kHugeTLB* in
// platform.hpp), not the advisory promotion kFlagHugePages asks for. The two
// are mutually exclusive at create() (both set = kErrInvalidArgs), and a
// request that cannot be honored fails with kErrNoHugePages rather than
// falling back to normal pages.
//
// Persisted but INFORMATIONAL: an opener takes no action on these bits. It
// does not need to — seg_open discovers the hugetlbfs file by name, and a
// MAP_SHARED mapping of a hugetlbfs file is huge-page backed because the file
// says so (MAP_HUGETLB is for anonymous mappings). The bits exist so tools,
// tests, and peers can SEE what the creator asked for.
constexpr uint32_t kFlagHugeTLB2M = 0x2;
constexpr uint32_t kFlagHugeTLB1G = 0x4;

// The segment carries the statistics counters (layout kVersionStats).
// SPECIAL, and the one exception to "a new bit needs no version bump": this
// bit implies the v2 header LAYOUT, and layout is version-checked. The flag
// itself is informational — `version` is the gate every opener acts on (see
// has_stats below). An old binary meeting a v2 segment therefore reports
// kErrBadVersion from its exact version check, which is the designed
// behavior: it must not map a header shape it does not know.
constexpr uint32_t kFlagStats = 0x8;

// Hot atomics get a full line each. 128 B = Apple Silicon line size; also
// correct (2x conservative) on x86 (binding minor amendment).
constexpr size_t kCacheLine = 128;

struct alignas(kCacheLine) ChannelHeader {
    // --- cold identity block: written once by the creator, immutable after
    //     init_state is published ---
    uint64_t magic;
    uint32_t version;
    uint32_t flags;
    uint64_t data_offset;    // segment base -> data region (byte offset)
    uint64_t data_capacity;  // bytes in the data region
    uint64_t max_payload;    // largest single payload accepted
    // Single-init publication (App. B #5): creator initializes everything
    // above and below, then release-stores kInitReady; openers acquire-spin
    // on this before trusting any other field.
    std::atomic<uint32_t> init_state;

    // --- BipBuffer cursors, amendment A1: three absolute offsets into the
    //     data region, each strictly single-writer. If write >= read, valid
    //     data is [read, write); else [read, watermark) then [0, write).
    //     Producer-private reserve state is process-local, NOT in here. ---
    alignas(kCacheLine) std::atomic<uint64_t> write;      // producer-owned
    alignas(kCacheLine) std::atomic<uint64_t> watermark;  // producer-owned
    alignas(kCacheLine) std::atomic<uint64_t> read;       // consumer-owned

    // --- parking flags (Phase 4; seq_cst park protocol per amendment A4) ---
    alignas(kCacheLine) std::atomic<uint32_t> producer_waiting;
    alignas(kCacheLine) std::atomic<uint32_t> consumer_waiting;

    // --- liveness heartbeats (Phase 5; primary mechanism on BOTH platforms
    //     per amendment A3) ---
    alignas(kCacheLine) std::atomic<uint64_t> producer_heartbeat;
    alignas(kCacheLine) std::atomic<uint64_t> consumer_heartbeat;

    // --- park/wake primitives, off the hot path (§2.3). END OF LAYOUT v1:
    //     everything below this point exists ONLY in kVersionStats segments.
    //     ---
    alignas(kCacheLine) pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    // --- opt-in statistics (kVersionStats / kFlagStats ONLY) ---------------
    // DANGER: on a v1 segment these bytes are not header at all — they are the
    // first 256 bytes of the DATA REGION. Nothing may read or write them
    // unless has_stats() says the segment is v2; Producer/Consumer resolve a
    // nullable pointer once, at construction, and branch on it.
    //
    // Ownership is single-writer, exactly like the cursors and heartbeats
    // above: the producer alone writes the first line, the consumer alone
    // writes the second. Every update is a relaxed load + relaxed store, never
    // an RMW (the heartbeat idiom) — there is no second writer to lose an
    // update to. Readers (get_stats, inspect, a peer) take relaxed loads: the
    // counters are monotonic and eventually-consistent statistics, deliberately
    // NOT synchronized with the data path, so observing them costs the hot
    // path no ordering.
    //
    // Byte counters count PAYLOAD bytes only — the 8-byte frame header is
    // transport overhead and is excluded on both sides, so bytes_written and
    // bytes_read are directly comparable to the lengths the caller passed.

    // producer-owned line
    alignas(kCacheLine) std::atomic<uint64_t> stat_msgs_written;
    std::atomic<uint64_t> stat_bytes_written;
    // RESERVED: written only by a later drop-policy package; always zero today
    // (nothing in this library drops a message — a full ring blocks or reports
    // kErrWouldBlock). The field exists now so its offset is frozen with the
    // rest of the v2 layout.
    std::atomic<uint64_t> stat_msgs_dropped;

    // consumer-owned line
    alignas(kCacheLine) std::atomic<uint64_t> stat_msgs_read;
    std::atomic<uint64_t> stat_bytes_read;
};

// Layout freeze: a change that moves these fields is an ABI break and must
// bump kVersion. offsetof on this type is conditionally-supported because
// std::atomic is not standard-layout, but on both target ABIs (libc++,
// libstdc++) atomic<integral> is layout-identical to the integral; the
// pragma silences the pedantic warning, it does not hide a real risk.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(ChannelHeader, magic) == 0);
// v1 offsets, pinned to their exact line index. These are the offsets other
// processes' binaries index by; appending the v2 stats lines must not move a
// single one of them, and this block is what proves it.
static_assert(offsetof(ChannelHeader, write) == 1 * kCacheLine);
static_assert(offsetof(ChannelHeader, watermark) == 2 * kCacheLine);
static_assert(offsetof(ChannelHeader, read) == 3 * kCacheLine);
static_assert(offsetof(ChannelHeader, producer_waiting) == 4 * kCacheLine);
static_assert(offsetof(ChannelHeader, consumer_waiting) == 5 * kCacheLine);
static_assert(offsetof(ChannelHeader, producer_heartbeat) == 6 * kCacheLine);
static_assert(offsetof(ChannelHeader, consumer_heartbeat) == 7 * kCacheLine);
static_assert(offsetof(ChannelHeader, lock) == 8 * kCacheLine);

// The v1 header size, derived from the v1 layout itself (the park block was
// its last member, rounded up to a line) rather than from sizeof, which now
// includes the v2 lines. This is the frozen v1 data_offset: the byte count an
// already-shipped binary computed as sizeof(ChannelHeader).
constexpr uint64_t kParkBlockEnd =
    offsetof(ChannelHeader, not_full) + sizeof(pthread_cond_t);
constexpr uint64_t kDataOffsetV1 =
    (kParkBlockEnd + kCacheLine - 1) / kCacheLine * kCacheLine;

// The v2 stats block therefore begins exactly where v1's data region began —
// the independent proof that v1 grew by appending and nothing else.
static_assert(offsetof(ChannelHeader, stat_msgs_written) == kDataOffsetV1);
static_assert(offsetof(ChannelHeader, stat_bytes_written) == kDataOffsetV1 + 8);
static_assert(offsetof(ChannelHeader, stat_msgs_dropped) == kDataOffsetV1 + 16);
static_assert(offsetof(ChannelHeader, stat_msgs_read) ==
              kDataOffsetV1 + kCacheLine);
static_assert(offsetof(ChannelHeader, stat_bytes_read) ==
              kDataOffsetV1 + kCacheLine + 8);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The data region starts immediately after the header — whose size now depends
// on the layout version. There is deliberately no version-agnostic kDataOffset
// constant any more: every site must say which layout it means.
constexpr uint64_t kDataOffsetV2 = sizeof(ChannelHeader);
static_assert(kDataOffsetV2 == kDataOffsetV1 + 2 * kCacheLine,
              "v2 appends exactly the two stats lines");
static_assert(kDataOffsetV1 % kCacheLine == 0,
              "v1 data region must start on a cache-line boundary");
static_assert(sizeof(ChannelHeader) % kCacheLine == 0,
              "data region must start on a cache-line boundary");
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == 8);
static_assert(sizeof(std::atomic<uint32_t>) == 4);

// The one gate on the stats block. Version, not the flag: kFlagStats is
// informational (and a v1 opener would ignore it anyway), whereas `version`
// is what every opener already validates exactly. >= rather than == so a
// future v3 that keeps the block inherits it.
inline bool has_stats(const ChannelHeader* h) noexcept {
    return h != nullptr && h->version >= kVersionStats;
}

// The one sanctioned way to turn a segment offset into a local address.
inline void* resolve(void* base, uint64_t offset) noexcept {
    return static_cast<unsigned char*>(base) + offset;
}
inline const void* resolve(const void* base, uint64_t offset) noexcept {
    return static_cast<const unsigned char*>(base) + offset;
}

}  // namespace shuttle
