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
// A4's seq_cst parking protocol applies to the Phase 4 waiting flags only;
// the data-path cursors here stay release/acquire by design.
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

    // Blocking (busy-poll in Phase 3) framed write.
    int write(const void* payload, uint64_t len) {
        int rc;
        uint64_t spins = 0;
        while ((rc = try_write(payload, len)) == kErrWouldBlock) {
            detail::spin_pause(spins);
        }
        return rc;
    }

 private:
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
                r = 0;
                // fall through to linear view below
            } else {
                return parse(r, m - r, payload, len);
            }
        }
        if (w == r) return kErrWouldBlock;  // empty
        return parse(r, w - r, payload, len);
    }

    // Blocking (busy-poll in Phase 3) borrow.
    int read(const unsigned char** payload, uint64_t* len) {
        int rc;
        uint64_t spins = 0;
        while ((rc = try_read(payload, len)) == kErrWouldBlock) {
            detail::spin_pause(spins);
        }
        return rc;
    }

    // C1: publish that the borrowed message's bytes are free to reuse.
    void release() {
        const uint64_t r = h_->read.load(std::memory_order_relaxed);
        h_->read.store(r + borrowed_, std::memory_order_release);
        borrowed_ = 0;
    }

 private:
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
