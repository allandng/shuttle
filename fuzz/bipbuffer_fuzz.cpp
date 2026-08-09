// libFuzzer harness for the BipBuffer core (include/shuttle/bipbuffer.hpp).
//
// The fuzz input is an OP TAPE: a byte stream decoded into a sequence of
// BipBuffer operations, replayed against a real BipBuffer over a small heap
// buffer AND against a reference model. Every operation is followed by a full
// invariant sweep, in the spirit of the existing property test
// (tests/bipbuffer_test.cpp) — that test drives the same protocol from a
// seeded PRNG; this one lets coverage feedback pick the schedule instead, so
// it reaches op orderings a uniform RNG effectively never produces (abort
// after a wrapping reserve, commit(0) on a live reservation, release of a
// partial block straddling the A->B handoff, ...).
//
// REFERENCE MODEL. The buffer is a byte FIFO built out of contiguous UNITS:
// one unit per successful commit()/write_msg(), holding the exact bytes that
// call published. `part` counts bytes of the front unit already released. The
// model therefore knows, at every instant, the full sequence of readable bytes
// — so a read is checked for CONTENT, not just for length.
//
// WHAT THE HARNESS MAY NOT DO. BipBuffer's framed layer documents a caller
// contract that the harness has to honor, or it would be testing a contract
// violation rather than the library:
//   * read_msg() assumes the readable block starts at a message boundary and
//     unconditionally reads 8 length bytes, so it is only called when the
//     front unit was written by write_msg() and nothing has been released out
//     of it (part == 0). Calling it after a raw commit() of < 8 bytes would
//     read past the valid data by construction — a harness bug, not a finding.
//   * release(n) is only called with n <= the len of the latest read_block().
//   * commit() only publishes bytes the harness actually wrote into the
//     reservation.
// Everything else — including the deliberately-invalid commit lengths and
// re-entrant reserves below — is fair game and is asserted to be a no-op.
//
// GUARD REGIONS. The allocation is kGuard + cap + kGuard bytes; BipBuffer is
// handed only the middle cap bytes. The guards are filled with a canary and
// re-verified after every operation, so a one-byte scribble outside the data
// region is caught at the operation that caused it rather than whenever the
// heap notices. ASan's redzones sit outside that again and catch anything that
// runs past the guards entirely.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "shuttle/bipbuffer.hpp"

namespace {

// Always-on check: the harness must fail identically whether or not NDEBUG is
// defined, and it must abort() so libFuzzer records a reproducer.
#define FCHECK(cond, ...)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "bipbuffer_fuzz: " __VA_ARGS__);      \
            std::fprintf(stderr, "  [%s] at %s:%d\n", #cond, __FILE__, \
                         __LINE__);                                    \
            std::abort();                                              \
        }                                                              \
    } while (0)

constexpr size_t kGuard = 64;
constexpr unsigned char kCanary = 0xA5;
constexpr uint64_t kMinCap = 32;
constexpr uint64_t kMaxCap = 512;

// Byte-tape decoder. Every accessor is total: past the end it yields 0 and
// done() goes true, so the replay loop always terminates and the harness is a
// pure deterministic function of the input.
class Tape {
 public:
    Tape(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool done() const { return i_ >= n_; }
    uint8_t u8() { return i_ < n_ ? p_[i_++] : 0; }
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
    }

 private:
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
};

// One committed, contiguous run of bytes. `framed` marks the units produced by
// write_msg(), the only ones read_msg() may be pointed at.
struct Unit {
    std::vector<uint8_t> bytes;
    bool framed = false;
    uint64_t payload_len = 0;
};

// Deterministic payload filler: content is a function of (unit index, offset),
// so a byte that ends up in the wrong message is detected even when the two
// messages have the same length.
uint8_t fill_byte(uint64_t unit_index, uint64_t i) {
    return static_cast<uint8_t>((unit_index * 1315423911ull) + i * 151ull +
                                (i >> 8) + 0x5Au);
}

class Harness {
 public:
    explicit Harness(uint64_t cap)
        : cap_(cap),
          mem_(kGuard + cap + kGuard, kCanary),
          buf_(mem_.data() + kGuard, cap) {}

    void run(Tape& t) {
        while (!t.done()) {
            step(t);
            check_invariants();
        }
        // Drain at the end: exercises the A->B handoff and proves the model
        // and the buffer agree about emptiness, not just about counts.
        drain();
        check_invariants();
        FCHECK(pending_ == 0, "drain left %llu bytes pending\n",
               (unsigned long long)pending_);
        FCHECK(buf_.size() == 0, "drain left size()=%llu\n",
               (unsigned long long)buf_.size());
    }

 private:
    unsigned char* data() { return mem_.data() + kGuard; }

    // --- operations ------------------------------------------------------

    void step(Tape& t) {
        switch (t.u8() % 8u) {
            case 0:
            case 1:
                op_reserve(t);
                return;
            case 2:
                op_commit(t);
                return;
            case 3:
                buf_.abort_reserve();
                res_ptr_ = nullptr;
                res_n_ = 0;
                return;
            case 4:
            case 5:
                op_read_block(t);
                return;
            case 6:
                op_write_msg(t);
                return;
            default:
                // read_msg only where the contract allows it; elsewhere fall
                // back to the raw read so the op byte is never wasted.
                if (msg_readable()) {
                    op_read_msg();
                } else {
                    op_read_block(t);
                }
                return;
        }
    }

    void op_reserve(Tape& t) {
        // Range deliberately overshoots cap so the oversize-rejection path
        // (n == 0, n > cap_) is hit as often as the accepting one.
        const uint64_t n = t.u16() % (cap_ + 16);
        unsigned char* p = nullptr;
        const bool active_before = res_ptr_ != nullptr;
        const uint64_t w_before = buf_.write_cursor();
        const uint64_t m_before = buf_.watermark_cursor();
        const bool ok = buf_.reserve(n, &p);
        // A reservation never publishes anything: the cursors that decide what
        // is readable must be untouched whether it succeeded or failed.
        FCHECK(buf_.write_cursor() == w_before &&
                   buf_.watermark_cursor() == m_before,
               "reserve moved a published cursor (n=%llu, ok=%d)\n",
               (unsigned long long)n, static_cast<int>(ok));
        if (!ok) return;
        FCHECK(!active_before, "reserve succeeded while one was active\n");
        FCHECK(n != 0 && n <= cap_, "reserve accepted n=%llu with cap=%llu\n",
               (unsigned long long)n, (unsigned long long)cap_);
        FCHECK(p >= data() && p + n <= data() + cap_,
               "reservation span escapes the data region (n=%llu)\n",
               (unsigned long long)n);
        res_ptr_ = p;
        res_n_ = n;
    }

    void op_commit(Tape& t) {
        const uint8_t sel = t.u8();
        if (res_ptr_ == nullptr) {
            // commit() with no live reservation is documented as a no-op.
            const uint64_t before = buf_.size();
            buf_.commit(1 + sel);
            FCHECK(buf_.size() == before,
                   "commit with no reservation moved "
                   "size %llu -> %llu\n",
                   (unsigned long long)before, (unsigned long long)buf_.size());
            return;
        }
        // Every 8th commit is deliberately out of contract (0, or longer than
        // the reservation). Both are documented no-ops that LEAVE THE
        // RESERVATION LIVE — asserted here, because a silent clear would leak
        // the reserved bytes forever.
        if ((sel & 7u) == 7u) {
            const uint64_t bad = (sel & 8u) ? 0 : res_n_ + 1;
            const uint64_t before = buf_.size();
            buf_.commit(bad);
            FCHECK(buf_.size() == before,
                   "out-of-contract commit(%llu) published bytes\n",
                   (unsigned long long)bad);
            return;
        }
        const uint64_t len = 1 + (static_cast<uint64_t>(sel) % res_n_);
        Unit u;
        u.bytes.resize(len);
        for (uint64_t i = 0; i < len; ++i) {
            u.bytes[i] = fill_byte(units_written_, i);
            res_ptr_[i] = u.bytes[i];
        }
        buf_.commit(len);
        push_unit(std::move(u));
        res_ptr_ = nullptr;
        res_n_ = 0;
    }

    void op_write_msg(Tape& t) {
        // Overshoots cap on purpose: a payload that cannot fit must be
        // refused, never truncated.
        const uint64_t len = t.u16() % (cap_ + 16);
        std::vector<uint8_t> payload(len);
        for (uint64_t i = 0; i < len; ++i)
            payload[i] = fill_byte(units_written_, i);
        const uint64_t before = buf_.size();
        if (!buf_.write_msg(payload.data(), len)) {
            FCHECK(buf_.size() == before, "failed write_msg moved size\n");
            return;
        }
        FCHECK(res_ptr_ == nullptr,
               "write_msg succeeded on top of a live reservation\n");
        Unit u;
        u.framed = true;
        u.payload_len = len;
        u.bytes.resize(shuttle::kFrameHeader + len);
        for (unsigned i = 0; i < 8; ++i)
            u.bytes[i] = static_cast<uint8_t>((len >> (8 * i)) & 0xFF);
        if (len != 0)
            std::memcpy(u.bytes.data() + shuttle::kFrameHeader, payload.data(),
                        len);
        push_unit(std::move(u));
    }

    void op_read_block(Tape& t) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const bool got = buf_.read_block(&p, &len);
        FCHECK(got == (pending_ != 0),
               "read_block=%d but model holds %llu bytes\n",
               static_cast<int>(got), (unsigned long long)pending_);
        if (!got) return;
        check_span(p, len);
        FCHECK(len <= pending_, "read_block len %llu > pending %llu\n",
               (unsigned long long)len, (unsigned long long)pending_);
        compare_stream(p, len);
        // Release a fuzz-chosen prefix (contract: n <= len), including 0.
        const uint64_t n = static_cast<uint64_t>(t.u16()) % (len + 1);
        buf_.release(n);
        consume(n);
    }

    void op_read_msg() {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        const bool got = buf_.read_msg(&p, &len);
        FCHECK(got, "read_msg found nothing with a framed unit queued\n");
        const Unit& u = units_.front();
        FCHECK(len == u.payload_len, "read_msg len %llu != model %llu\n",
               (unsigned long long)len, (unsigned long long)u.payload_len);
        // The payload span itself must live inside the data region...
        if (len != 0) check_span(p, len);
        // ...and match the model byte for byte.
        FCHECK(std::memcmp(p, u.bytes.data() + shuttle::kFrameHeader, len) == 0,
               "read_msg payload differs from the model (len=%llu)\n",
               (unsigned long long)len);
        const uint64_t span = shuttle::kFrameHeader + len;
        buf_.release_msg();
        consume(span);
    }

    void drain() {
        // Bounded by construction: every iteration releases the whole block,
        // and pending_ strictly decreases.
        while (pending_ != 0) {
            const unsigned char* p = nullptr;
            uint64_t len = 0;
            const bool got = buf_.read_block(&p, &len);
            FCHECK(got, "drain: %llu bytes pending but read_block said empty\n",
                   (unsigned long long)pending_);
            check_span(p, len);
            compare_stream(p, len);
            buf_.release(len);
            consume(len);
        }
    }

    // --- model bookkeeping ------------------------------------------------

    void push_unit(Unit&& u) {
        pending_ += u.bytes.size();
        units_.push_back(std::move(u));
        ++units_written_;
    }

    bool msg_readable() const {
        return part_ == 0 && !units_.empty() && units_.front().framed;
    }

    void consume(uint64_t n) {
        FCHECK(n <= pending_, "model consume %llu > pending %llu\n",
               (unsigned long long)n, (unsigned long long)pending_);
        pending_ -= n;
        while (n != 0) {
            const uint64_t left = units_.front().bytes.size() - part_;
            if (n < left) {
                part_ += n;
                return;
            }
            n -= left;
            units_.pop_front();
            part_ = 0;
        }
    }

    // The model's next `len` readable bytes must equal what the buffer just
    // handed out. This is the byte-exactness check, run on EVERY read.
    void compare_stream(const unsigned char* p, uint64_t len) {
        uint64_t off = part_;
        size_t idx = 0;
        uint64_t done = 0;
        while (done < len) {
            FCHECK(idx < units_.size(),
                   "model exhausted %llu bytes into a %llu-byte block\n",
                   (unsigned long long)done, (unsigned long long)len);
            const std::vector<uint8_t>& b = units_[idx].bytes;
            const uint64_t take = (b.size() - off) < (len - done)
                                      ? (b.size() - off)
                                      : (len - done);
            FCHECK(std::memcmp(p + done, b.data() + off, take) == 0,
                   "block content differs from the model at +%llu\n",
                   (unsigned long long)done);
            done += take;
            off = 0;
            ++idx;
        }
    }

    // --- invariants -------------------------------------------------------

    void check_span(const unsigned char* p, uint64_t len) {
        FCHECK(len != 0, "zero-length span returned as readable\n");
        FCHECK(p >= data() && len <= cap_ && p <= data() + cap_ - len,
               "span [%p,+%llu) escapes the data region\n",
               static_cast<const void*>(p), (unsigned long long)len);
    }

    void check_invariants() {
        // Guard regions first: if the data region was overrun, everything
        // below is reporting on corrupted state.
        for (size_t i = 0; i < kGuard; ++i) {
            FCHECK(mem_[i] == kCanary, "leading guard byte %zu clobbered\n", i);
            FCHECK(mem_[kGuard + cap_ + i] == kCanary,
                   "trailing guard byte %zu clobbered\n", i);
        }
        const uint64_t r = buf_.read_cursor();
        const uint64_t w = buf_.write_cursor();
        const uint64_t m = buf_.watermark_cursor();
        FCHECK(buf_.capacity() == cap_, "capacity changed\n");
        FCHECK(r <= cap_, "read cursor %llu > cap\n", (unsigned long long)r);
        FCHECK(w <= cap_, "write cursor %llu > cap\n", (unsigned long long)w);
        FCHECK(m <= cap_, "watermark %llu > cap\n", (unsigned long long)m);
        if (w < r) {
            // Wrapped: valid data is [r, m) then [0, w). r > m would make
            // size() underflow and the high region a negative range.
            FCHECK(r <= m, "wrapped but read %llu > watermark %llu\n",
                   (unsigned long long)r, (unsigned long long)m);
            // Strictness of the wrapped state: write must never climb to meet
            // read from below, or "wrapped full" would alias "linear empty".
            FCHECK(w < r, "wrapped state lost its strict w < r\n");
        }
        FCHECK(buf_.size() == pending_, "size() %llu != model pending %llu\n",
               (unsigned long long)buf_.size(), (unsigned long long)pending_);
        FCHECK(pending_ <= cap_, "model holds %llu bytes in a %llu-byte ring\n",
               (unsigned long long)pending_, (unsigned long long)cap_);
    }

    uint64_t cap_;
    std::vector<unsigned char> mem_;
    shuttle::BipBuffer buf_;
    std::deque<Unit> units_;
    uint64_t pending_ = 0;  // bytes committed and not yet released
    uint64_t part_ = 0;     // bytes of units_.front() already released
    uint64_t units_written_ = 0;
    unsigned char* res_ptr_ = nullptr;
    uint64_t res_n_ = 0;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3) return 0;
    Tape t(data, size);
    // Capacity varies with the input so wrap frequency varies too: a tight
    // ring wraps every couple of messages, a roomy one exercises the linear
    // path. Kept small so a whole tape replays in microseconds.
    const uint64_t cap = kMinCap + (t.u16() % (kMaxCap - kMinCap + 1));
    Harness h(cap);
    h.run(t);
    return 0;
}
