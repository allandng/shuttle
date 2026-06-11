// G3.1: two SEPARATE processes exchange >= 1 GiB of payload in random-sized
// messages over the lock-free busy-poll SPSC channel — byte-exact, FIFO.
//
// Per amendment A2 (TSan cannot see across processes), the same binary also
// has a `threads` mode: producer thread + consumer thread in ONE process,
// running the IDENTICAL Producer/Consumer code paths against a real
// MAP_SHARED segment. The TSan-clean claim of this gate attaches to that
// configuration; the two-process run proves pshared/offset/init correctness.
//
// Roles (posix_spawn per the fork-ban amendment):
//   (none)    driver: create channel, spawn producer+consumer, verify drain
//   threads   dual-thread single-process run (smaller N; same code paths)
//   producer </name>   write N seeded random messages, busy-poll variant
//   consumer </name>   read + verify byte-exact FIFO, total >= threshold
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kCapacity = 8ull << 20;     // 8 MiB data region
constexpr uint64_t kMaxPayload = 64ull << 10;  // 64 KiB
constexpr uint64_t kSeed = 0x5C5C0001;
constexpr uint64_t kTwoProcMsgs = 40000;   // ~1.25 GiB expected payload
constexpr uint64_t kThreadsMsgs = 8000;    // TSan config: same paths, smaller N
constexpr uint64_t kMinTotalTwoProc = 1ull << 30;  // the gate's >= 1 GiB
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t msg_len(uint64_t i) { return splitmix(kSeed ^ i) % (kMaxPayload + 1); }

unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 1315423911ull) + i * 151ull +
                                      (i >> 8));
}

// Returns 0 on success. Shared verbatim by the two-process roles and the
// dual-thread mode — the point of A2's "identical code paths".
int producer_loop(shuttle::Channel* ch, uint64_t nmsgs) {
    shuttle::Producer p(ch);
    std::vector<unsigned char> tmp(kMaxPayload);
    for (uint64_t i = 0; i < nmsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        const int rc = p.write(tmp.data(), len);
        if (rc != shuttle::kOk) {
            std::fprintf(stderr, "producer: write %llu failed rc=%d\n",
                         (unsigned long long)i, rc);
            return 1;
        }
    }
    return 0;
}

int consumer_loop(shuttle::Channel* ch, uint64_t nmsgs, uint64_t min_total) {
    shuttle::Consumer c(ch);
    uint64_t total = 0;
    for (uint64_t i = 0; i < nmsgs; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const int rc = c.read(&p, &len);
        if (rc != shuttle::kOk) {
            std::fprintf(stderr, "consumer: read %llu failed rc=%d\n",
                         (unsigned long long)i, rc);
            return 1;
        }
        if (len != msg_len(i)) {
            std::fprintf(stderr,
                         "consumer: msg %llu len %llu != %llu (FIFO broken?)\n",
                         (unsigned long long)i, (unsigned long long)len,
                         (unsigned long long)msg_len(i));
            return 1;
        }
        uint64_t bad = 0;
        for (uint64_t j = 0; j < len; ++j)
            if (p[j] != fill_byte(i, j)) ++bad;
        if (bad != 0) {
            std::fprintf(stderr, "consumer: msg %llu has %llu corrupt bytes\n",
                         (unsigned long long)i, (unsigned long long)bad);
            return 1;
        }
        c.release();
        total += len;
    }
    if (total < min_total) {
        std::fprintf(stderr, "consumer: total %llu < required %llu\n",
                     (unsigned long long)total, (unsigned long long)min_total);
        return 1;
    }
    std::printf("consumer: %llu msgs, %.2f GiB payload, byte-exact FIFO\n",
                (unsigned long long)nmsgs, (double)total / (1ull << 30));
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
    std::snprintf(name, sizeof name, "/shspsc.%d",
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
        std::fprintf(stderr, "driver: channel not drained at end\n");
        ++fails;
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("spsc_stress ok: two-process >=1GiB byte-exact FIFO\n");
    return fails == 0 ? 0 : 1;
}

int run_threads() {
    char name[32];
    std::snprintf(name, sizeof name, "/shspsct.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, kCapacity, kMaxPayload, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "threads: create err=%d\n", err);
        return 1;
    }
    int prc = -1;
    std::thread prod([&] { prc = producer_loop(ch, kThreadsMsgs); });
    const int crc = consumer_loop(ch, kThreadsMsgs, 0);
    prod.join();
    int fails = (prc != 0) + (crc != 0);
    if (!drained(ch->hdr)) {
        std::fprintf(stderr, "threads: channel not drained at end\n");
        ++fails;
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("spsc_stress ok: dual-thread MAP_SHARED config (TSan"
                    " target per A2)\n");
    return fails == 0 ? 0 : 1;
}

int run_role(const char* role, const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "%s: open err=%d\n", role, err);
        return 1;
    }
    int rc;
    if (std::strcmp(role, "producer") == 0) {
        rc = producer_loop(ch, kTwoProcMsgs);
    } else {
        rc = consumer_loop(ch, kTwoProcMsgs, kMinTotalTwoProc);
    }
    shuttle::close(ch);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 2 && std::strcmp(argv[1], "threads") == 0) return run_threads();
    if (argc == 3 && (std::strcmp(argv[1], "producer") == 0 ||
                      std::strcmp(argv[1], "consumer") == 0))
        return run_role(argv[1], argv[2]);
    std::fprintf(stderr,
                 "usage: %s [threads | producer </name> | consumer </name>]\n",
                 argv[0]);
    return 2;
}
