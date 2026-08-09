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

// Everything create() decided before the segment object existed, plus the one
// thing the two namespaces disagree about (`file`: is `id` an shm name or an
// absolute path, i.e. which unlink undoes it). Passed to publish_segment below
// so that create() and create_file() share ONE copy of the publication
// protocol — the ordering of the cold identity block, the park-primitive init,
// and the release-store is the part no second implementation may drift from.
struct SegPlan {
    const char* id;  // shm name, or absolute path when `file`
    bool file;
    uint32_t version;
    uint32_t flags;
    uint64_t data_offset;
    uint64_t capacity;
    uint64_t max_payload;
    bool advise_thp;    // opt-in THP advice on the fresh mapping
    int map_fail_code;  // what a failed seg_map means for this backing
};

// Destroy a partially-created segment through the namespace it lives in.
void destroy_segment(const SegPlan& p) noexcept {
    if (p.file) {
        (void)seg_unlink_file(p.id);
    } else {
        (void)seg_unlink(p.id);
    }
}

// POSIX errno -> Err for the path-typed entry points. The shm mapping cannot be
// reused verbatim: a path has a parent directory (ENOENT/ENOTDIR = "that
// directory is not there", which is a NotFound, not an opaque syscall failure)
// and a filesystem-imposed length limit, which is the same class of problem
// kErrNameTooLong already names for shm. No existing symbol's mapping changes.
int file_err(int e) noexcept {
    switch (e) {
        case EEXIST:
            return kErrExists;
        case ENOENT:
        case ENOTDIR:
            return kErrNotFound;
        case ENAMETOOLONG:
            return kErrNameTooLong;
        default:
            return kErrSys;
    }
}

// The half of create() that does not care HOW the segment object was obtained:
// map it, advise THP if asked, write the cold identity block, initialize the
// park primitives, and publish readiness with the release store. `seg` is a
// live handle to an object already sized to `map_len`. On any failure the
// object is unmapped, closed, and removed through its own namespace, and
// nothing is left behind.
Channel* publish_segment(SegHandle seg, size_t map_len, const SegPlan& p,
                         int* err) {
    void* base = seg_map(seg, map_len);
    if (base == nullptr) {
        seg_close(seg);
        destroy_segment(p);
        set_err(err, p.map_fail_code);
        return nullptr;
    }
    // POSIX drops the handle now (kSegInvalid); Windows RETAINS it in the
    // Channel — a named section vanishes with its last handle (seam decides).
    const SegHandle seg_keep = seg_keep_after_map(seg);

    // Opt-in THP: advise the fresh mapping before it is touched. Advisory and
    // masked to known bits — an unknown flag must never be persisted (openers
    // trust that flags carries only bits they may act on). The caller decides
    // whether the advice is meaningful for this backing at all (it is not on a
    // hugetlbfs file, whose pages are huge by construction).
    if (p.advise_thp) advise_huge_pages(base, map_len);

    // Creation zero-fills, so init_state is already 0 (uninitialized) and
    // cursors/heartbeats are already 0 — and so are the v2 stats lines, which
    // therefore need no explicit initialization. Set the rest explicitly.
    // flags is part of the cold identity block: written once here, before the
    // init_state release-store publish, and immutable after (single-init).
    auto* h = static_cast<ChannelHeader*>(base);
    h->magic = kMagic;
    h->version = p.version;
    h->flags = p.flags;
    h->data_offset = p.data_offset;
    h->data_capacity = p.capacity;
    h->max_payload = p.max_payload;
    if (mutex_init_pshared(&h->park.lock) != 0 ||
        cond_init_pshared_monotonic(&h->park.not_empty) != 0 ||
        cond_init_pshared_monotonic(&h->park.not_full) != 0) {
        seg_unmap(base, map_len);
        seg_close(
            seg_keep);  // release the retained handle (Windows); no-op POSIX
        destroy_segment(p);
        set_err(err, kErrSys);
        return nullptr;
    }
    // Publish: openers must not trust any field before this store (App. B #5).
    h->init_state.store(kInitReady, std::memory_order_release);

    set_err(err, kOk);
    return new Channel{base, map_len, h, seg_keep};
}

// The counterpart for the opening side: everything after the segment handle
// exists — size it, map it, wait for the creator's publication, validate, and
// re-advise THP. Shared by open() and open_file(), which differ ONLY in how
// they got `seg`. Consumes the handle: it is closed on every failure path.
Channel* attach_segment(SegHandle seg, int* err) {
    // An unreadable size (-1) and a too-small object are the same verdict:
    // whatever is behind this handle, it is not a channel. The bar is the
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
    // died mid-init cannot hang us (timeout = distinct error). This is also the
    // guard that keeps a STALE FILE (a file-backed segment left by a previous
    // boot, or any other file entirely) from hanging an opener forever: garbage
    // that never says kInitReady costs 5 s and then kErrInitTimeout.
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
    // (kFlagFileBacked needs nothing here either, and for the same shape of
    // reason: the opener named the file to get here, and a MAP_SHARED mapping
    // of it is all the backing requires.)
    const bool seg_is_hugetlb =
        (h->flags & (kFlagHugeTLB2M | kFlagHugeTLB1G)) != 0;
    if ((h->flags & kFlagHugePages) && !seg_is_hugetlb)
        advise_huge_pages(base, map_len);

    set_err(err, kOk);
    return new Channel{base, map_len, h, seg_keep};
}

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
    // Known-bits mask: every bit whose behavior has shipped. An unknown bit is
    // masked off and therefore never persisted, so no segment in the wild
    // carries a promise this binary did not keep.
    const uint32_t flags =
        create_flags & (kFlagHugePages | kFlagStats | kFlagHugeTLB2M |
                        kFlagHugeTLB1G | kFlagAlignedSpans);
    // Frame geometry, decided here and recorded in flags for the opener to
    // rediscover (bipbuffer.hpp). It changes what "a whole frame" costs, so it
    // has to be known before the FR-4 capacity rule below is applied.
    const uint64_t page = static_cast<uint64_t>(page_size());
    const uint64_t align = (flags & kFlagAlignedSpans) != 0 ? page : 0;

    // FR-4: a write that can never be satisfied must be impossible by
    // construction, or blocking backpressure would park the producer forever.
    // Written as subtraction against a checked floor, never as the sum
    // `max_payload_bytes + kFrameHeader`: that addition wraps for a
    // max_payload near 2^64 and would let this guard pass on a geometry
    // open()/validate_header now correctly reject (see validate_header's
    // overflow note). frame_fits() is that subtraction form for BOTH framings —
    // on an aligned channel the largest message costs page + round_up(max,
    // page) bytes, not 8 + max, and a capacity that cannot hold one is just as
    // unsatisfiable. Rejecting it here keeps create() from minting a segment no
    // opener will accept.
    if (!frame_fits(max_payload_bytes, capacity_bytes, align)) {
        set_err(err, kErrCapacityTooSmall);
        return nullptr;
    }
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
    const uint32_t version = with_stats ? kVersionStats : kVersion;
    // The version picks the header size; kFlagAlignedSpans then rounds it up to
    // a page so the data region — and so every frame start inside it — is page
    // aligned. Computed by the shared rule both create() and validate_header()
    // use, which is why a segment this writes always satisfies the check the
    // opener applies. It is ALSO what makes an aligned segment unopenable by a
    // binary that predates the flag: 4096 is not the 1280/1536 its rule
    // demands, so it stops at kErrCorrupt instead of misparsing every frame
    // (deliberate; see header.hpp's kFlagAlignedSpans and docs/API.md).
    const uint64_t data_offset = data_offset_for(version, flags, page);

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
    // THP advice is skipped when the segment is already on explicit huge pages:
    // advice about promoting normal pages is meaningless for a mapping whose
    // pages are huge by construction (the kernel would just return EINVAL).
    // Both bits are still persisted: the hugetlb bit is informational — an
    // opener needs no action from it, because seg_open finds the hugetlbfs file
    // and the file's inode already dictates the page size.
    //
    // hugetlbfs reserves its pages at MMAP time, not at create/ftruncate time,
    // so a box with no free huge pages fails inside publish_segment's map step
    // (ENOMEM) — the same "cannot deliver" verdict as above, which is why the
    // plan carries its own map_fail_code.
    const SegPlan plan{name,
                       false,
                       version,
                       flags,
                       data_offset,
                       capacity_bytes,
                       max_payload_bytes,
                       (flags & kFlagHugePages) != 0 && !explicit_huge,
                       explicit_huge ? kErrNoHugePages : kErrSys};
    return publish_segment(seg, map_len, plan, err);
}

Channel* create_file(const char* path, size_t capacity_bytes,
                     size_t max_payload_bytes, int* err,
                     uint32_t create_flags) {
    // The path IS the identifier (see the declaration). Absolute only, and no
    // length rule of our own — an over-long path is the filesystem's verdict,
    // reported as kErrNameTooLong by file_err. A NULL or empty path fails the
    // leading-'/' test, so both land here.
    if (!seg_path_ok(path) || capacity_bytes == 0 || max_payload_bytes == 0) {
        set_err(err, kErrInvalidArgs);
        return nullptr;
    }
    // A hugetlbfs backing and a caller-chosen path name two different segments:
    // honoring one means ignoring the other, and neither may be dropped
    // silently. Checked against the RAW create_flags, before masking, so the
    // request is refused rather than quietly turned into a plain file.
    if ((create_flags & (kFlagHugeTLB2M | kFlagHugeTLB1G)) != 0) {
        set_err(err, kErrInvalidArgs);
        return nullptr;
    }
    // Known-bits mask for THIS entry point: the layout/framing/advice bits,
    // plus kFlagFileBacked, which this path SETS rather than accepts (passing
    // it is legal and redundant; not passing it changes nothing). The two
    // hugetlb bits are the rest of the 0x3F known set and were rejected above,
    // so they can never reach the segment from here.
    const uint32_t flags =
        (create_flags & (kFlagHugePages | kFlagStats | kFlagAlignedSpans)) |
        kFlagFileBacked;
    const uint64_t page = static_cast<uint64_t>(page_size());
    const uint64_t align = (flags & kFlagAlignedSpans) != 0 ? page : 0;
    // FR-4, unchanged and for the same reason as in create(): a write that can
    // never be satisfied must be impossible by construction. A file-backed
    // channel is not exempt — capacity beyond RAM is still a finite capacity.
    if (!frame_fits(max_payload_bytes, capacity_bytes, align)) {
        set_err(err, kErrCapacityTooSmall);
        return nullptr;
    }
    const uint32_t version =
        (flags & kFlagStats) != 0 ? kVersionStats : kVersion;
    const uint64_t data_offset = data_offset_for(version, flags, page);
    // No rounding: a file has no granularity of its own (seg_map_len is the
    // identity for kFile). The one-shot ftruncate below therefore sizes the
    // object to exactly what the geometry needs.
    const size_t map_len = seg_map_len(
        static_cast<size_t>(data_offset) + capacity_bytes, SegBacking::kFile);

    int seg_err = 0;
    const SegHandle seg = seg_create_file(path, map_len, seg_err);
    if (seg == kSegInvalid) {
        // EEXIST is the documented recovery point for a STALE file left by a
        // previous boot: create refuses to reuse it, and the operator unlinks
        // and recreates (docs/API.md). It is never silently truncated — that
        // would hand a live peer's segment to a second creator.
        set_err(err, file_err(seg_err));
        return nullptr;
    }
    const SegPlan plan{path,
                       true,
                       version,
                       flags,
                       data_offset,
                       capacity_bytes,
                       max_payload_bytes,
                       (flags & kFlagHugePages) != 0,
                       kErrSys};
    return publish_segment(seg, map_len, plan, err);
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
    //
    // FLAGS-DEPENDENT GEOMETRY (v1.4). The version alone no longer names the
    // legal offset: kFlagAlignedSpans rounds it up to a page, and rounds the
    // frame layout with it. Reading flags here is safe and in keeping with the
    // rest of this function — it lives in the same cold identity block as the
    // fields above, written once before the init_state release-store, and it is
    // read only AFTER magic and version have been accepted. An UNKNOWN flag bit
    // is still ignored, exactly as the contract says; 0x10 is not unknown here.
    //
    // The deliberate consequence for a binary that predates the bit: it applies
    // the unaligned rule, sees data_offset 4096 where it wants 1280/1536, and
    // returns kErrCorrupt. That is the designed outcome — an ignorer would
    // misparse every frame — and kErrCorrupt is the honest verdict, because the
    // offset genuinely disagrees with the layout rule that binary knows. There
    // is no kVersion bump to report instead: the header shape did not change.
    const uint64_t page = static_cast<uint64_t>(page_size());
    const uint64_t want_offset = data_offset_for(h->version, h->flags, page);
    const uint64_t align = (h->flags & kFlagAlignedSpans) != 0 ? page : 0;
    if (h->data_offset != want_offset || map_len < want_offset ||
        h->data_capacity > map_len - want_offset ||
        !frame_fits(h->max_payload, h->data_capacity, align)) {
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
    return attach_segment(seg, err);
}

Channel* open_file(const char* path, int* err) {
    if (!seg_path_ok(path)) {
        set_err(err, kErrInvalidArgs);
        return nullptr;
    }
    int seg_err = 0;
    const SegHandle seg = seg_open_file(path, seg_err);
    if (seg == kSegInvalid) {
        set_err(err, file_err(seg_err));
        return nullptr;
    }
    // From here on a file-backed segment is validated by EXACTLY the checks an
    // shm one gets — same size floor, same init spin, same validate_header.
    // That is the whole answer to "what if the file is not a channel": a file
    // holding anything else fails one of them, and a file holding a stale
    // channel from a previous boot is refused or opened on its merits like any
    // other segment whose creator is gone (docs/API.md, "Stale files").
    return attach_segment(seg, err);
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

int unlink_file(const char* path) {
    if (!seg_path_ok(path)) return kErrInvalidArgs;
    const int seg_err = seg_unlink_file(path);
    return seg_err == 0 ? kOk : file_err(seg_err);
}

}  // namespace shuttle
