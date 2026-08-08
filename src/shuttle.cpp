#include "shuttle/shuttle.hpp"

#include <cerrno>

#include "shuttle/platform.hpp"  // seam: seg_*, sleep_us, monotonic_ns, ...

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
    // Written as subtraction against a checked floor, never as the sum
    // `max_payload_bytes + kFrameHeader`: that addition wraps for a
    // max_payload near 2^64 and would let this guard pass on a geometry
    // open()/validate_header now correctly reject (see validate_header's
    // overflow note). Rejecting it here too keeps create() from minting a
    // segment no opener will accept.
    if (capacity_bytes < kFrameHeader ||
        max_payload_bytes > capacity_bytes - kFrameHeader) {
        set_err(err, kErrCapacityTooSmall);
        return nullptr;
    }

    // Known-bits mask: every bit whose behavior has shipped. An unknown bit is
    // masked off and therefore never persisted, so no segment in the wild
    // carries a promise this binary did not keep.
    const uint32_t flags = create_flags & (kFlagHugePages | kFlagStats |
                                           kFlagHugeTLB2M | kFlagHugeTLB1G);
    // The two hugetlb bits select a page size, so asking for both is asking
    // for two different segments. Not maskable, not resolvable — reject.
    if ((flags & kFlagHugeTLB2M) && (flags & kFlagHugeTLB1G)) {
        set_err(err, kErrInvalidArgs);
        return nullptr;
    }
    // Backing choice is a seam concern; here it is only a bit-to-enum mapping.
    const SegBacking backing = (flags & kFlagHugeTLB2M) ? SegBacking::kHugeTLB2M
                               : (flags & kFlagHugeTLB1G)
                                   ? SegBacking::kHugeTLB1G
                                   : SegBacking::kShm;
    const bool explicit_huge = backing != SegBacking::kShm;
    // kFlagStats is the one flag that selects a LAYOUT, so it also selects the
    // version and the header size. Without it this writes exactly the v1
    // segment it always did — the default on-disk format is unchanged. It is
    // orthogonal to the backing: a stats + hugetlb segment (flags 0x8|0x2) is
    // an ordinary v2 header that happens to live on hugetlbfs.
    const bool with_stats = (flags & kFlagStats) != 0;
    const uint64_t data_offset = with_stats ? kDataOffsetV2 : kDataOffsetV1;

    // hugetlbfs sizes objects in whole huge pages, so the mapping may cover
    // more than data_offset + capacity_bytes. data_capacity below still
    // records the caller's exact value; the extra bytes are slack the coverage
    // checks (all >=) tolerate, exactly as they already tolerate macOS's shm
    // page rounding.
    const size_t map_len =
        seg_map_len(static_cast<size_t>(data_offset) + capacity_bytes, backing);
    // Creation is exclusive and sizes the object once, for good.
    int seg_err = 0;
    const SegHandle seg = seg_create(name, map_len, backing, seg_err);
    if (seg == kSegInvalid) {
        // A name collision is a name collision whatever the backing. Every
        // OTHER hugetlb failure — no mount of that page size, no permission,
        // no hugetlbfs at all (macOS) — means the guarantee cannot be
        // delivered, and the contract is to say so rather than quietly hand
        // back normal pages. EINVAL is the seam's "this name has no filename
        // form" verdict, which is an argument problem, not a capacity one.
        int code = kErrSys;
        if (seg_err == EEXIST) {
            code = kErrExists;
        } else if (explicit_huge) {
            code = seg_err == EINVAL ? kErrInvalidArgs : kErrNoHugePages;
        }
        set_err(err, code);
        return nullptr;
    }
    void* base = seg_map(seg, map_len);
    if (base == nullptr) {
        seg_close(seg);
        (void)seg_unlink(name);
        // hugetlbfs reserves its pages at MMAP time, not at create/ftruncate
        // time: this is where a box with no free huge pages actually fails
        // (ENOMEM). It is the same "cannot deliver" verdict as above.
        set_err(err, explicit_huge ? kErrNoHugePages : kErrSys);
        return nullptr;
    }
    // POSIX drops the handle now (kSegInvalid); Windows RETAINS it in the
    // Channel — a named section vanishes with its last handle (seam decides).
    const SegHandle seg_keep = seg_keep_after_map(seg);

    // Opt-in THP: advise the fresh mapping before it is touched. Advisory and
    // masked to known bits — an unknown flag must never be persisted (openers
    // trust that flags carries only bits they may act on). Skipped when the
    // segment is already on explicit huge pages: advice about promoting normal
    // pages is meaningless for a mapping whose pages are huge by construction
    // (the kernel would just return EINVAL). Both bits are still persisted:
    // the hugetlb bit is informational — an opener needs no action from it,
    // because seg_open finds the hugetlbfs file and the file's inode already
    // dictates the page size.
    if ((flags & kFlagHugePages) && !explicit_huge)
        advise_huge_pages(base, map_len);

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
    if (mutex_init_pshared(&h->park.lock) != 0 ||
        cond_init_pshared_monotonic(&h->park.not_empty) != 0 ||
        cond_init_pshared_monotonic(&h->park.not_full) != 0) {
        seg_unmap(base, map_len);
        seg_close(
            seg_keep);  // release the retained handle (Windows); no-op POSIX
        (void)seg_unlink(name);
        set_err(err, kErrSys);
        return nullptr;
    }
    // Publish: openers must not trust any field before this store (App. B #5).
    h->init_state.store(kInitReady, std::memory_order_release);

    set_err(err, kOk);
    return new Channel{base, map_len, h, seg_keep};
}

// The pure post-map validation, lifted verbatim out of open() (see the
// declaration for what deliberately stayed behind). Reads only the cold
// identity block — magic, version, flags-free geometry — and never touches a
// byte outside [base, base + map_len).
int validate_header(const void* base, size_t map_len) noexcept {
    // Totality guard, not a new rule: open() has already rejected an object
    // smaller than the smallest known header (kDataOffsetV1) BEFORE mapping
    // it, so on that path this is unreachable and open()'s behavior is
    // unchanged. It exists so the function is safe for a caller that hands
    // over an arbitrary short range — the verdict is the same kErrCorrupt
    // open() gives such an object.
    if (base == nullptr || map_len < kDataOffsetV1) return kErrCorrupt;
    const auto* h = static_cast<const ChannelHeader*>(base);

    // FR-3: magic first, then version, distinct errors.
    if (h->magic != kMagic) return kErrBadMagic;
    // Two layouts are readable: the v1 default and the opt-in stats layout.
    // Anything else is a header shape this binary does not know, and mapping
    // it would mean guessing where the data region starts — refuse. (This is
    // exactly what an OLD binary does when handed a v2 segment: its check is
    // `!= kVersion`, so it reports kErrBadVersion. Designed behavior.)
    if (h->version != kVersion && h->version != kVersionStats) {
        return kErrBadVersion;
    }
    // NFR-S2: never trust header geometry — everything later indexes off it.
    // The version chooses which data_offset is the ONLY legal one; a segment
    // whose version and geometry disagree (e.g. a v1 header with its version
    // word poked to 2) is corrupt, not merely unknown — hence the distinct
    // error. Coverage check is >= not ==: macOS rounds shm st_size up to page
    // size, so the mapping may legitimately be larger than the claimed
    // geometry.
    //
    // Every comparison here is written as a SUBTRACTION against a checked
    // floor, never as a sum. `data_offset + data_capacity` and
    // `max_payload + kFrameHeader` are uint64 additions on attacker-supplied
    // words, and a sum that wraps past 2^64 turns the guard into its own
    // opposite. That is not hypothetical: the frame-length form was written
    // as `h->max_payload + kFrameHeader > h->data_capacity`, and
    // fuzz/header_fuzz.cpp found in ~2 seconds that max_payload =
    // 0xFFFFFFFFFFFFFFFF wraps it to 7 > data_capacity == false — so open()
    // ACCEPTED a segment claiming an 18-exabyte max_payload. Downstream that
    // disarms Consumer::parse's `l > h_->max_payload` length guard, and a
    // forged frame length of 2^64-8 is then handed to the caller as a valid
    // span. Keep these in subtraction form.
    const uint64_t want_offset =
        h->version == kVersionStats ? kDataOffsetV2 : kDataOffsetV1;
    if (h->data_offset != want_offset || map_len < want_offset ||
        h->data_capacity > map_len - want_offset ||
        h->data_capacity < kFrameHeader ||
        h->max_payload > h->data_capacity - kFrameHeader) {
        return kErrCorrupt;
    }
    return kOk;
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
    if (base == nullptr) {
        seg_close(seg);
        set_err(err, kErrSys);
        return nullptr;
    }
    // POSIX drops the handle now (kSegInvalid); Windows RETAINS it (seam).
    const SegHandle seg_keep = seg_keep_after_map(seg);

    auto* h = static_cast<ChannelHeader*>(base);
    // Wait for the creator's release-publish; deadlined so a creator that
    // died mid-init cannot hang us (timeout = distinct error).
    const uint64_t deadline = monotonic_ns() + kInitWaitNs;
    while (h->init_state.load(std::memory_order_acquire) != kInitReady) {
        if (monotonic_ns() > deadline) {
            seg_unmap(base, map_len);
            seg_close(
                seg_keep);  // release retained handle (Windows); no-op POSIX
            set_err(err, kErrInitTimeout);
            return nullptr;
        }
        sleep_us(1000);
    }

    // FR-3 / NFR-S2: magic, version, geometry. Pure and self-contained, so it
    // lives in validate_header() where a fuzzer can reach it directly; the
    // codes and their precedence are exactly what this block reported inline.
    const int verdict = validate_header(base, map_len);
    if (verdict != kOk) {
        seg_unmap(base, map_len);
        seg_close(seg_keep);  // release retained handle (Windows); no-op POSIX
        set_err(err, verdict);
        return nullptr;
    }

    // The opener's mapping is independent of the creator's; if the creator
    // opted into THP, advise this mapping too (advisory, ignores unknown bits
    // per the flags contract). Only after the header is trusted.
    //
    // An explicitly hugetlb-backed segment needs NOTHING here: seg_open found
    // the file on the hugetlbfs mount, and a MAP_SHARED mapping of such a file
    // is huge-page backed by virtue of the file's filesystem — MAP_HUGETLB is
    // an ANONYMOUS-mapping flag and has no role on this path. So the hugetlb
    // bits are read only to suppress the meaningless THP advice.
    const bool seg_is_hugetlb =
        (h->flags & (kFlagHugeTLB2M | kFlagHugeTLB1G)) != 0;
    if ((h->flags & kFlagHugePages) && !seg_is_hugetlb)
        advise_huge_pages(base, map_len);

    set_err(err, kOk);
    return new Channel{base, map_len, h, seg_keep};
}

void close(Channel* ch) {
    if (ch == nullptr) return;
    seg_unmap(ch->base, ch->map_len);
    // Release the retained section handle. POSIX: ch->seg is kSegInvalid and
    // this is a no-op (the fd was dropped at create/open). Windows: this is the
    // last handle, so the named section is reclaimed here — the Windows analog
    // of unlink, which cannot remove a name while a handle is open.
    seg_close(ch->seg);
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
