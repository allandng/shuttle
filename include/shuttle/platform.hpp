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
#elif defined(_WIN32)
// EXPERIMENTAL third backend (WP8): named file mappings + WaitOnAddress.
// Compile- and smoke-tested in a windows-latest CI job ONLY; NOT at parity
// with the POSIX platforms — there is no robust-mutex crash recovery and no
// multi-process gate suite. Heartbeat liveness is the crash story, exactly
// as on macOS (WaitOnAddress, like os_sync_wait_on_address, holds nothing a
// dying process could orphan). Every Win32 divergence lives in THIS file.
#define SHUTTLE_PLATFORM_WINDOWS 1
#define SHUTTLE_PLATFORM_NAME "windows"
#else
#error "Shuttle supports Linux, macOS, and Windows only"
#endif

#if defined(SHUTTLE_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// WaitOnAddress / WakeByAddressAll are declared only when the target is Win8+.
// Pin it so a bare cmake+cl build with an unset default cannot drop them.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10
#endif
#include <intrin.h>  // _mm_pause

#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// ---------------------------------------------------------------------
// Park-area type: the header's park/wake block, hidden behind the seam so
// header.hpp carries NO platform type and no #ifdef of its own. On POSIX it is
// the three pthread primitives in the EXACT order the frozen v1 layout froze
// them, so every v1 offset is byte-for-byte unchanged; on Windows it is an
// inert placeholder, because WaitOnAddress waits directly on the header's
// cursor words and needs neither a lock nor a condition variable. The three
// member NAMES (lock / not_empty / not_full) are kept on both platforms so the
// shared spsc.hpp / shuttle.cpp code compiles unchanged everywhere — on Windows
// the seam simply never dereferences them.
// ---------------------------------------------------------------------
#if defined(SHUTTLE_PLATFORM_WINDOWS)
struct ParkMutex {
    std::atomic<uint32_t> reserved{0};
};
struct ParkCond {
    std::atomic<uint32_t> reserved{0};
};
#else
using ParkMutex = pthread_mutex_t;
using ParkCond = pthread_cond_t;
#endif

struct ParkArea {
    ParkMutex lock;
    ParkCond not_empty;
    ParkCond not_full;
};

// Frozen POSIX v1 data_offset, hard-coded so a header refactor that moves it
// trips a COMPILE-TIME assert in header.hpp (see the tripwire there). It is
// identical (1280) on both POSIX ABIs: glibc/libstdc++ (mutex 40 + 2*cond 48
// = 136, park block ends at 1160) and libc++/macOS (64 + 2*48 = 160, ends at
// 1184) both round UP to the same cache-line multiple, 1280. 0 means "no
// frozen expectation on this platform" — a Windows segment is single-OS and
// carries its own, independently derived (and smaller) data_offset.
#if defined(SHUTTLE_PLATFORM_WINDOWS)
inline constexpr uint64_t kExpectedDataOffsetV1 = 0;
#else
inline constexpr uint64_t kExpectedDataOffsetV1 = 1280;
#endif

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
//
// A THIRD NAMESPACE (kFile, v1.4) is deliberately NOT part of that probe: its
// identifier is an absolute PATH the caller chose, not a name this library
// decorates, and it is reached only through the seg_*_file trio below. See the
// block above those functions for why they are separate symbols rather than a
// fourth case inside seg_open.
// ---------------------------------------------------------------------

// How a segment's pages are obtained. Additive by design: the hugetlb backings
// slotted in beside kShm without changing a single signature below, and kFile
// slotted in beside them the same way.
enum class SegBacking : uint32_t {
    // POSIX shm object; default page size (THP advice is a separate flag).
    kShm = 0,
    kHugeTLB2M = 1,  // file on a 2 MB-pagesize hugetlbfs mount (Linux only)
    kHugeTLB1G = 2,  // file on a 1 GB-pagesize hugetlbfs mount (Linux only)
    // Ordinary file at a caller-supplied absolute path (v1.4). Normal pages, no
    // fixed granularity — so it behaves exactly like kShm everywhere the
    // BACKING is what matters (seg_map_len below returns `len` unchanged for
    // both). What differs is only how the object is obtained and destroyed,
    // which is the seg_*_file trio and nothing else.
    kFile = 3,
};

// Handle to a live segment. POSIX: a file descriptor. Windows: the Win32
// file-mapping HANDLE — which is why callers never touch it with anything but
// the seg_* calls. CreateFileMappingW / OpenFileMappingW return NULL (not
// INVALID_HANDLE_VALUE) on failure, so the invalid sentinel is nullptr there.
#if defined(SHUTTLE_PLATFORM_WINDOWS)
using SegHandle = HANDLE;
inline constexpr SegHandle kSegInvalid = nullptr;
#else
using SegHandle = int;
inline constexpr SegHandle kSegInvalid = -1;
#endif

// Buffer sizes for the hugetlbfs path discovery below. A hugetlbfs mount point
// is a plain path; eight distinct mounts is already far past anything real
// (one per page size, plus a few per-cgroup pools).
inline constexpr size_t kSegPathMax = 512;
inline constexpr size_t kMaxHugeMounts = 8;

inline void seg_close(SegHandle h) noexcept {
    if (h == kSegInvalid) return;  // tolerate the "already dropped" sentinel
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)::CloseHandle(h);
#else
    (void)::close(h);
#endif
}

// After a successful map, the segment handle's fate diverges. POSIX drops it —
// the mapping alone keeps the object alive — and returns kSegInvalid for the
// Channel to store (a no-op to close later). Windows RETAINS it: a named
// pagefile section is refcounted and vanishes with its LAST handle, so the
// creator must hold the handle open for the channel's whole life or peers can
// no longer OpenFileMappingW it by name. The returned handle is stored in the
// Channel and released at close(). This is the one real lifetime divergence.
inline SegHandle seg_keep_after_map(SegHandle h) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    return h;  // retain
#else
    seg_close(h);
    return kSegInvalid;
#endif
}

#if defined(SHUTTLE_PLATFORM_WINDOWS)
namespace detail {
// Build the Win32 object name "Local\\shuttle_<name>" (wide) for a channel
// name. The channel name is ASCII and includes its leading '/', which is legal
// in a Win32 object name — only '\\' is special there, as the namespace
// separator. The Local\ prefix scopes the section to the current session.
inline bool win_seg_name(const char* name, wchar_t* out,
                         size_t out_len) noexcept {
    static const wchar_t kPrefix[] = L"Local\\shuttle_";
    size_t i = 0;
    for (; kPrefix[i] != L'\0'; ++i) {
        if (i + 1 >= out_len) return false;
        out[i] = kPrefix[i];
    }
    for (size_t j = 0; name[j] != '\0'; ++j, ++i) {
        if (i + 1 >= out_len) return false;
        out[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[j]));
    }
    out[i] = L'\0';
    return true;
}
}  // namespace detail
#endif

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
// UP; kShm and kFile are exact (neither has a granularity of its own). The
// header's data_capacity keeps the caller's value — the rounding is slack past
// the end of the data region, and every coverage check in the codebase is `>=`,
// never `==`, precisely so a larger object is legal.
inline size_t seg_map_len(size_t len, SegBacking backing) noexcept {
    const size_t ps = seg_huge_page_size(backing);
    if (ps == 0) return len;
    return (len + ps - 1) / ps * ps;
}

// Destroy the name (the mapping, if any, outlives it — FR-5). Returns 0, or
// the errno; ENOENT means there was no such segment. Probes both namespaces
// (see the note above): shm first, then hugetlbfs mounts.
inline int seg_unlink(const char* name) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // Windows named sections are refcounted and destroyed with their LAST
    // handle; there is no shm_unlink equivalent, so this cannot actively remove
    // the name. It reports existence only, mapped into the seam's errno space:
    // 0 if a section by this name is currently open (it will vanish when the
    // creator's retained handle closes at Channel close()), ENOENT if none
    // exists. DOCUMENTED PARITY GAP: unlike POSIX, closing a channel — not
    // unlink — is what reclaims the name on Windows.
    wchar_t wname[kSegPathMax];
    if (!detail::win_seg_name(name, wname, kSegPathMax)) return ENOENT;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, wname);
    if (h == nullptr) return ENOENT;
    CloseHandle(h);
    return 0;
#else
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
#endif  // SHUTTLE_PLATFORM_WINDOWS
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
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // A pagefile-backed named section (INVALID_HANDLE_VALUE source), sized here
    // once and zero-initialized by the OS — matching the shm_open + ftruncate
    // contract, cursors/init_state included. O_EXCL semantics come from
    // GetLastError()==ERROR_ALREADY_EXISTS: CreateFileMappingW hands back a
    // (second) handle to the existing section, which we must drop and report as
    // a collision.
    wchar_t wname[kSegPathMax];
    if (!detail::win_seg_name(name, wname, kSegPathMax)) {
        err_out = EINVAL;
        return kSegInvalid;
    }
    const uint64_t sz = static_cast<uint64_t>(len);
    const DWORD hi = static_cast<DWORD>((sz >> 32) & 0xFFFFFFFFull);
    const DWORD lo = static_cast<DWORD>(sz & 0xFFFFFFFFull);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  hi, lo, wname);
    if (h == nullptr) {
        err_out = (GetLastError() == ERROR_ACCESS_DENIED) ? EACCES : EINVAL;
        return kSegInvalid;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        err_out = EEXIST;
        return kSegInvalid;
    }
    err_out = 0;
    return h;
#else
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
#endif
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
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    wchar_t wname[kSegPathMax];
    if (!detail::win_seg_name(name, wname, kSegPathMax)) {
        err_out = EINVAL;
        return kSegInvalid;
    }
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname);
    if (h == nullptr) {
        const DWORD e = GetLastError();
        err_out = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                      ? ENOENT
                      : EACCES;
        return kSegInvalid;
    }
    err_out = 0;
    return h;
#else
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
#endif  // SHUTTLE_PLATFORM_WINDOWS
}

// ---------------------------------------------------------------------
// FILE-BACKED SEGMENTS (SegBacking::kFile, v1.4).
//
// The segment object is an ordinary file at an absolute path the caller chose,
// so a channel's capacity is bounded by the FILESYSTEM rather than by RAM (or
// by /dev/shm's tmpfs limit) and residency is the page cache's problem. Only
// the three functions here differ from the shm path; seg_map / seg_unmap /
// seg_size / seg_close / seg_keep_after_map are backing-agnostic and are used
// unchanged, because an fd is an fd.
//
// WHY SEPARATE SYMBOLS instead of a `SegBacking` parameter on seg_open, or a
// fourth case in its probe: the identifier changes TYPE. seg_open's probe walks
// namespaces looking for a NAME the library decorates ("/x" -> /dev/shm/x, ->
// <mount>/shuttle_x); a path is already the object's location and needs no
// search. Deciding "is this string a name or a path?" by inspecting it is not
// possible — "/tmp/cache" is a syntactically legal shm name — so someone has to
// say, and the caller is the only one who knows. Saying it by CALLING A
// DIFFERENT FUNCTION makes that a compile-time fact instead of a runtime guess,
// and it leaves the name-based paths above byte-for-byte untouched: no existing
// signature moves, no existing branch is re-entered, and a name-typed caller
// cannot reach the file namespace by accident (which is exactly the
// dual-namespace ambiguity the hugetlb work had to document its way out of).
//
// WINDOWS: stubbed to ENOTSUP this pass — the code compiles, the capability is
// absent, and a documented parity gap says so (docs/API.md, "File-backed
// channels"). The Win32 form would be CreateFileW + CreateFileMappingW on that
// handle rather than on INVALID_HANDLE_VALUE; it is not attempted here because
// the experimental Windows backend has no crash-recovery story to test it
// against.
// ---------------------------------------------------------------------

// A usable file-backed identifier: an ABSOLUTE path. Relative paths are refused
// rather than resolved, because a channel's identity must not depend on which
// directory each peer happened to be started in. There is no length rule of our
// own — unlike shm names, the filesystem's own limit is the only one, and it
// reports itself as ENAMETOOLONG.
inline bool seg_path_ok(const char* path) noexcept {
    return path != nullptr && path[0] == '/';
}

// Create the file at `path` exclusively, owner-only (NFR-S1), and fix its size
// at `len`. Same contract as seg_create: kSegInvalid + err_out on failure,
// EEXIST for a collision, sizing is ONE-SHOT. The ftruncate leaves a sparse
// file — blocks are allocated as the pages are dirtied, which is what lets a
// capacity larger than free RAM (or even larger than the free disk, until it is
// actually used) be created at all.
inline SegHandle seg_create_file(const char* path, size_t len,
                                 int& err_out) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)path;
    (void)len;
    err_out = ENOTSUP;
    return kSegInvalid;
#else
    const int fd = ::open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        err_out = errno;
        return kSegInvalid;
    }
    if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
        err_out = errno;
        seg_close(fd);
        (void)::unlink(path);  // leave nothing behind, exactly as seg_create
        return kSegInvalid;
    }
    err_out = 0;
    return fd;
#endif
}

// Attach to an existing file-backed segment read/write. ENOENT means no such
// file — there is nothing to probe, so the verdict is immediate.
inline SegHandle seg_open_file(const char* path, int& err_out) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)path;
    err_out = ENOTSUP;
    return kSegInvalid;
#else
    const int fd = ::open(path, O_RDWR);
    if (fd < 0) {
        err_out = errno;
        return kSegInvalid;
    }
    err_out = 0;
    return fd;
#endif
}

// Destroy the file (the mapping, if any, outlives it — FR-5, ordinary POSIX
// unlink semantics). Returns 0 or the errno; ENOENT means there was no such
// file. Never touches the shm or hugetlbfs namespaces: a path names exactly one
// object, so unlike seg_unlink there is nothing to probe and no precedence rule
// to state.
inline int seg_unlink_file(const char* path) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)path;
    return ENOTSUP;
#else
    return ::unlink(path) == 0 ? 0 : errno;
#endif
}

// Byte size of the object behind `h`, or -1 if it cannot be determined — the
// opener sizes its mapping from this (the creator already knows `len`). Note
// macOS rounds an shm object up to a page, and a hugetlbfs file is rounded up
// to its huge page size, so this can exceed the geometry the header claims;
// callers validate with >=, never ==.
inline int64_t seg_size(SegHandle h) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // A section object has no fstat: map the whole view (len 0 = entire
    // section), read the region size, unmap. The section is rounded up to a
    // page, so this can exceed the header geometry — callers validate with >=.
    void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (v == nullptr) return -1;
    MEMORY_BASIC_INFORMATION mbi;
    const SIZE_T q = VirtualQuery(v, &mbi, sizeof mbi);
    (void)UnmapViewOfFile(v);
    if (q == 0) return -1;
    return static_cast<int64_t>(mbi.RegionSize);
#else
    struct stat st;
    if (fstat(h, &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
#endif
}

// Map `len` bytes of `h` shared read/write at an address of the kernel's
// choosing. Returns nullptr on failure — MAP_FAILED is a POSIX detail and
// does not escape the seam.
inline void* seg_map(SegHandle h, size_t len) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // MapViewOfFile returns NULL on failure, already the seam's sentinel.
    return MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0,
                         static_cast<SIZE_T>(len));
#else
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, h, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

inline void seg_unmap(void* base, size_t len) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)len;
    (void)UnmapViewOfFile(base);
#else
    (void)munmap(base, len);
#endif
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

// Advise the kernel that a mapped range will be read SOON, so it can start the
// I/O now instead of at the first touching fault (v1.4, the consumer-side
// prefetch hook in spsc.hpp). The point of it is the file-backed backing, where
// an unread page can be a disk read the consumer would otherwise pay for
// synchronously, in the middle of a borrow; on an shm segment there is nothing
// to bring in and the call is never made (Consumer gates on kFlagFileBacked).
//
// PURELY ADVISORY, exactly like advise_huge_pages above, and the return value
// is deliberately ignored for the same reason: WILLNEED is a hint the kernel
// is free to refuse (EINVAL on a range it dislikes, ENOMEM under pressure),
// and no caller may make a correctness decision from it. The kernel may also
// complete the readahead asynchronously or not at all — the consumer reads the
// bytes either way.
//
// Linux uses posix_madvise(POSIX_MADV_WILLNEED), the POSIX spelling, whose
// glibc implementation is madvise(MADV_WILLNEED) for this hint; macOS has no
// posix_madvise for WILLNEED semantics and takes madvise(MADV_WILLNEED)
// directly. Windows has no equivalent advice for a mapped view (PrefetchVirtual
// Memory exists but is a different contract and the file backing is stubbed out
// there anyway), so it is a no-op — as it is on any future platform.
//
// `addr` MUST be page-aligned: madvise operates on whole pages and rejects an
// unaligned address. The caller floors it (spsc.hpp's advise_run), because the
// data region is NOT page-aligned in the default framing.
inline void advise_willneed(void* addr, size_t len) noexcept {
#if defined(SHUTTLE_PLATFORM_LINUX)
    (void)posix_madvise(addr, len, POSIX_MADV_WILLNEED);  // ignored by design
#elif defined(SHUTTLE_PLATFORM_MACOS)
    (void)madvise(addr, len, MADV_WILLNEED);  // ignored by design
#else
    (void)addr;
    (void)len;
#endif
}

// The system's ordinary page size, and the alignment unit for
// kFlagAlignedSpans (SHUTTLE_CREATE_ALIGNED_SPANS). Same-host IPC makes this a
// HOST CONSTANT: every process that maps a given segment runs on the same
// kernel and therefore computes the same value, which is what lets the frame
// geometry it selects be agreed on without storing it in the segment.
//
// Deliberately the SYSTEM page size even on a hugetlbfs-backed segment. The
// point of the flag is that a borrowed payload can be handed to an API that
// wants page-aligned host memory (MTLBuffer newBufferWithBytesNoCopy,
// cudaHostRegister); those want the ordinary page granularity, and rounding
// every frame to 2 MB or 1 GB instead would turn a small message into a
// huge-page-sized hole.
//
// GUARANTEED A POWER OF TWO. The mask arithmetic in bipbuffer.hpp
// (round_up_page / floor_page) depends on that, so a platform that reports
// anything else — no supported target does; POSIX effectively requires it —
// falls back to 4096 rather than silently corrupting the geometry.
inline size_t page_size() noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    const size_t ps = static_cast<size_t>(si.dwPageSize);
    // dwPageSize, not dwAllocationGranularity: the granularity (64 KiB) is what
    // a view's BASE is aligned to, and it is a multiple of the page size, so a
    // page-aligned offset from a view base is still page-aligned.
#else
    const long raw = ::sysconf(_SC_PAGESIZE);
    const size_t ps = raw > 0 ? static_cast<size_t>(raw) : 0;
#endif
    return (ps != 0 && (ps & (ps - 1)) == 0) ? ps : 4096;
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
inline int mutex_init_pshared(ParkMutex* m) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // WaitOnAddress needs no lock: the ParkArea is inert on Windows. Nothing to
    // initialize — succeed so create() proceeds unchanged.
    (void)m;
    return 0;
#else
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
#endif
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
inline int park_mutex_recover_if_needed(ParkMutex* m, int rc) noexcept {
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
inline int cond_init_pshared_monotonic(ParkCond* c) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)c;  // inert on Windows (WaitOnAddress); nothing to initialize
    return 0;
#else
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
#endif
}

// Relative timed wait on a pshared condvar; mutex must be held.
// Returns 0 on wake (incl. spurious), ETIMEDOUT on timeout, else errno.
inline int cond_timedwait_rel(ParkCond* c, ParkMutex* m,
                              uint64_t rel_ns) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // Not on the Windows park path (park_wait_cursor uses WaitOnAddress
    // directly), but defined so the signature exists. Treat as an immediate
    // timeout — a caller would re-evaluate its predicate, exactly as on wake.
    (void)c;
    (void)m;
    (void)rel_ns;
    return ETIMEDOUT;
#elif defined(SHUTTLE_PLATFORM_LINUX)
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
    return park_mutex_recover_if_needed(m, pthread_cond_timedwait(c, m, &ts));
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
inline int park_mutex_lock(ParkMutex* m) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)m;  // inert: WaitOnAddress holds no lock
    return 0;
#elif defined(SHUTTLE_PLATFORM_MACOS)
    for (;;) {
        const int rc = pthread_mutex_trylock(m);
        if (rc != EBUSY) return rc;
        usleep(100);  // G5.4: caller-level heartbeat staleness bounds this
    }
#else
    return park_mutex_recover_if_needed(m, pthread_mutex_lock(m));
#endif
}

inline int park_mutex_unlock(ParkMutex* m) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)m;
    return 0;
#else
    return pthread_mutex_unlock(m);
#endif
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
                            ParkMutex* mu, ParkCond* cv,
                            uint64_t timeout_ns) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    // WaitOnAddress sleeps while *cursor == seen, up to timeout (milliseconds).
    // The compare-with-sleep is atomic, so a publish that changes the cursor
    // between the caller's recheck and here cannot be lost — the same guarantee
    // macOS gets from os_sync_wait_on_address, and why Windows (like macOS)
    // needs no lock or condvar. A wake or timeout both just return; the caller
    // re-evaluates its predicate and the peer heartbeat.
    (void)mu;
    (void)cv;
    uint64_t compare = seen;
    DWORD ms = static_cast<DWORD>(timeout_ns / 1000000ull);
    if (ms == 0) ms = 1;  // never a busy 0-ms poll
    (void)WaitOnAddress(static_cast<volatile void*>(cursor), &compare,
                        sizeof(uint64_t), ms);
    return 0;
#elif defined(SHUTTLE_PLATFORM_MACOS)
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

inline void park_wake_cursor(std::atomic<uint64_t>* cursor, ParkMutex* mu,
                             ParkCond* cv) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)mu;
    (void)cv;
    WakeByAddressAll(static_cast<void*>(cursor));
#elif defined(SHUTTLE_PLATFORM_MACOS)
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
#elif defined(_MSC_VER)
    _mm_pause();  // MSVC intrinsic (x86/x64); the Windows spin hint
#else
    // no hint available; plain spin
#endif
}

inline void yield_thread() noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    (void)SwitchToThread();
#else
    sched_yield();
#endif
}

// Short blocking sleep in microseconds; kept in the seam so src/ carries no
// platform sleep primitive. POSIX: usleep. Windows: Sleep rounds UP to whole
// milliseconds (a sub-ms request still yields at least one scheduler tick).
inline void sleep_us(unsigned us) noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    Sleep(static_cast<DWORD>((us + 999u) / 1000u));
#else
    usleep(us);
#endif
}

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

// Monotonic clock in nanoseconds. Kept here so test/driver code shares one
// definition. POSIX uses CLOCK_MONOTONIC; Windows uses QueryPerformanceCounter
// (steady, high-resolution), scaled to ns without overflow by splitting the
// counter into whole seconds and a sub-second remainder.
inline uint64_t monotonic_ns() noexcept {
#if defined(SHUTTLE_PLATFORM_WINDOWS)
    LARGE_INTEGER freq;
    LARGE_INTEGER ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    const uint64_t f = static_cast<uint64_t>(freq.QuadPart);
    const uint64_t c = static_cast<uint64_t>(ctr.QuadPart);
    if (f == 0) return 0;
    return (c / f) * 1000000000ull + (c % f) * 1000000000ull / f;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}

}  // namespace shuttle
