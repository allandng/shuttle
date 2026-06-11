// G3.3: targeted A->B handoff stress. The channel is deliberately tiny
// (16 KiB) and payloads biased large (2-6 KiB), so the early-wrap commit
// (P2: watermark then write-release) and the consumer handoff (C2: the
// read=0 store) fire every few messages, tens of thousands of times —
// hammering exactly the ordering hotspot the plan singles out.
//
// Falsifiability: the producer counts wrap commits (its own write cursor
// moving backward) and the run FAILS if wraps were rare. Byte-exact FIFO
// throughout. Runs as two processes (driver/producer/consumer roles) and
// as the A2 dual-thread TSan configuration (`threads` mode).
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kCapacity = 16ull << 10;   // 16 KiB: wrap every ~3 msgs
constexpr uint64_t kMaxPayload = 8ull << 10;
constexpr uint64_t kSeed = 0x33CC0003;
constexpr uint64_t kTwoProcMsgs = 200000;
constexpr uint64_t kThreadsMsgs = 50000;
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
// 2-6 KiB, biased large relative to the 16 KiB capacity.
uint64_t msg_len(uint64_t i) { return 2048 + splitmix(kSeed ^ i) % 4097; }
unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 40503ull) + i * 151ull + (i >> 8));
}

int producer_loop(shuttle::Channel* ch, uint64_t nmsgs, uint64_t min_wraps) {
    shuttle::Producer p(ch);
    std::vector<unsigned char> tmp(kMaxPayload);
    uint64_t wraps = 0;
    uint64_t prev_w = 0;
    for (uint64_t i = 0; i < nmsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        const int rc = p.write(tmp.data(), len);
        if (rc != shuttle::kOk) {
            std::fprintf(stderr, "producer: msg %llu rc=%d\n",
                         (unsigned long long)i, rc);
            return 1;
        }
        // producer is the sole writer of `write`; a backward move == a wrap
        const uint64_t w = ch->hdr->write.load(std::memory_order_relaxed);
        if (w < prev_w) ++wraps;
        prev_w = w;
    }
    if (wraps < min_wraps) {
        std::fprintf(stderr,
                     "producer: only %llu wraps (< %llu) — handoff path not"
                     " hammered\n",
                     (unsigned long long)wraps, (unsigned long long)min_wraps);
        return 1;
    }
    std::printf("producer: %llu wraps over %llu msgs\n",
                (unsigned long long)wraps, (unsigned long long)nmsgs);
    return 0;
}

int consumer_loop(shuttle::Channel* ch, uint64_t nmsgs) {
    shuttle::Consumer c(ch);
    for (uint64_t i = 0; i < nmsgs; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const int rc = c.read(&p, &len);
        if (rc != shuttle::kOk) {
            std::fprintf(stderr, "consumer: msg %llu rc=%d\n",
                         (unsigned long long)i, rc);
            return 1;
        }
        if (len != msg_len(i)) {
            std::fprintf(stderr, "consumer: msg %llu len %llu != %llu\n",
                         (unsigned long long)i, (unsigned long long)len,
                         (unsigned long long)msg_len(i));
            return 1;
        }
        uint64_t bad = 0;
        for (uint64_t j = 0; j < len; ++j)
            if (p[j] != fill_byte(i, j)) ++bad;
        if (bad != 0) {
            std::fprintf(stderr, "consumer: msg %llu corrupt (%llu bytes)\n",
                         (unsigned long long)i, (unsigned long long)bad);
            return 1;
        }
        c.release();
    }
    return 0;
}

bool drained(const shuttle::ChannelHeader* h) {
    const uint64_t w = h->write.load(std::memory_order_relaxed);
    const uint64_t r = h->read.load(std::memory_order_relaxed);
    const uint64_t m = h->watermark.load(std::memory_order_relaxed);
    return (w >= r ? w - r : (m - r) + w) == 0;
}

int run_driver(const char* self) {
    char name[32];
    std::snprintf(name, sizeof name, "/shwrap.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, kCapacity, kMaxPayload, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }
    int fails = shuttle_test::run_two_children_sync(
        self, "producer", "consumer", name, kChildTimeoutNs);
    if (!drained(ch->hdr)) {
        std::fprintf(stderr, "driver: channel not drained\n");
        ++fails;
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("spsc_wrap ok: two-process wrap-heavy handoff stress\n");
    return fails == 0 ? 0 : 1;
}

int run_threads() {
    char name[32];
    std::snprintf(name, sizeof name, "/shwrapt.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, kCapacity, kMaxPayload, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "threads: create err=%d\n", err);
        return 1;
    }
    int prc = -1;
    std::thread prod(
        [&] { prc = producer_loop(ch, kThreadsMsgs, kThreadsMsgs / 8); });
    const int crc = consumer_loop(ch, kThreadsMsgs);
    prod.join();
    int fails = (prc != 0) + (crc != 0);
    if (!drained(ch->hdr)) {
        std::fprintf(stderr, "threads: channel not drained\n");
        ++fails;
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("spsc_wrap ok: dual-thread wrap-heavy config (TSan"
                    " target per A2)\n");
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 2 && std::strcmp(argv[1], "threads") == 0) return run_threads();
    if (argc == 3) {
        int err = 0;
        shuttle::Channel* ch = shuttle::open(argv[2], &err);
        if (ch == nullptr) {
            std::fprintf(stderr, "%s: open err=%d\n", argv[1], err);
            return 1;
        }
        int rc = 2;
        if (std::strcmp(argv[1], "producer") == 0)
            rc = producer_loop(ch, kTwoProcMsgs, kTwoProcMsgs / 8);
        else if (std::strcmp(argv[1], "consumer") == 0)
            rc = consumer_loop(ch, kTwoProcMsgs);
        shuttle::close(ch);
        return rc;
    }
    std::fprintf(stderr,
                 "usage: %s [threads | producer </name> | consumer </name>]\n",
                 argv[0]);
    return 2;
}
