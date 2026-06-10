#pragma once

// BipBuffer core logic (Phase 2): single-threaded, plain cursors, plain
// memory — no shared memory, no atomics, no platform code. Phase 3 promotes
// exactly this protocol onto the shared header's atomic cursors.
//
// Cursor model per binding amendment A1 (bbqueue-style), three absolute
// offsets into the data region, each with exactly one writer:
//   write     (producer): end of committed data
//   watermark (producer): end of valid data before a wrap
//   read      (consumer): start of valid data
//
// Invariant:
//   write >= read  -> valid data is [read, write)            ("linear")
//   write <  read  -> valid data is [read, watermark) then [0, write)
//                                                            ("wrapped")
// Regions A/B exist only as these derived ranges; nothing else is stored.
// The wrapped state requires write < read STRICTLY: a reservation may never
// advance write to equal read from below, or "wrapped full" would alias
// "linear empty" (write == read). Hence the strict `>` in the wrapped-space
// checks below.
//
// The A->B handoff is the consumer alone observing read == watermark while
// write < read, then storing read = 0 — a single owned-variable update.
//
// Early-wrap rule (App. B #6): a reservation that does not fit between
// write and the physical end wraps WHOLE to offset 0; payloads never split.

#include <cstdint>
#include <cstring>

namespace shuttle {

// Transport framing: each message is [u64 little-endian length | payload].
// Owned by Shuttle; payload bytes are never interpreted (D6).
constexpr uint64_t kFrameHeader = 8;

class BipBuffer {
 public:
    BipBuffer(unsigned char* data, uint64_t capacity)
        : data_(data), cap_(capacity) {}

    // --- raw contiguous-block protocol (producer side) ---

    // Reserve n contiguous bytes. Fails (false) if no contiguous run of n
    // bytes exists right now — never splits, never overwrites unread data.
    bool reserve(uint64_t n, unsigned char** ptr) {
        if (n == 0 || n > cap_ || reserve_active_) return false;
        if (write_ >= read_) {
            if (cap_ - write_ >= n) {
                res_at_ = write_;  // grow the linear region in place
                res_wrap_ = false;
            } else if (read_ > n) {  // strict: post-commit write=n < read
                res_at_ = 0;  // early wrap: whole block to offset 0
                res_wrap_ = true;
            } else {
                return false;
            }
        } else {
            // Already wrapped: append in the low region, never past read.
            if (read_ - write_ > n) {  // strict: keep write < read
                res_at_ = write_;
                res_wrap_ = false;
            } else {
                return false;
            }
        }
        res_n_ = n;
        reserve_active_ = true;
        *ptr = data_ + res_at_;
        return true;
    }

    // Publish actual_len (<= reserved, > 0) bytes of the reservation (FR-10).
    void commit(uint64_t actual_len) {
        if (!reserve_active_ || actual_len == 0 || actual_len > res_n_) return;
        if (res_wrap_) {
            watermark_ = write_;  // end of valid data in the high region
            write_ = actual_len;  // strictly < read_ by the reserve check
        } else {
            write_ += actual_len;
        }
        reserve_active_ = false;
    }

    void abort_reserve() { reserve_active_ = false; }

    // --- raw contiguous-block protocol (consumer side) ---

    // Next readable contiguous block, or false if empty.
    bool read_block(const unsigned char** ptr, uint64_t* len) {
        if (write_ < read_ && read_ == watermark_) {
            // A->B handoff: high region drained; adopt the low region.
            read_ = 0;
        }
        if (write_ >= read_) {
            if (write_ == read_) return false;  // empty
            *ptr = data_ + read_;
            *len = write_ - read_;
        } else {
            *ptr = data_ + read_;
            *len = watermark_ - read_;
        }
        return true;
    }

    // Contract: n <= the len returned by the latest read_block.
    void release(uint64_t n) { read_ += n; }

    // --- framed message layer (§2.4) ---

    bool write_msg(const void* payload, uint64_t len) {
        unsigned char* dst = nullptr;
        if (!reserve(kFrameHeader + len, &dst)) return false;
        for (unsigned i = 0; i < 8; ++i) {
            dst[i] = static_cast<unsigned char>((len >> (8 * i)) & 0xFF);
        }
        if (len != 0) std::memcpy(dst + kFrameHeader, payload, len);
        commit(kFrameHeader + len);
        return true;
    }

    // Borrow the next payload in place; valid until release_msg().
    bool read_msg(const unsigned char** payload, uint64_t* len) {
        const unsigned char* blk = nullptr;
        uint64_t blen = 0;
        if (!read_block(&blk, &blen)) return false;
        // Whole-unit reservation means a readable block always starts at a
        // message boundary and contains only whole messages.
        uint64_t l = 0;
        for (unsigned i = 0; i < 8; ++i) {
            l |= static_cast<uint64_t>(blk[i]) << (8 * i);
        }
        *payload = blk + kFrameHeader;
        *len = l;
        last_msg_ = kFrameHeader + l;
        return true;
    }

    void release_msg() {
        release(last_msg_);
        last_msg_ = 0;
    }

    // --- introspection (tests, inspect tooling) ---
    uint64_t read_cursor() const { return read_; }
    uint64_t write_cursor() const { return write_; }
    uint64_t watermark_cursor() const { return watermark_; }
    uint64_t capacity() const { return cap_; }
    // Bytes currently committed and unread, derived from the invariant.
    uint64_t size() const {
        return write_ >= read_ ? write_ - read_
                               : (watermark_ - read_) + write_;
    }

 private:
    unsigned char* data_;
    uint64_t cap_;
    // Shared-in-Phase-3 cursors (plain here; atomics in the segment later).
    uint64_t write_ = 0;
    uint64_t watermark_ = 0;
    uint64_t read_ = 0;
    // Producer-private reservation state: per amendment A1 this NEVER moves
    // into the segment.
    uint64_t res_at_ = 0;
    uint64_t res_n_ = 0;
    bool res_wrap_ = false;
    bool reserve_active_ = false;
    // Consumer-private: span of the message returned by the last read_msg.
    uint64_t last_msg_ = 0;
};

}  // namespace shuttle
