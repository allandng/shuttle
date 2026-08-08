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

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// ---------------------------------------------------------------------
// Segment backend: the named object a channel's bytes live in.
//
// Every syscall that creates, sizes, maps, unmaps, or destroys that object
// goes through the six functions below, so a new way of obtaining the pages
// — explicit hugetlbfs on Linux, a Windows file mapping — arrives as a new
// SegBacking value plus a branch *here*, never as a syscall (or an #ifdef)
// in src/.
//
// Error convention: functions that return a resource report failure with a
// sentinel (kSegInvalid / nullptr) and write a POSIX errno-style code into
// `err_out`; the two that return nothing useful return 0-or-errno directly.
// A future non-POSIX backend translates its native error into that space,
// so callers keep one platform-independent errno -> shuttle::Err mapping.
//
// TWO NAMESPACES (hugetlbfs). A kShm segment lives in the POSIX shm namespace
// ("/name" -> /dev/shm/name on Linux); a hugetlb segment is an ordinary FILE
// named "shuttle_<name>" on a hugetlbfs mount, because that is the only way to
// get guaranteed reserved huge pages for a *named, cross-process* mapping.
// Openers do not know which one a channel used, so seg_open/seg_unlink probe:
// shm first, then every hugetlbfs mount. If the same channel name somehow
// exists in BOTH namespaces (two creators, different backings), shm wins on
// open and on unlink — the hugetlb file is then only reachable by unlinking
// twice. Nothing in the library creates that situation; it needs two creators
// racing on one name, which is already a contract breach (create is O_EXCL).
// ---------------------------------------------------------------------

// How a segment's pages are obtained. Additive by design: the hugetlb backings
// slotted in beside kShm without changing a single signature below.
enum class SegBacking : uint32_t {
    // POSIX shm object; default page size (THP advice is a separate flag).
    kShm = 0,
    kHugeTLB2M = 1,  // file on a 2 MB-pagesize hugetlbfs mount (Linux only)
    kHugeTLB1G = 2,  // file on a 1 GB-pagesize hugetlbfs mount (Linux only)
};

// Handle to a live segment. POSIX: a file descriptor. A Windows backend maps
// this onto its file-mapping HANDLE, which is why callers never touch it with
// anything but the seg_* calls.
using SegHandle = int;
inline constexpr SegHandle kSegInvalid = -1;

// Buffer sizes for the hugetlbfs path discovery below. A hugetlbfs mount point
// is a plain path; eight distinct mounts is already far past anything real
// (one per page size, plus a few per-cgroup pools).
inline constexpr size_t kSegPathMax = 512;
inline constexpr size_t kMaxHugeMounts = 8;

inline void seg_close(SegHandle h) noexcept { (void)::close(h); }

// Bytes per page for a hugetlb backing; 0 for kShm (no fixed granularity).
inline size_t seg_huge_page_size(SegBacking backing) noexcept {
    switch (backing) {
        case SegBacking::kHugeTLB2M:
            return 2ull * 1024 * 1024;
        case SegBacking::kHugeTLB1G:
            return 1024ull * 1024 * 1024;
        default:
            return 0;
    }
}

#if defined(SHUTTLE_PLATFORM_LINUX)
namespace detail {

// The system's DEFAULT huge page size, from /proc/meminfo's "Hugepagesize:"
// line (kB). Needed because a hugetlbfs mount with no pagesize= option uses
// the default size, and we must know which one that is before deciding
// whether the mount can serve a 2 MB or a 1 GB request. 0 = unknown.
inline size_t huge_default_page_size() noexcept {
    std::FILE* f = std::fopen("/proc/meminfo", "re");
    if (f == nullptr) return 0;
    char line[256];
    size_t out = 0;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        unsigned long kb = 0;
        if (std::sscanf(line, "Hugepagesize: %lu kB", &kb) == 1) {
            out = static_cast<size_t>(kb) * 1024;
            break;
        }
    }
    std::fclose(f);
    return out;
}

// Page size named by a mount's "pagesize=" option, e.g. pagesize=2M,
// pagesize=1024M, pagesize=2097152. 0 = option absent (mount uses the default).
inline size_t huge_opt_page_size(const char* opts) noexcept {
    const char* p = std::strstr(opts, "pagesize=");
    // Must be at the start of the option list or right after a comma, so
    // "nopagesize=..." (hypothetical) cannot match.
    while (p != nullptr && p != opts && p[-1] != ',')
        p = std::strstr(p + 1, "pagesize=");
    if (p == nullptr) return 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p + 9, &end, 10);
    if (end == p + 9) return 0;
    size_t mult = 1;
    switch (*end) {
        case 'k':
        case 'K':
            mult = 1024;
            break;
        case 'm':
        case 'M':
            mult = 1024 * 1024;
            break;
        case 'g':
        case 'G':
            mult = 1024 * 1024 * 1024;
            break;
        default:
            break;
    }
    return static_cast<size_t>(v) * mult;
}

// Collect hugetlbfs mount points from /proc/mounts whose page size equals
// `want` (0 = any page size). Returns how many were written to `out`.
//
// Mount points containing whitespace appear in /proc/mounts octal-escaped
// (\040); rather than decode, such a mount is skipped — a shared-memory pool
// under a path with a space in it is not worth the parser.
inline size_t huge_mounts(size_t want, char out[][kSegPathMax],
                          size_t max_out) noexcept {
    std::FILE* f = std::fopen("/proc/mounts", "re");
    if (f == nullptr) return 0;
    size_t n = 0;
    size_t dflt = 0;
    bool dflt_read = false;
    char line[1024];
    while (n < max_out && std::fgets(line, sizeof line, f) != nullptr) {
        char dev[256], dir[kSegPathMax], type[64], opts[kSegPathMax];
        static_assert(kSegPathMax == 512, "scanf widths below assume 512");
        if (std::sscanf(line, "%255s %511s %63s %511s", dev, dir, type, opts) !=
            4)
            continue;
        if (std::strcmp(type, "hugetlbfs") != 0) continue;
        if (std::strchr(dir, '\\') != nullptr) continue;  // escaped path
        size_t ps = huge_opt_page_size(opts);
        if (ps == 0) {
            if (!dflt_read) {
                dflt = huge_default_page_size();
                dflt_read = true;
            }
            ps = dflt;
        }
        if (want != 0 && ps != want) continue;
        std::snprintf(out[n], kSegPathMax, "%s", dir);
        ++n;
    }
    std::fclose(f);
    return n;
}

// "/my-chan" on mount "/dev/hugepages" -> "/dev/hugepages/shuttle_my-chan".
// A channel name with an embedded '/' has no filename form and is rejected
// (EINVAL at the call site) rather than silently flattened.
inline bool huge_seg_path(const char* mount, const char* name, char* out,
                          size_t out_len) noexcept {
    if (name == nullptr || name[0] != '/' ||
        std::strchr(name + 1, '/') != nullptr)
        return false;
    const int n = std::snprintf(out, out_len, "%s/shuttle_%s", mount, name + 1);
    return n > 0 && static_cast<size_t>(n) < out_len;
}

}  // namespace detail
#endif  // SHUTTLE_PLATFORM_LINUX

// Length the mapping must actually cover for `backing`. hugetlbfs refuses a
// size that is not a multiple of its page size, so a hugetlb segment is rounded
// UP; kShm is exact. The header's data_capacity keeps the caller's value — the
// rounding is slack past the end of the data region, and every coverage check
// in the codebase is `>=`, never `==`, precisely so a larger object is legal.
inline size_t seg_map_len(size_t len, SegBacking backing) noexcept {
    const size_t ps = seg_huge_page_size(backing);
    if (ps == 0) return len;
    return (len + ps - 1) / ps * ps;
}

// Destroy the name (the mapping, if any, outlives it — FR-5). Returns 0, or
// the errno; ENOENT means there was no such segment. Probes both namespaces
// (see the note above): shm first, then hugetlbfs mounts.
inline int seg_unlink(const char* name) noexcept {
    if (shm_unlink(name) == 0) return 0;
    const int shm_err = errno;
#if defined(SHUTTLE_PLATFORM_LINUX)
    if (shm_err == ENOENT) {
        char mounts[kMaxHugeMounts][kSegPathMax];
        const size_t nm = detail::huge_mounts(0, mounts, kMaxHugeMounts);
        char path[kSegPathMax];
        for (size_t i = 0; i < nm; ++i) {
            if (!detail::huge_seg_path(mounts[i], name, path, sizeof path))
                continue;
            if (::unlink(path) == 0) return 0;
        }
    }
#endif
    return shm_err;
}

// Create `name` exclusively, owner-only (NFR-S1), and fix its size at `len`.
// Returns kSegInvalid on failure with err_out set; a name collision reports
// EEXIST. Sizing is ONE-SHOT: macOS forbids re-truncating an shm object, so
// this ftruncate is the only one the object will ever see, and any backing
// added later must likewise settle its size right here. A failure after the
// object exists leaves nothing behind — it is closed and unlinked.
//
// hugetlb backings: the object is a file on a hugetlbfs mount of the matching
// page size. NOTE that creating and sizing it reserves NOTHING — hugetlbfs
// accounts pages at mmap() time, so a box with no free huge pages fails later,
// in seg_map, with ENOMEM. That is why the caller must treat a hugetlb mmap
// failure as "no huge pages", not as a generic syscall error.
inline SegHandle seg_create(const char* name, size_t len, SegBacking backing,
                            int& err_out) noexcept {
    if (backing != SegBacking::kShm) {
#if defined(SHUTTLE_PLATFORM_LINUX)
        const size_t ps = seg_huge_page_size(backing);
        char mounts[kMaxHugeMounts][kSegPathMax];
        // No mount of the right page size = the request cannot be honored.
        // Deliberately no fallback to a different page size or to kShm.
        if (detail::huge_mounts(ps, mounts, kMaxHugeMounts) == 0) {
            err_out = ENODEV;
            return kSegInvalid;
        }
        char path[kSegPathMax];
        if (!detail::huge_seg_path(mounts[0], name, path, sizeof path)) {
            err_out = EINVAL;
            return kSegInvalid;
        }
        const int fd = ::open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            err_out = errno;
            return kSegInvalid;
        }
        if (::ftruncate(fd, static_cast<off_t>(seg_map_len(len, backing))) !=
            0) {
            err_out = errno;
            seg_close(fd);
            (void)::unlink(path);
            return kSegInvalid;
        }
        err_out = 0;
        return fd;
#else
        // macOS has no hugetlbfs. Failing here is the whole point: the caller
        // maps this to kErrNoHugePages, and there is deliberately no silent
        // fallback to normal pages (that is what the advisory THP flag is for).
        (void)name;
        (void)len;
        err_out = ENOTSUP;
        return kSegInvalid;
#endif
    }
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        err_out = errno;
        return kSegInvalid;
    }
    if (ftruncate(fd, static_cast<off_t>(len)) != 0) {
        err_out = errno;
        seg_close(fd);
        (void)seg_unlink(name);
        return kSegInvalid;
    }
    err_out = 0;
    return fd;
}

// Attach to an existing segment read/write. Returns kSegInvalid on failure
// with err_out set; ENOENT means no such segment.
//
// The opener is NOT told which backing the creator used, and does not need to
// be: it probes the shm namespace, then the hugetlbfs mounts, and whichever
// fd it gets back carries its page size in the inode. Mapping needs no
// MAP_HUGETLB — that flag exists for ANONYMOUS mappings; for a file on
// hugetlbfs the filesystem dictates the page size, so a plain MAP_SHARED mmap
// of this fd is already huge-page backed.
inline SegHandle seg_open(const char* name, int& err_out) noexcept {
    const int fd = shm_open(name, O_RDWR, 0);
    if (fd >= 0) {
        err_out = 0;
        return fd;
    }
    const int shm_err = errno;
#if defined(SHUTTLE_PLATFORM_LINUX)
    if (shm_err == ENOENT) {
        char mounts[kMaxHugeMounts][kSegPathMax];
        const size_t nm = detail::huge_mounts(0, mounts, kMaxHugeMounts);
        char path[kSegPathMax];
        for (size_t i = 0; i < nm; ++i) {
            if (!detail::huge_seg_path(mounts[i], name, path, sizeof path))
                continue;
            const int hfd = ::open(path, O_RDWR);
            if (hfd >= 0) {
                err_out = 0;
                return hfd;
            }
        }
    }
#endif
    err_out = shm_err;
    return kSegInvalid;
}

// Byte size of the object behind `h`, or -1 if it cannot be determined — the
// opener sizes its mapping from this (the creator already knows `len`). Note
// macOS rounds an shm object up to a page, and a hugetlbfs file is rounded up
// to its huge page size, so this can exceed the geometry the header claims;
// callers validate with >=, never ==.
inline int64_t seg_size(SegHandle h) noexcept {
    struct stat st;
    if (fstat(h, &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

// Map `len` bytes of `h` shared read/write at an address of the kernel's
// choosing. Returns nullptr on failure — MAP_FAILED is a POSIX detail and
// does not escape the seam.
inline void* seg_map(SegHandle h, size_t len) noexcept {
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, h, 0);
    return p == MAP_FAILED ? nullptr : p;
}

inline void seg_unmap(void* base, size_t len) noexcept {
    (void)munmap(base, len);
}

// Advise the kernel that a mapping is a good candidate for transparent huge
// pages (opt-in, FR create-flag kFlagHugePages). Purely advisory: on Linux it
// takes effect only where the THP shmem policy permits — e.g.
// /sys/kernel/mm/transparent_hugepage/shmem_enabled set to "advise" or
// "always" — and a kernel that disallows it returns a harmless EINVAL we
// deliberately drop. Never a correctness dependency; a no-op on macOS, which
// has no THP knob. Both creator and opener call this on their own mapping.
inline void advise_huge_pages(void* base, size_t len) noexcept {
#if defined(SHUTTLE_PLATFORM_LINUX)
    (void)madvise(base, len, MADV_HUGEPAGE);  // result ignored by design
#else
    (void)base;
    (void)len;
#endif
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

// Filesystem view of a hugetlb-backed segment, the counterpart of the check
// above for the other namespace (NFR-R2 leak checks). Returns 1 if a file for
// `name` exists on some hugetlbfs mount, 0 if not, -1 where hugetlbfs cannot
// exist at all (macOS). 0 and -1 are different verdicts: 0 means "looked, not
// there", -1 means "nothing to look at".
inline int hugetlb_object_exists_fs(const char* name) noexcept {
#if defined(SHUTTLE_PLATFORM_LINUX)
    char mounts[kMaxHugeMounts][kSegPathMax];
    const size_t nm = detail::huge_mounts(0, mounts, kMaxHugeMounts);
    char path[kSegPathMax];
    for (size_t i = 0; i < nm; ++i) {
        if (!detail::huge_seg_path(mounts[i], name, path, sizeof path))
            continue;
        struct stat st;
        if (stat(path, &st) == 0) return 1;
    }
    return 0;
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
