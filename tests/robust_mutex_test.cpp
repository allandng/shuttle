// G5.2 (Linux container; no-ops on macOS — robust mutexes do not exist
// there, the heartbeat path of G5.1/G5.4 is its guarantee):
//
//   A) GOOD RECOVERY: a child locks the park mutex and is SIGKILLed while
//      holding it. The survivor's park_mutex_lock must return success
//      (EOWNERDEAD handled: repair [no-op by design] -> consistent), and
//      the mutex must remain fully usable afterward — lock/unlock cycles
//      and a condvar timedwait all work. No permanent deadlock (FR-18).
//
//   B) THE TEST CAN FAIL (plan 5b debugging strategy): same kill, but the
//      survivor deliberately performs the BUGGY recovery — unlock without
//      pthread_mutex_consistent. The next lock attempt must then return
//      ENOTRECOVERABLE, proving the permanently-dead-mutex failure mode is
//      real and that scenario A's consistent call is what prevents it.
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kHoldSentinel = 0xD00D;
constexpr uint64_t kWaitNs = 30ull * 1000000000ull;

// Child: take the park mutex, announce via a sentinel heartbeat value,
// then sleep until SIGKILLed — dying as the mutex owner.
int run_holder(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "holder: open err=%d\n", err);
        return 1;
    }
    if (shuttle::park_mutex_lock(&ch->hdr->lock) != 0) {
        std::fprintf(stderr, "holder: lock failed\n");
        return 1;
    }
    ch->hdr->producer_heartbeat.store(kHoldSentinel,
                                      std::memory_order_release);
    for (;;) pause();  // die holding the lock (SIGKILL: no cleanup)
}

// Spawn a holder on `name`, wait until it owns the mutex, SIGKILL it.
int kill_holder_midlock(const char* self, shuttle::Channel* ch,
                        const char* name) {
    pid_t pid = 0;
    char* argv[] = {const_cast<char*>(self), const_cast<char*>("holder"),
                    const_cast<char*>(name), nullptr};
    if (posix_spawn(&pid, self, nullptr, nullptr, argv, environ) != 0) {
        std::fprintf(stderr, "spawn holder failed\n");
        return -1;
    }
    const uint64_t deadline = shuttle::monotonic_ns() + kWaitNs;
    while (ch->hdr->producer_heartbeat.load(std::memory_order_acquire) !=
           kHoldSentinel) {
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "holder never took the lock\n");
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -1;
        }
        usleep(2000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return 0;
}

shuttle::Channel* fresh_channel(char* name, size_t cap, const char* tag) {
    std::snprintf(name, 32, "/shrob%s.%d", tag,
                  static_cast<int>(getpid()) % 100000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 16, 1u << 10, &err);
    if (ch == nullptr) std::fprintf(stderr, "create %s err=%d\n", name, err);
    (void)cap;
    return ch;
}

int scenario_good(const char* self) {
    char name[32];
    shuttle::Channel* ch = fresh_channel(name, 1u << 16, "a");
    if (ch == nullptr) return 1;
    int fails = 0;
    if (kill_holder_midlock(self, ch, name) != 0) ++fails;

    // Survivor path: the seam must absorb EOWNERDEAD and hand us a usable,
    // consistent mutex. A hang here = permanent deadlock = gate failure
    // (bounded by the ctest timeout).
    if (shuttle::park_mutex_lock(&ch->hdr->lock) != 0) {
        std::fprintf(stderr, "good: recovery lock failed\n");
        ++fails;
    } else {
        shuttle::park_mutex_unlock(&ch->hdr->lock);
    }
    // Mutex must be fully serviceable afterward: plain cycle + a condvar
    // timedwait (expects ETIMEDOUT — nobody signals).
    if (shuttle::park_mutex_lock(&ch->hdr->lock) != 0) {
        std::fprintf(stderr, "good: post-recovery lock failed\n");
        ++fails;
    } else {
        const int wrc = shuttle::cond_timedwait_rel(
            &ch->hdr->not_empty, &ch->hdr->lock, 50ull * 1000000);
        if (wrc != ETIMEDOUT) {
            std::fprintf(stderr, "good: timedwait rc=%d, want ETIMEDOUT\n",
                         wrc);
            ++fails;
        }
        shuttle::park_mutex_unlock(&ch->hdr->lock);
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("good recovery: EOWNERDEAD absorbed, mutex fully usable"
                    " after owner death\n");
    return fails;
}

int scenario_bad(const char* self) {
    char name[32];
    shuttle::Channel* ch = fresh_channel(name, 1u << 16, "b");
    if (ch == nullptr) return 1;
    int fails = 0;
    if (kill_holder_midlock(self, ch, name) != 0) ++fails;

    // Deliberately buggy recovery: raw lock -> observe EOWNERDEAD -> unlock
    // WITHOUT pthread_mutex_consistent.
    int rc = pthread_mutex_lock(&ch->hdr->lock);
    if (rc != EOWNERDEAD) {
        std::fprintf(stderr, "bad: lock rc=%d, want EOWNERDEAD\n", rc);
        ++fails;
    }
    pthread_mutex_unlock(&ch->hdr->lock);  // the bug: no consistent()

    // The mutex must now be permanently dead — and detectably so.
    rc = pthread_mutex_lock(&ch->hdr->lock);
    if (rc != ENOTRECOVERABLE) {
        std::fprintf(stderr,
                     "bad: post-bug lock rc=%d, want ENOTRECOVERABLE — the"
                     " failure mode this gate guards against is not"
                     " detectable\n",
                     rc);
        ++fails;
    }
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("bad recovery: skipping consistent() leaves mutex"
                    " ENOTRECOVERABLE — test verified able to fail\n");
    return fails;
}

}  // namespace

int main(int argc, char** argv) {
    if (!shuttle::kHasRobustMutex) {
        std::printf("robust_mutex_test: skipped (no robust mutexes on %s;"
                    " heartbeat path covers peer death — G5.1/G5.4)\n",
                    shuttle::platform_name());
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "holder") == 0)
        return run_holder(argv[2]);
    if (argc == 1) {
        int fails = scenario_good(argv[0]) + scenario_bad(argv[0]);
        if (fails == 0) std::printf("robust_mutex ok: FR-18 verified\n");
        return fails == 0 ? 0 : 1;
    }
    std::fprintf(stderr, "usage: %s [holder </name>]\n", argv[0]);
    return 2;
}
