#pragma once

// Segment layout: control header followed by the BipBuffer data region.
// Everything in the segment is fixed-width and referenced by byte OFFSET
// from the segment base — never by pointer; each process maps the segment
// at a different address (App. B #1).

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "shuttle/platform.hpp"

namespace shuttle {

constexpr uint64_t kMagic = 0x53485554544C4531ull;  // "SHUTTLE1"
constexpr uint32_t kVersion = 1;

// init_state values: 0 (zero-filled segment) = uninitialized.
constexpr uint32_t kInitReady = 0x52454459;  // "REDY"

// Hot atomics get a full line each. 128 B = Apple Silicon line size; also
// correct (2x conservative) on x86 (binding minor amendment).
constexpr size_t kCacheLine = 128;

// Transport framing: each message is [u64 little-endian length | payload].
constexpr uint64_t kFrameHeader = 8;

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

    // --- park/wake primitives, off the hot path (§2.3) ---
    alignas(kCacheLine) pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

// The data region starts immediately after the (cache-line-multiple) header.
constexpr uint64_t kDataOffset = sizeof(ChannelHeader);

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
static_assert(offsetof(ChannelHeader, write) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, watermark) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, read) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, producer_waiting) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, consumer_waiting) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, producer_heartbeat) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, consumer_heartbeat) % kCacheLine == 0);
static_assert(offsetof(ChannelHeader, lock) % kCacheLine == 0);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static_assert(sizeof(ChannelHeader) % kCacheLine == 0,
              "data region must start on a cache-line boundary");
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == 8);
static_assert(sizeof(std::atomic<uint32_t>) == 4);

// The one sanctioned way to turn a segment offset into a local address.
inline void* resolve(void* base, uint64_t offset) noexcept {
    return static_cast<unsigned char*>(base) + offset;
}
inline const void* resolve(const void* base, uint64_t offset) noexcept {
    return static_cast<const unsigned char*>(base) + offset;
}

}  // namespace shuttle
