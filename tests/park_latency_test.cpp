// G4.3, two claims:
//   1. WAKE LATENCY: p99 commit -> consumer-holds-payload is microsecond-
//      scale, not millisecond-scale. The producer stamps CLOCK_MONOTONIC ns
//      into each payload right before write (cross-process timestamps per
//      the minor amendment); messages are paced so the consumer is parked
//      when each commit lands — measuring the genuine wake path. Asserts
//      p99 < 1 ms (sanitized builds; Phase 7 measures unsanitized numbers).
//   2. HOT PATH IS MUTEX-FREE when the peer is not parked: a dual-thread
//      run where neither side ever blocks (huge capacity; consumer spins on
//      try_read and never sets its waiting flag) must end with ZERO park-
//      mutex acquisitions on both handles (instance lock counters).
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kLatMsgs = 20000;
constexpr uint64_t kLatPayload = 64;
constexpr uint64_t kPaceUs = 200;  // consumer is parked when commit lands
constexpr uint64_t kP99BudgetNs = 1ull * 1000000;  // 1 ms
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

int run_lat_producer(shuttle::Channel* ch) {
    shuttle::Producer p(ch);
    unsigned char buf[kLatPayload] = {0};
    for (uint64_t i = 0; i < kLatMsgs; ++i) {
        const uint64_t ts = shuttle::monotonic_ns();
        std::memcpy(buf, &ts, sizeof ts);
        if (p.write(buf, sizeof buf) != shuttle::kOk) {
            std::fprintf(stderr, "lat-producer: write %llu failed\n",
                         (unsigned long long)i);
            return 1;
        }
        usleep(kPaceUs);
    }
    return 0;
}

int run_lat_consumer(shuttle::Channel* ch) {
    shuttle::Consumer c(ch);
    std::vector<uint64_t> lat;
    lat.reserve(kLatMsgs);
    for (uint64_t i = 0; i < kLatMsgs; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.read(&p, &len) != shuttle::kOk || len != kLatPayload) {
            std::fprintf(stderr, "lat-consumer: read %llu failed\n",
                         (unsigned long long)i);
            return 1;
        }
        uint64_t ts = 0;
        std::memcpy(&ts, p, sizeof ts);
        lat.push_back(shuttle::monotonic_ns() - ts);
        c.release();
    }
    std::sort(lat.begin(), lat.end());
    const uint64_t p50 = lat[lat.size() / 2];
    const uint64_t p99 = lat[lat.size() * 99 / 100];
    const uint64_t pmax = lat.back();
    std::printf("wake latency over %llu parked msgs: p50=%.1fus p99=%.1fus"
                " max=%.1fus\n",
                (unsigned long long)kLatMsgs, p50 / 1e3, p99 / 1e3,
                pmax / 1e3);
    if (p99 > kP99BudgetNs) {
        std::fprintf(stderr, "lat-consumer: p99 %.1f us exceeds 1 ms budget"
                     " — wake is not microsecond-scale\n", p99 / 1e3);
        return 1;
    }
    return 0;
}

int run_hotpath() {
    // Capacity >> total bytes: the producer never blocks. The consumer uses
    // try_read spin only, so it never parks and never sets its flag.
    constexpr uint64_t kMsgs = 10000;
    constexpr uint64_t kLen = 1024;
    char name[32];
    std::snprintf(name, sizeof name, "/shhotp.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 64ull << 20, kLen, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "hotpath: create err=%d\n", err);
        return 1;
    }
    shuttle::Producer prod(ch);
    shuttle::Consumer cons(ch);
    int prc = 0;
    std::thread t([&] {
        std::vector<unsigned char> buf(kLen, 0x5A);
        for (uint64_t i = 0; i < kMsgs; ++i) {
            if (prod.write(buf.data(), kLen) != shuttle::kOk) {
                prc = 1;
                return;
            }
        }
    });
    uint64_t got = 0, spins = 0;
    while (got < kMsgs) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const int rc = cons.try_read(&p, &len);
        if (rc == shuttle::kOk) {
            cons.release();
            ++got;
        } else if (rc == shuttle::kErrWouldBlock) {
            shuttle::cpu_relax();
            if ((++spins & 0xFFF) == 0) shuttle::yield_thread();
        } else {
            std::fprintf(stderr, "hotpath: try_read rc=%d\n", rc);
            break;
        }
    }
    t.join();
    int fails = prc + (got != kMsgs);
    if (prod.locks_taken() != 0 || cons.locks_taken() != 0) {
        std::fprintf(stderr,
                     "hotpath: mutex touched with peer never parked"
                     " (producer=%llu consumer=%llu locks)\n",
                     (unsigned long long)prod.locks_taken(),
                     (unsigned long long)cons.locks_taken());
        ++fails;
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("hotpath ok: %llu msgs, 0 park-mutex acquisitions on"
                    " either side\n",
                    (unsigned long long)kMsgs);
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "hotpath") == 0) return run_hotpath();
    if (argc == 1) {
        char name[32];
        std::snprintf(name, sizeof name, "/shlat.%d",
                      static_cast<int>(getpid()) % 1000000);
        shuttle::unlink(name);
        int err = 0;
        shuttle::Channel* ch = shuttle::create(name, 1u << 20, 4096, &err);
        if (ch == nullptr) {
            std::fprintf(stderr, "driver: create err=%d\n", err);
            return 1;
        }
        int fails = shuttle_test::run_two_children_sync(
            argv[0], "lat-producer", "lat-consumer", name, kChildTimeoutNs);
        shuttle::close(ch);
        shuttle::unlink(name);
        if (fails == 0) std::printf("park_latency ok\n");
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
        if (std::strcmp(argv[1], "lat-producer") == 0)
            rc = run_lat_producer(ch);
        if (std::strcmp(argv[1], "lat-consumer") == 0)
            rc = run_lat_consumer(ch);
        shuttle::close(ch);
        return rc;
    }
    std::fprintf(stderr,
                 "usage: %s [hotpath | lat-producer </n> | lat-consumer </n>]\n",
                 argv[0]);
    return 2;
}
