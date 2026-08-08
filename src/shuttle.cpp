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

    const size_t map_len = kDataOffset + capacity_bytes;
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
    const uint32_t flags = create_flags & kFlagHugePages;
    if (flags & kFlagHugePages) advise_huge_pages(base, map_len);

    // Creation zero-fills, so init_state is already 0 (uninitialized) and
    // cursors/heartbeats are already 0; set the rest explicitly. flags is part
    // of the cold identity block: written once here, before the init_state
    // release-store publish, and immutable after (single-init contract).
    auto* h = static_cast<ChannelHeader*>(base);
    h->magic = kMagic;
    h->version = kVersion;
    h->flags = flags;
    h->data_offset = kDataOffset;
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
    // whatever is behind this name, it is not a channel.
    const int64_t seg_len = seg_size(seg);
    if (seg_len < static_cast<int64_t>(sizeof(ChannelHeader))) {
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
    if (h->version != kVersion) {
        seg_unmap(base, map_len);
        set_err(err, kErrBadVersion);
        return nullptr;
    }
    // NFR-S2: never trust header geometry — everything later indexes off it.
    // Coverage check is >= not ==: macOS rounds shm st_size up to page size,
    // so the mapping may legitimately be larger than the claimed geometry.
    if (h->data_offset != kDataOffset ||
        h->data_capacity > map_len - kDataOffset ||
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

int unlink(const char* name) {
    if (name == nullptr || name[0] != '/') return kErrInvalidArgs;
    const int seg_err = seg_unlink(name);
    if (seg_err != 0) {
        return seg_err == ENOENT ? kErrNotFound : kErrSys;
    }
    return kOk;
}

}  // namespace shuttle
