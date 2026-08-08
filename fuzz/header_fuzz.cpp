// libFuzzer harness for shuttle::validate_header() — the pure post-map half of
// open()'s validation (magic, version, version-selected geometry), extracted
// from src/shuttle.cpp precisely so it can be driven this way.
//
// THREAT MODEL. A segment is a file another process created. open() maps it
// and then indexes every later access off three numbers the segment itself
// supplies — data_offset, data_capacity, max_payload. NFR-S2 says those are
// never to be trusted. This harness is the adversary: it hands validate_header
// arbitrary bytes and asserts two things.
//
//   SAFETY: it never reads outside [base, base + map_len). The segment is an
//   EXACT-SIZED aligned allocation, so ASan's redzones sit immediately past
//   map_len and any over-read is a hard failure, not a silent one. map_len is
//   fuzz-chosen and is allowed to be shorter than any known header.
//
//   SOUNDNESS: whenever it answers kOk, the geometry it just blessed is
//   actually usable — data_offset + data_capacity fits inside the mapping and
//   max_payload + kFrameHeader fits inside data_capacity, both recomputed here
//   in OVERFLOW-SAFE form. A geometry check that wraps around 2^64 is exactly
//   the bug this assertion exists to catch, so it must not be written the same
//   way the code under test writes it.
//
// REACHING kOk. Blind bytes essentially never produce a valid header: the
// magic alone is eight exact bytes, and data_offset must hit kDataOffsetV1 or
// kDataOffsetV2 exactly. So the input is decoded through a mode byte, and two
// of the four modes START from a valid header and let the fuzzer perturb it —
// structure-aware fuzzing, which is what puts the interesting near-miss
// geometries (off-by-one capacities, saturated max_payload) within reach.
//
// POSITIVE CONTROLS. A validator that rejected everything would pass every
// safety assertion above while being completely broken, and the fuzzer would
// happily report full coverage of it. So a hand-built valid v1 header and a
// hand-built valid v2 header are asserted to return kOk before any fuzzing
// happens; if the accept path ever dies, the harness aborts on its first run.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

#include "shuttle/header.hpp"
#include "shuttle/shuttle.hpp"

namespace {

#define FCHECK(cond, ...)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "header_fuzz: " __VA_ARGS__);         \
            std::fprintf(stderr, "  [%s] at %s:%d\n", #cond, __FILE__, \
                         __LINE__);                                    \
            std::abort();                                              \
        }                                                              \
    } while (0)

// Cap on the simulated segment: big enough for either header plus a real data
// region, small enough that an input replays in microseconds.
constexpr size_t kMaxSeg = 64u * 1024u;

// An exact-sized, cache-line-aligned segment. Exact size is the whole point:
// it puts an ASan redzone immediately after byte map_len - 1. Aligned because
// ChannelHeader is alignas(kCacheLine) and a misaligned load of its members
// would be UBSan's finding, not the library's.
class Segment {
 public:
    explicit Segment(size_t n) : n_(n) {
        p_ = static_cast<unsigned char*>(::operator new(
            n_ == 0 ? 1 : n_, std::align_val_t(shuttle::kCacheLine)));
        std::memset(p_, 0, n_ == 0 ? 1 : n_);
    }
    ~Segment() { ::operator delete(p_, std::align_val_t(shuttle::kCacheLine)); }
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    unsigned char* data() { return p_; }
    size_t size() const { return n_; }
    // Only meaningful once size() >= kDataOffsetV1; callers check first.
    shuttle::ChannelHeader* hdr() {
        return reinterpret_cast<shuttle::ChannelHeader*>(p_);
    }

 private:
    unsigned char* p_ = nullptr;
    size_t n_ = 0;
};

class Tape {
 public:
    Tape(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t u8() { return i_ < n_ ? p_[i_++] : 0; }
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
    }
    uint32_t u32() {
        const uint32_t lo = u16();
        return lo | (static_cast<uint32_t>(u16()) << 16);
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | (static_cast<uint64_t>(u32()) << 32);
    }
    size_t left() const { return n_ - i_; }
    const uint8_t* rest() const { return p_ + i_; }

 private:
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
};

// Overflow-safe restatement of what a kOk verdict promises. Deliberately NOT
// written the way src/shuttle.cpp writes it — `a + b > c` in uint64 is the
// shape that can wrap, so this uses subtraction against a checked floor.
bool geometry_is_sound(const shuttle::ChannelHeader* h, size_t map_len) {
    const uint64_t want = h->version == shuttle::kVersionStats
                              ? shuttle::kDataOffsetV2
                              : shuttle::kDataOffsetV1;
    if (h->data_offset != want) return false;
    if (map_len < want) return false;
    // data_offset + data_capacity <= map_len, without forming the sum.
    if (h->data_capacity > map_len - want) return false;
    // max_payload + kFrameHeader <= data_capacity, without forming the sum.
    if (h->data_capacity < shuttle::kFrameHeader) return false;
    if (h->max_payload > h->data_capacity - shuttle::kFrameHeader) return false;
    return true;
}

// Build a header that must be accepted, sized to the segment it sits in.
void write_valid(Segment& seg, uint32_t version) {
    const uint64_t want = version == shuttle::kVersionStats
                              ? shuttle::kDataOffsetV2
                              : shuttle::kDataOffsetV1;
    FCHECK(seg.size() >= want + shuttle::kFrameHeader,
           "control segment too small for v%u\n", version);
    shuttle::ChannelHeader* h = seg.hdr();
    h->magic = shuttle::kMagic;
    h->version = version;
    h->flags = version == shuttle::kVersionStats ? shuttle::kFlagStats : 0u;
    h->data_offset = want;
    h->data_capacity = seg.size() - want;
    h->max_payload = h->data_capacity - shuttle::kFrameHeader;
}

// The reject-everything canary. Runs once, before any fuzz input is decoded.
bool positive_controls() {
    for (const uint32_t v : {shuttle::kVersion, shuttle::kVersionStats}) {
        Segment seg(kMaxSeg);
        write_valid(seg, v);
        const int rc = shuttle::validate_header(seg.data(), seg.size());
        FCHECK(rc == shuttle::kOk,
               "POSITIVE CONTROL FAILED: a hand-built valid v%u header was "
               "rejected with %d — the accept path is broken, so every "
               "'rejected' verdict below proves nothing\n",
               v, rc);
        FCHECK(geometry_is_sound(seg.hdr(), seg.size()),
               "control v%u header is not sound by the harness's own rule\n",
               v);
    }
    // A short mapping must be refused rather than read into.
    FCHECK(shuttle::validate_header(nullptr, kMaxSeg) == shuttle::kErrCorrupt,
           "null base was not kErrCorrupt\n");
    return true;
}

void check_verdict(Segment& seg, size_t map_len) {
    const int rc = shuttle::validate_header(seg.data(), map_len);

    // Pure function: same bytes, same answer. Cheap, and it catches a
    // validator that ever grows hidden state.
    FCHECK(rc == shuttle::validate_header(seg.data(), map_len),
           "validate_header is not deterministic (rc=%d)\n", rc);

    // Closed verdict set — open() forwards this value straight to the caller,
    // so a stray code would surface as an undocumented error at the ABI.
    FCHECK(rc == shuttle::kOk || rc == shuttle::kErrBadMagic ||
               rc == shuttle::kErrBadVersion || rc == shuttle::kErrCorrupt,
           "unexpected verdict %d\n", rc);

    if (map_len < shuttle::kDataOffsetV1) {
        FCHECK(rc == shuttle::kErrCorrupt,
               "a %zu-byte mapping (< the smallest header) returned %d\n",
               map_len, rc);
        return;
    }
    if (rc != shuttle::kOk) return;

    const shuttle::ChannelHeader* h = seg.hdr();
    FCHECK(h->magic == shuttle::kMagic, "accepted a bad magic\n");
    FCHECK(
        h->version == shuttle::kVersion || h->version == shuttle::kVersionStats,
        "accepted unknown version %u\n", h->version);
    FCHECK(geometry_is_sound(h, map_len),
           "ACCEPTED IMPOSSIBLE GEOMETRY: version=%u data_offset=%llu "
           "data_capacity=%llu max_payload=%llu map_len=%zu\n",
           h->version, (unsigned long long)h->data_offset,
           (unsigned long long)h->data_capacity,
           (unsigned long long)h->max_payload, map_len);

    // The promise made concrete: every byte of the data region the header
    // claims is really inside the mapping. Touching the two ends makes ASan,
    // not just arithmetic, the judge of that.
    const uint64_t off = h->data_offset;
    const uint64_t cap = h->data_capacity;
    if (cap != 0) {
        volatile const unsigned char* d = seg.data();
        (void)d[off];
        (void)d[off + cap - 1];
        // ...and a max_payload-sized frame fits in it.
        (void)d[off + h->max_payload + shuttle::kFrameHeader - 1];
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const bool controls_ok = positive_controls();
    (void)controls_ok;
    if (size < 2) return 0;

    Tape t(data, size);
    const uint8_t mode = t.u8();

    switch (mode & 3u) {
        case 0: {
            // Raw: the input IS the segment. The realistic case — a file some
            // other process wrote, or garbage under the right name.
            const size_t map_len = t.left() < kMaxSeg ? t.left() : kMaxSeg;
            Segment seg(map_len);
            if (map_len != 0) std::memcpy(seg.data(), t.rest(), map_len);
            check_verdict(seg, map_len);
            return 0;
        }
        case 1:
        case 2: {
            // Seeded: start from a header that IS valid, then let the fuzzer
            // poke individual bytes of the cold identity block. This is where
            // the near-miss geometries come from — the ones a validator gets
            // wrong are always one increment away from a legal segment.
            const uint32_t version =
                (mode & 3u) == 1 ? shuttle::kVersion : shuttle::kVersionStats;
            const uint64_t want = version == shuttle::kVersionStats
                                      ? shuttle::kDataOffsetV2
                                      : shuttle::kDataOffsetV1;
            // Segment size drawn from the tape, but never below what the
            // control header needs — the point of this mode is to start valid.
            const size_t room =
                kMaxSeg - static_cast<size_t>(want) - shuttle::kFrameHeader;
            const size_t extra = t.u16() % room;
            Segment seg(static_cast<size_t>(want) + shuttle::kFrameHeader +
                        extra);
            write_valid(seg, version);
            // Pokes land only in the identity block (magic..max_payload = the
            // first 40 bytes); anything past it is cursors and lock state,
            // which validate_header does not read.
            constexpr size_t kIdentity = 40;
            while (t.left() >= 2) {
                const size_t off = t.u8() % kIdentity;
                seg.data()[off] = t.u8();
            }
            check_verdict(seg, seg.size());
            return 0;
        }
        default: {
            // Field-directed: every geometry field comes straight off the
            // tape. The fastest route to a header that is well-formed enough
            // to be accepted but arithmetically absurd.
            const size_t map_len =
                shuttle::kDataOffsetV1 +
                (t.u16() % (kMaxSeg - shuttle::kDataOffsetV1));
            Segment seg(map_len);
            shuttle::ChannelHeader* h = seg.hdr();
            // Magic and version are drawn from a small biased set: spending
            // fuzz entropy rediscovering an 8-byte constant teaches nothing,
            // and the raw mode above already covers "wrong magic".
            const uint8_t sel = t.u8();
            h->magic = (sel & 1u) ? shuttle::kMagic : t.u64();
            switch ((sel >> 1) & 3u) {
                case 0:
                    h->version = shuttle::kVersion;
                    break;
                case 1:
                    h->version = shuttle::kVersionStats;
                    break;
                default:
                    h->version = t.u32();
                    break;
            }
            h->flags = t.u32();
            // data_offset likewise: mostly one of the two legal values, so the
            // fuzzer spends its budget on the capacity arithmetic instead.
            switch ((sel >> 3) & 3u) {
                case 0:
                    h->data_offset = shuttle::kDataOffsetV1;
                    break;
                case 1:
                    h->data_offset = shuttle::kDataOffsetV2;
                    break;
                default:
                    h->data_offset = t.u64();
                    break;
            }
            h->data_capacity = t.u64();
            h->max_payload = t.u64();
            check_verdict(seg, map_len);
            return 0;
        }
    }
}
