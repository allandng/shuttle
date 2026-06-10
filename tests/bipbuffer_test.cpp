// G2.1: byte-exact FIFO over >=100k random write/read pairs of random sizes
// against the single-threaded BipBuffer; invariants checked after EVERY
// operation. Two configurations: a roomy buffer, and a tight one sized so
// wraps happen every few messages (rehearsal for G3.3).
//
// Message payloads are regenerated from a deterministic function of the
// message index, so FIFO order + byte-exactness are verified without
// storing expected data.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#include "shuttle/bipbuffer.hpp"

namespace {

int fails = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::fprintf(stderr, "FAIL: " __VA_ARGS__);   \
            std::fprintf(stderr, " [%s]\n", #cond);       \
            ++fails;                                      \
            if (fails > 20) std::exit(1);                 \
        }                                                 \
    } while (0)

// splitmix64: deterministic, seedable, no global state.
struct Rng {
    uint64_t s;
    uint64_t next() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

unsigned char fill_byte(uint64_t msg_index, uint64_t i) {
    return static_cast<unsigned char>(
        (msg_index * 1315423911ull) + i * 151ull + (i >> 8));
}

void check_invariants(const shuttle::BipBuffer& b, uint64_t expect_bytes) {
    const uint64_t r = b.read_cursor();
    const uint64_t w = b.write_cursor();
    const uint64_t m = b.watermark_cursor();
    const uint64_t cap = b.capacity();
    CHECK(r <= cap, "read %llu > cap", (unsigned long long)r);
    CHECK(w <= cap, "write %llu > cap", (unsigned long long)w);
    CHECK(m <= cap, "watermark %llu > cap", (unsigned long long)m);
    if (w < r) {
        // wrapped: valid data [r, m) + [0, w); high region must be sane
        CHECK(r <= m, "wrapped but read %llu > watermark %llu",
              (unsigned long long)r, (unsigned long long)m);
    }
    CHECK(b.size() == expect_bytes,
          "size %llu != expected in-flight %llu",
          (unsigned long long)b.size(), (unsigned long long)expect_bytes);
}

void run_config(uint64_t cap, uint64_t max_len, uint64_t target_pairs,
                uint64_t seed, const char* label) {
    std::vector<unsigned char> mem(cap);
    shuttle::BipBuffer buf(mem.data(), cap);
    Rng rng{seed};

    std::deque<uint64_t> queued_len;  // model FIFO: len per message
    uint64_t next_write = 0;          // index of next message to write
    uint64_t next_read = 0;           // index of next message expected out
    uint64_t inflight_bytes = 0;
    uint64_t wraps_seen = 0;
    uint64_t prev_write_cursor = 0;
    std::vector<unsigned char> tmp(max_len);

    while (next_read < target_pairs) {
        const bool want_write =
            next_write < target_pairs &&
            (queued_len.empty() || rng.next() % 100 < 55);
        bool wrote = false;
        if (want_write) {
            const uint64_t len = rng.next() % (max_len + 1);
            for (uint64_t i = 0; i < len; ++i)
                tmp[i] = fill_byte(next_write, i);
            if (buf.write_msg(tmp.data(), len)) {
                if (buf.write_cursor() < prev_write_cursor) ++wraps_seen;
                prev_write_cursor = buf.write_cursor();
                queued_len.push_back(len);
                inflight_bytes += shuttle::kFrameHeader + len;
                ++next_write;
                wrote = true;
            }
            // write_msg returning false (full) is legal; fall through to read
        }
        if (!wrote) {
            const unsigned char* p = nullptr;
            uint64_t len = 0;
            const bool got = buf.read_msg(&p, &len);
            CHECK(got == !queued_len.empty(),
                  "read_msg=%d but model has %zu queued", (int)got,
                  queued_len.size());
            if (!got) continue;
            CHECK(len == queued_len.front(),
                  "msg %llu: len %llu != expected %llu (FIFO broken?)",
                  (unsigned long long)next_read, (unsigned long long)len,
                  (unsigned long long)queued_len.front());
            uint64_t bad = 0;
            for (uint64_t i = 0; i < len; ++i)
                if (p[i] != fill_byte(next_read, i)) ++bad;
            CHECK(bad == 0, "msg %llu: %llu corrupted bytes of %llu",
                  (unsigned long long)next_read, (unsigned long long)bad,
                  (unsigned long long)len);
            buf.release_msg();
            inflight_bytes -= shuttle::kFrameHeader + len;
            queued_len.pop_front();
            ++next_read;
        }
        check_invariants(buf, inflight_bytes);
        if (fails != 0) std::exit(1);
    }
    CHECK(inflight_bytes == 0 || !queued_len.empty(),
          "accounting drift at end");
    std::printf("  %s: %llu pairs, %llu wraps, byte-exact, invariants ok\n",
                label, (unsigned long long)target_pairs,
                (unsigned long long)wraps_seen);
}

}  // namespace

int main() {
    // Roomy: many messages in flight between wraps.
    run_config(/*cap=*/1u << 16, /*max_len=*/2000, /*pairs=*/100000,
               /*seed=*/0xC0FFEE, "roomy 64KB/0..2000B");
    // Tight: wrap every couple of messages, hammers early-wrap + handoff.
    run_config(/*cap=*/4096, /*max_len=*/1500, /*pairs=*/100000,
               /*seed=*/0xBEEF, "tight 4KB/0..1500B");
    if (fails == 0) std::printf("bipbuffer_test ok: G2.1 property run clean\n");
    return fails == 0 ? 0 : 1;
}
