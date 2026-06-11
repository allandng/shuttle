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

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#endif

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

// True where PTHREAD_MUTEX_ROBUST / EOWNERDEAD semantics exist (FR-18).
#if defined(SHUTTLE_PLATFORM_LINUX)
constexpr bool kHasRobustMutex = true;
#else
constexpr bool kHasRobustMutex = false;
#endif

// Process-shared mutex init; on Linux additionally ROBUST, so a peer dying
// while holding it hands EOWNERDEAD to the next locker instead of
// deadlocking it (FR-18). macOS has no robust attribute — its safety net is
// the trylock loop + heartbeat (A3).
inline int mutex_init_pshared(pthread_mutex_t* m) noexcept {
    pthread_mutexattr_t a;
    int rc = pthread_mutexattr_init(&a);
    if (rc != 0) return rc;
    rc = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
#if defined(SHUTTLE_PLATFORM_LINUX)
    if (rc == 0) rc = pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
#endif
    if (rc == 0) rc = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return rc;
}

// EOWNERDEAD recovery (Linux, App. B #3): we now OWN the lock the dead peer
// held. Repair protocol — repair state, THEN pthread_mutex_consistent, THEN
// continue/unlock; consistent-before-repair (or unlock-without-consistent)
// makes the mutex permanently ENOTRECOVERABLE. Repair here is deliberately
// a no-op because the park mutex guards only the park/wake handshake:
// the waiting flags are advisory and owner-cleared (a dead peer's stale
// flag merely causes one spurious signal), the condvars need no repair
// (every waiter is on a bounded timedwait per A3), and all data-path state
// is owned single-writer OUTSIDE the critical section by design (§2.3).
inline int park_mutex_recover_if_needed(pthread_mutex_t* m, int rc) noexcept {
#if defined(SHUTTLE_PLATFORM_LINUX)
    if (rc == EOWNERDEAD) {
        // (no state to repair — see comment above)
        pthread_mutex_consistent(m);
        return 0;  // we hold a now-consistent lock
    }
#else
    (void)m;
#endif
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
    // Re-acquisition inside timedwait can also surface EOWNERDEAD if the
    // peer died holding the robust mutex; recover identically.
    return park_mutex_recover_if_needed(m,
                                        pthread_cond_timedwait(c, m, &ts));
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
        usleep(100);  // G5.4: caller-level heartbeat staleness bounds this
    }
#else
    return park_mutex_recover_if_needed(m, pthread_mutex_lock(m));
#endif
}

inline int park_mutex_unlock(pthread_mutex_t* m) noexcept {
    return pthread_mutex_unlock(m);
}

// ---------------------------------------------------------------------
// Cross-process park/wake on a 64-bit cursor (the Phase 4/5 slow path).
//
// The waiter sleeps until the watched cursor differs from `seen` or the
// timeout elapses; the waker pokes the address after publishing. Two
// implementations:
//
//   macOS: os_sync_wait_on_address (14.4+, SHARED flag for cross-process).
//     Chosen over the pshared condvar because a condvar wait can only
//     return by re-acquiring its mutex — a bare lock that a trylock loop
//     cannot protect. A peer SIGKILLed inside its (tiny) critical section
//     would strand a survivor already inside cond_timedwait forever.
//     Wait-on-address holds NOTHING: there is no ownership to die with,
//     and the value comparison is atomic with the sleep (no lost wakeup).
//
//   Linux: robust pshared mutex + condvar. The cursor==seen recheck under
//     the lock is the lost-wakeup guard; EOWNERDEAD on either the lock or
//     the timedwait re-acquisition is absorbed by the recovery above.
//
// Both paths are bounded (A3): callers re-evaluate predicates and peer
// heartbeats at least every timeout_ns.
// ---------------------------------------------------------------------
inline int park_wait_cursor(std::atomic<uint64_t>* cursor, uint64_t seen,
                            pthread_mutex_t* mu, pthread_cond_t* cv,
                            uint64_t timeout_ns) noexcept {
#if defined(SHUTTLE_PLATFORM_MACOS)
    (void)mu;
    (void)cv;
    const int rc = os_sync_wait_on_address_with_timeout(
        static_cast<void*>(cursor), seen, sizeof(uint64_t),
        OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME,
        timeout_ns);
    // >=0: woken (value is the number of remaining waiters). <0: errno is
    // ETIMEDOUT / EINTR / EAGAIN(value already changed) — all "retry".
    return rc >= 0 ? 0 : errno;
#else
    int rc = park_mutex_lock(mu);
    if (rc != 0) return rc;
    if (cursor->load(std::memory_order_relaxed) == seen) {
        cond_timedwait_rel(cv, mu, timeout_ns);  // EOWNERDEAD-aware
    }
    park_mutex_unlock(mu);
    return 0;
#endif
}

inline void park_wake_cursor(std::atomic<uint64_t>* cursor,
                             pthread_mutex_t* mu, pthread_cond_t* cv) noexcept {
#if defined(SHUTTLE_PLATFORM_MACOS)
    (void)mu;
    (void)cv;
    os_sync_wake_by_address_any(static_cast<void*>(cursor), sizeof(uint64_t),
                                OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#else
    // Signal under the lock so the waiter's recheck-then-wait is atomic
    // with respect to this signal (no lost wakeup).
    if (park_mutex_lock(mu) == 0) {
        pthread_cond_signal(cv);
        park_mutex_unlock(mu);
    }
#endif
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
