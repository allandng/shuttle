// Phase 0 pshared smoke (G0.4): proves a process-shared mutex + condvar in
// POSIX shm can park one process and let another wake it, on this platform.
// This is the single mechanism Phase 4's parking lot depends on; the macOS
// leg is historically the riskiest unknown (fallback if it fails:
// os_sync_wait_on_address behind the platform seam).
//
// Structure (per the binding amendments):
//   - driver (no args): creates the segment, inits pshared primitives,
//     publishes init with a release store, then posix_spawn's THIS binary
//     twice with role arguments. Plain fork without exec is forbidden
//     (TSan on macOS cannot survive fork-without-exec).
//   - waiter: parks on the condvar; exits 0 only if genuinely signaled.
//   - signaler: waits until the waiter is ready, then signals.
//   - Every wait has a deadline; timeout = test failure, never a hang.
//   - A per-run nonce in the segment (also passed on argv) rejects stale
//     segments from crashed prior runs.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "shuttle/platform.hpp"

extern char** environ;

namespace {

constexpr uint64_t kMagic = 0x534850534D4F4B45ull;  // "SHPSMOKE"
constexpr uint64_t kWaitNs = 5ull * 1000000000ull;  // child-side deadline
constexpr uint64_t kDriverNs = 15ull * 1000000000ull;

struct SmokeSeg {
    std::atomic<uint32_t> init_state;  // 0 = uninit, 1 = ready (release/acquire)
    uint32_t pad_;
    uint64_t magic;
    uint64_t nonce;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t waiter_ready;  // guarded by mu
    uint32_t signaled;      // guarded by mu
};

SmokeSeg* open_segment(const char* name, uint64_t nonce) {
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        std::perror("child shm_open");
        return nullptr;
    }
    void* p = mmap(nullptr, sizeof(SmokeSeg), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        std::perror("child mmap");
        return nullptr;
    }
    auto* seg = static_cast<SmokeSeg*>(p);
    const uint64_t deadline = shuttle::monotonic_ns() + kWaitNs;
    while (seg->init_state.load(std::memory_order_acquire) != 1) {
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "child: init never published\n");
            return nullptr;
        }
        usleep(1000);
    }
    if (seg->magic != kMagic || seg->nonce != nonce) {
        std::fprintf(stderr, "child: magic/nonce mismatch (stale segment?)\n");
        return nullptr;
    }
    return seg;
}

int run_waiter(const char* name, uint64_t nonce) {
    SmokeSeg* seg = open_segment(name, nonce);
    if (!seg) return 1;
    int rc = pthread_mutex_lock(&seg->mu);
    if (rc != 0) {
        std::fprintf(stderr, "waiter: lock failed: %s\n", std::strerror(rc));
        return 1;
    }
    seg->waiter_ready = 1;
    while (seg->signaled == 0) {
        rc = shuttle::cond_timedwait_rel(&seg->cv, &seg->mu, kWaitNs);
        if (rc == ETIMEDOUT) {
            std::fprintf(stderr, "waiter: TIMEOUT — never signaled\n");
            break;
        }
        if (rc != 0) {
            std::fprintf(stderr, "waiter: timedwait failed: %s\n",
                         std::strerror(rc));
            break;
        }
    }
    const bool ok = (seg->signaled != 0);
    pthread_mutex_unlock(&seg->mu);
    if (ok) std::printf("waiter: woken by signal, ok\n");
    return ok ? 0 : 1;
}

int run_signaler(const char* name, uint64_t nonce) {
    SmokeSeg* seg = open_segment(name, nonce);
    if (!seg) return 1;
    const uint64_t deadline = shuttle::monotonic_ns() + kWaitNs;
    for (;;) {
        int rc = pthread_mutex_lock(&seg->mu);
        if (rc != 0) {
            std::fprintf(stderr, "signaler: lock failed: %s\n",
                         std::strerror(rc));
            return 1;
        }
        // waiter_ready was set under mu just before the waiter entered
        // cond_wait, so holding mu and seeing it set means the waiter is
        // parked (or already woken) — the signal cannot be lost.
        if (seg->waiter_ready != 0) {
            seg->signaled = 1;
            rc = pthread_cond_signal(&seg->cv);
            pthread_mutex_unlock(&seg->mu);
            if (rc != 0) {
                std::fprintf(stderr, "signaler: signal failed: %s\n",
                             std::strerror(rc));
                return 1;
            }
            std::printf("signaler: signaled parked waiter, ok\n");
            return 0;
        }
        pthread_mutex_unlock(&seg->mu);
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "signaler: TIMEOUT — waiter never ready\n");
            return 1;
        }
        usleep(1000);
    }
}

int run_driver(const char* self) {
    if (std::strchr(self, '/') == nullptr) {
        std::fprintf(stderr,
                     "driver: argv[0] must be a path for posix_spawn (%s)\n",
                     self);
        return 2;
    }
    char name[32];
    std::snprintf(name, sizeof name, "/pshsmk.%d",
                  static_cast<int>(getpid()) % 1000000);
    shm_unlink(name);  // clear any stale object
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        std::perror("driver shm_open");
        return 1;
    }
    if (ftruncate(fd, sizeof(SmokeSeg)) != 0) {
        std::perror("driver ftruncate");
        shm_unlink(name);
        return 1;
    }
    void* p = mmap(nullptr, sizeof(SmokeSeg), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        std::perror("driver mmap");
        shm_unlink(name);
        return 1;
    }
    auto* seg = static_cast<SmokeSeg*>(p);  // ftruncate zero-fills: init_state==0
    seg->magic = kMagic;
    seg->nonce = shuttle::monotonic_ns() ^
                 (static_cast<uint64_t>(getpid()) << 32);
    seg->waiter_ready = 0;
    seg->signaled = 0;
    if (shuttle::mutex_init_pshared(&seg->mu) != 0 ||
        shuttle::cond_init_pshared_monotonic(&seg->cv) != 0) {
        std::fprintf(stderr, "driver: pshared init failed\n");
        shm_unlink(name);
        return 1;
    }
    seg->init_state.store(1, std::memory_order_release);

    char noncebuf[32];
    std::snprintf(noncebuf, sizeof noncebuf, "%" PRIu64, seg->nonce);
    const char* roles[2] = {"waiter", "signaler"};
    pid_t pids[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>(roles[i]),
                        name, noncebuf, nullptr};
        int rc = posix_spawn(&pids[i], self, nullptr, nullptr, argv, environ);
        if (rc != 0) {
            std::fprintf(stderr, "driver: posix_spawn(%s) failed: %s\n",
                         roles[i], std::strerror(rc));
            if (i == 1) kill(pids[0], SIGKILL);
            shm_unlink(name);
            return 1;
        }
    }

    const uint64_t deadline = shuttle::monotonic_ns() + kDriverNs;
    bool done[2] = {false, false};
    int fails = 0;
    while (!(done[0] && done[1])) {
        for (int i = 0; i < 2; ++i) {
            if (done[i]) continue;
            int st = 0;
            pid_t r = waitpid(pids[i], &st, WNOHANG);
            if (r == pids[i]) {
                done[i] = true;
                if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                    std::fprintf(stderr, "driver: %s failed (status 0x%x)\n",
                                 roles[i], st);
                    ++fails;
                }
            }
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "driver: TIMEOUT — killing children\n");
            for (int i = 0; i < 2; ++i) {
                if (!done[i]) {
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], nullptr, 0);
                }
            }
            ++fails;
            break;
        }
        usleep(10000);
    }
    munmap(p, sizeof(SmokeSeg));
    shm_unlink(name);
    if (fails == 0) {
        std::printf("pshared_smoke ok: park + cross-process wake (platform=%s)\n",
                    shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 4) {
        const uint64_t nonce = std::strtoull(argv[3], nullptr, 10);
        if (std::strcmp(argv[1], "waiter") == 0) return run_waiter(argv[2], nonce);
        if (std::strcmp(argv[1], "signaler") == 0)
            return run_signaler(argv[2], nonce);
    }
    std::fprintf(stderr, "usage: %s [waiter|signaler <shm-name> <nonce>]\n",
                 argv[0]);
    return 2;
}
