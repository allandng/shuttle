#pragma once

// The single platform seam. Nothing else in the codebase may #ifdef on
// platform; every macOS-vs-Linux divergence (robust mutexes, timedwait
// clocks, shm name limits, one-shot ftruncate, ...) gets an interface here
// with two implementations.

#if defined(__linux__)
  #define SHUTTLE_PLATFORM_LINUX 1
  #define SHUTTLE_PLATFORM_NAME "linux"
#elif defined(__APPLE__)
  #define SHUTTLE_PLATFORM_MACOS 1
  #define SHUTTLE_PLATFORM_NAME "macos"
#else
  #error "Shuttle supports Linux and macOS only (v1.0)"
#endif

#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace shuttle {

const char* platform_name() noexcept;

// macOS caps shm names at PSHMNAMLEN (31) chars including the leading '/';
// we enforce 30 to stay clear of the off-by-one ambiguity in the docs.
// Linux allows NAME_MAX-ish (~254). An shm object can also effectively be
// ftruncate'd only ONCE on macOS — create() sizes it exactly once, at
// creation, on both platforms, so that divergence never surfaces.
inline bool shm_name_ok(const char* name) noexcept {
#if defined(SHUTTLE_PLATFORM_MACOS)
    constexpr size_t kMax = 30;
#else
    constexpr size_t kMax = 254;
#endif
    const size_t n = std::strlen(name);
    return n >= 2 && n <= kMax;
}

// Process-shared mutex init. Robust attribute (Linux) arrives in Phase 5.
inline int mutex_init_pshared(pthread_mutex_t* m) noexcept {
    pthread_mutexattr_t a;
    int rc = pthread_mutexattr_init(&a);
    if (rc != 0) return rc;
    rc = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (rc == 0) rc = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return rc;
}

// Process-shared condvar init. Timed waits must never use CLOCK_REALTIME
// (binding minor amendment): on Linux the condvar clock is CLOCK_MONOTONIC;
// on macOS setclock is unsupported and the relative-wait entry point below
// is monotonic by definition.
inline int cond_init_pshared_monotonic(pthread_cond_t* c) noexcept {
    pthread_condattr_t a;
    int rc = pthread_condattr_init(&a);
    if (rc != 0) return rc;
    rc = pthread_condattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
#if defined(SHUTTLE_PLATFORM_LINUX)
    if (rc == 0) rc = pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    if (rc == 0) rc = pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
    return rc;
}

// Relative timed wait on a pshared condvar; mutex must be held.
// Returns 0 on wake (incl. spurious), ETIMEDOUT on timeout, else errno.
inline int cond_timedwait_rel(pthread_cond_t* c, pthread_mutex_t* m,
                              uint64_t rel_ns) noexcept {
#if defined(SHUTTLE_PLATFORM_LINUX)
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += static_cast<time_t>(rel_ns / 1000000000ull);
    ts.tv_nsec += static_cast<long>(rel_ns % 1000000000ull);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts);
#else
    timespec rel;
    rel.tv_sec = static_cast<time_t>(rel_ns / 1000000000ull);
    rel.tv_nsec = static_cast<long>(rel_ns % 1000000000ull);
    return pthread_cond_timedwait_relative_np(c, m, &rel);
#endif
}

// Park-mutex acquisition (amendment A3): macOS has no robust mutexes and no
// pthread_mutex_timedlock, so acquiring the park mutex must NEVER be a bare
// lock — a peer that died holding it would strand us forever. The macOS
// path is a trylock loop with a short sleep; Phase 5 adds the heartbeat
// staleness check inside this loop, and Phase 5b adds EOWNERDEAD robust
// recovery on the Linux path.
inline int park_mutex_lock(pthread_mutex_t* m) noexcept {
#if defined(SHUTTLE_PLATFORM_MACOS)
    for (;;) {
        const int rc = pthread_mutex_trylock(m);
        if (rc != EBUSY) return rc;
        usleep(100);  // Phase 5: heartbeat staleness check joins here
    }
#else
    return pthread_mutex_lock(m);  // Phase 5b: robust EOWNERDEAD handling
#endif
}

inline int park_mutex_unlock(pthread_mutex_t* m) noexcept {
    return pthread_mutex_unlock(m);
}

// Spin-wait hint for busy-poll loops (Phase 3) — architecture divergence is
// also confined to this seam file.
inline void cpu_relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#else
    // no hint available; plain spin
#endif
}

inline void yield_thread() noexcept { sched_yield(); }

// Filesystem view of a named shm object, for leak checks (NFR-R2).
// Linux exposes "/name" as /dev/shm/name — returns 1 if present, 0 if not.
// macOS has no filesystem view of POSIX shm at all — returns -1
// ("unobservable"); callers must fall back to open()-fails verification.
inline int shm_object_exists_fs(const char* name) noexcept {
#if defined(SHUTTLE_PLATFORM_LINUX)
    char path[300];
    std::snprintf(path, sizeof path, "/dev/shm/%s", name + 1);
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
#else
    (void)name;
    return -1;
#endif
}

// Monotonic clock in nanoseconds (portable POSIX; not a platform seam, kept
// here so test/driver code shares one definition).
inline uint64_t monotonic_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace shuttle
