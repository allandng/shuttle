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

// ---------------------------------------------------------------------
// FRAME GEOMETRY — the ONE place the two framings are stated. `align` is 0 for
// the default (and only pre-v1.4) framing, or the system page size when the
// channel was created with kFlagAlignedSpans:
//
//   classic (align == 0):
//       [8B length][payload]                     span = 8 + len
//       payload at frame_start + 8
//
//   aligned (align == page):
//       [8B length][pad to page][payload][pad to page]
//       payload at frame_start + page             span = page + roundup(len)
//
// Frame starts stay page-aligned in aligned mode because every span is a whole
// number of pages, the data region itself starts page-aligned, and a wrap
// restarts at offset 0. That is the entire mechanism: the payload pointer a
// consumer borrows is then page-aligned unconditionally, which is what lets it
// be handed to MTLBuffer newBufferWithBytesNoCopy / cudaHostRegister with no
// copy. Cost is bounded internal fragmentation: at most `2*align - 8 - 1` bytes
// per message (the tail of the header page, plus the payload's rounding).
//
// `align` MUST be zero or a power of two (platform.hpp's page_size()
// guarantees the latter) — the mask arithmetic below assumes it.
// ---------------------------------------------------------------------

// Smallest multiple of `page` that is >= n. Caller must have bounded n
// (n <= UINT64_MAX - page + 1); frame_fits() is the overflow-safe gate.
inline uint64_t round_up_page(uint64_t n, uint64_t page) {
    return (n + (page - 1)) & ~(page - 1);
}

// Largest multiple of `page` that is <= n. Total, no overflow possible.
inline uint64_t floor_page(uint64_t n, uint64_t page) {
    return n & ~(page - 1);
}

// Bytes between a frame's start and its payload.
inline uint64_t frame_header_span(uint64_t align) {
    return align != 0 ? align : kFrameHeader;
}

// Total bytes a message of `len` payload bytes occupies. Only sound once
// frame_fits() has said the frame fits somewhere — otherwise the rounding can
// overflow.
inline uint64_t frame_span(uint64_t len, uint64_t align) {
    return align != 0 ? align + round_up_page(len, align) : kFrameHeader + len;
}

// Does a frame carrying `len` payload bytes fit in `avail` bytes? Written as
// SUBTRACTION against a checked floor, never as the sum `header + len`: that
// addition wraps for a len near 2^64 and turns the guard into its opposite (the
// exact class of bug fuzz/header_fuzz.cpp found in validate_header). In aligned
// mode the rounding is compared the same way — `round_up(len) <= room` is
// exactly `len <= floor_page(room)`, with no sum formed anywhere.
inline bool frame_fits(uint64_t len, uint64_t avail, uint64_t align) {
    const uint64_t hdr = frame_header_span(align);
    if (avail < hdr) return false;
    const uint64_t room = avail - hdr;
    return align != 0 ? len <= floor_page(room, align) : len <= room;
}

class BipBuffer {
 public:
    // `align` = 0 keeps the classic framing (every existing caller); a nonzero
    // power of two selects the aligned framing above. It is a per-instance
    // property, fixed at construction — never a global and never mutable, so
    // one process can hold buffers of both kinds.
    BipBuffer(unsigned char* data, uint64_t capacity, uint64_t align = 0)
        : data_(data), cap_(capacity), align_(align) {}

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
        // Overflow gate BEFORE the span is formed: a len near 2^64 makes
        // `kFrameHeader + len` (or the aligned rounding) wrap to a small number
        // that reserve() would happily accept.
        if (!frame_fits(len, cap_, align_)) return false;
        const uint64_t span = frame_span(len, align_);
        unsigned char* dst = nullptr;
        if (!reserve(span, &dst)) return false;
        for (unsigned i = 0; i < 8; ++i) {
            dst[i] = static_cast<unsigned char>((len >> (8 * i)) & 0xFF);
        }
        if (len != 0) {
            std::memcpy(dst + frame_header_span(align_), payload, len);
        }
        commit(span);
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
        *payload = blk + frame_header_span(align_);
        *len = l;
        last_msg_ = frame_span(l, align_);
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
    // 0 for the classic framing, the page size for an aligned-span buffer.
    uint64_t frame_align() const { return align_; }
    // Bytes currently committed and unread, derived from the invariant.
    uint64_t size() const {
        return write_ >= read_ ? write_ - read_
                               : (watermark_ - read_) + write_;
    }

 private:
    unsigned char* data_;
    uint64_t cap_;
    // Frame geometry, fixed at construction (see FRAME GEOMETRY above). The raw
    // reserve/commit/read_block protocol below is untouched by it: aligned mode
    // only ever asks for a LARGER contiguous block, so the never-straddle-wrap
    // invariant and the strict `>` wrapped-space checks carry over unchanged.
    uint64_t align_;
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
