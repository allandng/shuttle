#pragma once

// Segment layout: control header followed by the BipBuffer data region.
// Everything in the segment is fixed-width and referenced by byte OFFSET
// from the segment base — never by pointer; each process maps the segment
// at a different address (App. B #1).

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "shuttle/bipbuffer.hpp"  // kFrameHeader (transport framing)
#include "shuttle/platform.hpp"   // ParkArea (the park/wake block type)

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

// PAGE-ALIGNED PAYLOAD SPANS (v1.4). Every payload the channel carries starts
// on a system-page boundary, so a borrowed span can be handed straight to an
// API that requires page-aligned host memory — Metal's
// newBufferWithBytesNoCopy, cudaHostRegister — with no copy and no bounce
// buffer. Framing becomes, per message (see bipbuffer.hpp's FRAME GEOMETRY):
//
//     [8B length header][pad to page][payload][pad to page]
//
// so the payload sits at frame_start + page and the frame stride is
// page + round_up(len, page). data_offset is rounded up to a page too, which is
// what keeps every frame start page-aligned across wraps.
//
// SPECIAL, exactly like kFlagStats and for the same reason: an opener that
// merely IGNORED this bit would misparse every frame, because the bit changes
// the on-segment framing rather than adding to it. Ignoring is therefore not an
// option, and the geometry is what enforces it — an aligned segment's
// data_offset (4096) is not the value the unaligned rule demands (1280/1536),
// so a binary that does not know the bit REJECTS the segment at its geometry
// check with kErrCorrupt. Corrupt rather than kErrBadVersion is the deliberate,
// accurate verdict: the offset genuinely disagrees with the layout rule that
// binary knows, and no layout VERSION was added (the header shape is
// unchanged). See docs/API.md, "Page-aligned payload spans".
//
// The alignment unit is the SYSTEM page (platform.hpp's page_size()), never a
// huge page: 0x10 composes with the hugetlb backings and with kFlagStats.
constexpr uint32_t kFlagAlignedSpans = 0x10;

// FILE-BACKED SEGMENT (v1.4): the channel's bytes live in an ordinary file at
// an absolute path (SegBacking::kFile in platform.hpp) instead of a POSIX shm
// object, so capacity is bounded by the filesystem rather than by RAM and the
// page cache decides what is resident.
//
// Persisted but INFORMATIONAL, exactly like the hugetlb bits — an opener takes
// no action on it. It cannot: the opener already had to name the file to reach
// the segment at all, and the framing and geometry are identical to an shm
// segment's. The bit exists so tools, tests, and peers can SEE what the creator
// asked for (`inspect` prints it, and P3's prefetch gates on it).
//
// It is also the one bit that is NOT selected through create()/create_ex: those
// take an shm NAME, and there is no path to put in it, so they mask 0x20 off
// like any other bit they do not implement. The file-backed symbols
// (shuttle::create_file / shuttle_create_file) set it themselves. Asymmetric on
// purpose, and documented in docs/API.md.
constexpr uint32_t kFlagFileBacked = 0x20;

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
    //     The concrete TYPE lives behind the platform seam (ParkArea in
    //     platform.hpp): on POSIX it is { pthread_mutex_t lock; pthread_cond_t
    //     not_empty, not_full; } in that exact order, so `park.lock`,
    //     `park.not_empty`, `park.not_full` land at the identical byte offsets
    //     the v1 layout froze (the static_asserts below prove it); on Windows
    //     it is an inert placeholder (WaitOnAddress waits on the cursors), and
    //     the segment's data_offset is derived independently — segments never
    //     cross an OS boundary. header.hpp thus carries no pthread type and no
    //     platform #ifdef. ---
    alignas(kCacheLine) ParkArea park;

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
    // Messages the caller chose to discard under the opt-in lossy write
    // policy (the C ABI's SHUTTLE_DROP_NEWEST; see Producer::count_drop).
    // Stays zero unless the caller opts in per call — the library never drops
    // on its own: a full ring blocks or reports kErrWouldBlock by default.
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
static_assert(offsetof(ChannelHeader, park) == 8 * kCacheLine);

// The v1 header size, derived from the v1 layout itself (the park block is its
// last member, rounded up to a line) rather than from sizeof, which now
// includes the v2 lines. This is the frozen v1 data_offset: the byte count an
// already-shipped binary computed as sizeof(ChannelHeader). The park block is
// now the ParkArea member; on POSIX its three pthread members are laid out
// exactly as the three former standalone members were, so this value does not
// move (proven by the hard-coded tripwire below).
constexpr uint64_t kParkBlockEnd =
    offsetof(ChannelHeader, park) + sizeof(ParkArea);
constexpr uint64_t kDataOffsetV1 =
    (kParkBlockEnd + kCacheLine - 1) / kCacheLine * kCacheLine;

// REGRESSION TRIPWIRE (the WP8 safety rail): the POSIX v1 data_offset is a
// cross-process on-disk ABI value. kExpectedDataOffsetV1 is hard-coded in the
// seam (1280 on both POSIX ABIs; 0 = "no expectation", i.e. Windows, whose
// single-OS segments derive their own offset). If a header refactor ever moves
// the POSIX offset, this fires at COMPILE time — do not "fix" it by editing the
// number; a moved offset is an ABI break.
static_assert(kExpectedDataOffsetV1 == 0 ||
                  kDataOffsetV1 == kExpectedDataOffsetV1,
              "POSIX v1 data_offset changed — on-disk ABI break");

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
// (kFlagAlignedSpans segments round whichever of these applies UP to a system
// page — see data_offset_for below. That rounding is a property of the FLAGS,
// not of a layout version: the header shape is identical either way, only the
// gap between the header and the data region grows.)
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

// The gate on the aligned framing, and the mirror of has_stats: here the FLAG
// is the gate, because 0x10 adds no header fields and so bumps no version (see
// kFlagAlignedSpans). Producer and Consumer resolve it ONCE at construction,
// exactly as they resolve the stats pointer, so the default path pays a single
// perfectly-predictable branch per message and nothing else.
inline bool has_aligned_spans(const ChannelHeader* h) noexcept {
    return h != nullptr && (h->flags & kFlagAlignedSpans) != 0;
}

// The ONLY legal data_offset for a segment with this version and these flags.
// The version picks the header size (v1 vs the v2 stats layout); the aligned
// flag then rounds that up to a page so the data region — and therefore every
// frame start in it — is page-aligned. Both create() and validate_header()
// compute the offset here, so the writer and the reader of a segment can never
// disagree about it. (fuzz/header_fuzz.cpp deliberately does NOT call this: its
// oracle restates the rule independently, which is the point of an oracle.)
inline uint64_t data_offset_for(uint32_t version, uint32_t flags,
                                uint64_t page) noexcept {
    const uint64_t base =
        version == kVersionStats ? kDataOffsetV2 : kDataOffsetV1;
    return (flags & kFlagAlignedSpans) != 0 ? round_up_page(base, page) : base;
}

// The one sanctioned way to turn a segment offset into a local address.
inline void* resolve(void* base, uint64_t offset) noexcept {
    return static_cast<unsigned char*>(base) + offset;
}
inline const void* resolve(const void* base, uint64_t offset) noexcept {
    return static_cast<const unsigned char*>(base) + offset;
}

}  // namespace shuttle
