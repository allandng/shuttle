// G4.2: trickle stress — the producer sends one small message every random
// interval (0-80 us), 100k times, so the consumer parks and must be woken
// for nearly every message. "No lost wakeups" is checked sharply: a lost
// wake is masked by the 100 ms timedwait backstop and shows up as a
// ~100 ms read, so the consumer times every read and the run FAILS if more
// than a few exceed 50 ms (scheduling-noise allowance; a real lost-wakeup
// bug would trip thousands). "No extra wakeups" = exactly N messages,
// byte-exact FIFO, channel drained and WouldBlock afterward.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kCapacity = 256ull << 10;
constexpr uint64_t kMaxPayload = 1024;
constexpr uint64_t kMsgs = 100000;
constexpr uint64_t kSeed = 0x7E1C0004;
constexpr uint64_t kSlowReadNs = 50ull * 1000000;  // 50 ms: backstop fired
constexpr uint64_t kMaxSlowReads = 5;
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint64_t msg_len(uint64_t i) { return 16 + splitmix(kSeed ^ i) % 241; }
unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 48271ull) + i * 151ull);
}

int run_producer(shuttle::Channel* ch) {
    shuttle::Producer p(ch);
    std::vector<unsigned char> tmp(kMaxPayload);
    for (uint64_t i = 0; i < kMsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        if (p.write(tmp.data(), len) != shuttle::kOk) {
            std::fprintf(stderr, "producer: write %llu failed\n",
                         (unsigned long long)i);
            return 1;
        }
        usleep(static_cast<useconds_t>(splitmix(i * 31 + 7) % 81));
    }
    return 0;
}

int run_consumer(shuttle::Channel* ch) {
    shuttle::Consumer c(ch);
    uint64_t slow = 0, max_ns = 0;
    for (uint64_t i = 0; i < kMsgs; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const uint64_t t0 = shuttle::monotonic_ns();
        if (c.read(&p, &len) != shuttle::kOk) {
            std::fprintf(stderr, "consumer: read %llu failed\n",
                         (unsigned long long)i);
            return 1;
        }
        const uint64_t dt = shuttle::monotonic_ns() - t0;
        if (dt > kSlowReadNs) ++slow;
        if (dt > max_ns) max_ns = dt;
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
            std::fprintf(stderr, "consumer: msg %llu corrupt\n",
                         (unsigned long long)i);
            return 1;
        }
        c.release();
    }
    // No extras: the channel must be empty now.
    {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.try_read(&p, &len) != shuttle::kErrWouldBlock) {
            std::fprintf(stderr, "consumer: extra message after N!\n");
            return 1;
        }
    }
    if (slow > kMaxSlowReads) {
        std::fprintf(stderr,
                     "consumer: %llu reads exceeded 50 ms (max %.1f ms) —"
                     " wakeups being lost to the timedwait backstop\n",
                     (unsigned long long)slow, max_ns / 1e6);
        return 1;
    }
    std::printf("consumer: %llu trickled msgs, %llu slow reads, max wait"
                " %.2f ms\n",
                (unsigned long long)kMsgs, (unsigned long long)slow,
                max_ns / 1e6);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        char name[32];
        std::snprintf(name, sizeof name, "/shtrkl.%d",
                      static_cast<int>(getpid()) % 1000000);
        shuttle::unlink(name);
        int err = 0;
        shuttle::Channel* ch =
            shuttle::create(name, kCapacity, kMaxPayload, &err);
        if (ch == nullptr) {
            std::fprintf(stderr, "driver: create err=%d\n", err);
            return 1;
        }
        int fails = shuttle_test::run_two_children_sync(
            argv[0], "producer", "consumer", name, kChildTimeoutNs);
        shuttle::close(ch);
        shuttle::unlink(name);
        if (fails == 0)
            std::printf("trickle ok: 100k park/wake cycles, none lost\n");
        return fails == 0 ? 0 : 1;
    }
    if (argc == 3) {
        int err = 0;
        shuttle::Channel* ch = shuttle::open(argv[2], &err);
        if (ch == nullptr) {
            std::fprintf(stderr, "%s: open err=%d\n", argv[1], err);
            return 1;
        }
        int rc = 2;
        if (std::strcmp(argv[1], "producer") == 0) rc = run_producer(ch);
        if (std::strcmp(argv[1], "consumer") == 0) rc = run_consumer(ch);
        shuttle::close(ch);
        return rc;
    }
    std::fprintf(stderr, "usage: %s [producer|consumer </name>]\n", argv[0]);
    return 2;
}
