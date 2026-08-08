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
// STATISTICS COUNTERS (opt-in, kVersionStats segments only) fit this same
//   single-writer scheme and add NO ordering obligations. stat_msgs_written /
//   stat_bytes_written / stat_msgs_dropped are written only by the producer,
//   stat_msgs_read / stat_bytes_read only by the consumer, each on its own
//   cache line, each a relaxed load + relaxed store (never an RMW) — identical
//   to the heartbeat.
//   No agent ever reads a counter to make a decision, so no publish edge is
//   needed and none is created: an observer may see a counter that lags the
//   cursors, which is the documented cost of keeping the hot path free of
//   extra ordering. On a v1 segment those addresses are DATA, so both sides
//   resolve a null stats pointer at construction and never touch them.
//
// Staleness: each side re-loads the other side's cursor on every attempt.
//   An out-of-date value can only UNDER-estimate available space (producer)
//   or available data (consumer) — failure is spurious "would block",
//   never corruption.
//
// PEEK (v1.4, Consumer::peek_next) adds NO new writer and NO new edge. It is
//   the read-only half of try_read: it loads read (its own cursor, relaxed),
//   write (acquire — the SAME P1 edge try_read takes), and, only after
//   observing write < read, watermark (relaxed — valid by the SAME P2
//   argument, since that state can only have been produced by the wrap commit
//   whose write value was just acquired). It then decodes an 8-byte length
//   header out of the run those cursors delimit, under parse()'s own
//   overflow-safe guards. It STORES NOTHING: not read, not borrowed_, not a
//   waiting flag, not a heartbeat. In particular it does NOT perform the C2
//   A->B handoff when its position reaches the watermark — it reports the
//   wrapped message by reading at offset 0 directly, leaving the handoff to
//   the next try_read, so `read` keeps its single writer AND its single
//   writing SITE.
//   Sufficiency of reusing the existing edges: peek dereferences payload
//   header bytes at a position it has proven lies inside a committed run
//   [read+borrowed_, write) (or [read+borrowed_, watermark) plus [0, write)
//   when wrapped) — the identical proof obligation try_read discharges with
//   the identical loads, so P1 makes those bytes visible before they are read
//   and P2 makes the watermark visible before it is used.
//   It can UNDER-report: an acquire load that misses a just-published write
//   yields kErrWouldBlock for a message that is, by then, committed. That is
//   the same spurious-would-block staleness documented above, and it is
//   harmless for the same reason — a caller re-peeks or simply reads. It can
//   never OVER-report, because write is the ONLY publisher of committed data
//   and a consumer cannot observe a write value the producer never stored.
//
// PREFETCH (v1.4, the advise_willneed hooks) is outside this contract
//   entirely: madvise/posix_madvise on an already-mapped range is advice about
//   PAGE RESIDENCY, touches no shared state, and its result is discarded. The
//   hooks compute their range from the same relaxed/acquire cursor loads peek
//   uses and store nothing. They are gated on a bool resolved once at Consumer
//   construction (kFlagFileBacked), so a default channel pays one predictable
//   branch and never enters the code at all.
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
// A3: every park is bounded; the predicate and heartbeat staleness are
// re-evaluated at least this often.
constexpr uint64_t kParkTimeoutNs = 100ull * 1000000;  // 100 ms

// HEARTBEAT LIVENESS (Phase 5, amendment A3 — primary on BOTH platforms).
// Each side bumps its own heartbeat on every successful operation, on every
// park iteration, and via keepalive(). A BLOCKED wait samples the peer's
// heartbeat at each timedwait timeout; if it has not advanced within the
// staleness threshold, the wait aborts with kErrPeerDead instead of
// blocking forever. The threshold is process-local policy (constructor
// parameter), NOT segment state.
//
// Documented limitation: a peer that is alive but makes no Shuttle calls at
// all is indistinguishable from a dead one. Applications with sparse
// traffic must call keepalive() periodically (or raise the threshold).
constexpr uint64_t kDefaultStaleNs = 5ull * 1000000000ull;  // 5 s
}  // namespace detail

namespace detail {
// Tracks whether a peer's heartbeat is advancing; process-local.
struct StaleTracker {
    uint64_t last_hb = 0;
    uint64_t last_change_ns = 0;
    bool stale(uint64_t hb_now, uint64_t threshold_ns) {
        const uint64_t now = monotonic_ns();
        if (last_change_ns == 0 || hb_now != last_hb) {
            last_hb = hb_now;
            last_change_ns = now;
            return false;
        }
        return now - last_change_ns > threshold_ns;
    }
};
}  // namespace detail

class Producer {
 public:
    explicit Producer(Channel* ch,
                      uint64_t stale_threshold_ns = detail::kDefaultStaleNs)
        : h_(ch->hdr),
          data_(static_cast<unsigned char*>(
              resolve(ch->base, ch->hdr->data_offset))),
          cap_(ch->hdr->data_capacity),
          stale_ns_(stale_threshold_ns),
          // STATS GATE, resolved once: null on a v1 segment, where the stats
          // fields alias the data region and must never be touched. The hot
          // path then pays one perfectly-predictable branch per commit.
          stats_(has_stats(ch->hdr) ? ch->hdr : nullptr),
          // FRAME-GEOMETRY GATE, resolved once from the same immutable flags
          // word, by the same argument: 0 = the classic 8-byte framing, the
          // page size = kFlagAlignedSpans framing (bipbuffer.hpp). Both ends of
          // a channel read it from the segment, so creator and opener cannot
          // disagree, and neither can be configured out of step by its caller.
          align_(has_aligned_spans(ch->hdr) ? static_cast<uint64_t>(page_size())
                                            : 0) {}

    // A3: announce liveness without transferring data. Sparse-traffic
    // producers must call this periodically or the peer may declare us dead.
    void keepalive() { bump_heartbeat(); }

    // Count one message the CALLER chose to throw away under an opt-in lossy
    // policy (the C ABI's SHUTTLE_DROP_NEWEST; nothing in this class ever drops
    // on its own — "backpressure, never drops" remains the default). This is
    // BOOKKEEPING ONLY: it moves no cursor, publishes nothing, and never looks
    // at the consumer, so a drop cannot be observed by the peer at all and the
    // queued bytes are untouched. The counter is producer-owned and uses the
    // same single-writer relaxed load+store idiom as bump_write_stats, so it
    // adds no ordering obligation to the contract above. On a v1 segment the
    // stats pointer is null and this is a no-op: the drop still happens, it is
    // simply not counted (those bytes are payload there — see header.hpp).
    void count_drop() {
        if (stats_ == nullptr) return;
        stats_->stat_msgs_dropped.store(
            stats_->stat_msgs_dropped.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
    }

    // Non-blocking framed write (copy path).
    // kOk | kErrWouldBlock | kErrMsgTooLarge (fail fast, never blocks: G2.3).
    int try_write(const void* payload, uint64_t len) {
        if (res_active_) return kErrInvalidArgs;  // outstanding acquire owns the next span
        if (len > h_->max_payload) return kErrMsgTooLarge;
        // max_payload is validated at create/open to leave room for a whole
        // frame of the channel's geometry (FR-4), so the span cannot overflow
        // here — the guard above is what bounds it.
        const uint64_t n = frame_span(len, align_);
        uint64_t off = 0;
        bool wrap = false;
        if (!try_reserve(n, &off, &wrap)) return kErrWouldBlock;
        unsigned char* dst = data_ + off;
        for (unsigned i = 0; i < 8; ++i) {
            dst[i] = static_cast<unsigned char>((len >> (8 * i)) & 0xFF);
        }
        if (len != 0) {
            std::memcpy(dst + frame_header_span(align_), payload, len);
        }
        commit(n, off, wrap, len);  // P1/P2 publish
        return kOk;
    }

    // Blocking framed write: brief spin, then park on not_full.
    // kOk | kErrMsgTooLarge | kErrPeerDead (consumer heartbeat went stale).
    int write(const void* payload, uint64_t len) {
        if (len > h_->max_payload) return kErrMsgTooLarge;  // fail fast, never park
        int rc;
        for (int s = 0; s < detail::kSpinBeforePark; ++s) {
            if ((rc = try_write(payload, len)) != kErrWouldBlock) return rc;
            cpu_relax();
        }
        for (;;) {
            if ((rc = try_write(payload, len)) != kErrWouldBlock) return rc;
            const int prc = park_until_space(frame_span(len, align_));
            if (prc != kOk) return prc;
        }
    }

    // Zero-copy borrow path, producer side (IF-2 / FR-10): reserve a
    // contiguous writable span; publish nothing until commit_write. The
    // reservation is process-local state (A1) — a producer that dies
    // mid-reservation leaves NO shared-state inconsistency behind.
    int try_acquire_write(void** ptr, uint64_t len) {
        if (res_active_ || ptr == nullptr) return kErrInvalidArgs;
        if (len > h_->max_payload) return kErrMsgTooLarge;
        uint64_t off = 0;
        bool wrap = false;
        if (!try_reserve(frame_span(len, align_), &off, &wrap)) {
            return kErrWouldBlock;
        }
        res_off_ = off;
        res_len_ = len;
        res_wrap_ = wrap;
        res_active_ = true;
        // The aligned span the caller fills starts a whole page into the frame,
        // and off is a page multiple, so this pointer is page-aligned — the
        // producer-side mirror of what Consumer::parse hands out.
        *ptr = data_ + off + frame_header_span(align_);
        return kOk;
    }

    int acquire_write(void** ptr, uint64_t len) {
        int rc;
        for (;;) {
            if ((rc = try_acquire_write(ptr, len)) != kErrWouldBlock)
                return rc;
            const int prc = park_until_space(frame_span(len, align_));
            if (prc != kOk) return prc;
        }
    }

    // Publish actual_len (<= reserved) bytes of the acquired span (FR-10).
    int commit_write(uint64_t actual_len) {
        if (!res_active_ || actual_len > res_len_) return kErrInvalidArgs;
        unsigned char* dst = data_ + res_off_;
        for (unsigned i = 0; i < 8; ++i) {
            dst[i] =
                static_cast<unsigned char>((actual_len >> (8 * i)) & 0xFF);
        }
        res_active_ = false;
        // A partial commit shrinks the frame to the geometry of what was
        // actually written (aligned mode: still a whole number of pages, so the
        // next frame start stays aligned), exactly as the classic path shrinks
        // it to 8 + actual_len.
        commit(frame_span(actual_len, align_), res_off_, res_wrap_, actual_len);
        return kOk;
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

    // kOk after a wake/timeout (caller retries), kErrPeerDead if the
    // consumer's heartbeat has gone stale (A3).
    int park_until_space(uint64_t n) {
        bump_heartbeat();  // we are alive, even while blocked
        // Snapshot the watched cursor BEFORE the predicate: if the consumer
        // advances read after this load, park_wait_cursor returns at once.
        const uint64_t r_seen = h_->read.load(std::memory_order_relaxed);
        // Dekker pair, waiter side (see PARKING PROTOCOL block above).
        h_->producer_waiting.store(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (can_reserve(n)) {
            h_->producer_waiting.store(0, std::memory_order_relaxed);
            return kOk;
        }
        if (peer_stale_.stale(
                h_->consumer_heartbeat.load(std::memory_order_relaxed),
                stale_ns_)) {
            h_->producer_waiting.store(0, std::memory_order_relaxed);
            return kErrPeerDead;
        }
        ++lock_count_;
        // Sleep until the consumer publishes ANY progress on read (the
        // lost-wakeup guard lives inside the seam: cursor recheck under the
        // robust lock on Linux; value-atomic kernel wait on macOS).
        park_wait_cursor(&h_->read, r_seen, &h_->park.lock, &h_->park.not_full,
                         detail::kParkTimeoutNs);
        h_->producer_waiting.store(0, std::memory_order_relaxed);
        return kOk;  // staleness is re-evaluated on the next iteration
    }

    void bump_heartbeat() {
        // single-writer: plain load+store, never an RMW
        h_->producer_heartbeat.store(
            h_->producer_heartbeat.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
    }

    // Signaler side of the consumer's Dekker pair; called after every
    // commit. Peer not parked => one fence + one relaxed load, no mutex.
    void wake_consumer_if_waiting() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (h_->consumer_waiting.load(std::memory_order_relaxed) != 0) {
            ++lock_count_;
            park_wake_cursor(&h_->write, &h_->park.lock, &h_->park.not_empty);
        }
    }

 public:
    // Process-local count of park-mutex acquisitions by this handle; the
    // G4.3 hot-path proof: stays 0 while the peer never parks.
    uint64_t locks_taken() const { return lock_count_; }

 private:
    uint64_t lock_count_ = 0;
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

    // Producer-owned counters: single-writer plain load+store, exactly the
    // heartbeat idiom (never an RMW — nothing else writes this line). Counted
    // at the commit that publishes the message, so a message is "written" iff
    // the consumer can see it. PAYLOAD bytes only — the caller's own length,
    // passed down explicitly rather than derived from the frame span, so that
    // an aligned channel's header page and tail padding (transport overhead,
    // like the 8-byte header before it) are excluded exactly the same way.
    void bump_write_stats(uint64_t payload_len) {
        if (stats_ == nullptr) return;  // v1 segment: those bytes are data
        stats_->stat_msgs_written.store(
            stats_->stat_msgs_written.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        stats_->stat_bytes_written.store(
            stats_->stat_bytes_written.load(std::memory_order_relaxed) +
                payload_len,
            std::memory_order_relaxed);
    }

    void commit(uint64_t n, uint64_t off, bool wrap, uint64_t payload_len) {
        if (wrap) {
            // P2: watermark first (relaxed), sequenced before the write
            // release below — consumers acquire both atomically-in-effect.
            h_->watermark.store(h_->write.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
            h_->write.store(n, std::memory_order_release);  // P1
        } else {
            h_->write.store(off + n, std::memory_order_release);  // P1
        }
        bump_write_stats(payload_len);
        wake_consumer_if_waiting();  // A4 signaler side
        bump_heartbeat();
    }

    ChannelHeader* h_;
    unsigned char* data_;
    uint64_t cap_;
    uint64_t stale_ns_;
    ChannelHeader* stats_;  // == h_ on a v2 segment, nullptr on v1
    uint64_t align_;        // 0 = classic framing, page = kFlagAlignedSpans
    detail::StaleTracker peer_stale_;
    // Outstanding zero-copy reservation (process-local per A1).
    uint64_t res_off_ = 0;
    uint64_t res_len_ = 0;
    bool res_wrap_ = false;
    bool res_active_ = false;
};

class Consumer {
 public:
    explicit Consumer(Channel* ch,
                      uint64_t stale_threshold_ns = detail::kDefaultStaleNs)
        : h_(ch->hdr),
          data_(static_cast<unsigned char*>(
              resolve(ch->base, ch->hdr->data_offset))),
          stale_ns_(stale_threshold_ns),
          // Same gate as the producer's: null on v1, where those bytes are the
          // data region (see header.hpp).
          stats_(has_stats(ch->hdr) ? ch->hdr : nullptr),
          // ...and the same frame-geometry gate, read from the same immutable
          // flags word the producer read. Neither side is told the mode by its
          // caller: the SEGMENT says it, which is why an opener that knows the
          // bit parses an aligned channel correctly with no extra API.
          align_(has_aligned_spans(ch->hdr) ? static_cast<uint64_t>(page_size())
                                            : 0),
          // PREFETCH GATE, resolved once from that same immutable flags word,
          // by the same argument as the two gates above. True ONLY on a
          // file-backed segment (kFlagFileBacked), where an unread page can be
          // a disk read; on an shm segment there is nothing to bring in, so the
          // default path pays exactly one perfectly-predictable branch per
          // acquire and never reaches the advisory code. The existing perf
          // tests (bench, nocopy_cpu, park_latency) are the guard on that.
          prefetch_((ch->hdr->flags & kFlagFileBacked) != 0),
          // page_size() is a host constant but not free to query; resolve it
          // once, and only when the gate is on (0 = "never needed").
          page_(prefetch_ ? static_cast<uint64_t>(page_size()) : 0) {}

    // A3: announce liveness without consuming data.
    void keepalive() { bump_heartbeat(); }

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
                return acquired(parse(r, m - r, payload, len));
            }
        }
        if (w == r) return kErrWouldBlock;  // empty
        return acquired(parse(r, w - r, payload, len));
    }

    // READ-ONLY LOOKAHEAD (v1.4): is the next UN-BORROWED message committed,
    // and how long is its payload? kOk (with *len_out set) | kErrWouldBlock |
    // kErrCorrupt | kErrInvalidArgs (null out-param).
    //
    // "Un-borrowed" is what makes this a new capability rather than a
    // restatement of try_read: borrows here are strictly release-before-
    // acquire — a consumer holding message N cannot acquire N+1 — so with a
    // borrow outstanding there is no other way to learn that N+1 has arrived,
    // or how big it is, before committing to releasing N. The position it
    // reports on is therefore `read + borrowed_`, which is the next frame
    // start with 0 borrows outstanding AND with 1.
    //
    // It writes NOTHING (see the PEEK paragraph in the contract above): no
    // cursor, no borrowed_, no heartbeat. Calling it does not create, extend,
    // or invalidate a borrow, and it can be called with one outstanding.
    int peek_next(uint64_t* len_out) const {
        if (len_out == nullptr) return kErrInvalidArgs;
        const uint64_t r = h_->read.load(std::memory_order_relaxed);  // ours
        // The next frame start past whatever we are holding. borrowed_ is 0
        // when nothing is borrowed, so this one expression covers both cases.
        const uint64_t pos = r + borrowed_;
        const uint64_t w = h_->write.load(std::memory_order_acquire);  // P1
        if (w < r) {
            // Wrapped: committed data is [r, m) then [0, w). watermark
            // visibility is guaranteed by P2 exactly as in try_read (relaxed).
            const uint64_t m = h_->watermark.load(std::memory_order_relaxed);
            if (pos < m) return peek_at(pos, m - pos, len_out);
            // THE WRAP SUBTLETY. pos == m: the high region holds nothing we
            // have not already borrowed, so the next message is the one at
            // offset 0. try_read reaches this state and performs the C2 A->B
            // handoff (read = 0) before parsing there. Peek MUST NOT: it is
            // read-only, and the handoff is a store to `read`. It simply reads
            // the header at 0 instead and leaves the cursor exactly where it
            // was — the handoff still happens, later, in the try_read that
            // actually consumes the message. Nothing is lost by deferring it,
            // because the handoff publishes only that the high region is free,
            // and peek frees nothing.
            // (A wrapped state always has w > 0: the wrap commit publishes a
            // whole frame at offset 0. So [0, w) is a non-empty committed run
            // and this is never the empty case.)
            return peek_at(0, w, len_out);
        }
        // Linear: committed data is [r, w), and pos <= w always (a borrowed
        // frame lies inside the committed run). Equality means we have already
        // borrowed/consumed everything published.
        if (pos >= w) return kErrWouldBlock;
        return peek_at(pos, w - pos, len_out);
    }

    // Blocking borrow: brief spin, then park on not_empty.
    // kOk | kErrCorrupt | kErrPeerDead (producer heartbeat went stale).
    int read(const unsigned char** payload, uint64_t* len) {
        int rc;
        for (int s = 0; s < detail::kSpinBeforePark; ++s) {
            if ((rc = try_read(payload, len)) != kErrWouldBlock) return rc;
            cpu_relax();
        }
        for (;;) {
            if ((rc = try_read(payload, len)) != kErrWouldBlock) return rc;
            const int prc = park_until_data();
            if (prc != kOk) return prc;
        }
    }

    // C1: publish that the borrowed message's bytes are free to reuse.
    void release() {
        const uint64_t r = h_->read.load(std::memory_order_relaxed);
        // borrowed_ is the frame SPAN — 8 + len classically, page +
        // round_up(len, page) in aligned mode — so the cursor always lands on
        // the next frame start, and in aligned mode that start stays page
        // -aligned.
        h_->read.store(r + borrowed_, std::memory_order_release);
        bump_read_stats();
        borrowed_ = 0;
        borrowed_len_ = 0;
        wake_producer_if_waiting();  // A4 signaler side
        bump_heartbeat();
    }

 private:
    // Park-decision predicate (under the A4 fence). write != read covers the
    // wrapped state too; a spurious wake merely retries try_read.
    bool data_available() const {
        return h_->write.load(std::memory_order_relaxed) !=
               h_->read.load(std::memory_order_relaxed);
    }

    // kOk after a wake/timeout (caller retries), kErrPeerDead if the
    // producer's heartbeat has gone stale (A3).
    int park_until_data() {
        bump_heartbeat();  // we are alive, even while blocked
        // Snapshot the watched cursor BEFORE the predicate (see producer).
        const uint64_t w_seen = h_->write.load(std::memory_order_relaxed);
        // Dekker pair, waiter side (see PARKING PROTOCOL block above).
        h_->consumer_waiting.store(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (data_available()) {
            h_->consumer_waiting.store(0, std::memory_order_relaxed);
            return kOk;
        }
        if (peer_stale_.stale(
                h_->producer_heartbeat.load(std::memory_order_relaxed),
                stale_ns_)) {
            h_->consumer_waiting.store(0, std::memory_order_relaxed);
            return kErrPeerDead;
        }
        ++lock_count_;
        // PREFETCH HOOK (a): the last thing before we sleep. Normally there is
        // nothing to advise — we only get here because the predicate said the
        // ring is empty — but the window between that check and the sleep is
        // exactly where a commit can land, and a page we ask for now is one we
        // are not faulting in after the wake. Costs one predictable branch when
        // the gate is off, and no syscall when the region is empty.
        if (prefetch_) prefetch_unread();
        // Sleep until the producer publishes ANY progress on write.
        park_wait_cursor(&h_->write, w_seen, &h_->park.lock,
                         &h_->park.not_empty, detail::kParkTimeoutNs);
        h_->consumer_waiting.store(0, std::memory_order_relaxed);
        return kOk;  // staleness is re-evaluated on the next iteration
    }

    void bump_heartbeat() {
        // single-writer: plain load+store, never an RMW
        h_->consumer_heartbeat.store(
            h_->consumer_heartbeat.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
    }

    // Consumer-owned counters, mirroring the producer's: single-writer plain
    // load+store, counted at the release that frees the message's bytes, and
    // payload-only. The payload length is kept alongside the span rather than
    // recovered from it, so the aligned framing's header page and tail padding
    // are excluded exactly as the 8-byte header is — bytes_read stays directly
    // comparable to bytes_written and to the lengths the caller saw. A release
    // with no active borrow (span 0) counts nothing — the A->B handoff store of
    // read = 0 is not a message and must not be counted either.
    void bump_read_stats() {
        if (stats_ == nullptr || borrowed_ == 0) return;
        stats_->stat_msgs_read.store(
            stats_->stat_msgs_read.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        stats_->stat_bytes_read.store(
            stats_->stat_bytes_read.load(std::memory_order_relaxed) +
                borrowed_len_,
            std::memory_order_relaxed);
    }

    void wake_producer_if_waiting() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (h_->producer_waiting.load(std::memory_order_relaxed) != 0) {
            ++lock_count_;
            park_wake_cursor(&h_->read, &h_->park.lock, &h_->park.not_full);
        }
    }

 public:
    // Process-local count of park-mutex acquisitions by this handle (G4.3).
    uint64_t locks_taken() const { return lock_count_; }

    // Is the WILLNEED prefetch enabled on this handle? The gate itself, exposed
    // read-only for the same reason locks_taken() is: a test that claims the
    // default path is branch-off has to be able to SEE that it is off, and
    // "verified indirectly by timing" is not a check. Fixed at construction.
    bool prefetching() const { return prefetch_; }

 private:
    uint64_t lock_count_ = 0;

    // Prefetch hook (b), on the successful-acquire path: advise the committed
    // region BEYOND the borrow we just handed out, so the pages the caller will
    // want next are on their way in while it works on this one. Written as a
    // pass-through on parse()'s result so try_read keeps its shape, and gated
    // first so the default path is one predictable branch and nothing else.
    int acquired(int rc) {
        if (prefetch_ && rc == kOk) prefetch_unread();
        return rc;
    }

    // Advise the committed-but-unread region past the active borrow. Two runs
    // in the wrapped state, one otherwise; ONLY committed data is ever named
    // (advising the free part of the ring would ask the kernel to fetch pages
    // whose contents are about to be overwritten). Loads exactly what peek
    // loads, with the same orders, and stores nothing.
    void prefetch_unread() const {
        const uint64_t r = h_->read.load(std::memory_order_relaxed);
        const uint64_t pos = r + borrowed_;
        const uint64_t w = h_->write.load(std::memory_order_acquire);  // P1
        if (w >= r) {  // linear: committed [r, w)
            if (pos < w) advise_run(pos, w - pos);
            return;
        }
        // Wrapped: committed [r, m) then [0, w) — both worth having, and the
        // second one is where the consumer is headed next.
        const uint64_t m = h_->watermark.load(std::memory_order_relaxed);  // P2
        if (pos < m) advise_run(pos, m - pos);
        advise_run(0, w);
    }

    // One contiguous run [off, off + n) of the data region, page-clamped.
    // madvise operates on whole PAGES and rejects an unaligned address, and the
    // data region does NOT start on a page in the default framing (data_offset
    // is 1280/1536) — so the start ADDRESS is floored to its enclosing page and
    // the bytes that adds are folded into the length. Flooring can only walk
    // back into the header, never out of the mapping: the mapping's own base is
    // page-aligned and lies at or below this address, so its page floor is a
    // lower bound on ours. The end is inside the data region by construction.
    void advise_run(uint64_t off, uint64_t n) const {
        if (n == 0) return;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(data_ + off);
        const uintptr_t start = addr & ~(static_cast<uintptr_t>(page_) - 1);
        advise_willneed(reinterpret_cast<void*>(start),
                        static_cast<size_t>(addr - start + n));
    }

    // The read-only half of parse(): the identical little-endian header decode
    // and the identical guards — `l > max_payload` first, then frame_fits()'s
    // subtraction-against-a-checked-floor form, never the sum `header + l` that
    // wraps for an l near 2^64 (fuzz/header_fuzz.cpp's lesson). What it does
    // NOT do is everything that mutates: no *payload, no borrowed_, no
    // borrowed_len_. A corrupt length is kErrCorrupt here exactly as there.
    int peek_at(uint64_t at, uint64_t avail, uint64_t* len_out) const {
        const unsigned char* blk = data_ + at;
        uint64_t l = 0;
        for (unsigned i = 0; i < 8; ++i) {
            l |= static_cast<uint64_t>(blk[i]) << (8 * i);
        }
        if (l > h_->max_payload || !frame_fits(l, avail, align_))
            return kErrCorrupt;
        *len_out = l;
        return kOk;
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
        // segment is corrupt; never hand out an out-of-bounds span. The
        // available-space bound is subtraction against a checked floor, not
        // the sum `kFrameHeader + l`: that addition wraps for an l near 2^64.
        // The `l > max_payload` guard ahead of it already catches such an l on
        // a validated segment, but keeping this form makes the backstop a real
        // backstop rather than one that overflows in the same way (the class
        // of bug fuzz/header_fuzz.cpp surfaced in validate_header). frame_fits
        // IS that form, for both framings (bipbuffer.hpp).
        if (l > h_->max_payload || !frame_fits(l, avail, align_))
            return kErrCorrupt;
        // Aligned mode: the header occupies the whole first page of the frame
        // and the payload begins at the next page boundary. `at` is a frame
        // start, hence a page multiple, and the data region itself starts
        // page-aligned — so this pointer is page-aligned, which is the entire
        // promise of kFlagAlignedSpans.
        *payload = blk + frame_header_span(align_);
        *len = l;
        borrowed_ = frame_span(l, align_);
        borrowed_len_ = l;
        return kOk;
    }

    ChannelHeader* h_;
    unsigned char* data_;
    uint64_t borrowed_ = 0;      // consumer-private span of the active borrow
    uint64_t borrowed_len_ = 0;  // ...and its payload length (stats only)
    uint64_t stale_ns_;
    ChannelHeader* stats_;  // == h_ on a v2 segment, nullptr on v1
    uint64_t align_;        // 0 = classic framing, page = kFlagAlignedSpans
    bool prefetch_;         // kFlagFileBacked: WILLNEED the unread region
    uint64_t page_;         // page_size() when prefetch_, else 0 (unused)
    detail::StaleTracker peer_stale_;
};

}  // namespace shuttle
