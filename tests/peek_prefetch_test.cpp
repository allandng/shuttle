// PIPELINED CONSUMPTION (v1.4): the read-only lookahead shuttle_peek_next /
// Consumer::peek_next, and the WILLNEED prefetch hooks that ride the same
// cursors on a file-backed channel.
//
// Why peek is a new capability rather than a documentation exercise: borrows
// here are strictly release-before-acquire — Consumer::try_read parses at the
// UN-advanced `read` cursor and only release() moves it, so a consumer holding
// message N is handed N again, never N+1. There was therefore no way to learn
// that N+1 had arrived, or how big it was, before committing to release N.
// Peek answers exactly that question and touches nothing: it stores no cursor,
// sets no borrow, and — the subtle part — does NOT perform the A->B handoff
// store when its position reaches the watermark.
//
//   a. SEQUENCE. Empty ring -> WOULD_BLOCK. One message -> its exact length,
//      with no borrow held. Acquire it, write a second -> peek reports message
//      2's length WHILE the borrow on 1 is outstanding (the case that has no
//      other API). Release, acquire 2, peek -> WOULD_BLOCK again.
//   b. WRAP. The ring is engineered so message N+1 sits at offset 0 while the
//      borrow on N is still in the high region, i.e. exactly the state where
//      try_read would perform the handoff. Peek must report N+1's length and
//      leave `read` where it was; the normal acquire that follows must still
//      find the message, so nothing was lost by deferring the handoff.
//   c. TRICKLE. The G4.2 shape (tests/trickle_test.cpp) with peek in the loop:
//      50k park/wake cycles, peek before the read and again with the borrow
//      held, byte-exact, no lost wakeups. This is the concurrency stress — the
//      new atomic traffic runs against a live producer, which is what TSan is
//      here to judge.
//   d. PREFETCH. A file-backed channel drives the advisory hooks (acquire and
//      park), through a wrap so both the linear and the two-run wrapped range
//      computations fire. Behavioral only: WILLNEED is a hint, so the assertion
//      is that the data is byte-exact and nothing crashes. The negative is
//      sharp, though: a default shm Consumer's gate must read FALSE, which is
//      what "the default path is one predictable branch" means.
//   e. CORRUPT. A poked frame header with a length the producer could never
//      have written is CORRUPT from peek — reached by the same guards parse()
//      uses — and the cursor still does not move.
//   f. FALSIFIABILITY (CONTRIBUTING: a test that claims something subtle must
//      demonstrate it can fail). A deliberately buggy peek that performs the
//      handoff store is run against case (b)'s exact state, and the case (b)
//      assertion is shown to fire on it.
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"
#include "shuttle/shuttle_c.h"
#include "shuttle/spsc.hpp"

namespace {

constexpr uint64_t kChildTimeoutNs = 180ull * 1000000000ull;

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

unsigned char fill_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>((msg * 1315423911ull) + i * 151ull +
                                      (i >> 8));
}

// A per-case shm name that cannot collide with a concurrent ctest run.
char* name_for(char* buf, size_t n, const char* tag) {
    std::snprintf(buf, n, "/shpk%s.%d", tag,
                  static_cast<int>(getpid()) % 10000);
    shuttle::unlink(buf);
    return buf;
}

uint64_t cursor_read(shuttle::Channel* ch) {
    return ch->hdr->read.load(std::memory_order_relaxed);
}

// --- (a) the peek sequence ---------------------------------------------------

int case_sequence() {
    char name[64];
    name_for(name, sizeof name, "seq");
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 16, 4096, &err);
    if (ch == nullptr) return fail("sequence: create", err);
    shuttle::Producer p(ch);
    shuttle::Consumer c(ch);
    int fails = 0;
    uint64_t len = 0;

    // Empty: nothing is committed, so there is nothing to look ahead at.
    if (c.peek_next(&len) != shuttle::kErrWouldBlock)
        fails += fail("sequence: peek on an empty ring", 0);
    // A null out-param is a caller bug, not a queue state.
    if (c.peek_next(nullptr) != shuttle::kErrInvalidArgs)
        fails += fail("sequence: peek(nullptr)", 0);

    std::vector<unsigned char> m1(1000), m2(37);
    for (size_t i = 0; i < m1.size(); ++i) m1[i] = fill_byte(1, i);
    for (size_t i = 0; i < m2.size(); ++i) m2[i] = fill_byte(2, i);

    // One message, no borrow: the exact payload length, and the cursor stays.
    if (p.write(m1.data(), m1.size()) != shuttle::kOk)
        fails += fail("sequence: write 1", 0);
    const uint64_t r_before = cursor_read(ch);
    len = 0;
    if (c.peek_next(&len) != shuttle::kOk || len != m1.size())
        fails += fail("sequence: peek reports message 1's length",
                      static_cast<long>(len));
    if (cursor_read(ch) != r_before)
        fails += fail("sequence: peek moved read", 0);

    // Acquire it. The borrow is now outstanding and peek must look PAST it.
    const unsigned char* q = nullptr;
    uint64_t got = 0;
    if (c.try_read(&q, &got) != shuttle::kOk || got != m1.size())
        fails += fail("sequence: acquire 1", static_cast<long>(got));
    if (std::memcmp(q, m1.data(), m1.size()) != 0)
        fails += fail("sequence: message 1 bytes", 0);
    // ...and with nothing behind it, that is WOULD_BLOCK, not a re-report of
    // the message we are holding.
    if (c.peek_next(&len) != shuttle::kErrWouldBlock)
        fails += fail("sequence: peek re-reported the borrowed message", 0);

    // THE CASE THAT HAS NO OTHER API: message 2 arrives while 1 is borrowed.
    if (p.write(m2.data(), m2.size()) != shuttle::kOk)
        fails += fail("sequence: write 2", 0);
    len = 0;
    if (c.peek_next(&len) != shuttle::kOk || len != m2.size())
        fails += fail("sequence: peek past an outstanding borrow",
                      static_cast<long>(len));
    // The borrow is untouched by the peek — same pointer, same bytes.
    if (std::memcmp(q, m1.data(), m1.size()) != 0)
        fails += fail("sequence: peek disturbed the outstanding borrow", 0);
    // And the acquire path still hands back message 1, not message 2: peek is
    // not a substitute for release-before-acquire, it is a window through it.
    const unsigned char* again = nullptr;
    uint64_t again_len = 0;
    if (c.try_read(&again, &again_len) != shuttle::kOk ||
        again_len != m1.size() || again != q)
        fails += fail("sequence: re-acquire returned something else",
                      static_cast<long>(again_len));

    c.release();
    if (c.try_read(&q, &got) != shuttle::kOk || got != m2.size() ||
        std::memcmp(q, m2.data(), m2.size()) != 0)
        fails += fail("sequence: acquire 2", static_cast<long>(got));
    if (c.peek_next(&len) != shuttle::kErrWouldBlock)
        fails += fail("sequence: peek after draining", 0);
    c.release();

    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("peek: sequence ok (empty/one/past-a-borrow/drained)\n");
    return fails == 0 ? 0 : 1;
}

// --- (b) the wrap subtlety ---------------------------------------------------
//
// Builds the one state the plan singled out: `read + borrowed_ == watermark`
// with `write < read`, i.e. the borrow is the last frame in the high region and
// the next message lives at offset 0. Geometry (capacity 4096, classic 8-byte
// framing), driven entirely through the public API:
//
//   write 3000  -> span 3008, w=3008              consume it   -> r=3008
//   write  500  -> span  508, w=3516 (fits: 4096-3008 >= 508)
//   acquire it  -> borrow at 3008, borrowed_=508  (r stays 3008)
//   write  700  -> span  708 does NOT fit in 4096-3516=580, and read=3008>708,
//                  so it early-wraps WHOLE to offset 0: watermark=3516, w=708.
//
// Now read+borrowed_ == 3516 == watermark, write=708 < read=3008.
struct WrapState {
    shuttle::Channel* ch = nullptr;
    uint64_t borrow_span = 0;  // 8 + 500, the span the Consumer is holding
    uint64_t read_at = 0;      // where `read` must still be after a peek
    uint64_t next_len = 0;     // the wrapped message's payload length
};

constexpr uint64_t kWrapCap = 4096;
constexpr uint64_t kWrapMaxPayload = 3000;

// Drives a channel into the state above. Returns 0 on success; the Producer and
// Consumer are the caller's, so it can keep using them.
int build_wrap_state(shuttle::Channel* ch, shuttle::Producer& p,
                     shuttle::Consumer& c, WrapState* out) {
    std::vector<unsigned char> big(3000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = fill_byte(9, i);
    const unsigned char* q = nullptr;
    uint64_t got = 0;
    if (p.write(big.data(), 3000) != shuttle::kOk) return 1;
    if (c.try_read(&q, &got) != shuttle::kOk || got != 3000) return 1;
    c.release();
    if (p.write(big.data(), 500) != shuttle::kOk) return 1;
    // Hold this one: it is the borrow that must still be in the high region.
    if (c.try_read(&q, &got) != shuttle::kOk || got != 500) return 1;
    if (p.write(big.data(), 700) != shuttle::kOk) return 1;
    shuttle::ChannelHeader* h = ch->hdr;
    const uint64_t r = h->read.load(std::memory_order_relaxed);
    const uint64_t w = h->write.load(std::memory_order_relaxed);
    const uint64_t m = h->watermark.load(std::memory_order_relaxed);
    // The state the case is about — assert it, or the case proves nothing.
    if (!(w < r && r + 508 == m)) {
        std::fprintf(stderr, "wrap: state not built (r=%llu w=%llu m=%llu)\n",
                     (unsigned long long)r, (unsigned long long)w,
                     (unsigned long long)m);
        return 1;
    }
    out->ch = ch;
    out->borrow_span = 508;
    out->read_at = r;
    out->next_len = 700;
    return 0;
}

int case_wrap() {
    char name[64];
    name_for(name, sizeof name, "wrap");
    int err = 0;
    shuttle::Channel* ch =
        shuttle::create(name, kWrapCap, kWrapMaxPayload, &err);
    if (ch == nullptr) return fail("wrap: create", err);
    shuttle::Producer p(ch);
    shuttle::Consumer c(ch);
    int fails = 0;
    WrapState st;
    if (build_wrap_state(ch, p, c, &st) != 0) {
        shuttle::close(ch);
        shuttle::unlink(name);
        return fail("wrap: could not build the wrapped state", 0);
    }

    // THE ASSERTION. The next message is at offset 0, reachable only through
    // the handoff try_read would perform — and peek reports it without it.
    uint64_t len = 0;
    if (c.peek_next(&len) != shuttle::kOk || len != st.next_len)
        fails +=
            fail("wrap: peek across the watermark", static_cast<long>(len));
    // ...and `read` is EXACTLY where it was. This is the load-bearing check:
    // case (f) shows it firing against a peek that does the handoff store.
    if (cursor_read(ch) != st.read_at)
        fails += fail("wrap: peek moved the read cursor",
                      static_cast<long>(cursor_read(ch)));
    // Peeking twice is still read-only (an idempotent question).
    len = 0;
    if (c.peek_next(&len) != shuttle::kOk || len != st.next_len ||
        cursor_read(ch) != st.read_at)
        fails += fail("wrap: second peek disagreed or moved read", 0);

    // NO LOST HANDOFF: the ordinary path still works afterwards. Release the
    // high-region borrow, then acquire — that try_read performs the handoff
    // peek declined to perform, and finds the wrapped message.
    c.release();
    const unsigned char* q = nullptr;
    uint64_t got = 0;
    if (c.try_read(&q, &got) != shuttle::kOk || got != st.next_len)
        fails += fail("wrap: acquire after peek lost the handoff",
                      static_cast<long>(got));
    for (uint64_t i = 0; i < got; ++i) {
        if (q[i] != fill_byte(9, i)) {
            fails += fail("wrap: wrapped payload bytes", static_cast<long>(i));
            break;
        }
    }
    if (cursor_read(ch) != 0)
        fails += fail("wrap: handoff did not land read at 0",
                      static_cast<long>(cursor_read(ch)));
    c.release();
    if (c.peek_next(&len) != shuttle::kErrWouldBlock)
        fails += fail("wrap: peek after the ring drained", 0);

    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("peek: wrap ok (reports the message at offset 0, read "
                    "cursor unmoved, handoff still lands)\n");
    return fails == 0 ? 0 : 1;
}

// --- (f) falsifiability ------------------------------------------------------
//
// The deliberately buggy peek CONTRIBUTING asks for: byte-identical to the real
// one except that it takes try_read's A->B handoff store with it. If case (b)'s
// "read cursor unmoved" assertion could not tell the difference, it would be a
// test that cannot fail — so here it is, run on the very same state.
int buggy_peek_with_handoff(shuttle::Channel* ch, uint64_t borrow_span,
                            uint64_t* len_out) {
    shuttle::ChannelHeader* h = ch->hdr;
    const unsigned char* data = static_cast<const unsigned char*>(
        shuttle::resolve(static_cast<const void*>(ch->base), h->data_offset));
    const uint64_t r = h->read.load(std::memory_order_relaxed);
    const uint64_t pos = r + borrow_span;
    const uint64_t w = h->write.load(std::memory_order_acquire);
    uint64_t at = 0;
    if (w < r) {
        const uint64_t m = h->watermark.load(std::memory_order_relaxed);
        if (pos < m) {
            at = pos;
        } else {
            // THE BUG: peek performing the consumer's C2 handoff.
            h->read.store(0, std::memory_order_release);
            at = 0;
        }
    } else {
        if (pos >= w) return shuttle::kErrWouldBlock;
        at = pos;
    }
    uint64_t l = 0;
    for (unsigned i = 0; i < 8; ++i)
        l |= static_cast<uint64_t>(data[at + i]) << (8 * i);
    *len_out = l;
    return shuttle::kOk;
}

int case_falsifiable() {
    char name[64];
    name_for(name, sizeof name, "fals");
    int err = 0;
    shuttle::Channel* ch =
        shuttle::create(name, kWrapCap, kWrapMaxPayload, &err);
    if (ch == nullptr) return fail("falsifiable: create", err);
    shuttle::Producer p(ch);
    shuttle::Consumer c(ch);
    int fails = 0;
    WrapState st;
    if (build_wrap_state(ch, p, c, &st) != 0) {
        shuttle::close(ch);
        shuttle::unlink(name);
        return fail("falsifiable: could not build the wrapped state", 0);
    }

    uint64_t len = 0;
    if (buggy_peek_with_handoff(ch, st.borrow_span, &len) != shuttle::kOk ||
        len != st.next_len)
        fails += fail("falsifiable: the buggy peek did not even report the "
                      "right length (the mutation must be minimal)",
                      static_cast<long>(len));
    // The point: case (b)'s assertion FIRES here.
    if (cursor_read(ch) == st.read_at) {
        fails += fail("falsifiable: the buggy peek left read alone — case (b) "
                      "cannot fail and proves nothing",
                      0);
    } else {
        std::printf("peek: falsifiability ok — a peek that performs the "
                    "handoff moves read %llu -> %llu, which is exactly what "
                    "case (b) asserts against\n",
                    (unsigned long long)st.read_at,
                    (unsigned long long)cursor_read(ch));
        // And the damage is real, not cosmetic: the consumer still holds a
        // borrow in the HIGH region, so its release now advances a cursor that
        // was silently moved to the low region and the queue is desynchronized.
        c.release();
        if (cursor_read(ch) != st.borrow_span)
            fails += fail("falsifiable: release did not compound the bug",
                          static_cast<long>(cursor_read(ch)));
    }

    shuttle::close(ch);
    shuttle::unlink(name);
    return fails == 0 ? 0 : 1;
}

// --- (e) a corrupt length ----------------------------------------------------

int case_corrupt() {
    char name[64];
    name_for(name, sizeof name, "corr");
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 16, 4096, &err);
    if (ch == nullptr) return fail("corrupt: create", err);
    shuttle::Producer p(ch);
    shuttle::Consumer c(ch);
    int fails = 0;
    if (p.write("payload", 7) != shuttle::kOk)
        fails += fail("corrupt: write", 0);

    // Poke the frame's 8-byte length header to a value the producer could never
    // have written (> max_payload). This is the NFR-S2 case: a segment another
    // process could have scribbled on.
    unsigned char* data = static_cast<unsigned char*>(
        shuttle::resolve(ch->base, ch->hdr->data_offset));
    const uint64_t bogus = ch->hdr->max_payload + 1;
    for (unsigned i = 0; i < 8; ++i)
        data[i] = static_cast<unsigned char>((bogus >> (8 * i)) & 0xFF);

    const uint64_t r_before = cursor_read(ch);
    uint64_t len = 0xDEAD;
    if (c.peek_next(&len) != shuttle::kErrCorrupt)
        fails += fail("corrupt: peek did not report CORRUPT", 0);
    if (cursor_read(ch) != r_before)
        fails += fail("corrupt: peek moved read on a corrupt frame", 0);
    // The acquire path reaches the same verdict on the same bytes — peek is not
    // a second, weaker opinion about what is valid.
    const unsigned char* q = nullptr;
    uint64_t got = 0;
    if (c.try_read(&q, &got) != shuttle::kErrCorrupt)
        fails += fail("corrupt: try_read disagreed with peek", 0);

    // A length that is merely absurd (near 2^64) must not overflow the fit
    // check into an accept — the guard is subtraction against a checked floor.
    const uint64_t huge = ~0ull - 3;
    for (unsigned i = 0; i < 8; ++i)
        data[i] = static_cast<unsigned char>((huge >> (8 * i)) & 0xFF);
    if (c.peek_next(&len) != shuttle::kErrCorrupt)
        fails += fail("corrupt: peek accepted a 2^64-ish length", 0);
    if (cursor_read(ch) != r_before)
        fails += fail("corrupt: peek moved read on the huge length", 0);

    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("peek: corrupt ok (CORRUPT from peek, cursor unmoved, "
                    "same verdict as the acquire path)\n");
    return fails == 0 ? 0 : 1;
}

// --- (d) prefetch on a file-backed channel -----------------------------------

constexpr uint64_t kPfCap = 256u << 10;
constexpr uint64_t kPfMaxPayload = 8192;
constexpr uint64_t kPfMsgs = 64;  // 64 x 8 KiB = 512 KiB: the ring laps twice
constexpr uint64_t kPfParkMsgs = 200;

int case_prefetch() {
    const char* tmp = std::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0') tmp = "/tmp";
    char dir[512];
    std::snprintf(dir, sizeof dir, "%s/shuttle-pk.XXXXXX", tmp);
    if (mkdtemp(dir) == nullptr) return fail("prefetch: mkdtemp", 0);
    char path[600];
    std::snprintf(path, sizeof path, "%s/prefetch.seg", dir);

    int fails = 0;
    int err = 0;
    shuttle::Channel* ch =
        shuttle::create_file(path, kPfCap, kPfMaxPayload, &err);
    if (ch == nullptr) {
        ::rmdir(dir);
        return fail("prefetch: create_file", err);
    }
    {
        shuttle::Producer p(ch);
        shuttle::Consumer c(ch);

        // THE GATE, positive side: a file-backed segment turns the advisory
        // hooks on, resolved once from the persisted flag.
        if (!c.prefetching())
            fails += fail("prefetch: gate off on a file-backed channel", 0);
        if ((ch->hdr->flags & shuttle::kFlagFileBacked) == 0)
            fails += fail("prefetch: 0x20 not persisted",
                          static_cast<long>(ch->hdr->flags));

        // Batch through the ring, lapping it twice so the advisory range
        // computation runs in the linear state AND in the wrapped state (where
        // it names two runs). Behavioral assertion only — WILLNEED is a hint,
        // so what is checked is that the bytes are right and nothing faults.
        std::vector<unsigned char> msg(kPfMaxPayload);
        for (uint64_t n = 0; n < kPfMsgs; ++n) {
            for (uint64_t i = 0; i < kPfMaxPayload; ++i)
                msg[i] = fill_byte(n, i);
            if (p.write(msg.data(), kPfMaxPayload) != shuttle::kOk) {
                fails += fail("prefetch: write", static_cast<long>(n));
                break;
            }
            const unsigned char* q = nullptr;
            uint64_t got = 0;
            if (c.try_read(&q, &got) != shuttle::kOk || got != kPfMaxPayload) {
                fails += fail("prefetch: acquire", static_cast<long>(n));
                break;
            }
            uint64_t bad = 0;
            for (uint64_t i = 0; i < got; ++i)
                if (q[i] != fill_byte(n, i)) ++bad;
            if (bad != 0) {
                fails += fail("prefetch: payload bytes", static_cast<long>(n));
                break;
            }
            // Peek and prefetch coexist: with the borrow held, the lookahead
            // still reports nothing (this producer is one message ahead at
            // most) or the next message, never garbage.
            uint64_t plen = 0;
            const int prc = c.peek_next(&plen);
            if (prc != shuttle::kErrWouldBlock &&
                !(prc == shuttle::kOk && plen == kPfMaxPayload))
                fails += fail("prefetch: peek during the batch", prc);
            c.release();
        }
    }

    // The PARK hook (a), which needs a peer: a trickling producer thread makes
    // the consumer park, on a channel whose gate is ON. Small — the point is to
    // execute the hook, and under TSan, not to measure anything.
    {
        shuttle::Producer p(ch);
        shuttle::Consumer c(ch);
        std::atomic<int> prc{0};
        std::thread prod([&] {
            std::vector<unsigned char> m(64);
            for (uint64_t n = 0; n < kPfParkMsgs; ++n) {
                for (uint64_t i = 0; i < m.size(); ++i) m[i] = fill_byte(n, i);
                if (p.write(m.data(), m.size()) != shuttle::kOk) {
                    prc.store(1);
                    return;
                }
                usleep(50);  // long enough that the consumer parks
            }
        });
        for (uint64_t n = 0; n < kPfParkMsgs; ++n) {
            // SINGLE-PROCESS peek traffic against a LIVE producer. The trickle
            // case (c) is the volume stress, but it is two processes, and TSan
            // is a single-process tool (see the memory-ordering contract's
            // note on how FR-17 is discharged) — so the peek loads must also
            // race the producer's stores inside one address space somewhere,
            // and this loop is that somewhere. The answer is unconstrained
            // here (the producer runs free), so only the error code is judged.
            uint64_t plen = 0;
            const int prc = c.peek_next(&plen);
            if (prc != shuttle::kOk && prc != shuttle::kErrWouldBlock) {
                fails += fail("prefetch: concurrent peek", prc);
                break;
            }
            const unsigned char* q = nullptr;
            uint64_t got = 0;
            if (c.read(&q, &got) != shuttle::kOk || got != 64) {
                fails += fail("prefetch: parked read", static_cast<long>(n));
                break;
            }
            uint64_t nlen = 0;
            const int nrc = c.peek_next(&nlen);  // ...and with a borrow held
            if (nrc != shuttle::kOk && nrc != shuttle::kErrWouldBlock) {
                fails += fail("prefetch: concurrent peek behind a borrow", nrc);
                break;
            }
            if (nrc == shuttle::kOk && nlen != 64) {
                fails += fail("prefetch: concurrent peek length",
                              static_cast<long>(nlen));
                break;
            }
            uint64_t bad = 0;
            for (uint64_t i = 0; i < got; ++i)
                if (q[i] != fill_byte(n, i)) ++bad;
            if (bad != 0) {
                fails += fail("prefetch: parked payload bytes",
                              static_cast<long>(n));
                break;
            }
            c.release();
        }
        prod.join();
        if (prc.load() != 0) fails += fail("prefetch: trickle producer", 0);
    }

    shuttle::close(ch);
    if (shuttle::unlink_file(path) != shuttle::kOk)
        fails += fail("prefetch: unlink_file", 0);
    if (::rmdir(dir) != 0)
        std::fprintf(stderr, "warning: temp dir %s not empty\n", dir);

    // THE GATE, negative side: an ordinary shm channel must read FALSE. This is
    // the "provably zero-cost on the default path" claim, checked rather than
    // argued — with the gate off the advisory code is unreachable, and what the
    // default path pays is the one predictable branch that reads this bool.
    char name[64];
    name_for(name, sizeof name, "gate");
    err = 0;
    shuttle::Channel* shm = shuttle::create(name, 1u << 16, 4096, &err);
    if (shm == nullptr) {
        fails += fail("prefetch: create shm", err);
    } else {
        shuttle::Consumer sc(shm);
        if (sc.prefetching())
            fails += fail("prefetch: gate ON for a default shm channel", 0);
        shuttle::close(shm);
        shuttle::unlink(name);
    }

    if (fails == 0)
        std::printf("prefetch: file-backed hooks ok (%llu batched + %llu "
                    "parked messages byte-exact, gate on for kFile / off for "
                    "shm)\n",
                    (unsigned long long)kPfMsgs,
                    (unsigned long long)kPfParkMsgs);
    return fails == 0 ? 0 : 1;
}

// --- (c) the trickle variant, with peek in the loop --------------------------

constexpr uint64_t kTrickleCap = 256ull << 10;
constexpr uint64_t kTrickleMaxPayload = 1024;
constexpr uint64_t kTrickleMsgs = 50000;
constexpr uint64_t kSeed = 0x9E37C0DE;
constexpr uint64_t kSlowReadNs = 50ull * 1000000;  // 50 ms: backstop fired
constexpr uint64_t kMaxSlowReads = 8;

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint64_t msg_len(uint64_t i) { return 16 + splitmix(kSeed ^ i) % 241; }

int run_producer(shuttle::Channel* ch) {
    shuttle::Producer p(ch);
    std::vector<unsigned char> tmp(kTrickleMaxPayload);
    for (uint64_t i = 0; i < kTrickleMsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        if (p.write(tmp.data(), len) != shuttle::kOk) {
            std::fprintf(stderr, "producer: write %llu failed\n",
                         (unsigned long long)i);
            return 1;
        }
        // Trickle, but burst every fourth message (no sleep after it). The
        // trickle is what forces the parks; the bursts are what leave a second
        // message committed while the consumer holds the first, which is the
        // state peek exists for — and the one whose atomic traffic TSan is
        // being asked to judge. Without them the peek would answer WOULD_BLOCK
        // nearly every time and prove very little.
        if ((i & 3) != 3)
            usleep(static_cast<useconds_t>(splitmix(i * 31 + 7) % 41));
    }
    return 0;
}

// peek -> read -> peek-with-the-borrow-held -> release, with a park in between
// nearly every iteration. Two things are on trial at once: that peek never
// lies (every kOk it returns must name the message that actually arrives
// next), and that adding its loads to the loop costs no wakeups.
int run_consumer(shuttle::Channel* ch) {
    shuttle::Consumer c(ch);
    uint64_t slow = 0, max_ns = 0, peeked_before = 0, peeked_after = 0;
    for (uint64_t i = 0; i < kTrickleMsgs; ++i) {
        // (1) Before the read. A trickling producer means this is usually
        // WOULD_BLOCK — and when it is not, it must be exactly right.
        uint64_t plen = 0;
        int prc = c.peek_next(&plen);
        if (prc == shuttle::kOk) {
            ++peeked_before;
            if (plen != msg_len(i)) {
                std::fprintf(stderr,
                             "consumer: peek %llu said %llu, want %llu\n",
                             (unsigned long long)i, (unsigned long long)plen,
                             (unsigned long long)msg_len(i));
                return 1;
            }
        } else if (prc != shuttle::kErrWouldBlock) {
            std::fprintf(stderr, "consumer: peek %llu rc=%d\n",
                         (unsigned long long)i, prc);
            return 1;
        }

        // (2) The blocking read: spins briefly, then parks.
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const uint64_t t0 = shuttle::monotonic_ns();
        if (c.read(&p, &len) != shuttle::kOk) {
            std::fprintf(stderr, "consumer: read %llu failed\n",
                         (unsigned long long)i);
            return 1;
        }
        const uint64_t dt = shuttle::monotonic_ns() - t0;
        if (dt > kSlowReadNs) ++slow;
        if (dt > max_ns) max_ns = dt;
        if (len != msg_len(i)) {
            std::fprintf(stderr, "consumer: msg %llu len %llu != %llu\n",
                         (unsigned long long)i, (unsigned long long)len,
                         (unsigned long long)msg_len(i));
            return 1;
        }
        uint64_t bad = 0;
        for (uint64_t j = 0; j < len; ++j)
            if (p[j] != fill_byte(i, j)) ++bad;
        if (bad != 0) {
            std::fprintf(stderr, "consumer: msg %llu corrupt\n",
                         (unsigned long long)i);
            return 1;
        }

        // (3) With the borrow outstanding: peek must describe message i+1, and
        // must never claim one past the end of the run.
        plen = 0;
        prc = c.peek_next(&plen);
        if (prc == shuttle::kOk) {
            ++peeked_after;
            if (i + 1 >= kTrickleMsgs) {
                std::fprintf(stderr, "consumer: peek invented msg %llu\n",
                             (unsigned long long)(i + 1));
                return 1;
            }
            if (plen != msg_len(i + 1)) {
                std::fprintf(stderr,
                             "consumer: peek-behind-borrow %llu said %llu,"
                             " want %llu\n",
                             (unsigned long long)(i + 1),
                             (unsigned long long)plen,
                             (unsigned long long)msg_len(i + 1));
                return 1;
            }
        } else if (prc != shuttle::kErrWouldBlock) {
            std::fprintf(stderr, "consumer: peek-behind-borrow %llu rc=%d\n",
                         (unsigned long long)i, prc);
            return 1;
        }
        c.release();
    }
    {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.try_read(&p, &len) != shuttle::kErrWouldBlock) {
            std::fprintf(stderr, "consumer: extra message after N!\n");
            return 1;
        }
        uint64_t plen = 0;
        if (c.peek_next(&plen) != shuttle::kErrWouldBlock) {
            std::fprintf(stderr, "consumer: peek sees a message after N!\n");
            return 1;
        }
    }
    if (slow > kMaxSlowReads) {
        std::fprintf(stderr,
                     "consumer: %llu reads exceeded 50 ms (max %.1f ms) —"
                     " wakeups being lost to the timedwait backstop\n",
                     (unsigned long long)slow, max_ns / 1e6);
        return 1;
    }
    std::printf("consumer: %llu trickled msgs, %llu slow reads, max wait"
                " %.2f ms, peeks that saw the next message: %llu before /"
                " %llu behind a borrow\n",
                (unsigned long long)kTrickleMsgs, (unsigned long long)slow,
                max_ns / 1e6, (unsigned long long)peeked_before,
                (unsigned long long)peeked_after);
    return 0;
}

int case_trickle(const char* self) {
    char name[64];
    name_for(name, sizeof name, "trk");
    int err = 0;
    shuttle::Channel* ch =
        shuttle::create(name, kTrickleCap, kTrickleMaxPayload, &err);
    if (ch == nullptr) return fail("trickle: create", err);
    const int fails = shuttle_test::run_two_children_sync(
        self, "producer", "consumer", name, kChildTimeoutNs);
    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("peek: trickle ok (%llu peek/read/release cycles, none "
                    "lost)\n",
                    (unsigned long long)kTrickleMsgs);
    return fails == 0 ? 0 : 1;
}

// --- the C ABI surface -------------------------------------------------------
//
// The ABI is where peek actually ships, and its accounting differs from the C++
// one: the C layer caches the borrow it handed out, so a peek with a borrow
// active must look past a span THIS layer knows nothing about.
int case_cabi() {
    char name[64];
    name_for(name, sizeof name, "abi");
    int err = 0;
    shuttle_channel* prod = shuttle_create(name, 1u << 16, 4096, &err);
    if (prod == nullptr) return fail("cabi: create", err);
    shuttle_channel* cons = shuttle_open(name, &err);
    if (cons == nullptr) {
        shuttle_close(prod);
        shuttle_unlink(name);
        return fail("cabi: open", err);
    }
    int fails = 0;
    size_t len = 0;

    if (shuttle_peek_next(nullptr, &len) != SHUTTLE_ERR_INVALID_ARGS)
        fails += fail("cabi: peek(NULL channel)", 0);
    if (shuttle_peek_next(cons, nullptr) != SHUTTLE_ERR_INVALID_ARGS)
        fails += fail("cabi: peek(NULL out)", 0);
    if (shuttle_peek_next(cons, &len) != SHUTTLE_ERR_WOULD_BLOCK)
        fails += fail("cabi: peek on an empty channel", 0);

    std::vector<unsigned char> a(300), b(1700);
    for (size_t i = 0; i < a.size(); ++i) a[i] = fill_byte(3, i);
    for (size_t i = 0; i < b.size(); ++i) b[i] = fill_byte(4, i);
    if (shuttle_write(prod, a.data(), a.size(), 0) != SHUTTLE_OK)
        fails += fail("cabi: write a", 0);
    len = 0;
    if (shuttle_peek_next(cons, &len) != SHUTTLE_OK || len != a.size())
        fails += fail("cabi: peek reports a", static_cast<long>(len));

    // Borrow through the ABI, then write the second message and peek past the
    // ABI-cached borrow.
    const void* q = nullptr;
    size_t got = 0;
    if (shuttle_acquire_read(cons, &q, &got, 0) != SHUTTLE_OK ||
        got != a.size())
        fails += fail("cabi: acquire_read", static_cast<long>(got));
    if (shuttle_peek_next(cons, &len) != SHUTTLE_ERR_WOULD_BLOCK)
        fails += fail("cabi: peek re-reported the active borrow", 0);
    if (shuttle_write(prod, b.data(), b.size(), 0) != SHUTTLE_OK)
        fails += fail("cabi: write b", 0);
    len = 0;
    if (shuttle_peek_next(cons, &len) != SHUTTLE_OK || len != b.size())
        fails +=
            fail("cabi: peek past the cached borrow", static_cast<long>(len));
    if (std::memcmp(q, a.data(), a.size()) != 0)
        fails += fail("cabi: peek disturbed the borrow", 0);
    if (shuttle_release_read(cons) != SHUTTLE_OK)
        fails += fail("cabi: release_read", 0);
    len = 0;
    if (shuttle_peek_next(cons, &len) != SHUTTLE_OK || len != b.size())
        fails += fail("cabi: peek after release", static_cast<long>(len));
    // A copy read consumes b; then there is nothing left to look ahead at.
    std::vector<unsigned char> out(b.size());
    if (shuttle_read(cons, out.data(), out.size(), 0) !=
        static_cast<long>(b.size()))
        fails += fail("cabi: copy read", 0);
    if (shuttle_peek_next(cons, &len) != SHUTTLE_ERR_WOULD_BLOCK)
        fails += fail("cabi: peek after draining", 0);

    shuttle_close(cons);
    shuttle_close(prod);
    shuttle_unlink(name);
    if (fails == 0)
        std::printf("peek: C ABI ok (null args, empty, past an ABI-cached "
                    "borrow, after release)\n");
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    int fails = 0;
    fails += case_sequence();
    fails += case_wrap();
    fails += case_falsifiable();
    fails += case_corrupt();
    fails += case_cabi();
    fails += case_prefetch();
    fails += case_trickle(self);
    if (fails == 0)
        std::printf("peek_prefetch_test ok: lookahead + WILLNEED prefetch "
                    "(platform=%s)\n",
                    shuttle::platform_name());
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3) {
        int err = 0;
        shuttle::Channel* ch = shuttle::open(argv[2], &err);
        if (ch == nullptr) {
            std::fprintf(stderr, "%s: open err=%d\n", argv[1], err);
            return 1;
        }
        int rc = 2;
        if (std::strcmp(argv[1], "producer") == 0) rc = run_producer(ch);
        if (std::strcmp(argv[1], "consumer") == 0) rc = run_consumer(ch);
        shuttle::close(ch);
        return rc;
    }
    std::fprintf(stderr, "usage: %s [producer|consumer </name>]\n", argv[0]);
    return 2;
}
