// FILE-BACKED CHANNELS (shuttle_create_file / shuttle_open_file /
// shuttle_unlink_file, flag 0x20, v1.4): the segment object is an ordinary file
// at an absolute path instead of a POSIX shm object, so a channel's capacity is
// bounded by the FILESYSTEM rather than by RAM and the OS page cache decides
// what is resident.
//
// The backing is the only thing that changes, which is exactly why each case
// below exists — a transport whose bytes moved house must be re-proven, not
// assumed:
//
//   a. ROUNDTRIP. create/open/transfer over a file in a temp dir, byte-exact on
//      both the copy path and the borrow path, through the C ABI (the new
//      symbols are ABI first). The file exists on disk while the channel does,
//      and unlink_file removes it.
//   b. CAPACITY OVER A SMALL WINDOW. A channel far larger than the window it is
//      driven through, streaming 2x its own capacity: the ring laps the file
//      twice while at most a handful of messages are ever in flight. This is
//      the TurboFieldfare shape — a large backing store consumed through a
//      small resident window — and what it proves here is that nothing in the
//      transport cares how big the file is.
//   c. SIGKILL MID-TRANSFER. The G5.1 kill point re-run on a file mapping: the
//      survivor's blocked read must abort with kErrPeerDead, never deadlock.
//   d. SIGKILL HOLDING THE PARK MUTEX. The G5.4 kill point re-run on a file
//      mapping, same requirement.
//   e. ROBUST-MUTEX PROOF (Linux). The plan marked "robust pthread mutexes work
//      in a file-backed MAP_SHARED mapping" as INFERRED and required it to be
//      PROVEN, so this case does not settle for "the survivor did not hang": it
//      takes the orphaned lock RAW and asserts the kernel handed back
//      EOWNERDEAD, then repairs it the way the seam does and shows the mutex is
//      fully serviceable. The observed verdict is printed either way — it is
//      the fact docs/API.md's crash story is written from. The companion
//      negative (skip pthread_mutex_consistent -> ENOTRECOVERABLE) is what
//      keeps the positive from being a test that cannot fail.
//   f. ERROR PATHS. Missing path, existing file, hugetlb bits, relative path.
//   g. COMBOS. file+stats (0x28) and file+aligned (0x30) each roundtrip, and
//      the persisted flags word says what the creator asked for.
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"
#include "shuttle/shuttle_c.h"
#include "shuttle/spsc.hpp"

namespace {

constexpr uint64_t kChildTimeoutNs = 120ull * 1000000000ull;
constexpr uint64_t kHoldSentinel = 0xBEEF;

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

// argv[0], stashed so the spawn helpers do not have to thread it through every
// signature. Set once in main, before anything can spawn.
const char* g_self = nullptr;
const char* argv0() { return g_self; }

// --- the temp directory every file-backed segment in this test lives in -----

// mkdtemp under $TMPDIR (or /tmp): the paths must be absolute (the ABI rejects
// relative ones) and must not collide between concurrent ctest runs.
char g_dir[512];

bool make_temp_dir() {
    const char* tmp = std::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0') tmp = "/tmp";
    const int n =
        std::snprintf(g_dir, sizeof g_dir, "%s/shuttle-fb.XXXXXX", tmp);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof g_dir) return false;
    return mkdtemp(g_dir) != nullptr;
}

// <dir>/<tag>.seg — one segment file per case, so a leaked file from a failing
// case cannot make the next one report EXISTS instead of its own verdict.
char* path_for(char* buf, size_t n, const char* tag) {
    std::snprintf(buf, n, "%s/%s.seg", g_dir, tag);
    return buf;
}

bool file_exists(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0;
}

int64_t file_size(const char* path) {
    struct stat st;
    if (::stat(path, &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

// Free bytes on the filesystem holding the temp dir. Used to SKIP the big
// streaming case rather than fail it on a small runner: a CI box without the
// disk for a 256 MB segment is not a bug in this feature.
uint64_t free_bytes() {
    struct statvfs vfs;
    if (::statvfs(g_dir, &vfs) != 0) return 0;
    return static_cast<uint64_t>(vfs.f_bavail) *
           static_cast<uint64_t>(vfs.f_frsize);
}

// Resident set in bytes, or 0 where it cannot be read (this is reported, never
// asserted on — page-cache residency is the kernel's business, and a number
// that varies with reclaim pressure has no business being a gate).
uint64_t resident_bytes() {
    std::FILE* f = std::fopen("/proc/self/statm", "re");
    if (f == nullptr) return 0;
    unsigned long total = 0, resident = 0;
    const int got = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    return static_cast<uint64_t>(resident) *
           static_cast<uint64_t>(shuttle::page_size());
}

unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 1315423911ull) + i * 151ull +
                                      (i >> 8));
}

// --- (a) roundtrip over a file ---------------------------------------------

int case_roundtrip() {
    char path[600];
    path_for(path, sizeof path, "roundtrip");
    int err = 0, fails = 0;
    const size_t kCap = 1u << 20;
    const size_t kMax = 1u << 16;

    shuttle_channel* prod = shuttle_create_file(path, kCap, kMax, 0, &err);
    if (prod == nullptr) return fail("roundtrip: create_file", err);
    // The segment is a real file, sized to hold the geometry it claims.
    if (!file_exists(path)) fails += fail("roundtrip: no file on disk", 0);
    const int64_t sz = file_size(path);
    if (sz < static_cast<int64_t>(kCap))
        fails += fail("roundtrip: file smaller than the capacity",
                      static_cast<long>(sz));

    // A second handle attaches by PATH and nothing else — no name, no flag, no
    // knowledge of how the creator obtained the object.
    shuttle_channel* cons = shuttle_open_file(path, &err);
    if (cons == nullptr) {
        shuttle_close(prod);
        shuttle_unlink_file(path);
        return fails + fail("roundtrip: open_file", err);
    }

    // Copy path.
    std::vector<unsigned char> msg(4096);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = fill_byte(1, i);
    if (shuttle_write(prod, msg.data(), msg.size(), 0) != SHUTTLE_OK) {
        fails += fail("roundtrip: write", 0);
    } else {
        std::vector<unsigned char> out(msg.size());
        const long n = shuttle_read(cons, out.data(), out.size(), 0);
        if (n != static_cast<long>(msg.size()))
            fails += fail("roundtrip: copy read length", n);
        else if (std::memcmp(out.data(), msg.data(), msg.size()) != 0)
            fails += fail("roundtrip: copy read bytes", 0);
    }

    // Borrow path, both ends zero-copy: reserve in the file mapping, commit,
    // borrow in place, compare without a copy.
    const size_t kBorrowLen = 12345;
    void* dst = nullptr;
    if (shuttle_acquire_write(prod, &dst, kBorrowLen, 0) != SHUTTLE_OK) {
        fails += fail("roundtrip: acquire_write", 0);
    } else {
        unsigned char* d = static_cast<unsigned char*>(dst);
        for (size_t i = 0; i < kBorrowLen; ++i) d[i] = fill_byte(2, i);
        if (shuttle_commit_write(prod, kBorrowLen) != SHUTTLE_OK)
            fails += fail("roundtrip: commit_write", 0);
        const void* q = nullptr;
        size_t got = 0;
        if (shuttle_acquire_read(cons, &q, &got, 0) != SHUTTLE_OK ||
            got != kBorrowLen) {
            fails += fail("roundtrip: acquire_read", static_cast<long>(got));
        } else {
            const unsigned char* p = static_cast<const unsigned char*>(q);
            bool bad = false;
            for (size_t i = 0; i < kBorrowLen && !bad; ++i)
                bad = p[i] != fill_byte(2, i);
            if (bad) fails += fail("roundtrip: borrowed bytes", 0);
            shuttle_release_read(cons);
        }
    }

    shuttle_close(cons);
    shuttle_close(prod);
    // unlink removes the file; a second unlink is NOT_FOUND, exactly as for a
    // name (the mapping-outlives-the-name rule is unchanged).
    if (shuttle_unlink_file(path) != SHUTTLE_OK)
        fails += fail("roundtrip: unlink_file", 0);
    if (file_exists(path)) fails += fail("roundtrip: file survived unlink", 0);
    if (shuttle_unlink_file(path) != SHUTTLE_ERR_NOT_FOUND)
        fails += fail("roundtrip: second unlink not NOT_FOUND", 0);
    if (fails == 0)
        std::printf("filebacked: roundtrip ok over %s (copy + borrow, "
                    "byte-exact; unlink removed the file)\n",
                    path);
    return fails == 0 ? 0 : 1;
}

// --- (b) a large channel driven through a small window ---------------------

constexpr uint64_t kBigCap = 256ull << 20;    // channel capacity: 256 MB
constexpr uint64_t kBigMsg = 1ull << 20;      // 1 MB messages
constexpr uint64_t kBigTotal = 512ull << 20;  // stream 2x the capacity
constexpr uint64_t kWindow = 8;               // messages in flight, at most

int case_streaming() {
    char path[600];
    path_for(path, sizeof path, "stream");
    // Skip rather than fail where the disk cannot hold the segment: the file is
    // created sparse, but streaming 2x the capacity dirties every block of it.
    const uint64_t need = kBigCap + (64ull << 20);
    const uint64_t avail = free_bytes();
    if (avail < need) {
        std::printf("filebacked: streaming case SKIPPED — %llu MB free on %s, "
                    "needs %llu MB\n",
                    (unsigned long long)(avail >> 20), g_dir,
                    (unsigned long long)(need >> 20));
        return 0;
    }

    int err = 0, fails = 0;
    shuttle::Channel* ch = shuttle::create_file(
        path, static_cast<size_t>(kBigCap), static_cast<size_t>(kBigMsg), &err);
    if (ch == nullptr) return fail("streaming: create_file", err);
    if (ch->hdr->flags != shuttle::kFlagFileBacked)
        fails += fail("streaming: flags", static_cast<long>(ch->hdr->flags));
    // The file really is the storage: it is at least as large as the geometry.
    const int64_t sz = file_size(path);
    if (sz < static_cast<int64_t>(kBigCap))
        fails += fail("streaming: file smaller than capacity",
                      static_cast<long>(sz >> 20));

    const uint64_t rss_before = resident_bytes();
    {
        shuttle::Producer p(ch);
        shuttle::Consumer c(ch);
        // One pattern buffer reused for every message; only the first 8 bytes
        // (the sequence number) differ, so verification is a memcmp of the tail
        // plus an exact check of the number. Byte-exact without spending the
        // whole test budget on a per-byte loop over half a gigabyte.
        std::vector<unsigned char> pattern(static_cast<size_t>(kBigMsg));
        for (size_t i = 0; i < pattern.size(); ++i)
            pattern[i] = fill_byte(7, i);
        const uint64_t nmsgs = kBigTotal / kBigMsg;
        uint64_t sent = 0, got = 0, wraps = 0, last_write = 0;
        bool bad = false;

        while (got < nmsgs && !bad) {
            // Keep at most kWindow messages in flight: the ring is 256 MB, the
            // working set is 8 MB. That gap is the point of the case.
            while (sent < nmsgs && sent - got < kWindow) {
                void* dst = nullptr;
                const int rc =
                    p.try_acquire_write(&dst, static_cast<size_t>(kBigMsg));
                if (rc == shuttle::kErrWouldBlock) break;
                if (rc != shuttle::kOk) {
                    fails += fail("streaming: acquire_write", rc);
                    bad = true;
                    break;
                }
                std::memcpy(dst, pattern.data(), pattern.size());
                const uint64_t seq = sent;
                std::memcpy(dst, &seq, sizeof seq);
                if (p.commit_write(static_cast<size_t>(kBigMsg)) !=
                    shuttle::kOk) {
                    fails += fail("streaming: commit_write",
                                  static_cast<long>(sent));
                    bad = true;
                    break;
                }
                ++sent;
            }
            if (bad) break;
            const unsigned char* q = nullptr;
            uint64_t len = 0;
            const int rc = c.try_read(&q, &len);
            if (rc == shuttle::kErrWouldBlock) continue;
            if (rc != shuttle::kOk) {
                fails += fail("streaming: read", rc);
                break;
            }
            if (len != kBigMsg) {
                fails += fail("streaming: length", static_cast<long>(len));
                break;
            }
            uint64_t seq = 0;
            std::memcpy(&seq, q, sizeof seq);
            if (seq != got) {
                fails +=
                    fail("streaming: FIFO sequence", static_cast<long>(seq));
                break;
            }
            if (std::memcmp(q + sizeof seq, pattern.data() + sizeof seq,
                            static_cast<size_t>(kBigMsg) - sizeof seq) != 0) {
                fails +=
                    fail("streaming: payload bytes", static_cast<long>(got));
                break;
            }
            c.release();
            const uint64_t w = ch->hdr->write.load(std::memory_order_relaxed);
            if (w < last_write) ++wraps;
            last_write = w;
            ++got;
        }
        if (got != kBigTotal / kBigMsg && fails == 0)
            fails += fail("streaming: short stream", static_cast<long>(got));
        // 512 MB through a 256 MB ring must lap it: without a wrap this case
        // would only be proving that a big file can be written once.
        if (wraps == 0)
            fails += fail("streaming: the ring never wrapped — the case is not "
                          "testing what it claims",
                          0);
        if (fails == 0)
            std::printf(
                "  streamed %llu MB through a %llu MB file-backed "
                "channel, %llu-message window, %llu wraps, byte-exact\n",
                (unsigned long long)(kBigTotal >> 20),
                (unsigned long long)(kBigCap >> 20),
                (unsigned long long)kWindow, (unsigned long long)wraps);
    }
    const uint64_t rss_after = resident_bytes();
    if (rss_before != 0 && rss_after != 0)
        std::printf(
            "  RSS %llu MB -> %llu MB (mapped file pages; the page "
            "cache owns residency, so this is reported, not asserted)\n",
            (unsigned long long)(rss_before >> 20),
            (unsigned long long)(rss_after >> 20));

    shuttle::close(ch);
    shuttle::unlink_file(path);
    if (fails == 0)
        std::printf("filebacked: capacity is a FILESYSTEM question — %llu MB "
                    "channel, %llu MB streamed\n",
                    (unsigned long long)(kBigCap >> 20),
                    (unsigned long long)(kBigTotal >> 20));
    return fails == 0 ? 0 : 1;
}

// --- (c) SIGKILL mid-transfer, (d) SIGKILL holding the park mutex ----------
//
// Both are the existing crash gates (tests/crash_heartbeat_test.cpp G5.1 and
// tests/crash_mutex_test.cpp G5.4) re-run against a file-backed channel. The
// choreography is deliberately identical, because the QUESTION is whether the
// backing changed the answer.

constexpr uint64_t kStaleNs = 1500ull * 1000000ull;  // victim's threshold
constexpr uint64_t kKillAfterNs = 1ull * 1000000000ull;
constexpr uint64_t kMaxDetectNs = 8ull * 1000000000ull;
constexpr char kMarker[] = "marker";

// Producer child: publish a marker, then die at the requested kill point while
// still heartbeating (so the victim cannot declare it dead early).
//   "reserve": mid-transfer — an acquired, never-committed reservation.
//   "mutex":   holding the park mutex, announced via a heartbeat sentinel.
int run_crasher(const char* path, bool hold_mutex) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open_file(path, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "crasher: open_file err=%d\n", err);
        return 1;
    }
    shuttle::Producer p(ch);
    if (p.write(kMarker, sizeof(kMarker)) != shuttle::kOk) {
        std::fprintf(stderr, "crasher: marker write failed\n");
        return 1;
    }
    if (hold_mutex) {
        if (shuttle::park_mutex_lock(&ch->hdr->park.lock) != 0) {
            std::fprintf(stderr, "crasher: mutex lock failed\n");
            return 1;
        }
        ch->hdr->producer_heartbeat.store(kHoldSentinel,
                                          std::memory_order_release);
    } else {
        void* span = nullptr;
        if (p.acquire_write(&span, 4096) != shuttle::kOk) {
            std::fprintf(stderr, "crasher: acquire failed\n");
            return 1;
        }
        std::memset(span, 0xDD, 4096);  // partially written, never committed
    }
    for (;;) {
        p.keepalive();
        usleep(50000);
    }
}

// Consumer child: read the marker, then block. The blocked read must abort with
// kErrPeerDead inside the staleness window — never hang.
int run_victim(const char* path, bool expect_phantom_check) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open_file(path, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "victim: open_file err=%d\n", err);
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
    const int rc = c.read(&p, &len);  // the peer dies while we are parked here
    const uint64_t elapsed = shuttle::monotonic_ns() - t0;

    int fails = 0;
    if (rc != shuttle::kErrPeerDead) {
        std::fprintf(stderr, "victim: rc=%d, want kErrPeerDead\n", rc);
        ++fails;
    }
    if (elapsed < kStaleNs) {
        std::fprintf(stderr,
                     "victim: aborted after %.2f s, below the threshold —"
                     " premature death verdict on a live peer\n",
                     elapsed / 1e9);
        ++fails;
    }
    if (elapsed > kMaxDetectNs) {
        std::fprintf(stderr, "victim: detection took %.2f s\n", elapsed / 1e9);
        ++fails;
    }
    // The dead producer's uncommitted reservation must never surface as data.
    if (expect_phantom_check &&
        c.try_read(&p, &len) != shuttle::kErrWouldBlock) {
        std::fprintf(stderr, "victim: phantom data after peer death!\n");
        ++fails;
    }
    if (fails == 0)
        std::printf("  victim: kErrPeerDead %.2f s after parking on a "
                    "file-backed channel\n",
                    elapsed / 1e9);
    shuttle::close(ch);
    return fails == 0 ? 0 : 1;
}

// The driver half both crash cases share.
int crash_case(const char* self, const char* tag, bool hold_mutex) {
    char path[600];
    path_for(path, sizeof path, tag);
    int err = 0;
    shuttle::Channel* ch = shuttle::create_file(path, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) return fail("crash: create_file", err);

    const char* roles[2] = {hold_mutex ? "mx_crasher" : "hb_crasher",
                            hold_mutex ? "mx_victim" : "hb_victim"};
    pid_t pids[2] = {0, 0};
    int fails = 0;
    for (int i = 0; i < 2; ++i) {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>(roles[i]),
                        path, nullptr};
        if (posix_spawn(&pids[i], self, nullptr, nullptr, argv, environ) != 0) {
            std::fprintf(stderr, "driver: spawn %s failed\n", roles[i]);
            if (i == 1) {
                kill(pids[0], SIGKILL);
                waitpid(pids[0], nullptr, 0);
            }
            shuttle::close(ch);
            shuttle::unlink_file(path);
            return 1;
        }
    }

    if (hold_mutex) {
        // Do not kill until the crasher demonstrably owns the park mutex,
        // otherwise the case degenerates into case (c).
        const uint64_t lockwait = shuttle::monotonic_ns() + kChildTimeoutNs;
        while (ch->hdr->producer_heartbeat.load(std::memory_order_acquire) <
               kHoldSentinel) {
            if (shuttle::monotonic_ns() > lockwait) {
                fails += fail("crash: crasher never took the lock", 0);
                break;
            }
            usleep(2000);
        }
    }
    usleep(static_cast<useconds_t>(kKillAfterNs / 1000));
    kill(pids[0], SIGKILL);  // no cleanup whatsoever
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
            std::fprintf(stderr,
                         "driver: victim DEADLOCKED after the peer died on a "
                         "file-backed channel\n");
            kill(pids[1], SIGKILL);
            waitpid(pids[1], nullptr, 0);
            ++fails;
            break;
        }
        usleep(10000);
    }

    // Where robust mutexes exist, an orphaned park mutex must still be usable
    // afterward — recovered, not poisoned. (Case (e) is what proves the
    // recovery is the ROBUST one rather than a lock that was never really
    // held.)
    if (shuttle::kHasRobustMutex) {
        if (shuttle::park_mutex_lock(&ch->hdr->park.lock) != 0) {
            fails += fail("crash: post-crash mutex unusable", 0);
        } else {
            shuttle::park_mutex_unlock(&ch->hdr->park.lock);
        }
    }
    shuttle::close(ch);
    shuttle::unlink_file(path);
    if (fails == 0)
        std::printf(
            "filebacked: SIGKILL %s on a file-backed channel -> "
            "kErrPeerDead, no deadlock\n",
            hold_mutex ? "while holding the park mutex" : "mid-transfer");
    return fails == 0 ? 0 : 1;
}

// --- (e) the robust-mutex proof on a file mapping --------------------------

// Child: open the file-backed segment, take the park mutex, announce, and wait
// to be SIGKILLed as the mutex owner.
int run_holder(const char* path) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open_file(path, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "holder: open_file err=%d\n", err);
        return 1;
    }
    if (shuttle::park_mutex_lock(&ch->hdr->park.lock) != 0) {
        std::fprintf(stderr, "holder: lock failed\n");
        return 1;
    }
    ch->hdr->producer_heartbeat.store(kHoldSentinel, std::memory_order_release);
    for (;;) pause();  // die owning the lock
}

int kill_holder_midlock(const char* self, shuttle::Channel* ch,
                        const char* path) {
    pid_t pid = 0;
    char* argv[] = {const_cast<char*>(self), const_cast<char*>("holder"),
                    const_cast<char*>(path), nullptr};
    if (posix_spawn(&pid, self, nullptr, nullptr, argv, environ) != 0) {
        std::fprintf(stderr, "spawn holder failed\n");
        return -1;
    }
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
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

int case_robust_mutex() {
    if (!shuttle::kHasRobustMutex) {
        // macOS: there is no robust attribute to test. The crash story there is
        // the heartbeat, and cases (c)/(d) above already ran it on a file
        // mapping — which is exactly what docs/API.md claims for this platform.
        std::printf("filebacked: robust-mutex proof SKIPPED (no robust mutexes "
                    "on %s; heartbeat is the story, proven by cases c/d)\n",
                    shuttle::platform_name());
        return 0;
    }
    int fails = 0;

    // A) THE PROOF. The orphaned lock is taken RAW — not through
    // park_mutex_lock, which would absorb the very code being measured.
    {
        char path[600];
        path_for(path, sizeof path, "robust");
        int err = 0;
        shuttle::Channel* ch =
            shuttle::create_file(path, 1u << 16, 1u << 10, &err);
        if (ch == nullptr) return fail("robust: create_file", err);
        if (kill_holder_midlock(argv0(), ch, path) != 0) ++fails;

        const int raw = pthread_mutex_lock(&ch->hdr->park.lock);
        std::printf("  OBSERVED on a file-backed MAP_SHARED mapping: raw "
                    "pthread_mutex_lock on the orphaned park mutex returned "
                    "%d (%s)\n",
                    raw, raw == EOWNERDEAD ? "EOWNERDEAD" : std::strerror(raw));
        if (raw != EOWNERDEAD) {
            fails += fail("robust: no EOWNERDEAD on a file mapping — the "
                          "file-backed crash story is NOT the shm one",
                          raw);
        }
        // Repair in the seam's order: (no state to repair) -> consistent ->
        // unlock. Then the mutex must be entirely ordinary again.
        if (shuttle::park_mutex_recover_if_needed(&ch->hdr->park.lock, raw) !=
            0)
            fails += fail("robust: seam recovery rejected EOWNERDEAD", 0);
        shuttle::park_mutex_unlock(&ch->hdr->park.lock);
        if (shuttle::park_mutex_lock(&ch->hdr->park.lock) != 0) {
            fails += fail("robust: post-recovery lock failed", 0);
        } else {
            const int wrc = shuttle::cond_timedwait_rel(
                &ch->hdr->park.not_empty, &ch->hdr->park.lock, 50ull * 1000000);
            if (wrc != ETIMEDOUT)
                fails += fail("robust: post-recovery timedwait", wrc);
            shuttle::park_mutex_unlock(&ch->hdr->park.lock);
        }
        // ...and the channel itself still carries data over the same mapping.
        {
            shuttle::Producer p(ch);
            shuttle::Consumer c(ch);
            const unsigned char probe[] = "after the owner died";
            const unsigned char* q = nullptr;
            uint64_t len = 0;
            if (p.try_write(probe, sizeof probe) != shuttle::kOk ||
                c.try_read(&q, &len) != shuttle::kOk || len != sizeof probe ||
                std::memcmp(q, probe, sizeof probe) != 0)
                fails += fail("robust: transfer after recovery", 0);
            else
                c.release();
        }
        shuttle::close(ch);
        shuttle::unlink_file(path);
    }

    // B) THE TEST CAN FAIL. Same kill, deliberately buggy recovery (unlock
    // without pthread_mutex_consistent): the mutex must end up permanently
    // ENOTRECOVERABLE. Without this leg, A) would pass on a platform where the
    // lock was silently not robust at all.
    {
        char path[600];
        path_for(path, sizeof path, "robustbad");
        int err = 0;
        shuttle::Channel* ch =
            shuttle::create_file(path, 1u << 16, 1u << 10, &err);
        if (ch == nullptr) return fails + fail("robust-bad: create_file", err);
        if (kill_holder_midlock(argv0(), ch, path) != 0) ++fails;

        int rc = pthread_mutex_lock(&ch->hdr->park.lock);
        if (rc != EOWNERDEAD) fails += fail("robust-bad: want EOWNERDEAD", rc);
        pthread_mutex_unlock(&ch->hdr->park.lock);  // the bug: no consistent()
        rc = pthread_mutex_lock(&ch->hdr->park.lock);
        if (rc != ENOTRECOVERABLE)
            fails += fail("robust-bad: want ENOTRECOVERABLE — the failure mode "
                          "this case guards against is not detectable",
                          rc);
        shuttle::close(ch);
        shuttle::unlink_file(path);
    }

    if (fails == 0)
        std::printf("filebacked: EOWNERDEAD recovery VERIFIED on a file-backed "
                    "mapping — identical to shm (and the check can fail: "
                    "skipping consistent() poisons it)\n");
    return fails == 0 ? 0 : 1;
}

// --- (f) error paths -------------------------------------------------------

int case_errors() {
    char path[600];
    path_for(path, sizeof path, "errors");
    int err = 0, fails = 0;
    const size_t kCap = 1u << 20;
    const size_t kMax = 1u << 16;

    // open_file on a path that is not there.
    if (shuttle_open_file(path, &err) != nullptr ||
        err != SHUTTLE_ERR_NOT_FOUND)
        fails += fail("errors: open_file(missing) not NOT_FOUND", err);
    if (shuttle_unlink_file(path) != SHUTTLE_ERR_NOT_FOUND)
        fails += fail("errors: unlink_file(missing) not NOT_FOUND", 0);

    // create_file on a path that already exists — the stale-file recovery
    // point. Nothing is truncated: the existing file is left exactly as it was.
    shuttle_channel* first = shuttle_create_file(path, kCap, kMax, 0, &err);
    if (first == nullptr) return fails + fail("errors: create_file", err);
    const int64_t before = file_size(path);
    err = 0;
    if (shuttle_create_file(path, kCap, kMax, 0, &err) != nullptr ||
        err != SHUTTLE_ERR_EXISTS)
        fails += fail("errors: create_file(existing) not EXISTS", err);
    if (file_size(path) != before)
        fails += fail("errors: the existing file was disturbed", 0);
    shuttle_close(first);
    shuttle_unlink_file(path);

    // hugetlb bits: a hugetlbfs backing and a caller-chosen path are two
    // different segments, so the request is REFUSED rather than half-honored.
    for (uint32_t bit : {static_cast<uint32_t>(SHUTTLE_CREATE_HUGETLB_2MB),
                         static_cast<uint32_t>(SHUTTLE_CREATE_HUGETLB_1GB),
                         static_cast<uint32_t>(SHUTTLE_CREATE_HUGETLB_2MB |
                                               SHUTTLE_CREATE_ALIGNED_SPANS)}) {
        err = 0;
        if (shuttle_create_file(path, kCap, kMax, bit, &err) != nullptr ||
            err != SHUTTLE_ERR_INVALID_ARGS)
            fails += fail("errors: hugetlb bit not INVALID_ARGS",
                          static_cast<long>(bit));
        if (file_exists(path))
            fails += fail("errors: a rejected create left a file behind", 0);
    }

    // Relative path, empty path, NULL: all INVALID_ARGS. A channel's identity
    // must not depend on anyone's working directory.
    const char* bad_paths[] = {"relative/x.seg", "x.seg", "", nullptr};
    for (const char* bad : bad_paths) {
        err = 0;
        if (shuttle_create_file(bad, kCap, kMax, 0, &err) != nullptr ||
            err != SHUTTLE_ERR_INVALID_ARGS)
            fails += fail("errors: relative/empty create_file not INVALID_ARGS",
                          err);
        err = 0;
        if (shuttle_open_file(bad, &err) != nullptr ||
            err != SHUTTLE_ERR_INVALID_ARGS)
            fails +=
                fail("errors: relative/empty open_file not INVALID_ARGS", err);
        if (shuttle_unlink_file(bad) != SHUTTLE_ERR_INVALID_ARGS)
            fails +=
                fail("errors: relative/empty unlink_file not INVALID_ARGS", 0);
    }

    // The FR-4 capacity rule is not relaxed by the backing.
    err = 0;
    if (shuttle_create_file(path, 16, 1024, 0, &err) != nullptr ||
        err != SHUTTLE_ERR_CAPACITY_TOO_SMALL)
        fails += fail("errors: capacity floor not enforced", err);

    // A path whose parent directory does not exist is NOT_FOUND, not an opaque
    // syscall failure.
    char nodir[700];
    std::snprintf(nodir, sizeof nodir, "%s/no-such-dir/x.seg", g_dir);
    err = 0;
    if (shuttle_create_file(nodir, kCap, kMax, 0, &err) != nullptr ||
        err != SHUTTLE_ERR_NOT_FOUND)
        fails += fail("errors: missing directory not NOT_FOUND", err);

    // The asymmetry, asserted: 0x20 is NOT selectable through the name-typed
    // entry point. It is masked off there like any bit that call cannot
    // implement — there is nowhere to put a path in an shm name.
    char name[32];
    std::snprintf(name, sizeof name, "/shfb.%d",
                  static_cast<int>(getpid()) % 100000);
    shuttle_unlink(name);
    err = 0;
    shuttle_channel* shm =
        shuttle_create_ex(name, kCap, kMax, SHUTTLE_CREATE_FILE_BACKED, &err);
    if (shm == nullptr) {
        fails +=
            fail("errors: create_ex(0x20) rejected instead of masking", err);
    } else {
        int perr = 0;
        shuttle::Channel* view = shuttle::open(name, &perr);
        if (view == nullptr) {
            fails += fail("errors: open of the masked segment", perr);
        } else {
            if (view->hdr->flags != 0)
                fails += fail("errors: 0x20 was persisted by create_ex",
                              static_cast<long>(view->hdr->flags));
            shuttle::close(view);
        }
        shuttle_close(shm);
        shuttle_unlink(name);
    }

    if (fails == 0)
        std::printf("filebacked: error paths ok (missing/exists/hugetlb/"
                    "relative/no-dir; 0x20 stays masked on create_ex)\n");
    return fails == 0 ? 0 : 1;
}

// --- (g) flag combinations -------------------------------------------------

int roundtrip_once(shuttle_channel* prod, shuttle_channel* cons, uint64_t seed,
                   size_t len) {
    std::vector<unsigned char> msg(len);
    for (size_t i = 0; i < len; ++i) msg[i] = fill_byte(seed, i);
    if (shuttle_write(prod, msg.data(), len, 0) != SHUTTLE_OK) return 1;
    const void* q = nullptr;
    size_t got = 0;
    if (shuttle_acquire_read(cons, &q, &got, 0) != SHUTTLE_OK || got != len)
        return 1;
    const int bad = std::memcmp(q, msg.data(), len) != 0 ? 1 : 0;
    shuttle_release_read(cons);
    return bad;
}

int case_combos() {
    const uint64_t page = static_cast<uint64_t>(shuttle::page_size());
    struct Combo {
        const char* tag;
        uint32_t flags;
        uint32_t want_flags;
        uint32_t want_version;
        bool aligned;
    } combos[] = {
        {"cstats", SHUTTLE_CREATE_STATS,
         shuttle::kFlagStats | shuttle::kFlagFileBacked, shuttle::kVersionStats,
         false},
        {"caligned", SHUTTLE_CREATE_ALIGNED_SPANS,
         shuttle::kFlagAlignedSpans | shuttle::kFlagFileBacked,
         shuttle::kVersion, true},
    };
    int fails = 0;
    for (const Combo& c : combos) {
        char path[600];
        path_for(path, sizeof path, c.tag);
        int err = 0;
        shuttle_channel* prod = shuttle_create_file(
            path, static_cast<size_t>(64 * page), 4096, c.flags, &err);
        if (prod == nullptr) {
            fails += fail("combos: create_file", err);
            continue;
        }
        shuttle_channel* cons = shuttle_open_file(path, &err);
        if (cons == nullptr) {
            fails += fail("combos: open_file", err);
            shuttle_close(prod);
            shuttle_unlink_file(path);
            continue;
        }
        // The persisted identity block says what the creator asked for — 0x28
        // and 0x30 respectively, the file bit included.
        int perr = 0;
        shuttle::Channel* view = shuttle::open_file(path, &perr);
        if (view == nullptr) {
            fails += fail("combos: C++ open_file", perr);
        } else {
            if (view->hdr->flags != c.want_flags)
                fails +=
                    fail("combos: flags", static_cast<long>(view->hdr->flags));
            if (view->hdr->version != c.want_version)
                fails += fail("combos: version",
                              static_cast<long>(view->hdr->version));
            const uint64_t want_off =
                shuttle::data_offset_for(c.want_version, c.want_flags, page);
            if (view->hdr->data_offset != want_off)
                fails += fail("combos: data_offset",
                              static_cast<long>(view->hdr->data_offset));
            shuttle::close(view);
        }
        if (roundtrip_once(prod, cons, 5, 3000) != 0)
            fails += fail("combos: roundtrip", 0);
        // The flag-specific promise still holds over a file mapping.
        if (c.aligned) {
            const void* q = nullptr;
            size_t got = 0;
            if (shuttle_write(prod, "aligned", 7, 0) != SHUTTLE_OK ||
                shuttle_acquire_read(cons, &q, &got, 0) != SHUTTLE_OK) {
                fails += fail("combos: aligned transfer", 0);
            } else {
                if ((reinterpret_cast<uintptr_t>(q) & (page - 1)) != 0)
                    fails += fail("combos: borrow not page-aligned on a file "
                                  "mapping",
                                  0);
                shuttle_release_read(cons);
            }
        } else {
            shuttle_stats st{};
            if (shuttle_get_stats(prod, &st) != SHUTTLE_OK)
                fails += fail("combos: get_stats on a file+stats channel", 0);
            else if (st.msgs_written != 1 || st.bytes_written != 3000)
                fails += fail("combos: counters",
                              static_cast<long>(st.bytes_written));
        }
        shuttle_close(cons);
        shuttle_close(prod);
        shuttle_unlink_file(path);
    }
    if (fails == 0)
        std::printf("filebacked: combos ok (0x28 file+stats, 0x30 "
                    "file+aligned)\n");
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    if (!make_temp_dir()) {
        std::fprintf(stderr, "FAIL: could not create a temp directory\n");
        return 1;
    }
    std::printf("filebacked_test: segments under %s\n", g_dir);
    int fails = 0;
    fails += case_roundtrip();
    fails += case_streaming();
    fails += crash_case(self, "crashhb", false);
    fails += crash_case(self, "crashmx", true);
    fails += case_robust_mutex();
    fails += case_errors();
    fails += case_combos();
    if (::rmdir(g_dir) != 0)
        std::fprintf(stderr,
                     "warning: temp dir %s not empty (a case leaked a file)\n",
                     g_dir);
    if (fails == 0)
        std::printf("filebacked_test ok: file-backed channels end to end "
                    "(platform=%s)\n",
                    shuttle::platform_name());
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3) {
        if (std::strcmp(argv[1], "hb_crasher") == 0)
            return run_crasher(argv[2], false);
        if (std::strcmp(argv[1], "hb_victim") == 0)
            return run_victim(argv[2], true);
        if (std::strcmp(argv[1], "mx_crasher") == 0)
            return run_crasher(argv[2], true);
        if (std::strcmp(argv[1], "mx_victim") == 0)
            return run_victim(argv[2], false);
        if (std::strcmp(argv[1], "holder") == 0) return run_holder(argv[2]);
    }
    std::fprintf(stderr,
                 "usage: %s [hb_crasher|hb_victim|mx_crasher|mx_victim|holder "
                 "</abs/path>]\n",
                 argv[0]);
    return 2;
}
