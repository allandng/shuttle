// G5.1 (A3: BOTH platforms): SIGKILL the producer mid-reservation; the
// consumer's blocked wait must abort via heartbeat staleness with the
// documented error (kErrPeerDead) within ~the configured threshold — never
// a permanent block.
//
// Choreography:
//   crasher (producer): writes one marker message, ACQUIRES a 4 KiB
//     reservation and never commits it (reservation state is process-local
//     per A1, so the kill leaves no shared-state inconsistency), then
//     keepalives every 50 ms until the driver SIGKILLs it at t~1.0 s.
//   victim (consumer, stale threshold 1 s): reads the marker, then blocks
//     on the next read. Expectations: the read fails with kErrPeerDead;
//     elapsed time is >= the 1 s threshold (no premature abort while the
//     crasher was alive and keepaliving) and well under the 60 s hang
//     deadline; the uncommitted reservation never surfaces as data.
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kStaleNs = 1ull * 1000000000ull;  // victim's threshold
constexpr uint64_t kKillAfterNs = 1ull * 1000000000ull;
constexpr uint64_t kMaxDetectNs = 6ull * 1000000000ull;
constexpr uint64_t kChildTimeoutNs = 60ull * 1000000000ull;
constexpr char kMarker[] = "marker";

int run_crasher(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "crasher: open err=%d\n", err);
        return 1;
    }
    shuttle::Producer p(ch);
    if (p.write(kMarker, sizeof(kMarker)) != shuttle::kOk) {
        std::fprintf(stderr, "crasher: marker write failed\n");
        return 1;
    }
    void* span = nullptr;
    if (p.acquire_write(&span, 4096) != shuttle::kOk) {
        std::fprintf(stderr, "crasher: acquire failed\n");
        return 1;
    }
    std::memset(span, 0xDD, 4096);  // partially-written, never committed
    // Stay demonstrably alive until SIGKILLed: the victim must not declare
    // us dead while we are heartbeating.
    for (;;) {
        p.keepalive();
        usleep(50000);
    }
    // unreachable
}

int run_victim(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "victim: open err=%d\n", err);
        return 1;
    }
    shuttle::Consumer c(ch, kStaleNs);
    const unsigned char* p = nullptr;
    uint64_t len = 0;
    if (c.read(&p, &len) != shuttle::kOk || len != sizeof(kMarker)) {
        std::fprintf(stderr, "victim: marker read failed\n");
        return 1;
    }
    c.release();

    const uint64_t t0 = shuttle::monotonic_ns();
    const int rc = c.read(&p, &len);  // producer dies while we are parked
    const uint64_t elapsed = shuttle::monotonic_ns() - t0;

    int fails = 0;
    if (rc != shuttle::kErrPeerDead) {
        std::fprintf(stderr, "victim: rc=%d, want kErrPeerDead\n", rc);
        ++fails;
    }
    if (elapsed < kStaleNs) {
        std::fprintf(stderr,
                     "victim: aborted after %.2f s < 1 s threshold —"
                     " premature death verdict on a live, keepaliving peer\n",
                     elapsed / 1e9);
        ++fails;
    }
    if (elapsed > kMaxDetectNs) {
        std::fprintf(stderr, "victim: detection took %.2f s (> 6 s)\n",
                     elapsed / 1e9);
        ++fails;
    }
    // The dead producer's uncommitted reservation must never surface.
    if (c.try_read(&p, &len) != shuttle::kErrWouldBlock) {
        std::fprintf(stderr, "victim: phantom data after peer death!\n");
        ++fails;
    }
    if (fails == 0) {
        std::printf("victim: kErrPeerDead %.2f s after parking (threshold"
                    " 1 s), no phantom data\n",
                    elapsed / 1e9);
    }
    shuttle::close(ch);
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    char name[32];
    std::snprintf(name, sizeof name, "/shcrash.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }

    const char* roles[2] = {"crasher", "victim"};
    pid_t pids[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>(roles[i]),
                        name, nullptr};
        if (posix_spawn(&pids[i], self, nullptr, nullptr, argv, environ) !=
            0) {
            std::fprintf(stderr, "driver: spawn %s failed\n", roles[i]);
            shuttle::close(ch);
            shuttle::unlink(name);
            return 1;
        }
    }

    usleep(static_cast<useconds_t>(kKillAfterNs / 1000));
    kill(pids[0], SIGKILL);  // no cleanup, mid-reservation, mid-keepalive
    waitpid(pids[0], nullptr, 0);

    int fails = 0;
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pids[1], &st, WNOHANG) == pids[1]) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "driver: victim failed (0x%x)\n", st);
                ++fails;
            }
            break;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr,
                         "driver: victim STILL BLOCKED %ds after producer"
                         " death — heartbeat abort failed\n",
                         60);
            kill(pids[1], SIGKILL);
            waitpid(pids[1], nullptr, 0);
            ++fails;
            break;
        }
        usleep(10000);
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("crash_heartbeat ok: kill mid-reservation -> stale-abort"
                    " within threshold\n");
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3 && std::strcmp(argv[1], "crasher") == 0)
        return run_crasher(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "victim") == 0)
        return run_victim(argv[2]);
    std::fprintf(stderr, "usage: %s [crasher|victim </name>]\n", argv[0]);
    return 2;
}
