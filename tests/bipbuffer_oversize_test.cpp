// G2.3: a write logically larger than usable capacity (max_payload + 1,
// i.e. framed unit > capacity) must FAIL FAST: one immediate refusal, zero
// state mutation, no looping, no incorrect wrap — from empty, partially
// filled, and wrapped states. The channel layer (Phase 3+) turns this
// refusal into the distinct fail-fast error of SRS §2.2; create-time FR-4
// validation (G1.2) guarantees such a write can never be a legal blocker.
#include <cstdint>
#include <cstdio>
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
constexpr uint64_t kCap = 1024;
constexpr uint64_t kMaxPayload = kCap - kF;  // largest payload that can fit

struct Snapshot {
    uint64_t r, w, m, size;
    explicit Snapshot(const shuttle::BipBuffer& b)
        : r(b.read_cursor()),
          w(b.write_cursor()),
          m(b.watermark_cursor()),
          size(b.size()) {}
    bool operator==(const Snapshot& o) const {
        return r == o.r && w == o.w && m == o.m && size == o.size;
    }
};

// The oversize write must be refused and must not move any cursor.
void expect_refused_unchanged(shuttle::BipBuffer& b, uint64_t payload_len,
                              const char* what) {
    std::vector<unsigned char> big(payload_len, 0xEE);
    const Snapshot before(b);
    CHECK(!b.write_msg(big.data(), payload_len), "%s: oversize accepted",
          what);
    CHECK(Snapshot(b) == before, "%s: refusal mutated cursors", what);
    // Raw path too: the reservation itself must refuse n > capacity.
    unsigned char* p = nullptr;
    CHECK(!b.reserve(kF + payload_len, &p), "%s: raw reserve accepted", what);
    CHECK(Snapshot(b) == before, "%s: raw refusal mutated cursors", what);
}

}  // namespace

int main() {
    // From EMPTY: max_payload fits (boundary), max_payload + 1 is refused.
    {
        std::vector<unsigned char> mem(kCap);
        shuttle::BipBuffer b(mem.data(), kCap);
        std::vector<unsigned char> max_ok(kMaxPayload, 0x42);
        expect_refused_unchanged(b, kMaxPayload + 1, "empty");
        CHECK(b.write_msg(max_ok.data(), max_ok.size()),
              "empty: exact max_payload must fit");
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        CHECK(b.read_msg(&p, &len) && len == kMaxPayload,
              "empty: max readback");
        b.release_msg();
        CHECK(b.size() == 0, "empty: drain");
    }

    // From PARTIALLY FILLED (linear): refusal must not corrupt live data.
    {
        std::vector<unsigned char> mem(kCap);
        shuttle::BipBuffer b(mem.data(), kCap);
        std::vector<unsigned char> m0(300 - kF, 0x31);
        CHECK(b.write_msg(m0.data(), m0.size()), "linear: m0");
        expect_refused_unchanged(b, kMaxPayload + 1, "linear");
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        CHECK(b.read_msg(&p, &len) && len == m0.size() && p[0] == 0x31,
              "linear: m0 intact after refusal");
        b.release_msg();
    }

    // From WRAPPED state: same guarantees while write < read.
    {
        // Build a wrapped state: fill to 900, drain 600, wrap a 500-unit.
        std::vector<unsigned char> mem(kCap);
        shuttle::BipBuffer b(mem.data(), kCap);
        std::vector<unsigned char> a(600 - kF, 0x51), c(300 - kF, 0x52),
            d(500 - kF, 0x53);
        CHECK(b.write_msg(a.data(), a.size()), "wrapped: a");
        CHECK(b.write_msg(c.data(), c.size()), "wrapped: c");
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        CHECK(b.read_msg(&p, &len) && len == a.size(), "wrapped: drain a");
        b.release_msg();
        CHECK(b.write_msg(d.data(), d.size()), "wrapped: d (early wrap)");
        CHECK(b.write_cursor() < b.read_cursor(), "wrapped: not wrapped?");

        expect_refused_unchanged(b, kMaxPayload + 1, "wrapped");

        // Everything still drains byte-exact in FIFO order.
        CHECK(b.read_msg(&p, &len) && len == c.size() && p[0] == 0x52,
              "wrapped: c intact");
        b.release_msg();
        CHECK(b.read_msg(&p, &len) && len == d.size() && p[0] == 0x53,
              "wrapped: d intact");
        b.release_msg();
        CHECK(b.size() == 0, "wrapped: drain");
    }

    if (fails == 0)
        std::printf("bipbuffer_oversize_test ok: oversize refused fast,"
                    " state untouched, from empty/linear/wrapped\n");
    return fails == 0 ? 0 : 1;
}
