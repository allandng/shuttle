#include "shuttle/shuttle.hpp"

#include <unistd.h>

#include <cerrno>

#include "shuttle/platform.hpp"

namespace shuttle {

const char* platform_name() noexcept { return SHUTTLE_PLATFORM_NAME; }

namespace {

void set_err(int* err, int code) noexcept {
    if (err != nullptr) *err = code;
}

constexpr uint64_t kInitWaitNs = 5ull * 1000000000ull;

}  // namespace

Channel* create(const char* name, size_t capacity_bytes,
                size_t max_payload_bytes, int* err, uint32_t create_flags) {
    if (name == nullptr || name[0] != '/' || capacity_bytes == 0 ||
        max_payload_bytes == 0) {
        set_err(err, kErrInvalidArgs);
        return nullptr;
    }
    if (!shm_name_ok(name)) {
        set_err(err, kErrNameTooLong);
        return nullptr;
    }
    // FR-4: a write that can never be satisfied must be impossible by
    // construction, or blocking backpressure would park the producer forever.
    if (capacity_bytes < max_payload_bytes + kFrameHeader) {
        set_err(err, kErrCapacityTooSmall);
        return nullptr;
    }

    // Known-bits mask. kFlagHugeTLB2M/1G are pinned but unimplemented, so they
    // stay OUT of this mask: passing one today is masked off like any unknown
    // bit, preserving "an unknown bit is never persisted". They join the mask
    // in the package that implements them.
    const uint32_t flags = create_flags & (kFlagHugePages | kFlagStats);
    // kFlagStats is the one flag that selects a LAYOUT, so it also selects the
    // version and the header size. Without it this writes exactly the v1
    // segment it always did — the default on-disk format is unchanged.
    const bool with_stats = (flags & kFlagStats) != 0;
    const uint64_t data_offset = with_stats ? kDataOffsetV2 : kDataOffsetV1;

    const size_t map_len = static_cast<size_t>(data_offset) + capacity_bytes;
    // Backing choice is a seam concern (kShm today; hugetlbfs backings land
    // there later); creation is exclusive and sizes the object once, for good.
    int seg_err = 0;
    const SegHandle seg = seg_create(name, map_len, SegBacking::kShm, seg_err);
    if (seg == kSegInvalid) {
        set_err(err, seg_err == EEXIST ? kErrExists : kErrSys);
        return nullptr;
    }
    void* base = seg_map(seg, map_len);
    seg_close(seg);
    if (base == nullptr) {
        (void)seg_unlink(name);
        set_err(err, kErrSys);
        return nullptr;
    }

    // Opt-in THP: advise the fresh mapping before it is touched. Advisory and
    // masked to known bits — an unknown flag must never be persisted (openers
    // trust that flags carries only bits they may act on).
    if (flags & kFlagHugePages) advise_huge_pages(base, map_len);

    // Creation zero-fills, so init_state is already 0 (uninitialized) and
    // cursors/heartbeats are already 0 — and so are the v2 stats lines, which
    // therefore need no explicit initialization. Set the rest explicitly.
    // flags is part of the cold identity block: written once here, before the
    // init_state release-store publish, and immutable after (single-init).
    auto* h = static_cast<ChannelHeader*>(base);
    h->magic = kMagic;
    h->version = with_stats ? kVersionStats : kVersion;
    h->flags = flags;
    h->data_offset = data_offset;
    h->data_capacity = capacity_bytes;
    h->max_payload = max_payload_bytes;
    if (mutex_init_pshared(&h->lock) != 0 ||
        cond_init_pshared_monotonic(&h->not_empty) != 0 ||
        cond_init_pshared_monotonic(&h->not_full) != 0) {
        seg_unmap(base, map_len);
        (void)seg_unlink(name);
        set_err(err, kErrSys);
        return nullptr;
    }
    // Publish: openers must not trust any field before this store (App. B #5).
    h->init_state.store(kInitReady, std::memory_order_release);

    set_err(err, kOk);
    return new Channel{base, map_len, h};
}

Channel* open(const char* name, int* err) {
    if (name == nullptr || name[0] != '/') {
        set_err(err, kErrInvalidArgs);
        return nullptr;
    }
    int seg_err = 0;
    const SegHandle seg = seg_open(name, seg_err);
    if (seg == kSegInvalid) {
        set_err(err, seg_err == ENOENT ? kErrNotFound : kErrSys);
        return nullptr;
    }
    // An unreadable size (-1) and a too-small object are the same verdict:
    // whatever is behind this name, it is not a channel. The bar is the
    // SMALLEST header any known version has (v1) — the version is not readable
    // until the object is mapped, and a legitimately small v1 segment must not
    // be rejected just because the v2 header is bigger. The per-version
    // geometry check below is what enforces the actual size requirement.
    const int64_t seg_len = seg_size(seg);
    if (seg_len < static_cast<int64_t>(kDataOffsetV1)) {
        seg_close(seg);
        set_err(err, kErrCorrupt);
        return nullptr;
    }
    const size_t map_len = static_cast<size_t>(seg_len);
    void* base = seg_map(seg, map_len);
    seg_close(seg);
    if (base == nullptr) {
        set_err(err, kErrSys);
        return nullptr;
    }

    auto* h = static_cast<ChannelHeader*>(base);
    // Wait for the creator's release-publish; deadlined so a creator that
    // died mid-init cannot hang us (timeout = distinct error).
    const uint64_t deadline = monotonic_ns() + kInitWaitNs;
    while (h->init_state.load(std::memory_order_acquire) != kInitReady) {
        if (monotonic_ns() > deadline) {
            seg_unmap(base, map_len);
            set_err(err, kErrInitTimeout);
            return nullptr;
        }
        usleep(1000);
    }

    // FR-3: magic first, then version, distinct errors.
    if (h->magic != kMagic) {
        seg_unmap(base, map_len);
        set_err(err, kErrBadMagic);
        return nullptr;
    }
    // Two layouts are readable: the v1 default and the opt-in stats layout.
    // Anything else is a header shape this binary does not know, and mapping
    // it would mean guessing where the data region starts — refuse. (This is
    // exactly what an OLD binary does when handed a v2 segment: its check is
    // `!= kVersion`, so it reports kErrBadVersion. Designed behavior.)
    if (h->version != kVersion && h->version != kVersionStats) {
        seg_unmap(base, map_len);
        set_err(err, kErrBadVersion);
        return nullptr;
    }
    // NFR-S2: never trust header geometry — everything later indexes off it.
    // The version chooses which data_offset is the ONLY legal one; a segment
    // whose version and geometry disagree (e.g. a v1 header with its version
    // word poked to 2) is corrupt, not merely unknown — hence the distinct
    // error. Coverage check is >= not ==: macOS rounds shm st_size up to page
    // size, so the mapping may legitimately be larger than the claimed
    // geometry.
    const uint64_t want_offset =
        h->version == kVersionStats ? kDataOffsetV2 : kDataOffsetV1;
    if (h->data_offset != want_offset || map_len < want_offset ||
        h->data_capacity > map_len - want_offset ||
        h->max_payload + kFrameHeader > h->data_capacity) {
        seg_unmap(base, map_len);
        set_err(err, kErrCorrupt);
        return nullptr;
    }

    // The opener's mapping is independent of the creator's; if the creator
    // opted into huge pages, advise this mapping too (advisory, ignores
    // unknown bits per the flags contract). Only after the header is trusted.
    if (h->flags & kFlagHugePages) advise_huge_pages(base, map_len);

    set_err(err, kOk);
    return new Channel{base, map_len, h};
}

void close(Channel* ch) {
    if (ch == nullptr) return;
    seg_unmap(ch->base, ch->map_len);
    delete ch;
}

int get_stats(Channel* ch, Stats& out) {
    if (ch == nullptr || ch->hdr == nullptr) return kErrInvalidArgs;
    const ChannelHeader* h = ch->hdr;
    if (!has_stats(h)) return kErrNoStats;
    // Relaxed throughout: each counter has exactly one writer and is monotonic,
    // and the snapshot is explicitly not an atomic tuple (see the declaration).
    // Reading them must cost the data path nothing.
    out.msgs_written = h->stat_msgs_written.load(std::memory_order_relaxed);
    out.bytes_written = h->stat_bytes_written.load(std::memory_order_relaxed);
    out.msgs_dropped = h->stat_msgs_dropped.load(std::memory_order_relaxed);
    out.msgs_read = h->stat_msgs_read.load(std::memory_order_relaxed);
    out.bytes_read = h->stat_bytes_read.load(std::memory_order_relaxed);
    return kOk;
}

int unlink(const char* name) {
    if (name == nullptr || name[0] != '/') return kErrInvalidArgs;
    const int seg_err = seg_unlink(name);
    if (seg_err != 0) {
        return seg_err == ENOENT ? kErrNotFound : kErrSys;
    }
    return kOk;
}

}  // namespace shuttle
