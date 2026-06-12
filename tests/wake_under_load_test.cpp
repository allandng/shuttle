// G7.3 (NFR-P3): wake latency stays microsecond-scale UNDER LOAD —
// a sustained 16 KB frame stream (~20k frames/s, ~320 MB/s) rather than
// G4.3's gentle pacing. The consumer drains each frame far faster than the
// inter-frame gap, so it parks between frames and nearly every receipt is
// a genuine commit->wake->payload-held cycle measured under streaming
// pressure. Methodology identical to G4.3 (CLOCK_MONOTONIC stamped into
// the payload immediately before write; fill excluded); pass criterion the
// same budget G4.3 passed: p99 < 1 ms, p50 < 100 us — "microseconds, not
// milliseconds", consistent across the two gates.
//
// Built unsanitized at -O2 (latency measurement).
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "shuttle/platform.hpp"
#include "shuttle/shuttle.hpp"
#include "shuttle/spsc.hpp"

extern char** environ;

namespace {

constexpr uint64_t kFrame = 16 * 1024;
constexpr int kWarmup = 2000;
constexpr int kIters = 20000;
constexpr uint64_t kPaceUs = 50;  // ~20k frames/s offered load
constexpr uint64_t kP99BudgetNs = 1ull * 1000000;   // 1 ms (as G4.3)
constexpr uint64_t kP50BudgetNs = 100ull * 1000;    // 100 us
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

int producer(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Producer p(ch);
    std::vector<unsigned char> buf(kFrame);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        std::memset(buf.data(), 0x60 + (i & 0xF), kFrame);
        const uint64_t t = shuttle::monotonic_ns();
        std::memcpy(buf.data(), &t, 8);
        if (p.write(buf.data(), kFrame) != shuttle::kOk) return 1;
        usleep(kPaceUs);
    }
    shuttle::close(ch);
    return 0;
}

int consumer(const char* name, const char* outpath) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Consumer c(ch);
    std::vector<uint64_t> lat;
    lat.reserve(kWarmup + kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.read(&p, &len) != shuttle::kOk || len != kFrame) return 1;
        uint64_t t = 0;
        std::memcpy(&t, p, 8);
        lat.push_back(shuttle::monotonic_ns() - t);
        c.release();
    }
    shuttle::close(ch);
    lat.erase(lat.begin(), lat.begin() + kWarmup);
    std::sort(lat.begin(), lat.end());
    FILE* f = std::fopen(outpath, "w");
    if (f == nullptr) return 1;
    std::fprintf(f, "%llu %llu %llu\n",
                 (unsigned long long)lat[lat.size() / 2],
                 (unsigned long long)lat[lat.size() * 99 / 100],
                 (unsigned long long)lat.back());
    std::fclose(f);
    return 0;
}

int wait_deadline(pid_t pid, const char* what) {
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "%s failed (0x%x)\n", what, st);
                return 1;
            }
            return 0;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "%s TIMEOUT\n", what);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return 1;
        }
        usleep(5000);
    }
}

int run_driver(const char* self) {
    char name[32], out[64];
    std::snprintf(name, sizeof name, "/shwul.%d",
                  static_cast<int>(getpid()) % 1000000);
    std::snprintf(out, sizeof out, "/tmp/shwul.%d.lat",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    // Capacity for ~256 frames in flight: load, not unbounded queueing.
    shuttle::Channel* ch = shuttle::create(name, 4ull << 20, 1u << 20, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }

    pid_t cons = 0, prod = 0;
    {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>("cons"),
                        name, out, nullptr};
        if (posix_spawn(&cons, self, nullptr, nullptr, argv, environ) != 0)
            return 1;
    }
    {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>("prod"),
                        name, nullptr};
        if (posix_spawn(&prod, self, nullptr, nullptr, argv, environ) != 0) {
            kill(cons, SIGKILL);
            waitpid(cons, nullptr, 0);
            return 1;
        }
    }
    int fails = wait_deadline(prod, "producer");
    fails += wait_deadline(cons, "consumer");
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails != 0) return 1;

    unsigned long long p50 = 0, p99 = 0, pmax = 0;
    FILE* f = std::fopen(out, "r");
    if (f == nullptr ||
        std::fscanf(f, "%llu %llu %llu", &p50, &p99, &pmax) != 3) {
        std::fprintf(stderr, "driver: missing results\n");
        return 1;
    }
    std::fclose(f);

    std::printf("wake-under-load (%d x 16 KB frames @ ~%llu/s offered):\n",
                kIters, 1000000ull / kPaceUs);
    std::printf("  commit->payload-held: p50=%.1fus p99=%.1fus max=%.1fus\n",
                p50 / 1e3, p99 / 1e3, pmax / 1e3);
    if (p99 > kP99BudgetNs || p50 > kP50BudgetNs) {
        std::fprintf(stderr,
                     "FAIL: not microsecond-scale under load (budgets:"
                     " p50<100us, p99<1ms — the G4.3 criteria)\n");
        return 1;
    }
    std::printf("wake_under_load ok: NFR-P3 holds under streaming load,"
                " consistent with G4.3\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3 && std::strcmp(argv[1], "prod") == 0)
        return producer(argv[2]);
    if (argc == 4 && std::strcmp(argv[1], "cons") == 0)
        return consumer(argv[2], argv[3]);
    std::fprintf(stderr, "usage: %s [prod </name> | cons </name> <out>]\n",
                 argv[0]);
    return 2;
}
