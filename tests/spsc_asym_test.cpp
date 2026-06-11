// G3.2: asymmetric-speed stress, both directions, two processes.
//   A) producer-fast / consumer-slow  -> buffer runs FULL; the producer's
//      spin-based backpressure path is exercised constantly.
//   B) producer-slow / consumer-fast  -> buffer runs EMPTY; the consumer's
//      spin-based empty path is exercised constantly.
// Byte-exact FIFO both ways. The FAST side counts messages on which it
// observed kErrWouldBlock at least once and asserts the count is large —
// proof the pressure path genuinely engaged (the test can fail if spinning
// never happens), not merely that data arrived.
//
// Small capacity (256 KiB) so scenario A saturates within a few messages.
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
constexpr uint64_t kMaxPayload = 16ull << 10;
constexpr uint64_t kSeed = 0xA5A50002;
constexpr uint64_t kMsgs = 5000;
constexpr uint64_t kSlowDelayUs = 100;
constexpr uint64_t kMinBlockedMsgs = kMsgs / 10;  // far below the expected ~all
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint64_t msg_len(uint64_t i) { return splitmix(kSeed ^ i) % (kMaxPayload + 1); }
unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 2654435761ull) + i * 151ull +
                                      (i >> 8));
}

// delay_us throttles this side; if min_blocked > 0 this side must have hit
// would-block on at least that many messages.
int producer_loop(shuttle::Channel* ch, uint64_t delay_us,
                  uint64_t min_blocked) {
    shuttle::Producer p(ch);
    std::vector<unsigned char> tmp(kMaxPayload);
    uint64_t blocked_msgs = 0;
    for (uint64_t i = 0; i < kMsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        bool blocked = false;
        int rc;
        uint64_t spins = 0;
        while ((rc = p.try_write(tmp.data(), len)) ==
               shuttle::kErrWouldBlock) {
            blocked = true;
            shuttle::cpu_relax();
            if ((++spins & 0xFFF) == 0) shuttle::yield_thread();
        }
        if (rc != shuttle::kOk) {
            std::fprintf(stderr, "producer: msg %llu rc=%d\n",
                         (unsigned long long)i, rc);
            return 1;
        }
        if (blocked) ++blocked_msgs;
        if (delay_us != 0) usleep(static_cast<useconds_t>(delay_us));
    }
    if (blocked_msgs < min_blocked) {
        std::fprintf(stderr,
                     "producer: only %llu blocked msgs (< %llu) — "
                     "backpressure path not exercised\n",
                     (unsigned long long)blocked_msgs,
                     (unsigned long long)min_blocked);
        return 1;
    }
    return 0;
}

int consumer_loop(shuttle::Channel* ch, uint64_t delay_us,
                  uint64_t min_blocked) {
    shuttle::Consumer c(ch);
    uint64_t blocked_msgs = 0;
    for (uint64_t i = 0; i < kMsgs; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        bool blocked = false;
        int rc;
        uint64_t spins = 0;
        while ((rc = c.try_read(&p, &len)) == shuttle::kErrWouldBlock) {
            blocked = true;
            shuttle::cpu_relax();
            if ((++spins & 0xFFF) == 0) shuttle::yield_thread();
        }
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
        if (blocked) ++blocked_msgs;
        if (delay_us != 0) usleep(static_cast<useconds_t>(delay_us));
    }
    if (blocked_msgs < min_blocked) {
        std::fprintf(stderr,
                     "consumer: only %llu blocked msgs (< %llu) — "
                     "empty-spin path not exercised\n",
                     (unsigned long long)blocked_msgs,
                     (unsigned long long)min_blocked);
        return 1;
    }
    return 0;
}

int run_scenario(const char* self, const char* prole, const char* crole,
                 const char* tag) {
    char name[32];
    std::snprintf(name, sizeof name, "/shasym%s.%d", tag,
                  static_cast<int>(getpid()) % 100000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, kCapacity, kMaxPayload, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create %s err=%d\n", name, err);
        return 1;
    }
    int fails = shuttle_test::run_two_children_sync(self, prole, crole, name,
                                                    kChildTimeoutNs);
    shuttle::close(ch);
    shuttle::unlink(name);
    return fails;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        int fails = 0;
        // A: fast producer must spin on FULL against a throttled consumer.
        fails += run_scenario(argv[0], "pfast", "cslow", "a");
        // B: fast consumer must spin on EMPTY against a throttled producer.
        fails += run_scenario(argv[0], "pslow", "cfast", "b");
        if (fails == 0)
            std::printf("spsc_asym ok: full-pressure and empty-pressure both"
                        " byte-exact, spin paths verified engaged\n");
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
        if (std::strcmp(argv[1], "pfast") == 0)
            rc = producer_loop(ch, 0, kMinBlockedMsgs);
        else if (std::strcmp(argv[1], "pslow") == 0)
            rc = producer_loop(ch, kSlowDelayUs, 0);
        else if (std::strcmp(argv[1], "cslow") == 0)
            rc = consumer_loop(ch, kSlowDelayUs, 0);
        else if (std::strcmp(argv[1], "cfast") == 0)
            rc = consumer_loop(ch, 0, kMinBlockedMsgs);
        shuttle::close(ch);
        return rc;
    }
    std::fprintf(stderr, "usage: %s [pfast|pslow|cslow|cfast </name>]\n",
                 argv[0]);
    return 2;
}
