// SHUTTLE_CREATE_ALIGNED_SPANS / kFlagAlignedSpans (0x10, v1.4): every payload
// span starts on a system-page boundary, so a borrow can be handed to an API
// that demands page-aligned host memory (MTLBuffer newBufferWithBytesNoCopy,
// cudaHostRegister) with no copy.
//
// The flag changes FRAMING, which is the whole reason each case below exists —
// a framing change that is only half-implemented shows up as a misparse, a lost
// message, or a pointer that is aligned by luck rather than by construction.
//
//   a. PROPERTY. >= 10k operations across two ring geometries, one of them
//      deliberately wrap-heavy, asserting after EVERY operation: the borrowed
//      payload pointer is page-aligned, the reserved write span is
//      page-aligned, the cursors stay page multiples, and the bytes are
//      byte-exact and FIFO.
//   b. NEGATIVE. With the flag OFF the segment is the classic one, byte for
//      byte: v1 data_offset, payload at frame_start + 8, cursor advancing by
//      exactly 8 + len. Asserted against the raw bytes in the data region, not
//      just against the API — a check that only read back through the same
//      framing it wrote with would pass no matter what the layout became.
//   c. STRESS. The byte-exact FIFO stress pattern rerun under the flag: a
//      producer thread and a consumer thread over one MAP_SHARED aligned
//      segment, random sizes; plus a two-PROCESS roundtrip, which is what
//      proves the opener resolves the framing from the segment's flags rather
//      than from anything its caller told it.
//   d. OLD-BINARY REJECTION. The deliberate compatibility verdict: a binary
//      predating the flag applies the unaligned geometry rule and must REFUSE
//      an aligned segment (kErrCorrupt), never ignore the bit and misparse
//      every frame. Checked in both directions, against a real segment's bytes
//      and against crafted mismatches.
//   e. STATS COMBO. 0x18 (aligned + stats) is legal, and the byte counters
//      count PAYLOAD bytes — never the padded stride.
//   f. FRAGMENTATION. The documented worst case, as a number: 2*page - 9 bytes
//      of padding per message (8183 at page = 4096), attained at len % page ==
//      1 and never exceeded.
//   g. CAPACITY FLOOR. FR-4 measured in the channel's own geometry: an aligned
//      channel needs page + round_up(max_payload, page), and a capacity that
//      only satisfies the classic rule is CAPACITY_TOO_SMALL.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"
#include "shuttle/shuttle_c.h"
#include "shuttle/spsc.hpp"

namespace {

constexpr uint64_t kChildTimeoutNs = 120ull * 1000000000ull;
constexpr uint64_t kSeed = 0xA11C4ED5ull;  // seed for the size/byte streams

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

uint64_t page() { return static_cast<uint64_t>(shuttle::page_size()); }

bool is_page_aligned(const void* p) {
    return (reinterpret_cast<uintptr_t>(p) & (page() - 1)) == 0;
}

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Deterministic, position-dependent so a byte-exact check means something.
unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 1315423911ull) + i * 151ull +
                                      (i >> 8));
}

uint64_t msg_len(uint64_t seed, uint64_t i, uint64_t max_len) {
    return splitmix(seed ^ i) % (max_len + 1);
}

char* name_for(char* buf, size_t n, const char* tag) {
    std::snprintf(buf, n, "/sha%s.%d", tag,
                  static_cast<int>(getpid()) % 100000);
    return buf;
}

// --- (a) alignment property, over two ring geometries ----------------------

// One channel's worth of the property loop. Producer and consumer are the same
// thread here on purpose: this case is about GEOMETRY, and a single thread lets
// every cursor be inspected between operations, which a concurrent run cannot
// do. Returns the number of operations performed, or -1 on failure.
long property_loop(const char* name, uint64_t capacity, uint64_t max_payload,
                   uint64_t nmsgs, uint64_t seed) {
    shuttle_unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, capacity, max_payload, &err,
                                           shuttle::kFlagAlignedSpans);
    if (ch == nullptr) {
        fail("property: create", err);
        return -1;
    }
    long ops = -1;
    do {
        // Preconditions the whole promise rests on. A mapping base that was
        // not page-aligned would make every later assertion vacuous.
        if (!is_page_aligned(ch->base)) {
            fail("property: mapping base not page-aligned", 0);
            break;
        }
        if (ch->hdr->data_offset !=
            shuttle::round_up_page(shuttle::kDataOffsetV1, page())) {
            fail("property: data_offset",
                 static_cast<long>(ch->hdr->data_offset));
            break;
        }
        unsigned char* data = static_cast<unsigned char*>(
            shuttle::resolve(ch->base, ch->hdr->data_offset));
        if (!is_page_aligned(data)) {
            fail("property: data region not page-aligned", 0);
            break;
        }

        shuttle::Producer p(ch);
        shuttle::Consumer c(ch);
        std::vector<unsigned char> tmp(max_payload);
        uint64_t sent = 0, got = 0, wraps = 0, blocked = 0;
        uint64_t last_write = 0;
        long n_ops = 0;
        int rc = 0;
        bool bad = false;

        // Mixed schedule: try to keep a few messages in flight, so the ring
        // runs full (early wraps) and empty (A->B handoffs) many times over.
        while (got < nmsgs && !bad) {
            const bool want_write =
                sent < nmsgs &&
                (sent - got < 3 || (splitmix(sent * 7 + got) & 1));
            if (want_write) {
                const uint64_t len = msg_len(seed, sent, max_payload);
                for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(sent, j);
                // Half the messages take the zero-copy producer path, so the
                // reserved span's alignment is exercised too, not only the
                // consumer's borrow.
                if ((sent & 1) == 0) {
                    void* dst = nullptr;
                    rc = p.try_acquire_write(&dst, len);
                    if (rc == shuttle::kErrWouldBlock) {
                        ++blocked;
                    } else if (rc != shuttle::kOk) {
                        fail("property: try_acquire_write", rc);
                        bad = true;
                    } else {
                        if (!is_page_aligned(dst)) {
                            fail("property: reserved span not page-aligned",
                                 static_cast<long>(sent));
                            bad = true;
                            break;
                        }
                        if (len != 0) std::memcpy(dst, tmp.data(), len);
                        rc = p.commit_write(len);
                        if (rc != shuttle::kOk) {
                            fail("property: commit_write", rc);
                            bad = true;
                            break;
                        }
                        ++sent;
                        ++n_ops;
                    }
                } else {
                    rc = p.try_write(tmp.data(), len);
                    if (rc == shuttle::kErrWouldBlock) {
                        ++blocked;
                    } else if (rc != shuttle::kOk) {
                        fail("property: try_write", rc);
                        bad = true;
                    } else {
                        ++sent;
                        ++n_ops;
                    }
                }
            }
            if (bad) break;
            // Drain when the ring refused us, otherwise drain probabilistically
            // — both orders have to hold.
            const bool want_read =
                got < sent && (!want_write || blocked != 0 ||
                               (splitmix(got * 13 + sent) & 1));
            if (!want_read) continue;
            const unsigned char* q = nullptr;
            uint64_t len = 0;
            rc = c.try_read(&q, &len);
            if (rc == shuttle::kErrWouldBlock) continue;
            if (rc != shuttle::kOk) {
                fail("property: try_read", rc);
                break;
            }
            blocked = 0;
            // THE PROPERTY.
            if (!is_page_aligned(q)) {
                fail("property: borrowed payload not page-aligned",
                     static_cast<long>(got));
                break;
            }
            const uint64_t want = msg_len(seed, got, max_payload);
            if (len != want) {
                fail("property: FIFO length mismatch", static_cast<long>(got));
                break;
            }
            bool byte_bad = false;
            for (uint64_t j = 0; j < len && !byte_bad; ++j)
                byte_bad = q[j] != fill_byte(got, j);
            if (byte_bad) {
                fail("property: payload bytes", static_cast<long>(got));
                break;
            }
            // The borrowed span must be exactly one page-aligned frame in from
            // the read cursor, and releasing it must leave the cursor on a page
            // boundary — that is what keeps the NEXT borrow aligned.
            const uint64_t r_before =
                ch->hdr->read.load(std::memory_order_relaxed);
            if ((r_before & (page() - 1)) != 0) {
                fail("property: read cursor off-page",
                     static_cast<long>(r_before));
                break;
            }
            if (q != data + r_before + page()) {
                fail("property: payload not one page into the frame",
                     static_cast<long>(got));
                break;
            }
            c.release();
            const uint64_t r_after =
                ch->hdr->read.load(std::memory_order_relaxed);
            const uint64_t w_now =
                ch->hdr->write.load(std::memory_order_relaxed);
            if ((r_after & (page() - 1)) != 0 || (w_now & (page() - 1)) != 0) {
                fail("property: cursor left a page boundary",
                     static_cast<long>(r_after));
                break;
            }
            if (r_after !=
                r_before + page() + shuttle::round_up_page(len, page())) {
                fail("property: release advanced by the wrong stride",
                     static_cast<long>(r_after - r_before));
                break;
            }
            if (w_now < last_write) ++wraps;
            last_write = w_now;
            ++got;
            ++n_ops;
        }
        if (got != nmsgs) break;  // a failure above already printed
        if (wraps == 0) {
            fail("property: the ring never wrapped — this case is not testing "
                 "what it claims",
                 0);
            break;
        }
        std::printf("  aligned property: cap=%llu max=%llu msgs=%llu ops=%ld "
                    "wraps=%llu — every span page-aligned\n",
                    (unsigned long long)capacity,
                    (unsigned long long)max_payload, (unsigned long long)nmsgs,
                    n_ops, (unsigned long long)wraps);
        ops = n_ops;
    } while (false);
    shuttle::close(ch);
    shuttle::unlink(name);
    return ops;
}

int case_property() {
    char small[40], big[40];
    name_for(small, sizeof small, "ps");
    name_for(big, sizeof big, "pb");
    // Wrap-heavy: the ring holds only a handful of frames, so a wrap and an
    // A->B handoff happen every few messages.
    const long a =
        property_loop(small, 8 * page(), 2 * page() - 1, 6000, kSeed ^ 1);
    // Roomier ring, small messages: mostly linear, a different mix of the same
    // invariants.
    const long b = property_loop(big, 64 * page(), 900, 6000, kSeed ^ 2);
    if (a < 0 || b < 0) return 1;
    if (a + b < 10000) {
        return fail("property: fewer than 10k operations", a + b);
    }
    std::printf("aligned_spans: property ok (%ld operations)\n", a + b);
    return 0;
}

// --- (b) flag off: the classic segment, byte for byte ----------------------

int case_flag_off() {
    char aname[40], cname[40];
    name_for(aname, sizeof aname, "fa");
    name_for(cname, sizeof cname, "fc");
    shuttle_unlink(aname);
    shuttle_unlink(cname);
    const uint64_t cap = 64 * page();
    const uint64_t maxp = 4000;
    int err = 0, fails = 0;
    shuttle::Channel* classic = shuttle::create(cname, cap, maxp, &err, 0);
    shuttle::Channel* aligned =
        shuttle::create(aname, cap, maxp, &err, shuttle::kFlagAlignedSpans);
    if (classic == nullptr || aligned == nullptr) {
        if (classic != nullptr) shuttle::close(classic);
        if (aligned != nullptr) shuttle::close(aligned);
        return fail("flag-off: create", err);
    }

    // Geometry: the control is the pre-v1.4 segment exactly.
    if (classic->hdr->flags != 0)
        fails += fail("flag-off: flags not clear",
                      static_cast<long>(classic->hdr->flags));
    if (classic->hdr->data_offset != shuttle::kDataOffsetV1)
        fails += fail("flag-off: data_offset moved",
                      static_cast<long>(classic->hdr->data_offset));
    if (aligned->hdr->flags != shuttle::kFlagAlignedSpans)
        fails += fail("flag-off: aligned flags",
                      static_cast<long>(aligned->hdr->flags));
    if (aligned->hdr->data_offset !=
        shuttle::round_up_page(shuttle::kDataOffsetV1, page()))
        fails += fail("flag-off: aligned data_offset",
                      static_cast<long>(aligned->hdr->data_offset));

    const unsigned char kMsg[] = "the framing is not a matter of opinion";
    const uint64_t len = sizeof kMsg - 1;

    // Drive both channels through the identical API sequence, then look at the
    // RAW BYTES each laid down. Reading back only through the same framing that
    // wrote them would agree with any layout at all.
    struct Side {
        shuttle::Channel* ch;
        uint64_t hdr_span;
        const char* what;
    } sides[2] = {{classic, shuttle::kFrameHeader, "classic"},
                  {aligned, page(), "aligned"}};
    for (const Side& s : sides) {
        auto sfail = [&s](const char* msg, long code) {
            std::fprintf(stderr, "FAIL: flag-off[%s]: %s (code=%ld)\n", s.what,
                         msg, code);
            return 1;
        };
        unsigned char* data = static_cast<unsigned char*>(
            shuttle::resolve(s.ch->base, s.ch->hdr->data_offset));
        shuttle::Producer p(s.ch);
        shuttle::Consumer c(s.ch);
        if (p.try_write(kMsg, len) != shuttle::kOk) {
            fails += sfail("write", 0);
            continue;
        }
        // The 8-byte little-endian length header sits at the frame start in
        // BOTH framings; only the payload's distance from it differs.
        uint64_t stored = 0;
        for (unsigned i = 0; i < 8; ++i)
            stored |= static_cast<uint64_t>(data[i]) << (8 * i);
        if (stored != len)
            fails += sfail("stored length", static_cast<long>(stored));
        if (std::memcmp(data + s.hdr_span, kMsg, len) != 0)
            fails += sfail("payload not at the expected offset", 0);
        const uint64_t want_span =
            s.hdr_span + (s.hdr_span == shuttle::kFrameHeader
                              ? len
                              : shuttle::round_up_page(len, page()));
        if (s.ch->hdr->write.load(std::memory_order_relaxed) != want_span)
            fails +=
                sfail("write cursor", static_cast<long>(s.ch->hdr->write.load(
                                          std::memory_order_relaxed)));

        const unsigned char* q = nullptr;
        uint64_t got = 0;
        if (c.try_read(&q, &got) != shuttle::kOk || got != len ||
            std::memcmp(q, kMsg, len) != 0) {
            fails += sfail("read back", static_cast<long>(got));
            continue;
        }
        if (q != data + s.hdr_span) fails += sfail("borrow pointer", 0);
        // The control's payload lands 8 bytes in, which is NOT page-aligned —
        // stated as an assertion so the negative case cannot pass by accident
        // on a build where everything happens to be aligned.
        if (s.hdr_span == shuttle::kFrameHeader && is_page_aligned(q))
            fails += sfail("control borrow was page-aligned anyway", 0);
        if (s.hdr_span != shuttle::kFrameHeader && !is_page_aligned(q))
            fails += sfail("aligned borrow was not page-aligned", 0);
        c.release();
        if (s.ch->hdr->read.load(std::memory_order_relaxed) != want_span)
            fails += sfail("read cursor advance", 0);
    }

    shuttle::close(classic);
    shuttle::close(aligned);
    shuttle::unlink(cname);
    shuttle::unlink(aname);
    if (fails == 0)
        std::printf("aligned_spans: flag-off control is the classic layout "
                    "(data_offset %llu, payload at +8)\n",
                    (unsigned long long)shuttle::kDataOffsetV1);
    return fails == 0 ? 0 : 1;
}

// --- (c) byte-exact FIFO stress under the flag -----------------------------

constexpr uint64_t kStressCap = 2ull << 20;
constexpr uint64_t kStressMax = 24ull << 10;
constexpr uint64_t kStressMsgs = 6000;
constexpr uint64_t kProcMsgs = 1500;

int stress_producer(shuttle::Channel* ch, uint64_t nmsgs, uint64_t max_len) {
    shuttle::Producer p(ch);
    std::vector<unsigned char> tmp(max_len);
    for (uint64_t i = 0; i < nmsgs; ++i) {
        const uint64_t len = msg_len(kSeed, i, max_len);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        const int rc = p.write(tmp.data(), len);
        if (rc != shuttle::kOk) return fail("stress producer: write", rc);
    }
    return 0;
}

int stress_consumer(shuttle::Channel* ch, uint64_t nmsgs, uint64_t max_len) {
    shuttle::Consumer c(ch);
    uint64_t total = 0;
    for (uint64_t i = 0; i < nmsgs; ++i) {
        const unsigned char* q = nullptr;
        uint64_t len = 0;
        const int rc = c.read(&q, &len);
        if (rc != shuttle::kOk) return fail("stress consumer: read", rc);
        // The property, asserted on every single message of the stress too:
        // alignment must survive thousands of wraps, not just the first few.
        if (!is_page_aligned(q))
            return fail("stress consumer: payload not page-aligned",
                        static_cast<long>(i));
        if (len != msg_len(kSeed, i, max_len))
            return fail("stress consumer: FIFO length", static_cast<long>(i));
        for (uint64_t j = 0; j < len; ++j) {
            if (q[j] != fill_byte(i, j))
                return fail("stress consumer: payload byte",
                            static_cast<long>(i));
        }
        c.release();
        total += len;
    }
    std::printf("  aligned stress: %llu msgs, %llu payload bytes, byte-exact "
                "FIFO, every borrow page-aligned\n",
                (unsigned long long)nmsgs, (unsigned long long)total);
    return 0;
}

int case_stress_threads() {
    char name[40];
    name_for(name, sizeof name, "st");
    shuttle_unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, kStressCap, kStressMax, &err,
                                           shuttle::kFlagAlignedSpans);
    if (ch == nullptr) return fail("stress: create", err);
    int prc = -1;
    std::thread prod(
        [&] { prc = stress_producer(ch, kStressMsgs, kStressMax); });
    const int crc = stress_consumer(ch, kStressMsgs, kStressMax);
    prod.join();
    const uint64_t w = ch->hdr->write.load(std::memory_order_relaxed);
    const uint64_t r = ch->hdr->read.load(std::memory_order_relaxed);
    int fails = (prc != 0) + (crc != 0) + (w != r ? 1 : 0);
    if (w != r) fail("stress: channel not drained", static_cast<long>(w - r));
    shuttle::close(ch);
    shuttle::unlink(name);
    return fails == 0 ? 0 : 1;
}

// The cross-process leg: the child knows nothing but the NAME. If it parses the
// stream correctly, it resolved the aligned framing from the segment's own
// flags word — which is the only place that information exists.
int case_stress_process(const char* self) {
    char name[40];
    name_for(name, sizeof name, "sp");
    shuttle_unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, kStressCap, kStressMax, &err,
                                           shuttle::kFlagAlignedSpans);
    if (ch == nullptr) return fail("stress-proc: create", err);
    char count[24];
    std::snprintf(count, sizeof count, "%llu", (unsigned long long)kProcMsgs);
    // The producer's result lands in its own variable, joined before it is
    // read: two threads accumulating into one counter is a data race, and this
    // suite runs under TSan.
    int prc = -1;
    std::thread prod([&] { prc = stress_producer(ch, kProcMsgs, kStressMax); });
    int fails = 0;
    if (shuttle_test::run_child_sync(self, "consumer", name, count,
                                     kChildTimeoutNs) != 0) {
        fail("stress-proc: child consumer", 0);
        ++fails;
    }
    prod.join();
    fails += (prc != 0);
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("aligned_spans: two-process roundtrip ok (opener resolved "
                    "the framing from flags)\n");
    return fails == 0 ? 0 : 1;
}

int run_consumer_child(const char* name, uint64_t nmsgs) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return fail("child: open", err);
    if (!shuttle::has_aligned_spans(ch->hdr)) {
        shuttle::close(ch);
        return fail("child: opened segment does not report aligned spans", 0);
    }
    const int rc = stress_consumer(ch, nmsgs, kStressMax);
    shuttle::close(ch);
    return rc;
}

// --- (d) the old-binary rejection ------------------------------------------

// validate_header EXACTLY as it read before v1.4: the version alone picks the
// legal data_offset, and a frame costs 8 bytes of overhead. This is the whole
// of what a pre-v1.4 binary knows, restated here so the compatibility claim is
// tested rather than asserted in a comment.
int validate_header_pre_v14(const void* base, size_t map_len) {
    if (base == nullptr || map_len < shuttle::kDataOffsetV1)
        return shuttle::kErrCorrupt;
    const auto* h = static_cast<const shuttle::ChannelHeader*>(base);
    if (h->magic != shuttle::kMagic) return shuttle::kErrBadMagic;
    if (h->version != shuttle::kVersion && h->version != shuttle::kVersionStats)
        return shuttle::kErrBadVersion;
    const uint64_t want = h->version == shuttle::kVersionStats
                              ? shuttle::kDataOffsetV2
                              : shuttle::kDataOffsetV1;
    if (h->data_offset != want || map_len < want ||
        h->data_capacity > map_len - want ||
        h->data_capacity < shuttle::kFrameHeader ||
        h->max_payload > h->data_capacity - shuttle::kFrameHeader)
        return shuttle::kErrCorrupt;
    return shuttle::kOk;
}

// A standalone, exact-sized header buffer (the fuzz harness's trick): exact
// size puts an ASan redzone right after the last byte, so a validator that read
// past the geometry it was handed would fault here rather than pass quietly.
class Crafted {
 public:
    explicit Crafted(size_t n) : n_(n) {
        p_ = static_cast<unsigned char*>(
            ::operator new(n_, std::align_val_t(shuttle::kCacheLine)));
        std::memset(p_, 0, n_);
    }
    ~Crafted() { ::operator delete(p_, std::align_val_t(shuttle::kCacheLine)); }
    Crafted(const Crafted&) = delete;
    Crafted& operator=(const Crafted&) = delete;
    unsigned char* data() { return p_; }
    size_t size() const { return n_; }
    shuttle::ChannelHeader* hdr() {
        return reinterpret_cast<shuttle::ChannelHeader*>(p_);
    }

 private:
    unsigned char* p_;
    size_t n_;
};

int case_old_binary() {
    char name[40];
    name_for(name, sizeof name, "ob");
    shuttle_unlink(name);
    int err = 0, fails = 0;
    const uint64_t cap = 32 * page();
    shuttle::Channel* ch = shuttle::create(name, cap, 4 * page(), &err,
                                           shuttle::kFlagAlignedSpans);
    if (ch == nullptr) return fail("old-binary: create", err);

    // Direction 1, on a REAL aligned segment: this binary accepts it; a binary
    // that predates the flag refuses it as CORRUPT. Not BAD_VERSION — no layout
    // version was added, and the disagreement really is about the offset.
    if (shuttle::validate_header(ch->base, ch->map_len) != shuttle::kOk)
        fails += fail("old-binary: current build rejected its own segment", 0);
    const int old_verdict = validate_header_pre_v14(ch->base, ch->map_len);
    if (old_verdict != shuttle::kErrCorrupt)
        fails += fail("old-binary: pre-v1.4 rule did not report CORRUPT",
                      old_verdict);
    // And the same segment WITHOUT the flag would have been accepted by that
    // old rule — proof the rejection is caused by the aligned geometry and not
    // by something incidental about this segment.
    {
        shuttle::Channel* plain = nullptr;
        char pname[40];
        name_for(pname, sizeof pname, "op");
        shuttle_unlink(pname);
        plain = shuttle::create(pname, cap, 4 * page(), &err, 0);
        if (plain == nullptr) {
            fails += fail("old-binary: control create", err);
        } else {
            if (validate_header_pre_v14(plain->base, plain->map_len) !=
                shuttle::kOk)
                fails += fail("old-binary: pre-v1.4 rule rejected a classic "
                              "segment (the check is not measuring the flag)",
                              0);
            shuttle::close(plain);
            shuttle::unlink(pname);
        }
    }

    // Direction 2, crafted mismatches: the current validator must refuse a
    // header whose flags and geometry disagree, either way round. These are the
    // two forgeries that would let an aligned parser loose on a classic segment
    // or vice versa.
    const uint64_t aligned_off =
        shuttle::round_up_page(shuttle::kDataOffsetV1, page());
    {
        Crafted seg(static_cast<size_t>(aligned_off + 8 * page()));
        shuttle::ChannelHeader* h = seg.hdr();
        h->magic = shuttle::kMagic;
        h->version = shuttle::kVersion;
        h->flags = shuttle::kFlagAlignedSpans;
        h->data_offset = aligned_off;
        h->data_capacity = seg.size() - aligned_off;
        h->max_payload = h->data_capacity - 2 * page();
        if (shuttle::validate_header(seg.data(), seg.size()) != shuttle::kOk)
            fails += fail("old-binary: crafted aligned header rejected", 0);
        // (i) aligned flag, classic offset.
        h->data_offset = shuttle::kDataOffsetV1;
        if (shuttle::validate_header(seg.data(), seg.size()) !=
            shuttle::kErrCorrupt)
            fails +=
                fail("old-binary: aligned flag + classic offset accepted", 0);
        // (ii) classic flags, aligned offset — what an aligned segment looks
        // like to a reader that ignores the bit.
        h->data_offset = aligned_off;
        h->flags = 0;
        if (shuttle::validate_header(seg.data(), seg.size()) !=
            shuttle::kErrCorrupt)
            fails +=
                fail("old-binary: classic flags + aligned offset accepted", 0);
        // (iii) the aligned FR-4 rule is enforced too: a max_payload one page
        // too large for the stride is corrupt, though it satisfies 8 + max.
        h->flags = shuttle::kFlagAlignedSpans;
        h->max_payload = h->data_capacity - page() + 1;
        if (shuttle::validate_header(seg.data(), seg.size()) !=
            shuttle::kErrCorrupt)
            fails +=
                fail("old-binary: oversized aligned max_payload accepted", 0);
    }

    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("aligned_spans: an aligned segment is CORRUPT to a "
                    "pre-v1.4 opener (deliberate), and forged geometries are "
                    "refused both ways\n");
    return fails == 0 ? 0 : 1;
}

// --- (e) aligned + stats ---------------------------------------------------

int case_stats_combo() {
    char name[40];
    name_for(name, sizeof name, "sc");
    shuttle_unlink(name);
    int err = 0, fails = 0;
    const uint32_t flags = SHUTTLE_CREATE_ALIGNED_SPANS | SHUTTLE_CREATE_STATS;
    shuttle_channel* ch =
        shuttle_create_ex(name, 64 * page(), 4000, flags, &err);
    if (ch == nullptr) return fail("stats-combo: create_ex", err);

    // The combination is legal and the offsets compose: v2 header, rounded up.
    int perr = 0;
    shuttle::Channel* view = shuttle::open(name, &perr);
    if (view == nullptr) {
        fails += fail("stats-combo: open", perr);
    } else {
        if (view->hdr->version != shuttle::kVersionStats)
            fails += fail("stats-combo: version",
                          static_cast<long>(view->hdr->version));
        if (view->hdr->flags !=
            (shuttle::kFlagAlignedSpans | shuttle::kFlagStats))
            fails +=
                fail("stats-combo: flags", static_cast<long>(view->hdr->flags));
        if (view->hdr->data_offset !=
            shuttle::round_up_page(shuttle::kDataOffsetV2, page()))
            fails += fail("stats-combo: data_offset",
                          static_cast<long>(view->hdr->data_offset));
        shuttle::close(view);
    }

    // Byte counters count PAYLOAD bytes. The whole point of the assertion is
    // that the padded stride is much larger, so a counter that accidentally
    // counted the frame span could not pass.
    const uint64_t kN = 64;
    uint64_t payload_total = 0, stride_total = 0;
    std::vector<unsigned char> buf(4000);
    for (uint64_t i = 0; i < kN; ++i) {
        const uint64_t len = msg_len(kSeed ^ 9, i, 4000);
        for (uint64_t j = 0; j < len; ++j) buf[j] = fill_byte(i, j);
        if (shuttle_write(ch, buf.data(), static_cast<size_t>(len), 0) !=
            SHUTTLE_OK) {
            fails += fail("stats-combo: write", static_cast<long>(i));
            break;
        }
        payload_total += len;
        stride_total += page() + shuttle::round_up_page(len, page());
        const void* q = nullptr;
        size_t got = 0;
        if (shuttle_acquire_read(ch, &q, &got, 0) != SHUTTLE_OK || got != len) {
            fails += fail("stats-combo: acquire_read", static_cast<long>(i));
            break;
        }
        if (!is_page_aligned(q))
            fails += fail("stats-combo: borrow not page-aligned",
                          static_cast<long>(i));
        shuttle_release_read(ch);
    }
    shuttle_stats st{};
    if (shuttle_get_stats(ch, &st) != SHUTTLE_OK) {
        fails += fail("stats-combo: get_stats", 0);
    } else {
        if (st.msgs_written != kN || st.msgs_read != kN)
            fails += fail("stats-combo: message counts",
                          static_cast<long>(st.msgs_written));
        if (st.bytes_written != payload_total)
            fails += fail("stats-combo: bytes_written counted padding",
                          static_cast<long>(st.bytes_written - payload_total));
        if (st.bytes_read != payload_total)
            fails += fail("stats-combo: bytes_read counted padding",
                          static_cast<long>(st.bytes_read - payload_total));
        if (stride_total <= payload_total)
            fails += fail("stats-combo: the padded stride was not larger — the "
                          "assertion above proves nothing",
                          0);
    }
    shuttle_close(ch);
    shuttle_unlink(name);
    if (fails == 0)
        std::printf("aligned_spans: 0x18 (aligned|stats) counts %llu payload "
                    "bytes, not the %llu-byte padded stride\n",
                    (unsigned long long)payload_total,
                    (unsigned long long)stride_total);
    return fails == 0 ? 0 : 1;
}

// --- (f) internal fragmentation, as a number -------------------------------

// Padding a message of `len` pays under the aligned framing: the tail of the
// header page, plus the payload's rounding. The 8-byte length header itself is
// NOT padding — it is transport overhead in both framings, so it is excluded,
// which is why the documented worst case is 2*page - 8 - 1 and not 2*page - 1.
uint64_t padding_for(uint64_t len) {
    return shuttle::frame_span(len, page()) - shuttle::kFrameHeader - len;
}

int case_fragmentation() {
    int fails = 0;
    const uint64_t worst = 2 * page() - shuttle::kFrameHeader - 1;
    if (page() == 4096 && worst != 8183)
        fails += fail("fragmentation: the documented number is 8183 at "
                      "page=4096",
                      static_cast<long>(worst));
    // Attained exactly at len % page == 1: a whole empty page after the header,
    // and all but one byte of the payload's last page wasted.
    if (padding_for(1) != worst)
        fails += fail("fragmentation: len=1 is not the worst case",
                      static_cast<long>(padding_for(1)));
    if (padding_for(page() + 1) != worst)
        fails += fail("fragmentation: len=page+1 differs from len=1",
                      static_cast<long>(padding_for(page() + 1)));
    // ...and never exceeded, anywhere in the first few pages of lengths.
    for (uint64_t len = 0; len <= 4 * page(); ++len) {
        if (padding_for(len) > worst) {
            fails += fail("fragmentation: worst case exceeded",
                          static_cast<long>(len));
            break;
        }
    }
    // The best case is a page-multiple payload: only the header page's tail.
    if (padding_for(page()) != page() - shuttle::kFrameHeader)
        fails += fail("fragmentation: page-sized payload",
                      static_cast<long>(padding_for(page())));

    // The same number, measured on a real channel rather than derived: a 1-byte
    // message must consume exactly two pages of the ring.
    char name[40];
    name_for(name, sizeof name, "fr");
    shuttle_unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 32 * page(), 4 * page(), &err,
                                           shuttle::kFlagAlignedSpans);
    if (ch == nullptr) return fails + fail("fragmentation: create", err);
    shuttle::Producer p(ch);
    const unsigned char one = 0x5A;
    if (p.try_write(&one, 1) != shuttle::kOk) {
        fails += fail("fragmentation: write", 0);
    } else {
        const uint64_t span = ch->hdr->write.load(std::memory_order_relaxed);
        if (span != 2 * page())
            fails += fail("fragmentation: a 1-byte message did not occupy two "
                          "pages",
                          static_cast<long>(span));
        if (span - shuttle::kFrameHeader - 1 != worst)
            fails += fail("fragmentation: measured padding != documented worst "
                          "case",
                          static_cast<long>(span - shuttle::kFrameHeader - 1));
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("aligned_spans: worst-case internal fragmentation is "
                    "%llu bytes/message (2*page - 8 - 1), measured\n",
                    (unsigned long long)worst);
    return fails == 0 ? 0 : 1;
}

// --- (g) the capacity floor moves with the geometry ------------------------

int case_capacity_floor() {
    char name[40];
    name_for(name, sizeof name, "cf");
    shuttle_unlink(name);
    int fails = 0, err = 0;
    const size_t maxp = static_cast<size_t>(page() + 1);
    // An aligned frame for this payload costs page + 2*page = 3 pages.
    const size_t too_small = static_cast<size_t>(3 * page() - 1);
    const size_t just_right = static_cast<size_t>(3 * page());

    shuttle_channel* bad = shuttle_create_ex(
        name, too_small, maxp, SHUTTLE_CREATE_ALIGNED_SPANS, &err);
    if (bad != nullptr) {
        shuttle_close(bad);
        shuttle_unlink(name);
        fails +=
            fail("capacity: an unsatisfiable aligned channel was created", 0);
    } else if (err != SHUTTLE_ERR_CAPACITY_TOO_SMALL) {
        fails += fail("capacity: wrong error for the aligned floor", err);
    }
    // The SAME geometry is fine without the flag — the floor moved because the
    // framing did, not because the numbers are small.
    shuttle_channel* classic =
        shuttle_create_ex(name, too_small, maxp, 0, &err);
    if (classic == nullptr) {
        fails +=
            fail("capacity: classic channel of the same size refused", err);
    } else {
        shuttle_close(classic);
        shuttle_unlink(name);
    }
    shuttle_channel* ok = shuttle_create_ex(name, just_right, maxp,
                                            SHUTTLE_CREATE_ALIGNED_SPANS, &err);
    if (ok == nullptr) {
        fails += fail("capacity: the exact aligned floor was refused", err);
    } else {
        // ...and the largest legal message really does fit in it.
        std::vector<unsigned char> buf(maxp, 0x11);
        if (shuttle_write(ok, buf.data(), maxp, SHUTTLE_NONBLOCK) != SHUTTLE_OK)
            fails += fail("capacity: max_payload did not fit at the floor", 0);
        shuttle_close(ok);
        shuttle_unlink(name);
    }
    if (fails == 0)
        std::printf("aligned_spans: FR-4 floor is page + round_up(max, page) "
                    "under the flag\n");
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    std::printf("aligned_spans_test: page=%llu, aligned data_offset=%llu "
                "(classic %llu)\n",
                (unsigned long long)page(),
                (unsigned long long)shuttle::round_up_page(
                    shuttle::kDataOffsetV1, page()),
                (unsigned long long)shuttle::kDataOffsetV1);
    int fails = 0;
    fails += case_property();
    fails += case_flag_off();
    fails += case_stress_threads();
    fails += case_stress_process(self);
    fails += case_old_binary();
    fails += case_stats_combo();
    fails += case_fragmentation();
    fails += case_capacity_floor();
    if (fails == 0)
        std::printf("aligned_spans_test ok: page-aligned spans end to end "
                    "(platform=%s)\n",
                    shuttle::platform_name());
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 4 && std::strcmp(argv[1], "consumer") == 0)
        return run_consumer_child(
            argv[2], static_cast<uint64_t>(std::strtoull(argv[3], nullptr, 0)));
    std::fprintf(stderr, "usage: %s [consumer </name> <count>]\n", argv[0]);
    return 2;
}
