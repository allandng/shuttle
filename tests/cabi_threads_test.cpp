// G6.4: SANITIZER COVERAGE FOR THE C ABI TRANSLATION UNIT ITSELF.
//
// Why this test exists at all. libshuttle_c is deliberately UNsanitized (see
// the note above add_library(shuttle_c) in CMakeLists.txt): python3 and the
// Rust test binaries dlopen it, and they carry no sanitizer runtime. The three
// existing cabi gates (G6.1 python, G6.2 rust, G6.3 errors) therefore drive
// src/shuttle_c.cpp through that uninstrumented dylib, and the two other tests
// that reach the ABI concurrently (stats_test case (f), drop_policy_test) link
// the same dylib. The header-only core it embeds is sanitizer-verified through
// every other target in the file — but the ABI translation unit's OWN code is
// not: its handle bookkeeping (the lazily-bound prod/cons pointers and the
// cached borrow triple), its stats plumbing, and its error translation have
// never been in front of ASan or TSan.
//
// This test closes exactly that gap and nothing else. It compiles
// src/shuttle_c.cpp DIRECTLY into the executable (CMakeLists, below
// link_libraries(shuttle_san)), so the ABI code inherits whichever sanitizer
// the tree was configured with. The dylib and the three cabi gates are
// untouched; this is additive coverage.
//
// TSan sees races only WITHIN a process, so both ends of a real channel run
// here in one address space on separate threads. That is a supported
// configuration, not a testing trick: shuttle_create and shuttle_open are
// independent mappings of the same shm object, and stats_test case (f) already
// runs three same-process handles over one segment.
//
//   a. ERRORS (own throwaway channel, single-threaded). Every rejection branch
//      in the ABI layer: null handle, null buffer, oversize payload, the
//      SHUTTLE_DROP_NEWEST bit on the three calls that refuse it, commit with
//      no reservation, release with no borrow, peek with a null out-param,
//      stats with a null out-param, open/unlink of a name that is not there,
//      and a capacity create() must refuse. Under ASan this is the pass that
//      runs the error-translation code with the checker watching.
//   b. BORROW REUSE (same channel). shuttle_read with a cap SMALLER than the
//      waiting message returns MSG_TOO_LARGE and — this is the part that lives
//      only in the ABI layer — leaves the borrow CACHED. The next
//      shuttle_acquire_read must hand back that same message rather than
//      acquire a second one, and shuttle_release_read must then clear it.
//      Handle-local bookkeeping with no counterpart in the core headers.
//   c. STRESS (the stress channel; four threads, one process). A producer
//      thread and a consumer thread drive the FULL surface over a ring far
//      smaller than the traffic, so the wrap/watermark handoff runs constantly:
//        producer  i%3==0 shuttle_write (copy path, blocking)
//                  i%3==1 acquire_write + commit_write (zero-copy, exact len)
//                  i%3==2 acquire_write of a LARGER span + partial commit
//        consumer  i%3==0 shuttle_read (copy path, blocking)
//                  i%3==1 acquire_read + shuttle_peek_next WITH THE BORROW
//                         OUTSTANDING + release_read
//                  i%3==2 acquire_read + release_read
//      plus a stats thread calling shuttle_get_stats on a third handle while
//      traffic flows, and a keepalive thread ticking shuttle_keepalive on all
//      three handles on a timer. Byte-exact FIFO throughout.
//   d. DROP BURST (quiescent, after the threads join). The ring is filled with
//      SHUTTLE_NONBLOCK writes until it reports WOULD_BLOCK, then a burst of
//      SHUTTLE_DROP_NEWEST writes must each return SHUTTLE_DROPPED and land in
//      msgs_dropped — the one ABI path that counts something the core never
//      does on its own.
//   e. EXACT COUNTERS + CLEAN TEARDOWN. Once drained, all five counters must
//      equal what this test wrote and read, read back through both a role-bound
//      handle and the role-less watcher; then close all handles and unlink,
//      and a second unlink must report NOT_FOUND.
//
// WHAT IS SYNCHRONIZED, STATED HONESTLY — the point of a TSan gate is worth
// nothing if the test papers over its own races:
//
//   * A shuttle_channel handle is NOT thread-safe, and this test never pretends
//     otherwise. Each thread owns its handle: `prod` is the producer thread's,
//     `cons` is the consumer thread's, `watch` is the stats thread's. What the
//     threads share is the SEGMENT, which is where the SPSC contract lives.
//   * Role binding (producer()/consumer() in shuttle_c.cpp) is a lazy WRITE to
//     a handle field. The keepalive ticker READS those same fields. So the
//     warm-up phase binds both roles before any thread starts, and the join at
//     the end of the stress phase closes the interval — concurrently those
//     pointers are read-only. An application that ticks keepalive from a timer
//     thread must do the same thing, so this is the configuration worth
//     testing, not a weakened one.
//   * shuttle_get_stats is genuinely concurrent with the producer's and
//     consumer's counter stores, and that is deliberate. It is race-free
//     because the counters are std::atomic<uint64_t> with single-writer relaxed
//     stores (header.hpp; get_stats in shuttle.cpp) — not because the test
//     avoids the overlap. It is NOT an atomic five-tuple, so the watcher
//     asserts only what is actually promised: each counter is monotonic and
//     never runs backward. It does not compare written against read.
//   * Ticking shuttle_keepalive on the producer handle while the producer
//     thread writes means two threads bump the same heartbeat with a relaxed
//     load+store pair. That can LOSE an increment, and it does not matter: the
//     heartbeat is a liveness token whose value is never interpreted, only its
//     movement. No data race — it is an atomic — and no lost meaning.
//
// Falsifiability (CONTRIBUTING). Three of the claims here could pass while
// testing nothing, so each carries an enforced floor rather than a printed
// statistic: the wrap count (a run that stopped wrapping would still be
// byte-exact FIFO), the number of peeks that actually SAW the next message (a
// peek that always answered WOULD_BLOCK would satisfy every check in (c)), and
// the number of ring-filling writes in (d) (a burst on a ring that was never
// full would return OK, not DROPPED, and is checked to). All three were run
// inverted while writing this test.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "shuttle/shuttle.hpp"  // shuttle::open — the read-only wrap observer
#include "shuttle/shuttle_c.h"  // the surface under test

namespace {

int fails = 0;

#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            std::fprintf(stderr, "FAIL: " __VA_ARGS__); \
            std::fprintf(stderr, " [%s]\n", #cond);     \
            ++fails;                                    \
            if (fails > 20) std::exit(1);               \
        }                                               \
    } while (0)

// A ring far smaller than the traffic: every few messages wraps. Same shape as
// stats_test case (e)/(f) and bipbuffer_test's "tight" configuration.
constexpr size_t kCapacity = 8192;
constexpr size_t kMaxPayload = 1024;

// Sized so the stress phase runs a few seconds under TSan, comparable to the
// other threaded gates (spsc_stress_test `threads`, stats_test case (f)).
constexpr uint64_t kStressMsgs = 400000;

// (d): the drop burst.
constexpr uint64_t kDrops = 256;
constexpr size_t kDropLen = 400;

// Payload the CONSUMER must see for message i. The producer's mode-2 path
// reserves more than this and commits exactly this, so both sides agree on the
// committed length without either storing it.
uint64_t payload_len(uint64_t i) { return 1 + (i * 47) % 600; }

// Span mode 2 reserves before shrinking it at commit. Stays <= kMaxPayload.
uint64_t reserve_len(uint64_t i) {
    const uint64_t want = payload_len(i) + 128;
    return want > kMaxPayload ? kMaxPayload : want;
}

unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 1315423911ull) + i * 151ull +
                                      (i >> 8));
}

void fill(unsigned char* dst, uint64_t msg, uint64_t len) {
    for (uint64_t j = 0; j < len; ++j) dst[j] = fill_byte(msg, j);
}

// Returns the count of mismatched bytes.
uint64_t verify(const unsigned char* got, uint64_t msg, uint64_t len) {
    uint64_t bad = 0;
    for (uint64_t j = 0; j < len; ++j)
        if (got[j] != fill_byte(msg, j)) ++bad;
    return bad;
}

void unique_name(char* out, size_t n, const char* tag) {
    std::snprintf(out, n, "/shcabt%s.%d", tag,
                  static_cast<int>(getpid()) % 1000000);
}

// ------------------------------------------------------------------ (a) + (b)

// Every rejection branch in shuttle_c.cpp, plus the cached-borrow path that
// exists nowhere else. Own channel so the stress channel's counters stay the
// stress phase's own.
void run_errors() {
    char name[48];
    unique_name(name, sizeof name, "e");
    shuttle_unlink(name);

    int err = 0;

    // create() rejections, before anything exists.
    CHECK(shuttle_create(name, 16, kMaxPayload, &err) == nullptr &&
              err == SHUTTLE_ERR_CAPACITY_TOO_SMALL,
          "create with capacity < one frame must be CAPACITY_TOO_SMALL "
          "(err=%d)",
          err);
    CHECK(shuttle_create("no-leading-slash", kCapacity, kMaxPayload, &err) ==
              nullptr &&
              err == SHUTTLE_ERR_INVALID_ARGS,
          "create with a malformed name must be INVALID_ARGS (err=%d)", err);
    CHECK(shuttle_open(name, &err) == nullptr && err == SHUTTLE_ERR_NOT_FOUND,
          "open of an absent name must be NOT_FOUND (err=%d)", err);
    CHECK(shuttle_unlink(name) == SHUTTLE_ERR_NOT_FOUND,
          "unlink of an absent name must be NOT_FOUND");

    shuttle_channel* prod = shuttle_create_ex(name, kCapacity, kMaxPayload,
                                              SHUTTLE_CREATE_STATS, &err);
    CHECK(prod != nullptr, "create_ex(errors channel) failed err=%d", err);
    if (prod == nullptr) return;

    CHECK(shuttle_create_ex(name, kCapacity, kMaxPayload, SHUTTLE_CREATE_STATS,
                            &err) == nullptr &&
              err == SHUTTLE_ERR_EXISTS,
          "re-create of a live name must be EXISTS (err=%d)", err);

    shuttle_channel* cons = shuttle_open(name, &err);
    shuttle_channel* bare = shuttle_open(name, &err);  // stays role-less
    CHECK(cons != nullptr && bare != nullptr, "open(errors channel) err=%d",
          err);
    if (cons == nullptr || bare == nullptr) {
        shuttle_close(cons);
        shuttle_close(bare);
        shuttle_close(prod);
        shuttle_unlink(name);
        return;
    }

    std::vector<unsigned char> buf(kMaxPayload + 8, 0xA5);
    std::vector<unsigned char> out(kMaxPayload + 8, 0);
    void* wp = nullptr;
    const void* rp = nullptr;
    size_t rlen = 0;
    shuttle_stats st;

    // Null / malformed arguments on every entry point that takes them.
    CHECK(shuttle_write(nullptr, buf.data(), 8, 0) == SHUTTLE_ERR_INVALID_ARGS,
          "write(null handle) must be INVALID_ARGS");
    CHECK(shuttle_write(prod, nullptr, 8, 0) == SHUTTLE_ERR_INVALID_ARGS,
          "write(null data, len!=0) must be INVALID_ARGS");
    CHECK(shuttle_read(nullptr, out.data(), out.size(), 0) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "read(null handle) must be INVALID_ARGS");
    CHECK(shuttle_read(cons, nullptr, 8, 0) == SHUTTLE_ERR_INVALID_ARGS,
          "read(null out, cap!=0) must be INVALID_ARGS");
    CHECK(shuttle_acquire_write(prod, nullptr, 8, 0) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "acquire_write(null ptr) must be INVALID_ARGS");
    CHECK(shuttle_acquire_read(cons, nullptr, &rlen, 0) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "acquire_read(null ptr) must be INVALID_ARGS");
    CHECK(shuttle_acquire_read(cons, &rp, nullptr, 0) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "acquire_read(null len) must be INVALID_ARGS");
    CHECK(shuttle_peek_next(cons, nullptr) == SHUTTLE_ERR_INVALID_ARGS,
          "peek_next(null out) must be INVALID_ARGS");
    CHECK(shuttle_peek_next(nullptr, &rlen) == SHUTTLE_ERR_INVALID_ARGS,
          "peek_next(null handle) must be INVALID_ARGS");
    CHECK(shuttle_get_stats(prod, nullptr) == SHUTTLE_ERR_INVALID_ARGS,
          "get_stats(null out) must be INVALID_ARGS");
    CHECK(shuttle_get_stats(nullptr, &st) == SHUTTLE_ERR_INVALID_ARGS,
          "get_stats(null handle) must be INVALID_ARGS");
    CHECK(shuttle_commit_write(nullptr, 0) == SHUTTLE_ERR_INVALID_ARGS,
          "commit_write(null handle) must be INVALID_ARGS");
    CHECK(shuttle_release_read(nullptr) == SHUTTLE_ERR_INVALID_ARGS,
          "release_read(null handle) must be INVALID_ARGS");
    // The two no-op-on-null entry points: must not crash, must not touch
    // anything. ASan is the judge here, not a return value.
    shuttle_close(nullptr);
    shuttle_keepalive(nullptr);

    // Handle-state rejections: no reservation, no borrow, no role.
    CHECK(shuttle_commit_write(bare, 0) == SHUTTLE_ERR_INVALID_ARGS,
          "commit_write on a role-less handle must be INVALID_ARGS");
    CHECK(shuttle_release_read(prod) == SHUTTLE_ERR_INVALID_ARGS,
          "release_read with no borrow must be INVALID_ARGS");
    // Role-less keepalive: the branch where neither prod nor cons exists.
    shuttle_keepalive(bare);

    // The drop-policy bit is refused, not ignored, on the three calls that
    // cannot honor it.
    CHECK(shuttle_read(cons, out.data(), out.size(), SHUTTLE_DROP_NEWEST) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "read + DROP_NEWEST must be INVALID_ARGS");
    CHECK(shuttle_acquire_write(prod, &wp, 8, SHUTTLE_DROP_NEWEST) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "acquire_write + DROP_NEWEST must be INVALID_ARGS");
    CHECK(shuttle_acquire_read(cons, &rp, &rlen, SHUTTLE_DROP_NEWEST) ==
              SHUTTLE_ERR_INVALID_ARGS,
          "acquire_read + DROP_NEWEST must be INVALID_ARGS");

    // Oversize on both write paths, with and without the lossy policy: a
    // payload that could never fit is a caller bug, never backpressure.
    CHECK(shuttle_write(prod, buf.data(), kMaxPayload + 1, 0) ==
              SHUTTLE_ERR_MSG_TOO_LARGE,
          "oversize write must be MSG_TOO_LARGE");
    CHECK(shuttle_write(prod, buf.data(), kMaxPayload + 1,
                        SHUTTLE_DROP_NEWEST) == SHUTTLE_ERR_MSG_TOO_LARGE,
          "oversize + DROP_NEWEST must be MSG_TOO_LARGE, not DROPPED");
    CHECK(shuttle_acquire_write(prod, &wp, kMaxPayload + 1, 0) ==
              SHUTTLE_ERR_MSG_TOO_LARGE,
          "oversize acquire_write must be MSG_TOO_LARGE");

    // Nothing is queued yet, so the non-blocking consumer paths must say so
    // rather than park.
    CHECK(shuttle_peek_next(cons, &rlen) == SHUTTLE_ERR_WOULD_BLOCK,
          "peek_next on an empty ring must be WOULD_BLOCK");
    CHECK(shuttle_read(cons, out.data(), out.size(), SHUTTLE_NONBLOCK) ==
              SHUTTLE_ERR_WOULD_BLOCK,
          "nonblocking read of an empty ring must be WOULD_BLOCK");

    // ---- (b) the cached borrow -------------------------------------------
    // A short-cap read leaves the message queued AND the borrow cached in the
    // handle. The acquire that follows must reuse it, not take a second one.
    constexpr uint64_t kBorrowLen = 300;
    fill(buf.data(), 7, kBorrowLen);
    CHECK(shuttle_write(prod, buf.data(), kBorrowLen, 0) == SHUTTLE_OK,
          "write of the borrow-reuse message failed");
    CHECK(shuttle_read(cons, out.data(), 10, 0) == SHUTTLE_ERR_MSG_TOO_LARGE,
          "read with cap < payload must be MSG_TOO_LARGE");
    rp = nullptr;
    rlen = 0;
    CHECK(shuttle_acquire_read(cons, &rp, &rlen, SHUTTLE_NONBLOCK) ==
              SHUTTLE_OK,
          "acquire after a short-cap read must reuse the cached borrow");
    CHECK(rlen == kBorrowLen, "cached borrow len %llu != %llu",
          (unsigned long long)rlen, (unsigned long long)kBorrowLen);
    CHECK(rp != nullptr && verify(static_cast<const unsigned char*>(rp), 7,
                                  kBorrowLen) == 0,
          "cached borrow bytes differ from what was written");
    // Peek looks PAST the outstanding borrow, and there is nothing past it.
    CHECK(shuttle_peek_next(cons, &rlen) == SHUTTLE_ERR_WOULD_BLOCK,
          "peek past the only borrowed message must be WOULD_BLOCK");
    CHECK(shuttle_release_read(cons) == SHUTTLE_OK, "release of the borrow");
    CHECK(shuttle_release_read(cons) == SHUTTLE_ERR_INVALID_ARGS,
          "double release must be INVALID_ARGS");
    CHECK(shuttle_read(cons, out.data(), out.size(), SHUTTLE_NONBLOCK) ==
              SHUTTLE_ERR_WOULD_BLOCK,
          "the message must be consumed exactly once by the reused borrow");

    // The one message really did move, and the stats plumbing agrees.
    std::memset(&st, 0xAA, sizeof st);  // poison: every field must be written
    CHECK(shuttle_get_stats(bare, &st) == SHUTTLE_OK,
          "get_stats on the role-less handle");
    CHECK(st.msgs_written == 1 && st.bytes_written == kBorrowLen &&
              st.msgs_dropped == 0 && st.msgs_read == 1 &&
              st.bytes_read == kBorrowLen,
          "errors-channel counters w=%llu/%llu d=%llu r=%llu/%llu",
          (unsigned long long)st.msgs_written,
          (unsigned long long)st.bytes_written,
          (unsigned long long)st.msgs_dropped,
          (unsigned long long)st.msgs_read, (unsigned long long)st.bytes_read);

    shuttle_close(bare);
    shuttle_close(cons);
    shuttle_close(prod);
    CHECK(shuttle_unlink(name) == SHUTTLE_OK, "unlink(errors channel)");
    std::printf("  errors: every ABI rejection branch + cached-borrow reuse\n");
}

// ---------------------------------------------------------------------- (c)

struct StressResult {
    std::atomic<int> prod_err{0};
    std::atomic<int> cons_err{0};
    std::atomic<int> watch_err{0};
    std::atomic<bool> traffic_done{false};
    std::atomic<uint64_t> wraps{0};
    std::atomic<uint64_t> peeks_hit{0};
    std::atomic<uint64_t> stats_polls{0};
};

// Producer thread: rotates the three ABI write paths.
void producer_thread(shuttle_channel* prod, const shuttle::ChannelHeader* obs,
                     StressResult* r) {
    std::vector<unsigned char> buf(kMaxPayload);
    uint64_t prev_write = obs->write.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < kStressMsgs; ++i) {
        const uint64_t len = payload_len(i);
        const int mode = static_cast<int>(i % 3);
        int rc;
        if (mode == 0) {
            fill(buf.data(), i, len);
            rc = shuttle_write(prod, buf.data(), len, 0);  // blocking copy path
        } else {
            // Zero-copy: mode 1 reserves exactly, mode 2 reserves more and
            // shrinks the frame at commit.
            const uint64_t res = mode == 1 ? len : reserve_len(i);
            void* p = nullptr;
            rc = shuttle_acquire_write(prod, &p, res, 0);  // blocking
            if (rc == SHUTTLE_OK) {
                fill(static_cast<unsigned char*>(p), i, len);
                rc = shuttle_commit_write(prod, len);
            }
        }
        if (rc != SHUTTLE_OK) {
            r->prod_err.store(rc == 0 ? -1 : rc);
            return;
        }
        // The producer is the only writer of `write`, and it reads the cursor
        // synchronously after its own publish, so no wrap can be missed.
        const uint64_t now = obs->write.load(std::memory_order_relaxed);
        if (now < prev_write) r->wraps.fetch_add(1, std::memory_order_relaxed);
        prev_write = now;
    }
}

// Consumer thread: rotates the copy path and the borrow path, and peeks at the
// next message while a borrow is outstanding — the state that has no other API.
void consumer_thread(shuttle_channel* cons, StressResult* r) {
    std::vector<unsigned char> out(kMaxPayload);
    for (uint64_t i = 0; i < kStressMsgs; ++i) {
        const uint64_t len = payload_len(i);
        const int mode = static_cast<int>(i % 3);
        if (mode == 0) {
            const long n = shuttle_read(cons, out.data(), out.size(), 0);
            if (n < 0 || static_cast<uint64_t>(n) != len) {
                r->cons_err.store(n < 0 ? static_cast<int>(n) : -2);
                return;
            }
            if (verify(out.data(), i, len) != 0) {
                r->cons_err.store(-3);
                return;
            }
            continue;
        }
        const void* p = nullptr;
        size_t got = 0;
        const int rc = shuttle_acquire_read(cons, &p, &got, 0);  // blocking
        if (rc != SHUTTLE_OK) {
            r->cons_err.store(rc);
            return;
        }
        if (got != len || p == nullptr ||
            verify(static_cast<const unsigned char*>(p), i, len) != 0) {
            r->cons_err.store(-4);
            return;
        }
        if (mode == 1) {
            // Peek WITH the borrow held. It reports the next UN-borrowed
            // message, so an OK answer must be message i+1's exact length; a
            // WOULD_BLOCK answer means the producer has not published it yet
            // and can only under-report. Past the last message there is
            // nothing, ever.
            size_t next = 0;
            const int prc = shuttle_peek_next(cons, &next);
            if (i + 1 == kStressMsgs) {
                if (prc != SHUTTLE_ERR_WOULD_BLOCK) {
                    r->cons_err.store(-5);
                    return;
                }
            } else if (prc == SHUTTLE_OK) {
                if (static_cast<uint64_t>(next) != payload_len(i + 1)) {
                    r->cons_err.store(-6);
                    return;
                }
                r->peeks_hit.fetch_add(1, std::memory_order_relaxed);
            } else if (prc != SHUTTLE_ERR_WOULD_BLOCK) {
                r->cons_err.store(prc);
                return;
            }
        }
        const int rrc = shuttle_release_read(cons);
        if (rrc != SHUTTLE_OK) {
            r->cons_err.store(rrc);
            return;
        }
    }
}

// Third handle, third thread: the counters read while they are being written.
// Race-free because they are atomics with single-writer relaxed stores, NOT
// because the overlap is avoided — and so the only claim made is the one the
// ABI actually offers: per-counter monotonicity. Nothing here compares written
// against read; the five are explicitly not sampled as a tuple.
void stats_thread(shuttle_channel* watch, StressResult* r) {
    shuttle_stats prev;
    std::memset(&prev, 0, sizeof prev);
    while (!r->traffic_done.load(std::memory_order_relaxed)) {
        shuttle_stats s;
        std::memset(&s, 0xAA, sizeof s);
        const int rc = shuttle_get_stats(watch, &s);
        if (rc != SHUTTLE_OK) {
            r->watch_err.store(rc);
            return;
        }
        if (s.msgs_written < prev.msgs_written ||
            s.bytes_written < prev.bytes_written ||
            s.msgs_read < prev.msgs_read || s.bytes_read < prev.bytes_read ||
            s.msgs_dropped != 0) {
            r->watch_err.store(-1);
            return;
        }
        prev = s;
        r->stats_polls.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();  // the traffic threads are the point
    }
}

// Timer thread: keepalive on all three handles, including the role-less one.
// Both roles are already bound (warm-up), so the handle fields this reads are
// read-only for the life of this thread — see the header note.
void keepalive_thread(shuttle_channel* prod, shuttle_channel* cons,
                      shuttle_channel* watch, StressResult* r) {
    while (!r->traffic_done.load(std::memory_order_relaxed)) {
        shuttle_keepalive(prod);
        shuttle_keepalive(cons);
        shuttle_keepalive(watch);  // role-less: the both-null branch
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// ------------------------------------------------------------------ (c)-(e)

void run_stress() {
    char name[48];
    unique_name(name, sizeof name, "s");
    shuttle_unlink(name);

    int err = 0;
    shuttle_channel* prod = shuttle_create_ex(name, kCapacity, kMaxPayload,
                                              SHUTTLE_CREATE_STATS, &err);
    CHECK(prod != nullptr, "create_ex(stress channel) failed err=%d", err);
    if (prod == nullptr) return;
    shuttle_channel* cons = shuttle_open(name, &err);
    shuttle_channel* watch = shuttle_open(name, &err);
    // A read-only C++ view of the same segment, used only to count wraps from
    // inside the producer thread. It takes no role and moves no cursor.
    shuttle::Channel* obs = shuttle::open(name, &err);
    CHECK(cons != nullptr && watch != nullptr && obs != nullptr,
          "open(stress handles) err=%d", err);
    if (cons == nullptr || watch == nullptr || obs == nullptr) {
        shuttle_close(watch);
        shuttle_close(cons);
        shuttle_close(prod);
        if (obs != nullptr) shuttle::close(obs);
        shuttle_unlink(name);
        return;
    }

    uint64_t exp_written = 0, exp_bytes_w = 0, exp_read = 0, exp_bytes_r = 0;

    // ---- warm-up: bind both roles BEFORE any thread exists ----------------
    // This is what makes the keepalive ticker below race-free: producer() and
    // consumer() write handle fields lazily, and after this they are only read.
    {
        constexpr uint64_t kWarmLen = 64;
        unsigned char w[kWarmLen], rd[kWarmLen];
        fill(w, 0xFFFF, kWarmLen);
        CHECK(shuttle_write(prod, w, kWarmLen, 0) == SHUTTLE_OK, "warm-up write");
        const long n = shuttle_read(cons, rd, sizeof rd, 0);
        CHECK(n == static_cast<long>(kWarmLen) && verify(rd, 0xFFFF, kWarmLen) == 0,
              "warm-up read n=%ld", n);
        exp_written += 1;
        exp_bytes_w += kWarmLen;
        exp_read += 1;
        exp_bytes_r += kWarmLen;
    }

    // ---- (c) four threads, one process ------------------------------------
    StressResult r;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread tp(producer_thread, prod, obs->hdr, &r);
    std::thread tc(consumer_thread, cons, &r);
    std::thread ts(stats_thread, watch, &r);
    std::thread tk(keepalive_thread, prod, cons, watch, &r);
    tp.join();
    tc.join();
    r.traffic_done.store(true, std::memory_order_relaxed);
    ts.join();
    tk.join();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();

    CHECK(r.prod_err.load() == 0, "stress producer failed rc=%d",
          r.prod_err.load());
    CHECK(r.cons_err.load() == 0, "stress consumer failed rc=%d",
          r.cons_err.load());
    CHECK(r.watch_err.load() == 0, "stats watcher failed rc=%d",
          r.watch_err.load());

    for (uint64_t i = 0; i < kStressMsgs; ++i) {
        exp_bytes_w += payload_len(i);
        exp_bytes_r += payload_len(i);
    }
    exp_written += kStressMsgs;
    exp_read += kStressMsgs;

    // Enforced floors, not statistics. The observed values on an M3 (ASan) are
    // in the comments; the floors sit far enough below to absorb scheduling
    // while any COLLAPSE of the coverage they stand for fails the gate.
    CHECK(r.wraps.load() >= 12000,  // observed 15666 on both legs; it is a
                                    // near-deterministic function of the
                                    // geometry, not a scheduling artifact
          "only %llu wraps — this configuration stopped exercising the "
          "wrap/watermark handoff",
          (unsigned long long)r.wraps.load());
    CHECK(r.peeks_hit.load() >= 10000,  // observed ~133330 of 133334; the floor
                                        // is loose because how far the producer
                                        // runs ahead is a scheduling matter
          "only %llu peeks saw the next message — peek-with-a-borrow-held is "
          "no longer being exercised",
          (unsigned long long)r.peeks_hit.load());
    CHECK(r.stats_polls.load() >= 100,  // observed ~10^6
          "only %llu concurrent get_stats calls — the ABI stats path is no "
          "longer overlapping live traffic",
          (unsigned long long)r.stats_polls.load());

    std::printf("  stress: %llu msgs, %llu wraps, %llu peek hits, %llu stats "
                "polls, %.2fs\n",
                (unsigned long long)kStressMsgs,
                (unsigned long long)r.wraps.load(),
                (unsigned long long)r.peeks_hit.load(),
                (unsigned long long)r.stats_polls.load(), secs);

    // ---- (d) drop burst, quiescent ----------------------------------------
    std::vector<unsigned char> dbuf(kDropLen, 0x5A);
    uint64_t filled = 0;
    bool fill_ok = true;
    while (filled < 10000) {
        const int rc = shuttle_write(prod, dbuf.data(), kDropLen,
                                     SHUTTLE_NONBLOCK);
        if (rc == SHUTTLE_ERR_WOULD_BLOCK) break;
        if (rc != SHUTTLE_OK) {
            CHECK(false, "ring-fill write rc=%d", rc);
            fill_ok = false;
            break;
        }
        ++filled;
        exp_written += 1;
        exp_bytes_w += kDropLen;
    }
    CHECK(!fill_ok || filled < 10000, "the ring never reported WOULD_BLOCK");
    // Falsifiable: a burst on a ring that was never full returns OK, not
    // DROPPED, so the floor below is what makes the DROPPED checks mean
    // anything.
    CHECK(filled >= 4, "ring filled after only %llu writes",
          (unsigned long long)filled);
    uint64_t dropped = 0;
    for (uint64_t i = 0; i < kDrops; ++i) {
        const int rc = shuttle_write(prod, dbuf.data(), kDropLen,
                                     SHUTTLE_DROP_NEWEST);
        CHECK(rc == SHUTTLE_DROPPED, "DROP_NEWEST on a full ring rc=%d", rc);
        if (rc != SHUTTLE_DROPPED) break;
        ++dropped;
    }
    CHECK(dropped == kDrops, "%llu of %llu writes dropped",
          (unsigned long long)dropped, (unsigned long long)kDrops);

    // Drain what the fill queued, so the final counters are exact.
    std::vector<unsigned char> dout(kMaxPayload);
    for (uint64_t i = 0; i < filled; ++i) {
        const long n = shuttle_read(cons, dout.data(), dout.size(),
                                    SHUTTLE_NONBLOCK);
        CHECK(n == static_cast<long>(kDropLen), "drain read %llu n=%ld",
              (unsigned long long)i, n);
        if (n != static_cast<long>(kDropLen)) break;
        exp_read += 1;
        exp_bytes_r += kDropLen;
    }
    CHECK(shuttle_read(cons, dout.data(), dout.size(), SHUTTLE_NONBLOCK) ==
              SHUTTLE_ERR_WOULD_BLOCK,
          "channel must be empty after the drain");
    size_t peek_len = 0;
    CHECK(shuttle_peek_next(cons, &peek_len) == SHUTTLE_ERR_WOULD_BLOCK,
          "peek must be WOULD_BLOCK on the drained channel");

    // ---- (e) exact counters, from a bound handle and the role-less one ----
    shuttle_channel* handles[2] = {prod, watch};
    const char* labels[2] = {"producer handle", "role-less watcher"};
    for (int k = 0; k < 2; ++k) {
        shuttle_stats s;
        std::memset(&s, 0xAA, sizeof s);
        CHECK(shuttle_get_stats(handles[k], &s) == SHUTTLE_OK,
              "final get_stats via %s", labels[k]);
        CHECK(s.msgs_written == exp_written && s.bytes_written == exp_bytes_w &&
                  s.msgs_dropped == kDrops && s.msgs_read == exp_read &&
                  s.bytes_read == exp_bytes_r,
              "final counters via %s: w=%llu/%llu d=%llu r=%llu/%llu want "
              "w=%llu/%llu d=%llu r=%llu/%llu",
              labels[k], (unsigned long long)s.msgs_written,
              (unsigned long long)s.bytes_written,
              (unsigned long long)s.msgs_dropped,
              (unsigned long long)s.msgs_read,
              (unsigned long long)s.bytes_read,
              (unsigned long long)exp_written, (unsigned long long)exp_bytes_w,
              (unsigned long long)kDrops, (unsigned long long)exp_read,
              (unsigned long long)exp_bytes_r);
    }

    std::printf("  drops: %llu queued then %llu DROP_NEWEST writes dropped, "
                "counters exact\n",
                (unsigned long long)filled, (unsigned long long)dropped);

    shuttle::close(obs);
    shuttle_close(watch);
    shuttle_close(cons);
    shuttle_close(prod);
    CHECK(shuttle_unlink(name) == SHUTTLE_OK, "unlink(stress channel)");
    CHECK(shuttle_unlink(name) == SHUTTLE_ERR_NOT_FOUND,
          "second unlink must be NOT_FOUND — nothing survived close");
}

}  // namespace

int main() {
    run_errors();
    run_stress();
    if (fails == 0) {
        std::printf("cabi_threads ok: src/shuttle_c.cpp compiled INTO this "
                    "binary and driven across its full surface by four threads "
                    "in one process — handle bookkeeping, borrow caching, "
                    "stats plumbing and error translation all under the "
                    "sanitizer (platform=%s)\n",
                    shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}
