// G4.1: an idle blocked consumer consumes ~0% CPU — true parking, not
// residual spin. The driver creates the channel and spawns an idler child;
// the child starts a blocking read with NOTHING to read, the driver lets it
// sit for 3 seconds, then writes one message. The child measures its own
// CPU time (user+sys, getrusage) across the blocked read and fails if it
// burned more than a small budget. The Phase 3 busy-poll implementation
// would burn ~3 full CPU-seconds here — this gate genuinely distinguishes
// parking from spinning.
#include <sys/resource.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kIdleNs = 3ull * 1000000000ull;
constexpr uint64_t kMinObservedWaitNs = 2ull * 1000000000ull;
constexpr uint64_t kMaxCpuNs = 250ull * 1000000;  // 250 ms; spin would be ~3 s
constexpr uint64_t kChildTimeoutNs = 60ull * 1000000000ull;
constexpr char kWakeMsg[] = "wake-up";

uint64_t cpu_self_ns() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    auto tv_ns = [](const timeval& tv) {
        return static_cast<uint64_t>(tv.tv_sec) * 1000000000ull +
               static_cast<uint64_t>(tv.tv_usec) * 1000ull;
    };
    return tv_ns(ru.ru_utime) + tv_ns(ru.ru_stime);
}

int run_idler(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "idler: open err=%d\n", err);
        return 1;
    }
    shuttle::Consumer c(ch);

    const uint64_t wall0 = shuttle::monotonic_ns();
    const uint64_t cpu0 = cpu_self_ns();
    const unsigned char* p = nullptr;
    uint64_t len = 0;
    const int rc = c.read(&p, &len);  // parks: nothing arrives for ~3 s
    const uint64_t cpu_used = cpu_self_ns() - cpu0;
    const uint64_t waited = shuttle::monotonic_ns() - wall0;

    int fails = 0;
    if (rc != shuttle::kOk || len != sizeof(kWakeMsg) ||
        std::memcmp(p, kWakeMsg, sizeof(kWakeMsg)) != 0) {
        std::fprintf(stderr, "idler: bad wake message (rc=%d len=%llu)\n", rc,
                     (unsigned long long)len);
        ++fails;
    }
    c.release();
    if (waited < kMinObservedWaitNs) {
        std::fprintf(stderr,
                     "idler: only waited %.2f s — driver fed us too early,"
                     " measurement void\n",
                     waited / 1e9);
        ++fails;
    }
    if (cpu_used > kMaxCpuNs) {
        std::fprintf(stderr,
                     "idler: burned %.0f ms CPU while blocked %.2f s —"
                     " spinning, not parked\n",
                     cpu_used / 1e6, waited / 1e9);
        ++fails;
    }
    if (fails == 0) {
        std::printf("idler: blocked %.2f s using %.1f ms CPU (%.2f%%)\n",
                    waited / 1e9, cpu_used / 1e6,
                    100.0 * cpu_used / waited);
    }
    shuttle::close(ch);
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    char name[32];
    std::snprintf(name, sizeof name, "/shidle.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }

    pid_t pid = 0;
    {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>("idler"),
                        name, nullptr};
        const int rc = posix_spawn(&pid, self, nullptr, nullptr, argv, environ);
        if (rc != 0) {
            std::fprintf(stderr, "driver: posix_spawn: %s\n",
                         std::strerror(rc));
            shuttle::close(ch);
            shuttle::unlink(name);
            return 1;
        }
    }

    // Let the child park with an empty channel.
    usleep(static_cast<useconds_t>(kIdleNs / 1000));

    shuttle::Producer prod(ch);
    int fails = 0;
    if (prod.write(kWakeMsg, sizeof(kWakeMsg)) != shuttle::kOk) {
        std::fprintf(stderr, "driver: wake write failed\n");
        ++fails;
    }

    // Reap with deadline; timeout = failure (never hang the gate).
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "driver: idler failed (0x%x)\n", st);
                ++fails;
            }
            break;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "driver: TIMEOUT waiting for idler\n");
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            ++fails;
            break;
        }
        usleep(5000);
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0) std::printf("park_idle ok: idle blocked peer ~0%% CPU\n");
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3 && std::strcmp(argv[1], "idler") == 0)
        return run_idler(argv[2]);
    std::fprintf(stderr, "usage: %s [idler </name>]\n", argv[0]);
    return 2;
}
