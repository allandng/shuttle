#pragma once

// Phase 3: the lock-free SPSC data path over the shared segment, promoting
// the Phase-2-proven BipBuffer protocol onto the header's atomic cursors.
// Busy-poll on empty/full (parking arrives in Phase 4). No mutex anywhere
// on this path.
//
// ===================== MEMORY-ORDERING CONTRACT ========================
// (normative, per amendments A1/A2: FR-17 is satisfied by single-process
// TSan over these exact code paths PLUS this written argument)
//
// Shared atomics and their single writers (A1):
//   write     — written ONLY by the producer
//   watermark — written ONLY by the producer
//   read      — written ONLY by the consumer
// Single-writer ownership means each store can be a simple store (never
// RMW), and the only cross-agent edges needed are publish edges:
//
// P1 (payload publish): the producer writes payload bytes into the data
//   region BEFORE write.store(release). A consumer observing the new
//   value via write.load(acquire) synchronizes-with that store, so the
//   payload bytes are visible before the consumer dereferences them.
//
// P2 (watermark publish): on a wrap commit the producer stores watermark
//   (relaxed) and THEN write (release). The watermark store is
//   sequenced-before the write release in the producer, so a consumer
//   that acquired that write value also observes the watermark value.
//   A consumer only consults watermark when it observes write < read —
//   a state that can only be produced by that same wrap commit, whose
//   write value it must therefore have acquired. Hence the consumer's
//   watermark.load can be relaxed.
//
// C1 (free publish): the consumer finishes reading payload bytes BEFORE
//   read.store(release) (per-message release and the handoff store both).
//   A producer observing the new value via read.load(acquire)
//   synchronizes-with it, so reusing/overwriting the freed bytes cannot
//   race the consumer's reads.
//
// C2 (A->B handoff): the handoff is the consumer ALONE storing read = 0
//   after observing read == watermark (its own cursor vs P2-published
//   watermark) while write < read. read has exactly one writer, so the
//   producer can only ever observe the old value (high region occupied)
//   or 0 (high region freed) — no torn intermediate state exists. The
//   producer's space arithmetic is conservative under both values.
//
// Staleness: each side re-loads the other side's cursor on every attempt.
//   An out-of-date value can only UNDER-estimate available space (producer)
//   or available data (consumer) — failure is spurious "would block",
//   never corruption.
//
// PARKING PROTOCOL (Phase 4, amendments A3/A4) — the park decision is a
// Dekker-style store->load pattern with a seq_cst fence on BOTH sides:
//
//   waiter:  waiting.store(1)            signaler:  cursor.store(release)
//            fence(seq_cst)                         fence(seq_cst)
//            recheck cursors                        load waiting flag
//
// Claim: the waiter parking AND the signaler skipping the wake cannot both
// happen for the same publish. In the seq_cst total order, either the
// waiter's recheck is ordered after the signaler's cursor store (recheck
// sees the data/space -> waiter does not park), or the signaler's flag
// load is ordered after the waiter's flag store (signaler sees the flag ->
// signals). Plain release/acquire permits both loads to read stale values
// simultaneously — that is the A4 hole this fence pair closes.
//
// Belt-and-braces, per A3: even a lost wake cannot strand anyone, because
// EVERY park is pthread_cond_timedwait with a bounded interval and the
// predicate is re-checked on each timeout. (Phase 5 adds the heartbeat
// staleness verdict at those timeouts; Phase 5b adds robust recovery.)
//
// The mutex guards nothing but the park/wake handshake itself; when the
// peer is not parked the publishing side pays one fence + one relaxed
// load — it never touches the mutex (§2.3).
// ========================================================================

#include <atomic>
#include <cstdint>
#include <cstring>

#include "shuttle/platform.hpp"
#include "shuttle/shuttle.hpp"

namespace shuttle {

namespace detail {
inline void spin_pause(uint64_t& spins) {
    cpu_relax();
    if ((++spins & 0xFFF) == 0) yield_thread();  // be polite under oversubscription
}

// Brief adaptive spin before parking: covers the common case where the peer
// is actively draining/filling, without measurable idle CPU.
constexpr int kSpinBeforePark = 256;
// A3: every park is bounded; the predicate (and, from Phase 5, heartbeat
// staleness) is re-evaluated at least this often.
constexpr uint64_t kParkTimeoutNs = 100ull * 1000000;  // 100 ms
}  // namespace detail

class Producer {
 public:
    explicit Producer(Channel* ch)
        : h_(ch->hdr),
          data_(static_cast<unsigned char*>(
              resolve(ch->base, ch->hdr->data_offset))),
          cap_(ch->hdr->data_capacity) {}

    // Non-blocking framed write (copy path).
    // kOk | kErrWouldBlock | kErrMsgTooLarge (fail fast, never blocks: G2.3).
    int try_write(const void* payload, uint64_t len) {
        if (len > h_->max_payload) return kErrMsgTooLarge;
        const uint64_t n = kFrameHeader + len;
        uint64_t off = 0;
        bool wrap = false;
        if (!try_reserve(n, &off, &wrap)) return kErrWouldBlock;
        unsigned char* dst = data_ + off;
        for (unsigned i = 0; i < 8; ++i) {
            dst[i] = static_cast<unsigned char>((len >> (8 * i)) & 0xFF);
        }
        if (len != 0) std::memcpy(dst + kFrameHeader, payload, len);
        commit(n, off, wrap);  // P1/P2 publish
        return kOk;
    }

    // Blocking framed write: brief spin, then park on not_full (Phase 4).
    int write(const void* payload, uint64_t len) {
        if (len > h_->max_payload) return kErrMsgTooLarge;  // fail fast, never park
        int rc;
        for (int s = 0; s < detail::kSpinBeforePark; ++s) {
            if ((rc = try_write(payload, len)) != kErrWouldBlock) return rc;
            cpu_relax();
        }
        for (;;) {
            if ((rc = try_write(payload, len)) != kErrWouldBlock) return rc;
            park_until_space(kFrameHeader + len);
        }
    }

 private:
    // Park-decision predicate; called with seq_cst semantics established by
    // the fence in park_until_space (A4).
    bool can_reserve(uint64_t n) const {
        const uint64_t w = h_->write.load(std::memory_order_relaxed);
        const uint64_t r = h_->read.load(std::memory_order_relaxed);
        if (w >= r) return (cap_ - w >= n) || (r > n);
        return r - w > n;
    }

    void park_until_space(uint64_t n) {
        // Dekker pair, waiter side (see PARKING PROTOCOL block above).
        h_->producer_waiting.store(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (can_reserve(n)) {
            h_->producer_waiting.store(0, std::memory_order_relaxed);
            return;
        }
        park_mutex_lock(&h_->lock);
        // Lost-wakeup guard (App. B #7): re-check under the lock — the
        // consumer's wake path takes this same lock, so it cannot signal
        // between this check and the wait.
        if (!can_reserve(n)) {
            cond_timedwait_rel(&h_->not_full, &h_->lock,
                               detail::kParkTimeoutNs);
            // Phase 5: heartbeat staleness verdict on timeout goes here.
        }
        park_mutex_unlock(&h_->lock);
        h_->producer_waiting.store(0, std::memory_order_relaxed);
    }

    // Signaler side of the consumer's Dekker pair; called after every
    // commit. Peer not parked => one fence + one relaxed load, no mutex.
    void wake_consumer_if_waiting() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (h_->consumer_waiting.load(std::memory_order_relaxed) != 0) {
            park_mutex_lock(&h_->lock);
            pthread_cond_signal(&h_->not_empty);
            park_mutex_unlock(&h_->lock);
        }
    }
    bool try_reserve(uint64_t n, uint64_t* off, bool* wrap) {
        // write/watermark are producer-owned: relaxed loads observe our own
        // most recent stores. read is consumer-owned: acquire pairs with C1.
        const uint64_t w = h_->write.load(std::memory_order_relaxed);
        const uint64_t r = h_->read.load(std::memory_order_acquire);
        if (w >= r) {  // linear: valid data [r, w)
            if (cap_ - w >= n) {
                *off = w;
                *wrap = false;
                return true;
            }
            // Early wrap (whole unit; App. B #6). Strict r > n keeps the
            // post-commit state write = n < read: wrapped-full must never
            // alias linear-empty (write == read).
            if (r > n) {
                *off = 0;
                *wrap = true;
                return true;
            }
            return false;
        }
        // Wrapped: append in the low region, strictly keeping write < read.
        if (r - w > n) {
            *off = w;
            *wrap = false;
            return true;
        }
        return false;
    }

    void commit(uint64_t n, uint64_t off, bool wrap) {
        if (wrap) {
            // P2: watermark first (relaxed), sequenced before the write
            // release below — consumers acquire both atomically-in-effect.
            h_->watermark.store(h_->write.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
            h_->write.store(n, std::memory_order_release);  // P1
        } else {
            h_->write.store(off + n, std::memory_order_release);  // P1
        }
        wake_consumer_if_waiting();  // A4 signaler side
    }

    ChannelHeader* h_;
    unsigned char* data_;
    uint64_t cap_;
};

class Consumer {
 public:
    explicit Consumer(Channel* ch)
        : h_(ch->hdr),
          data_(static_cast<unsigned char*>(
              resolve(ch->base, ch->hdr->data_offset))) {}

    // Non-blocking zero-copy borrow of the next message. At most one
    // outstanding borrow; must be paired with release().
    // kOk | kErrWouldBlock | kErrCorrupt.
    int try_read(const unsigned char** payload, uint64_t* len) {
        uint64_t r = h_->read.load(std::memory_order_relaxed);  // own cursor
        const uint64_t w = h_->write.load(std::memory_order_acquire);  // P1
        if (w < r) {
            // Wrapped. watermark visibility is guaranteed by P2 (relaxed ok).
            const uint64_t m = h_->watermark.load(std::memory_order_relaxed);
            if (r == m) {
                // C2: A->B handoff — single owned-variable update. The freed
                // high region [m, cap) was fully read before this store.
                h_->read.store(0, std::memory_order_release);
                wake_producer_if_waiting();  // handoff frees space too
                r = 0;
                // fall through to linear view below
            } else {
                return parse(r, m - r, payload, len);
            }
        }
        if (w == r) return kErrWouldBlock;  // empty
        return parse(r, w - r, payload, len);
    }

    // Blocking borrow: brief spin, then park on not_empty (Phase 4).
    int read(const unsigned char** payload, uint64_t* len) {
        int rc;
        for (int s = 0; s < detail::kSpinBeforePark; ++s) {
            if ((rc = try_read(payload, len)) != kErrWouldBlock) return rc;
            cpu_relax();
        }
        for (;;) {
            if ((rc = try_read(payload, len)) != kErrWouldBlock) return rc;
            park_until_data();
        }
    }

    // C1: publish that the borrowed message's bytes are free to reuse.
    void release() {
        const uint64_t r = h_->read.load(std::memory_order_relaxed);
        h_->read.store(r + borrowed_, std::memory_order_release);
        borrowed_ = 0;
        wake_producer_if_waiting();  // A4 signaler side
    }

 private:
    // Park-decision predicate (under the A4 fence). write != read covers the
    // wrapped state too; a spurious wake merely retries try_read.
    bool data_available() const {
        return h_->write.load(std::memory_order_relaxed) !=
               h_->read.load(std::memory_order_relaxed);
    }

    void park_until_data() {
        // Dekker pair, waiter side (see PARKING PROTOCOL block above).
        h_->consumer_waiting.store(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (data_available()) {
            h_->consumer_waiting.store(0, std::memory_order_relaxed);
            return;
        }
        park_mutex_lock(&h_->lock);
        // Lost-wakeup guard (App. B #7): re-check under the lock.
        if (!data_available()) {
            cond_timedwait_rel(&h_->not_empty, &h_->lock,
                               detail::kParkTimeoutNs);
            // Phase 5: heartbeat staleness verdict on timeout goes here.
        }
        park_mutex_unlock(&h_->lock);
        h_->consumer_waiting.store(0, std::memory_order_relaxed);
    }

    void wake_producer_if_waiting() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (h_->producer_waiting.load(std::memory_order_relaxed) != 0) {
            park_mutex_lock(&h_->lock);
            pthread_cond_signal(&h_->not_full);
            park_mutex_unlock(&h_->lock);
        }
    }
    int parse(uint64_t at, uint64_t avail, const unsigned char** payload,
              uint64_t* len) {
        // Whole-unit reservation: a readable run always starts at a message
        // boundary and contains only whole messages, so avail >= 8 + L.
        const unsigned char* blk = data_ + at;
        uint64_t l = 0;
        for (unsigned i = 0; i < 8; ++i) {
            l |= static_cast<uint64_t>(blk[i]) << (8 * i);
        }
        // NFR-S2: a length the producer could never have written means the
        // segment is corrupt; never hand out an out-of-bounds span.
        if (l > h_->max_payload || kFrameHeader + l > avail) return kErrCorrupt;
        *payload = blk + kFrameHeader;
        *len = l;
        borrowed_ = kFrameHeader + l;
        return kOk;
    }

    ChannelHeader* h_;
    unsigned char* data_;
    uint64_t borrowed_ = 0;  // consumer-private span of the active borrow
};

}  // namespace shuttle
