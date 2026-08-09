// G5.4: the second crash kill-point — the peer dies while HOLDING the park
// mutex, with the survivor blocked in the real blocking API.
//
//   Linux:  the survivor's park path takes the robust mutex (or hits it on
//           timedwait re-acquisition); EOWNERDEAD is absorbed by the seam,
//           and the wait then aborts via heartbeat staleness. The driver
//           afterward proves the mutex is fully usable (recovered, not
//           poisoned).
//   macOS:  the parking protocol holds NOTHING (os_sync_wait_on_address —
//           the recorded Phase 5 decision): a peer dying with the legacy
//           mutex held cannot strand anyone, and the survivor aborts via
//           heartbeat exactly as in G5.1.
//
// Either way the survivor's blocking read MUST return kErrPeerDead within
// the staleness bound — never deadlock.
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kStaleNs = 1500ull * 1000000;  // 1.5 s
constexpr uint64_t kKillAfterNs = 1ull * 1000000000ull;
constexpr uint64_t kMaxDetectNs = 8ull * 1000000000ull;
constexpr uint64_t kChildTimeoutNs = 60ull * 1000000000ull;
constexpr uint64_t kHoldSentinel = 0xBEEF;

// Producer-side child: write a marker, grab the park mutex, announce, then
// keepalive (heartbeat alive, mutex held) until SIGKILLed.
int run_crasher(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Producer p(ch);
    static const char kMarker[] = "marker";
    if (p.write(kMarker, sizeof(kMarker)) != shuttle::kOk) return 1;
    if (shuttle::park_mutex_lock(&ch->hdr->park.lock) != 0) {
        std::fprintf(stderr, "crasher: mutex lock failed\n");
        return 1;
    }
    // Sentinel on the producer heartbeat: driver knows the lock is held.
    // (Heartbeat keepalives continue from this value.)
    ch->hdr->producer_heartbeat.store(kHoldSentinel,
                                      std::memory_order_release);
    for (;;) {
        p.keepalive();
        usleep(50000);
    }
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
    if (c.read(&p, &len) != shuttle::kOk) {
        std::fprintf(stderr, "victim: marker read failed\n");
        return 1;
    }
    c.release();

    const uint64_t t0 = shuttle::monotonic_ns();
    const int rc = c.read(&p, &len);  // peer dies HOLDING the park mutex
    const uint64_t elapsed = shuttle::monotonic_ns() - t0;

    int fails = 0;
    if (rc != shuttle::kErrPeerDead) {
        std::fprintf(stderr, "victim: rc=%d, want kErrPeerDead\n", rc);
        ++fails;
    }
    if (elapsed < kStaleNs || elapsed > kMaxDetectNs) {
        std::fprintf(stderr, "victim: detection at %.2f s outside"
                     " [1.5 s, 8 s]\n", elapsed / 1e9);
        ++fails;
    }
    if (fails == 0)
        std::printf("victim: peer died holding park mutex -> kErrPeerDead"
                    " %.2f s after parking\n",
                    elapsed / 1e9);
    shuttle::close(ch);
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    char name[32];
    std::snprintf(name, sizeof name, "/shcmx.%d",
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

    // Confirm the crasher holds the mutex, then let the victim park, then
    // kill the holder with no cleanup.
    int fails = 0;
    const uint64_t lockwait = shuttle::monotonic_ns() + kChildTimeoutNs;
    while (ch->hdr->producer_heartbeat.load(std::memory_order_acquire) <
           kHoldSentinel) {
        if (shuttle::monotonic_ns() > lockwait) {
            std::fprintf(stderr, "driver: crasher never took the lock\n");
            ++fails;
            break;
        }
        usleep(2000);
    }
    usleep(static_cast<useconds_t>(kKillAfterNs / 1000));
    kill(pids[0], SIGKILL);
    waitpid(pids[0], nullptr, 0);

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
            std::fprintf(stderr, "driver: victim DEADLOCKED after peer died"
                                 " holding the park mutex\n");
            kill(pids[1], SIGKILL);
            waitpid(pids[1], nullptr, 0);
            ++fails;
            break;
        }
        usleep(10000);
    }

    // Where robust mutexes exist, the orphaned lock must be recoverable
    // and serviceable afterward — not poisoned.
    if (shuttle::kHasRobustMutex) {
        if (shuttle::park_mutex_lock(&ch->hdr->park.lock) != 0) {
            std::fprintf(stderr, "driver: post-crash mutex unusable\n");
            ++fails;
        } else {
            shuttle::park_mutex_unlock(&ch->hdr->park.lock);
        }
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("crash_mutex ok: kill-while-holding-mutex never strands"
                    " the survivor (platform=%s)\n",
                    shuttle::platform_name());
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
