// G2.2: the three named edge cases, as deterministic scripts with
// hand-computed cursor expectations (G2.1's random run covers the bulk;
// these pin the exact boundaries):
//   1. a payload that EXACTLY fills the space after region A,
//   2. a payload that forces an early wrap to region B (verified by pointer
//      identity at offset 0, plus the strict-inequality refusals around it),
//   3. a max-size payload (whole buffer in one message), incl. the SRS
//      2x-capacity pipelining shape.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "shuttle/bipbuffer.hpp"

namespace {

int fails = 0;

#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            std::fprintf(stderr, "FAIL: " __VA_ARGS__); \
            std::fprintf(stderr, " [%s]\n", #cond);     \
            ++fails;                                    \
        }                                               \
    } while (0)

constexpr uint64_t kF = shuttle::kFrameHeader;

std::vector<unsigned char> pattern(uint64_t len, unsigned char tag) {
    std::vector<unsigned char> v(len);
    for (uint64_t i = 0; i < len; ++i)
        v[i] = static_cast<unsigned char>(tag + i * 13);
    return v;
}

// Read one message, check bytes match `want`, release. Returns payload ptr.
const unsigned char* read_expect(shuttle::BipBuffer& b,
                                 const std::vector<unsigned char>& want,
                                 const char* what) {
    const unsigned char* p = nullptr;
    uint64_t len = 0;
    CHECK(b.read_msg(&p, &len), "%s: read_msg empty", what);
    if (p == nullptr) return nullptr;
    CHECK(len == want.size(), "%s: len %llu != %zu", what,
          (unsigned long long)len, want.size());
    CHECK(len == 0 || std::memcmp(p, want.data(), want.size()) == 0,
          "%s: payload bytes differ", what);
    b.release_msg();
    return p;
}

// Edge 1: a message that exactly fills the space after A. write_ must land
// exactly on the physical end, and the buffer must then wrap correctly.
void edge_exact_fill() {
    constexpr uint64_t kCap = 1024;
    std::vector<unsigned char> mem(kCap);
    shuttle::BipBuffer b(mem.data(), kCap);

    auto m0 = pattern(192, 0x11);  // unit 200: write=200
    CHECK(b.write_msg(m0.data(), m0.size()), "exact: m0 write");
    read_expect(b, m0, "exact: m0");  // read=200, A empty at [200,200)

    auto m1 = pattern(kCap - 200 - kF, 0x22);  // unit exactly cap-200
    CHECK(b.write_msg(m1.data(), m1.size()), "exact: m1 write");
    CHECK(b.write_cursor() == kCap, "exact: write %llu != cap %llu",
          (unsigned long long)b.write_cursor(), (unsigned long long)kCap);

    // Zero bytes after A: a further write must early-wrap (fits: read=200
    // > unit 100), NOT fail and NOT touch [write, cap).
    auto m2 = pattern(100 - kF, 0x33);  // unit 100 < read=200
    CHECK(b.write_msg(m2.data(), m2.size()), "exact: m2 wrap write");
    CHECK(b.write_cursor() == 100 && b.watermark_cursor() == kCap,
          "exact: post-wrap cursors write=%llu watermark=%llu",
          (unsigned long long)b.write_cursor(),
          (unsigned long long)b.watermark_cursor());

    const unsigned char* p1 = read_expect(b, m1, "exact: m1");
    CHECK(p1 == mem.data() + 200 + kF, "exact: m1 not in place");
    const unsigned char* p2 = read_expect(b, m2, "exact: m2");
    CHECK(p2 == mem.data() + kF, "exact: m2 not at offset 0 after handoff");
    CHECK(b.size() == 0, "exact: not drained");
}

// Edge 2: forced early wrap to B, with the strict-inequality refusals that
// guard the wrapped-full vs linear-empty ambiguity.
void edge_early_wrap() {
    constexpr uint64_t kCap = 1024;
    std::vector<unsigned char> mem(kCap);
    shuttle::BipBuffer b(mem.data(), kCap);

    auto m0 = pattern(600 - kF, 0x44);  // unit 600: write=600
    auto m1 = pattern(200 - kF, 0x55);  // unit 200: write=800
    CHECK(b.write_msg(m0.data(), m0.size()), "wrap: m0 write");
    CHECK(b.write_msg(m1.data(), m1.size()), "wrap: m1 write");
    read_expect(b, m0, "wrap: m0");  // read=600; A=[600,800), 224 after A

    // Strict refusal #1: unit 600 does not fit after A (224) and equals
    // read exactly — wrapping would make write == read (alias). Must fail.
    {
        auto big = pattern(600 - kF, 0x66);
        CHECK(!b.write_msg(big.data(), big.size()),
              "wrap: unit==read must be refused");
    }
    // Forced early wrap: unit 500 (>224 after A, <600 read). B begins.
    auto m2 = pattern(500 - kF, 0x77);
    CHECK(b.write_msg(m2.data(), m2.size()), "wrap: m2 write");
    CHECK(b.write_cursor() == 500 && b.watermark_cursor() == 800 &&
              b.read_cursor() == 600,
          "wrap: cursors w=%llu m=%llu r=%llu",
          (unsigned long long)b.write_cursor(),
          (unsigned long long)b.watermark_cursor(),
          (unsigned long long)b.read_cursor());
    CHECK(b.size() == 200 + 500, "wrap: size %llu",
          (unsigned long long)b.size());

    // Strict refusal #2: wrapped, read-write == 600-500 == 100: unit 100
    // would advance write to read exactly. Must fail; unit 99 must fit.
    {
        auto exact = pattern(100 - kF, 0x88);
        CHECK(!b.write_msg(exact.data(), exact.size()),
              "wrap: wrapped write==read must be refused");
        auto fits = pattern(99 - kF, 0x99);
        CHECK(b.write_msg(fits.data(), fits.size()),
              "wrap: wrapped unit 99 must fit");
        CHECK(b.write_cursor() == 599, "wrap: write %llu != 599",
              (unsigned long long)b.write_cursor());
        // Drain in FIFO order: m1 (high region), then handoff, m2, fits.
        read_expect(b, m1, "wrap: m1");
        const unsigned char* p2 = read_expect(b, m2, "wrap: m2");
        CHECK(p2 == mem.data() + kF, "wrap: m2 not at offset 0");
        read_expect(b, fits, "wrap: fits");
    }
    CHECK(b.size() == 0, "wrap: not drained");
}

// Edge 3: max-size payload — one message occupying the entire buffer, and
// the SRS 2x shape (two max messages pipelined through a 2x buffer).
void edge_max_payload() {
    constexpr uint64_t kCap = 1024;
    {
        std::vector<unsigned char> mem(kCap);
        shuttle::BipBuffer b(mem.data(), kCap);
        auto m = pattern(kCap - kF, 0xAA);  // unit == cap
        CHECK(b.write_msg(m.data(), m.size()), "max: whole-buffer write");
        CHECK(b.write_cursor() == kCap, "max: write != cap");
        // Buffer is one full message; nothing else fits anywhere.
        unsigned char one = 0;
        CHECK(!b.write_msg(&one, 1), "max: anything more must be refused");
        const unsigned char* p = read_expect(b, m, "max: m");
        CHECK(p == mem.data() + kF, "max: not in place");
        CHECK(b.size() == 0, "max: not drained");
        // Drained at write==read==cap: the next message must wrap cleanly.
        auto m2 = pattern(64 - kF, 0xBB);
        CHECK(b.write_msg(m2.data(), m2.size()), "max: post-drain write");
        const unsigned char* q = read_expect(b, m2, "max: m2");
        CHECK(q == mem.data() + kF, "max: m2 not at offset 0");
    }
    {
        // 2x capacity: both max messages in flight at once (pipelining).
        constexpr uint64_t kMaxUnit = 512;
        std::vector<unsigned char> mem(2 * kMaxUnit);
        shuttle::BipBuffer b(mem.data(), 2 * kMaxUnit);
        auto a = pattern(kMaxUnit - kF, 0xCC);
        auto c = pattern(kMaxUnit - kF, 0xDD);
        CHECK(b.write_msg(a.data(), a.size()), "2x: first max write");
        CHECK(b.write_msg(c.data(), c.size()), "2x: second max write");
        CHECK(b.size() == 2 * kMaxUnit, "2x: both in flight");
        read_expect(b, a, "2x: a");
        read_expect(b, c, "2x: c");
        CHECK(b.size() == 0, "2x: not drained");
    }
}

}  // namespace

int main() {
    edge_exact_fill();
    edge_early_wrap();
    edge_max_payload();
    if (fails == 0)
        std::printf("bipbuffer_edge_test ok: exact-fill, early-wrap (with"
                    " strict refusals), max-size all pinned\n");
    return fails == 0 ? 0 : 1;
}
