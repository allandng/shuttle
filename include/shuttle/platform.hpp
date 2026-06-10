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

#include <cstdint>
#include <ctime>

namespace shuttle {

const char* platform_name() noexcept;

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

// Monotonic clock in nanoseconds (portable POSIX; not a platform seam, kept
// here so test/driver code shares one definition).
inline uint64_t monotonic_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace shuttle
