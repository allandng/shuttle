// WP5: the opt-in drop-newest backpressure policy (SHUTTLE_DROP_NEWEST, and
// the ABI's first positive return, SHUTTLE_DROPPED). Driver + posix_spawn
// child, matching stats_test's pattern.
//
// What must hold, and what each case pins down:
//
//   a. NEVER CORRUPTS. Fill a small ring with known payloads, then write with
//      SHUTTLE_DROP_NEWEST: the call returns SHUTTLE_DROPPED promptly (it may
//      not park), and the ring afterwards contains EXACTLY the pre-drop
//      messages, byte-identical and in order, with nothing extra behind them.
//   b. COUNTED, and only in the segment that has counters. On a
//      SHUTTLE_CREATE_STATS segment msgs_dropped equals the exact number of
//      drops (observed in-process AND from a third process, since the counter
//      is segment state) while msgs_written/bytes_written are untouched by
//      drops. On a v1 segment the identical calls still return SHUTTLE_DROPPED
//      — the policy works — and shuttle_get_stats still says NO_STATS.
//   c. Not sticky: once the consumer drains, a DROP_NEWEST write succeeds with
//      SHUTTLE_OK and the message arrives byte-exact.
//   d. Flag misuse is refused, not ignored: DROP_NEWEST on shuttle_read,
//      shuttle_acquire_write and shuttle_acquire_read is INVALID_ARGS. Each
//      rejection is preceded by a POSITIVE CONTROL — the same call with
//      SHUTTLE_NONBLOCK on the same handle — so an INVALID_ARGS verdict can
//      only come from the flag.
//   e. SHUTTLE_NONBLOCK|SHUTTLE_DROP_NEWEST is redundant-but-accepted: same
//      SHUTTLE_DROPPED on a full ring.
//   f. NEVER A DEFAULT. On the very same full ring, in the same state: plain
//      SHUTTLE_NONBLOCK returns WOULD_BLOCK, DROP_NEWEST returns DROPPED, and
//      flags == 0 does not return at all — a blocking write is still parked
//      200 ms later and only completes once the consumer frees space. Three
//      distinct outcomes from three flag words is the proof that the lossy
//      path is reachable only by asking for it.
//   g. Threaded churn for TSan: a slow consumer holding borrows while the
//      producer alternates plain-nonblock and drop-flag writes, plus a third
//      handle polling get_stats throughout (which puts the drop counter's
//      relaxed store in front of the race detector against a concurrent
//      reader). The consumer's received stream must equal EXACTLY the
//      successfully-written messages, in order, byte-exact: drops may lose
//      messages, never corrupt or reorder the survivors.
//
// FALSIFIABILITY for (a) — the case that could otherwise always pass. The
// dropped messages carry ids from a disjoint range (kDropId..) and lengths
// unlike the queued ones, so:
//   - if a drop wrote its payload into the ring, the drain trips
//     "drain: id mismatch" (the dropped id surfaces) or "drain: length";
//   - if a drop advanced the write cursor without payload, the drain trips
//     "drain: extra message" (an unexpected message after the last known one)
//     or "drain: corrupt/length";
//   - if a drop clobbered queued bytes, "drain: payload byte mismatch" fires;
//   - if the drop path had blocked instead, it parks with nothing draining the
//     ring, so it either trips expect_drop_batch's elapsed-time bound or comes
//     back PEER_DEAD — both fail as "not DROPPED".
// Both halves were run inverted while writing this test, not merely asserted:
//   - the commented-out wrong expectation in (a) (which claims the dropped id
//     is queued) fails with "deliberately wrong expectation: ring short by 2
//     messages" — after a correct drain the ring is EMPTY, because a dropped
//     message never entered it;
//   - an implementation patched to return SHUTTLE_OK instead of
//     SHUTTLE_DROPPED (silent loss, the worst plausible bug here) is caught by
//     five independent assertions, ending with (g)'s "received 34 of 1016
//     written messages".
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"  // shuttle::open / Stats / platform_name
#include "shuttle/shuttle_c.h"  // the surface under test

namespace {

constexpr uint64_t kChildTimeoutNs = 10ull * 1000000000ull;

// Small ring on purpose: full after a couple of dozen messages, so "full" is
// reached deterministically and single-threaded.
constexpr size_t kCapacity = 2048;
constexpr size_t kMaxPayload = 512;
constexpr size_t kFixedLen = 100;

// Disjoint id ranges, so a stray payload is identifiable on sight.
constexpr uint32_t kDropId = 2000;        // (a)/(e) drop attempts
constexpr uint32_t kBlockedId = 4000;     // (f) the blocking writer's message
constexpr uint32_t kAfterDrainId = 5000;  // (c)
constexpr int kDrops = 5;

// (g) threaded churn.
constexpr uint32_t kChurnMsgs = 2000;
constexpr uint32_t kSentinelId = 0xFFFFFFFFu;
constexpr size_t kChurnMax = kChurnMsgs + 4;

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

unsigned char pattern_byte(uint32_t msg, size_t i) {
    return static_cast<unsigned char>(msg * 131u + i * 31u + 7u);
}

// Message layout: 4-byte little-endian id, then pattern bytes. len >= 4.
void fill_msg(unsigned char* buf, uint32_t id, size_t len) {
    for (unsigned i = 0; i < 4; ++i)
        buf[i] = static_cast<unsigned char>((id >> (8 * i)) & 0xFFu);
    for (size_t i = 4; i < len; ++i) buf[i] = pattern_byte(id, i);
}

uint32_t msg_id(const unsigned char* buf) {
    uint32_t id = 0;
    for (unsigned i = 0; i < 4; ++i)
        id |= static_cast<uint32_t>(buf[i]) << (8 * i);
    return id;
}

// Byte-exact check of a received message against (id, len).
int check_msg(const char* what, const unsigned char* buf, long n, uint32_t id,
              size_t len) {
    if (n < 0 || static_cast<size_t>(n) != len) {
        std::fprintf(stderr, "FAIL: %s: length %ld want %zu (id %u)\n", what, n,
                     len, id);
        return 1;
    }
    const uint32_t got = msg_id(buf);
    if (got != id) {
        std::fprintf(stderr, "FAIL: %s: id mismatch, got %u want %u\n", what,
                     got, id);
        return 1;
    }
    for (size_t i = 4; i < len; ++i) {
        if (buf[i] != pattern_byte(id, i)) {
            std::fprintf(stderr,
                         "FAIL: %s: payload byte mismatch at %zu (id %u)\n",
                         what, i, id);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- children

// Role "observe": open the segment from a fresh process and check
// msgs_written/msgs_dropped. The point is that a drop is recorded in the
// SEGMENT, not in the producer handle that dropped it.
int run_observe(const char* name, const char* spec) {
    unsigned long long want_written = 0, want_dropped = 0;
    if (std::sscanf(spec, "%llu:%llu", &want_written, &want_dropped) != 2)
        return fail("observe: bad spec", 0);
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return fail("observe: open()", err);
    shuttle::Stats s{};
    int rc = shuttle::get_stats(ch, s);
    if (rc != shuttle::kOk) {
        rc = fail("observe: get_stats", rc);
    } else if (s.msgs_written != want_written) {
        rc = fail("observe: msgs_written", static_cast<long>(s.msgs_written));
    } else if (s.msgs_dropped != want_dropped) {
        rc = fail("observe: msgs_dropped", static_cast<long>(s.msgs_dropped));
    } else {
        rc = 0;
    }
    shuttle::close(ch);
    return rc;
}

// ----------------------------------------------------------- driver helpers

int expect_observed(const char* self, const char* name, uint64_t written,
                    uint64_t dropped) {
    char spec[64];
    std::snprintf(spec, sizeof spec, "%llu:%llu",
                  static_cast<unsigned long long>(written),
                  static_cast<unsigned long long>(dropped));
    return shuttle_test::run_child_sync(self, "observe", name, spec,
                                        kChildTimeoutNs);
}

int check_stats(const char* what, shuttle_channel* ch, uint64_t mw, uint64_t bw,
                uint64_t md, uint64_t mr) {
    shuttle_stats s;
    std::memset(&s, 0xAA, sizeof s);  // poison: every field must be written
    const int rc = shuttle_get_stats(ch, &s);
    if (rc != SHUTTLE_OK) return fail(what, rc);
    if (s.msgs_written != mw || s.bytes_written != bw || s.msgs_dropped != md ||
        s.msgs_read != mr) {
        std::fprintf(stderr,
                     "FAIL: %s counters w=%llu/%llu d=%llu r=%llu "
                     "want w=%llu/%llu d=%llu r=%llu\n",
                     what, (unsigned long long)s.msgs_written,
                     (unsigned long long)s.bytes_written,
                     (unsigned long long)s.msgs_dropped,
                     (unsigned long long)s.msgs_read, (unsigned long long)mw,
                     (unsigned long long)bw, (unsigned long long)md,
                     (unsigned long long)mr);
        return 1;
    }
    return 0;
}

// Fill the ring with kFixedLen messages, ids 0.., until the ring says full.
// Returns the count written, or -1 on an unexpected code.
long fill_ring(shuttle_channel* prod) {
    unsigned char buf[kFixedLen];
    long n = 0;
    for (;;) {
        fill_msg(buf, static_cast<uint32_t>(n), kFixedLen);
        const int rc = shuttle_write(prod, buf, kFixedLen, SHUTTLE_NONBLOCK);
        if (rc == SHUTTLE_ERR_WOULD_BLOCK) return n;
        if (rc != SHUTTLE_OK) {
            fail("fill: write", rc);
            return -1;
        }
        ++n;
        if (n > 1000) {  // a 2 KB ring cannot hold this; something is wrong
            fail("fill: ring never filled", n);
            return -1;
        }
    }
}

// `count` DROP_NEWEST writes on a full ring, each of which must return
// SHUTTLE_DROPPED. Also the never-parks assertion: nothing drains the ring
// during this call, so a path that parked would sit here for the full 5 s
// staleness window (or forever). The bound is 1 s against a code path that
// makes no syscall at all — three orders of magnitude of headroom, so this is
// not a latency measurement and cannot flap on a loaded machine.
int expect_drop_batch(shuttle_channel* prod, uint32_t first_id, int count,
                      int flags) {
    unsigned char buf[kFixedLen];
    const uint64_t t0 = shuttle::monotonic_ns();
    for (int i = 0; i < count; ++i) {
        fill_msg(buf, first_id + static_cast<uint32_t>(i), kFixedLen);
        const int rc = shuttle_write(prod, buf, kFixedLen, flags);
        if (rc != SHUTTLE_DROPPED) return fail("drop write: not DROPPED", rc);
    }
    const uint64_t elapsed = shuttle::monotonic_ns() - t0;
    if (elapsed > 1000ull * 1000000ull)
        return fail("drop write: parked (elapsed ms)",
                    static_cast<long>(elapsed / 1000000));
    return 0;
}

// Drain and byte-check `count` messages with the given ids, then require the
// ring to be empty.
int drain_and_check(const char* what, shuttle_channel* cons,
                    const uint32_t* ids, size_t count, size_t len) {
    unsigned char out[kMaxPayload];
    for (size_t i = 0; i < count; ++i) {
        const long n = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
        if (n == SHUTTLE_ERR_WOULD_BLOCK) {
            std::fprintf(stderr, "FAIL: %s: ring short by %zu messages\n", what,
                         count - i);
            return 1;
        }
        if (check_msg(what, out, n, ids[i], len) != 0) return 1;
    }
    const long extra = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
    if (extra != SHUTTLE_ERR_WOULD_BLOCK) {
        std::fprintf(stderr,
                     "FAIL: %s: extra message after drain (rc=%ld, id=%u)\n",
                     what, extra, extra >= 4 ? msg_id(out) : 0u);
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ cases

// (a) (b) (c) (e) (f) on one SHUTTLE_CREATE_STATS ring.
int run_stats_ring(const char* self, const char* name) {
    int fails = 0;
    int err = 0;
    shuttle_channel* prod = shuttle_create_ex(name, kCapacity, kMaxPayload,
                                              SHUTTLE_CREATE_STATS, &err);
    if (prod == nullptr) return fail("create_ex(STATS)", err);
    shuttle_channel* cons = shuttle_open(name, &err);
    if (cons == nullptr) {
        shuttle_close(prod);
        return fail("open(consumer)", err);
    }

    const long filled = fill_ring(prod);
    if (filled <= 0) {
        shuttle_close(cons);
        shuttle_close(prod);
        return 1;
    }
    const uint64_t written_bytes = static_cast<uint64_t>(filled) * kFixedLen;

    // (f) THREE flag words, ONE ring state — this is the never-a-default
    // proof. First: the default try path still reports backpressure.
    unsigned char probe[kFixedLen];
    fill_msg(probe, kDropId - 1, kFixedLen);
    const int wb = shuttle_write(prod, probe, kFixedLen, SHUTTLE_NONBLOCK);
    if (wb != SHUTTLE_ERR_WOULD_BLOCK)
        fails += fail("full ring: NONBLOCK must be WOULD_BLOCK", wb);

    // (a) Second: the opt-in path drops instead, promptly.
    fails += expect_drop_batch(prod, kDropId, kDrops, SHUTTLE_DROP_NEWEST);
    // (e) redundant-but-accepted combination, same verdict.
    fails += expect_drop_batch(prod, kDropId + 100, 1,
                               SHUTTLE_NONBLOCK | SHUTTLE_DROP_NEWEST);

    // (b) drops counted exactly; the write counters are untouched by them.
    fails += check_stats("after drops", prod, static_cast<uint64_t>(filled),
                         written_bytes, kDrops + 1, 0);
    fails += check_stats("after drops (consumer handle)", cons,
                         static_cast<uint64_t>(filled), written_bytes,
                         kDrops + 1, 0);
    // ...and a third process reading the segment agrees: the counter is
    // segment state, not handle bookkeeping.
    if (expect_observed(self, name, static_cast<uint64_t>(filled),
                        kDrops + 1) != 0) {
        std::fprintf(stderr, "FAIL: child opener disagrees on msgs_dropped\n");
        ++fails;
    }

    // (f) Third: flags == 0 on the SAME full ring still parks. If the drop
    // policy had leaked into the default, this thread would return at once.
    std::atomic<int> phase{0};  // 0 = not entered, 1 = in write, 2 = returned
    std::atomic<int> wrc{0};
    std::thread blocker([&] {
        unsigned char b[kFixedLen];
        fill_msg(b, kBlockedId, kFixedLen);
        phase.store(1);
        const int rc = shuttle_write(prod, b, kFixedLen, 0);  // blocking
        wrc.store(rc);
        phase.store(2);
    });
    // 200 ms is two full park timeouts (100 ms each), so a writer that was
    // going to give up spuriously has had the chance to.
    const uint64_t deadline = shuttle::monotonic_ns() + 200ull * 1000000ull;
    while (shuttle::monotonic_ns() < deadline) usleep(2000);
    if (phase.load() == 2) {
        ++fails;
        fail("blocking write returned on a full ring (rc)", wrc.load());
    }
    // Free space; the parked writer must now complete. Two messages, not one:
    // the ring is full at its high end, so the writer needs an early wrap, and
    // try_reserve's wrap test is the STRICT `read > n` that keeps wrapped-full
    // from aliasing linear-empty — one freed 108-byte frame is exactly not
    // enough for another.
    unsigned char out[kMaxPayload];
    for (uint32_t i = 0; i < 2; ++i) {
        const long n = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
        fails += check_msg("drain: head message", out, n, i, kFixedLen);
    }
    blocker.join();
    if (wrc.load() != SHUTTLE_OK)
        fails += fail("blocking write did not complete with OK", wrc.load());

    // (a) The whole point: what survived is exactly what was written, in
    // order, byte-identical — the dropped ids never entered the ring.
    uint32_t expect[1024];
    size_t n_expect = 0;
    for (long i = 2; i < filled; ++i)
        expect[n_expect++] = static_cast<uint32_t>(i);
    expect[n_expect++] = kBlockedId;
    fails += drain_and_check("drain", cons, expect, n_expect, kFixedLen);
    // Falsifiability: uncomment to watch (a) fail — the drop id is NOT in the
    // ring, and this asserts that it is.
    // {
    //     uint32_t wrong[2] = {kDropId, kBlockedId};
    //     fails += drain_and_check("deliberately wrong expectation", cons,
    //                              wrong, 2, kFixedLen);
    // }

    // (c) Not sticky: with space free, the same flag writes normally.
    unsigned char buf[kFixedLen];
    fill_msg(buf, kAfterDrainId, kFixedLen);
    const int ok = shuttle_write(prod, buf, kFixedLen, SHUTTLE_DROP_NEWEST);
    if (ok != SHUTTLE_OK) {
        fails += fail("DROP_NEWEST on an empty ring must be OK", ok);
    } else {
        const long n = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
        fails +=
            check_msg("after-drain message", out, n, kAfterDrainId, kFixedLen);
    }

    // An oversize payload is a caller bug, not backpressure: still TOO_LARGE,
    // never swallowed as a drop (and not counted as one).
    unsigned char big[kMaxPayload + 16];
    std::memset(big, 0x5A, sizeof big);
    const int too_big =
        shuttle_write(prod, big, sizeof big, SHUTTLE_DROP_NEWEST);
    if (too_big != SHUTTLE_ERR_MSG_TOO_LARGE)
        fails += fail("oversize + DROP_NEWEST must be MSG_TOO_LARGE", too_big);

    // Final tally: two more messages written (the parked one and (c)'s), the
    // drop count unchanged by any of the above.
    fails += check_stats("final", prod, static_cast<uint64_t>(filled) + 2,
                         written_bytes + 2 * kFixedLen, kDrops + 1,
                         static_cast<uint64_t>(filled) + 2);

    shuttle_close(cons);
    shuttle_close(prod);
    return fails;
}

// (b, v1 half) The policy works on a version-1 segment too — the drop happens,
// there is simply nowhere to count it.
int run_v1_ring(const char* name) {
    int fails = 0;
    int err = 0;
    shuttle_channel* prod = shuttle_create(name, kCapacity, kMaxPayload, &err);
    if (prod == nullptr) return fail("create(v1)", err);
    shuttle_channel* cons = shuttle_open(name, &err);
    if (cons == nullptr) {
        shuttle_close(prod);
        return fail("open(v1 consumer)", err);
    }

    const long filled = fill_ring(prod);
    if (filled <= 0) {
        shuttle_close(cons);
        shuttle_close(prod);
        return 1;
    }
    fails += expect_drop_batch(prod, kDropId, kDrops, SHUTTLE_DROP_NEWEST);

    shuttle_stats s;
    const int grc = shuttle_get_stats(prod, &s);
    if (grc != SHUTTLE_ERR_NO_STATS)
        fails += fail("v1 get_stats after drops must be NO_STATS", grc);

    // The uncounted drops must still have left the ring untouched — this is
    // the case that would catch a drop path writing into the bytes a v2 header
    // would have used (they are payload here).
    uint32_t expect[1024];
    size_t n_expect = 0;
    for (long i = 0; i < filled; ++i)
        expect[n_expect++] = static_cast<uint32_t>(i);
    fails += drain_and_check("v1 drain", cons, expect, n_expect, kFixedLen);

    shuttle_close(cons);
    shuttle_close(prod);
    return fails;
}

// (d) The flag is rejected, not ignored, everywhere it is meaningless. Every
// rejection is paired with a positive control on the same handle.
int run_flag_misuse(const char* name) {
    int fails = 0;
    int err = 0;
    shuttle_channel* prod = shuttle_create(name, kCapacity, kMaxPayload, &err);
    if (prod == nullptr) return fail("create(misuse)", err);
    shuttle_channel* cons = shuttle_open(name, &err);
    if (cons == nullptr) {
        shuttle_close(prod);
        return fail("open(misuse consumer)", err);
    }
    unsigned char buf[kFixedLen], out[kMaxPayload];
    fill_msg(buf, 1, kFixedLen);

    // --- shuttle_read -----------------------------------------------------
    // Control: the same handle, same empty ring, NONBLOCK -> WOULD_BLOCK. So
    // INVALID_ARGS below cannot be "this handle refuses everything".
    const long ctl_read = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
    if (ctl_read != SHUTTLE_ERR_WOULD_BLOCK)
        fails += fail("control: empty read must be WOULD_BLOCK", ctl_read);
    const long r1 = shuttle_read(cons, out, sizeof out, SHUTTLE_DROP_NEWEST);
    if (r1 != SHUTTLE_ERR_INVALID_ARGS)
        fails += fail("read + DROP_NEWEST must be INVALID_ARGS", r1);
    const long r2 = shuttle_read(cons, out, sizeof out,
                                 SHUTTLE_NONBLOCK | SHUTTLE_DROP_NEWEST);
    if (r2 != SHUTTLE_ERR_INVALID_ARGS)
        fails += fail("read + NONBLOCK|DROP_NEWEST must be INVALID_ARGS", r2);

    // --- shuttle_acquire_read ---------------------------------------------
    const void* rp = nullptr;
    size_t rlen = 0;
    const int ctl_ar =
        shuttle_acquire_read(cons, &rp, &rlen, SHUTTLE_NONBLOCK);  // control
    if (ctl_ar != SHUTTLE_ERR_WOULD_BLOCK)
        fails +=
            fail("control: empty acquire_read must be WOULD_BLOCK", ctl_ar);
    const int ar = shuttle_acquire_read(cons, &rp, &rlen, SHUTTLE_DROP_NEWEST);
    if (ar != SHUTTLE_ERR_INVALID_ARGS)
        fails += fail("acquire_read + DROP_NEWEST must be INVALID_ARGS", ar);

    // --- shuttle_acquire_write --------------------------------------------
    void* awp = nullptr;
    const int aw =
        shuttle_acquire_write(prod, &awp, kFixedLen, SHUTTLE_DROP_NEWEST);
    if (aw != SHUTTLE_ERR_INVALID_ARGS)
        fails += fail("acquire_write + DROP_NEWEST must be INVALID_ARGS", aw);
    // Control AFTER the rejection, which also proves the refused call left no
    // reservation behind: this acquire would be INVALID_ARGS if one had.
    void* wp = nullptr;
    const int ctl_aw =
        shuttle_acquire_write(prod, &wp, kFixedLen, SHUTTLE_NONBLOCK);
    if (ctl_aw != SHUTTLE_OK) {
        fails += fail("control: acquire_write(NONBLOCK) must be OK", ctl_aw);
    } else {
        fill_msg(static_cast<unsigned char*>(wp), 7, kFixedLen);
        if (shuttle_commit_write(prod, kFixedLen) != SHUTTLE_OK)
            fails += fail("control: commit_write", 0);
        const long n = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
        fails += check_msg("control: acquired message", out, n, 7, kFixedLen);
    }

    shuttle_close(cons);
    shuttle_close(prod);
    return fails;
}

// (g) Threaded churn. Producer alternates plain-nonblock and drop-flag writes
// on a ring far too small for the traffic; consumer is deliberately slow and
// holds each borrow. Nothing here retries a failed write: whatever the ring
// refused is gone on purpose, and the survivors must arrive in order.
struct ChurnState {
    uint32_t sent[kChurnMax];  // ids that a write reported as WRITTEN
    size_t n_sent = 0;
    uint32_t got[kChurnMax];  // ids the consumer actually received
    size_t n_got = 0;
    uint64_t drops = 0;    // DROPPED verdicts
    uint64_t blocked = 0;  // WOULD_BLOCK verdicts (plain nonblock)
    std::atomic<int> perr{0};
    std::atomic<int> cerr{0};
    std::atomic<bool> done{false};
};

uint32_t churn_len(uint32_t i) {
    return 8 + (i * 29u) % 200u;  // >= 4 for the id, varied to force wraps
}

int run_churn(const char* name) {
    int fails = 0;
    int err = 0;
    shuttle_channel* prod = shuttle_create_ex(name, kCapacity, kMaxPayload,
                                              SHUTTLE_CREATE_STATS, &err);
    if (prod == nullptr) return fail("create_ex(churn)", err);
    shuttle_channel* cons = shuttle_open(name, &err);
    shuttle_channel* watch = shuttle_open(name, &err);
    if (cons == nullptr || watch == nullptr) {
        shuttle_close(cons);
        shuttle_close(watch);
        shuttle_close(prod);
        return fail("open(churn handles)", err);
    }

    ChurnState* st = new ChurnState();

    std::thread producer([&] {
        unsigned char buf[kMaxPayload];
        for (uint32_t i = 0; i < kChurnMsgs; ++i) {
            const uint32_t len = churn_len(i);
            fill_msg(buf, i, len);
            // Alternate the two never-parking policies. Neither retries.
            const int flags = (i & 1u) ? SHUTTLE_DROP_NEWEST : SHUTTLE_NONBLOCK;
            const int rc = shuttle_write(prod, buf, len, flags);
            if (rc == SHUTTLE_OK) {
                st->sent[st->n_sent++] = i;
            } else if (rc == SHUTTLE_DROPPED) {
                if (flags != SHUTTLE_DROP_NEWEST) {  // must never leak
                    st->perr.store(-100);
                    return;
                }
                ++st->drops;
            } else if (rc == SHUTTLE_ERR_WOULD_BLOCK) {
                if (flags !=
                    SHUTTLE_NONBLOCK) {  // drop flag must not report it
                    st->perr.store(-101);
                    return;
                }
                ++st->blocked;
            } else {
                st->perr.store(rc);
                return;
            }
        }
        // Sentinel: the one blocking write in this case, so the consumer has a
        // definite stopping point even though every other write may be lost.
        const uint32_t len = churn_len(0);
        fill_msg(buf, kSentinelId, len);
        const int rc = shuttle_write(prod, buf, len, 0);
        if (rc != SHUTTLE_OK) {
            st->perr.store(rc);
            return;
        }
        st->sent[st->n_sent++] = kSentinelId;
    });

    std::thread consumer([&] {
        // Whatever exit the body takes, the watcher below must stop polling.
        auto body = [&] {
            for (;;) {
                const void* p = nullptr;
                size_t len = 0;
                const int rc =
                    shuttle_acquire_read(cons, &p, &len, 0);  // blocking
                if (rc != SHUTTLE_OK) {
                    st->cerr.store(rc);
                    return;
                }
                const unsigned char* q = static_cast<const unsigned char*>(p);
                const uint32_t id = msg_id(q);
                // Verify in place, while the borrow is still held: this is the
                // window in which a producer that overwrote queued data would
                // be caught (and the one TSan watches).
                const uint32_t want_len =
                    id == kSentinelId ? churn_len(0) : churn_len(id);
                if (len != want_len) {
                    st->cerr.store(-200);
                    shuttle_release_read(cons);
                    return;
                }
                for (size_t i = 4; i < len; ++i) {
                    if (q[i] != pattern_byte(id, i)) {
                        st->cerr.store(-201);
                        shuttle_release_read(cons);
                        return;
                    }
                }
                if (st->n_got >= kChurnMax) {
                    st->cerr.store(-202);
                    shuttle_release_read(cons);
                    return;
                }
                st->got[st->n_got++] = id;
                // Hold the borrow a little: a slow consumer is the whole point,
                // and it is what makes the producer's ring full often enough
                // for the drop path to fire thousands of times.
                usleep(20);
                if (shuttle_release_read(cons) != SHUTTLE_OK) {
                    st->cerr.store(-203);
                    return;
                }
                if (id == kSentinelId) return;
            }
        };
        body();
        st->done.store(true, std::memory_order_relaxed);
    });

    // Third-party observer: reads all five counters while the producer is
    // writing AND dropping. Monotonicity is the only claim that survives
    // unsynchronized sampling — and this is what puts the drop counter's
    // relaxed store in front of TSan against a concurrent relaxed load.
    int watch_err = 0;
    shuttle_stats prev;
    std::memset(&prev, 0, sizeof prev);
    while (!st->done.load(std::memory_order_relaxed)) {
        shuttle_stats s;
        if (shuttle_get_stats(watch, &s) != SHUTTLE_OK) {
            watch_err = 1;
            break;
        }
        if (s.msgs_written < prev.msgs_written ||
            s.bytes_written < prev.bytes_written ||
            s.msgs_dropped < prev.msgs_dropped ||
            s.msgs_read < prev.msgs_read) {
            watch_err = 2;
            break;
        }
        prev = s;
        if (st->perr.load() != 0) break;
        std::this_thread::yield();
        usleep(200);
    }
    producer.join();
    consumer.join();

    if (st->perr.load() != 0) fails += fail("churn: producer", st->perr.load());
    if (st->cerr.load() != 0) fails += fail("churn: consumer", st->cerr.load());
    if (watch_err == 1) fails += fail("churn: get_stats failed mid-run", 0);
    if (watch_err == 2) fails += fail("churn: counter went backward", 0);

    // The claim: received == successfully-written, EXACTLY, in order. Not a
    // subsequence check with slack — every message a write reported as OK must
    // arrive, and nothing else may.
    if (fails == 0) {
        if (st->n_got != st->n_sent) {
            std::fprintf(stderr,
                         "FAIL: churn: received %zu of %zu written messages\n",
                         st->n_got, st->n_sent);
            ++fails;
        } else {
            for (size_t i = 0; i < st->n_sent; ++i) {
                if (st->got[i] != st->sent[i]) {
                    std::fprintf(stderr,
                                 "FAIL: churn: stream diverges at %zu "
                                 "(got id %u, wrote id %u)\n",
                                 i, st->got[i], st->sent[i]);
                    ++fails;
                    break;
                }
            }
        }
    }
    // The drop path must actually have fired, or this case proves nothing.
    if (fails == 0 && st->drops == 0) {
        std::fprintf(stderr,
                     "FAIL: churn: no drops occurred — case is vacuous\n");
        ++fails;
    }
    // And the counter must equal the number of DROPPED verdicts the producer
    // saw, exactly.
    if (fails == 0) {
        shuttle_stats s;
        if (shuttle_get_stats(watch, &s) != SHUTTLE_OK) {
            fails += fail("churn: final get_stats", 0);
        } else if (s.msgs_dropped != st->drops) {
            std::fprintf(stderr,
                         "FAIL: churn: msgs_dropped=%llu, producer counted "
                         "%llu DROPPED returns\n",
                         (unsigned long long)s.msgs_dropped,
                         (unsigned long long)st->drops);
            ++fails;
        } else if (s.msgs_written != st->n_sent) {
            std::fprintf(stderr, "FAIL: churn: msgs_written=%llu, wrote %zu\n",
                         (unsigned long long)s.msgs_written, st->n_sent);
            ++fails;
        } else {
            std::printf("  churn: %zu written, %llu dropped, %llu would-block, "
                        "%zu received in order\n",
                        st->n_sent, (unsigned long long)st->drops,
                        (unsigned long long)st->blocked, st->n_got);
        }
    }

    delete st;
    shuttle_close(watch);
    shuttle_close(cons);
    shuttle_close(prod);
    return fails;
}

// ------------------------------------------------------------------ driver

int run_driver(const char* self) {
    const int pid = static_cast<int>(getpid()) % 1000000;
    char stats_name[32], v1_name[32], misuse_name[32], churn_name[32];
    std::snprintf(stats_name, sizeof stats_name, "/shdp.s%d", pid);
    std::snprintf(v1_name, sizeof v1_name, "/shdp.v%d", pid);
    std::snprintf(misuse_name, sizeof misuse_name, "/shdp.m%d", pid);
    std::snprintf(churn_name, sizeof churn_name, "/shdp.c%d", pid);
    shuttle_unlink(stats_name);  // clear any stale objects
    shuttle_unlink(v1_name);
    shuttle_unlink(misuse_name);
    shuttle_unlink(churn_name);

    int fails = 0;
    fails += run_stats_ring(self, stats_name);
    fails += run_v1_ring(v1_name);
    fails += run_flag_misuse(misuse_name);
    fails += run_churn(churn_name);

    if (shuttle_unlink(stats_name) != SHUTTLE_OK ||
        shuttle_unlink(v1_name) != SHUTTLE_OK ||
        shuttle_unlink(misuse_name) != SHUTTLE_OK ||
        shuttle_unlink(churn_name) != SHUTTLE_OK) {
        std::fprintf(stderr, "FAIL: unlink left an object behind\n");
        ++fails;
    }

    if (fails == 0) {
        std::printf(
            "drop_policy_test ok: SHUTTLE_DROP_NEWEST drops without "
            "corrupting or reordering, counted exactly on v2 and "
            "uncounted-but-working on v1, rejected on the read/acquire "
            "paths, and never a default (WOULD_BLOCK / DROPPED / parked "
            "from the same full ring) (platform=%s)\n",
            shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 4 && std::strcmp(argv[1], "observe") == 0)
        return run_observe(argv[2], argv[3]);
    std::fprintf(stderr, "usage: %s [observe </shm> <spec>]\n", argv[0]);
    return 2;
}
