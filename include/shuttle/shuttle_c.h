/* Shuttle C ABI v1 — the frozen extern "C" surface (SRS §3.1, IF-1..IF-4).
 *
 * This header is the single source of truth for foreign-language bindings
 * (Rust bindgen, Python cffi). C only: fixed-width/standard types, no C++.
 * No exception ever crosses this boundary; every failure is an integer
 * code. Functions returning a pointer report the code through *err;
 * functions returning int/long return SHUTTLE_OK / a negative code (the
 * copy-read returns the non-negative payload length on success).
 *
 * Changing any signature, constant, or semantic here is an ABI break and
 * requires bumping SHUTTLE_ABI_VERSION. The v1 surface (the 10 functions
 * below shuttle_create..shuttle_keepalive) is FROZEN and unchanged; the
 * v1.1 additions (shuttle_create_ex + SHUTTLE_CREATE_* below) are strictly
 * additive — new symbols only, no existing signature or semantic touched —
 * so SHUTTLE_ABI_VERSION stays 1 (old binaries keep linking and running).
 *
 * v1.2 adds shuttle_get_stats + shuttle_stats + SHUTTLE_CREATE_STATS and the
 * error codes below, on the same additive terms: new symbols and new
 * constants only. SHUTTLE_ABI_VERSION stays 1. One thing to know about
 * SHUTTLE_CREATE_STATS specifically: unlike every other create-flag, it
 * changes the segment's LAYOUT VERSION (1 -> 2). Layout versions are checked
 * exactly, not ignored like unknown flag bits, so a segment created with it
 * cannot be opened by a binary built before v1.2 — that opener reports
 * SHUTTLE_ERR_BAD_VERSION. Opting in is therefore a decision about which
 * peers can attach; leaving it off keeps the unchanged v1 format.
 *
 * v1.3 adds the per-op flag SHUTTLE_DROP_NEWEST and the return SHUTTLE_DROPPED.
 * Additive again — no signature and no existing semantic changes, so
 * SHUTTLE_ABI_VERSION stays 1 — but READ THE NOTE ON SHUTTLE_DROPPED BELOW:
 * it is the first POSITIVE (non-error, non-zero) return in this ABI, and only
 * a call that opted into the flag can ever receive it.
 *
 * v1.4 adds the create-flag SHUTTLE_CREATE_ALIGNED_SPANS. It is additive in the
 * ABI sense — one new constant, no new function, no signature and no existing
 * semantic touched, so SHUTTLE_ABI_VERSION stays 1 — and the acquire/read
 * entry points already hand back the span pointers it aligns. But it belongs in
 * the same class as SHUTTLE_CREATE_STATS: it changes what is ON THE SEGMENT, so
 * a segment created with it cannot be opened by a binary built before v1.4.
 * That opener reports SHUTTLE_ERR_CORRUPT (not BAD_VERSION — see the flag's
 * note below). Opting in is therefore a decision about which peers can attach.
 *
 * v1.4 also adds FILE-BACKED channels: shuttle_create_file / shuttle_open_file
 * / shuttle_unlink_file, three new symbols that take an absolute PATH where the
 * v1 lifecycle functions take an shm name. Additive in the strictest sense —
 * the name-typed functions are untouched in signature, semantics, and behavior,
 * and a caller that never mentions a path sees no change at all — so
 * SHUTTLE_ABI_VERSION stays 1. The segment they produce is byte-for-byte an
 * ordinary Shuttle segment that happens to live in a file, which is what lets a
 * channel's capacity exceed physical RAM. See docs/API.md, "File-backed
 * channels", for the crash story, the stale-file recipe, and the explicit
 * non-goal of durability.
 */
#ifndef SHUTTLE_C_H
#define SHUTTLE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHUTTLE_ABI_VERSION 1

/* Error codes (mirror shuttle::Err; static_asserted in the implementation). */
#define SHUTTLE_OK 0

/* NOT an error: the ONE positive return in this ABI (v1.3). shuttle_write
 * returns it when — and only when — the caller passed SHUTTLE_DROP_NEWEST and
 * the ring could not take the message, so the message was dropped. Nothing was
 * written and nothing already queued was disturbed.
 *
 * IT IS NOT SHUTTLE_OK. Code written as `if (rc != SHUTTLE_OK) fail();` counts
 * a successful drop as a failure. Since only a call that passed the flag can
 * receive it, existing code is unaffected — but new code should test errors as
 * `rc < 0`, which is correct for every entry point here, present and future:
 *
 *     int rc = shuttle_write(ch, p, n, SHUTTLE_DROP_NEWEST);
 *     if (rc < 0) return rc;                 // real failure
 *     if (rc == SHUTTLE_DROPPED) ++dropped;  // policy fired, not an error
 */
#define SHUTTLE_DROPPED 1
#define SHUTTLE_ERR_INVALID_ARGS (-1)
#define SHUTTLE_ERR_NAME_TOO_LONG (-2)
#define SHUTTLE_ERR_EXISTS (-3)
#define SHUTTLE_ERR_NOT_FOUND (-4)
#define SHUTTLE_ERR_SYS (-5)
#define SHUTTLE_ERR_BAD_MAGIC (-6)
#define SHUTTLE_ERR_BAD_VERSION (-7)
#define SHUTTLE_ERR_CAPACITY_TOO_SMALL (-8)
#define SHUTTLE_ERR_INIT_TIMEOUT (-9)
#define SHUTTLE_ERR_CORRUPT (-10)
#define SHUTTLE_ERR_MSG_TOO_LARGE (-11)
#define SHUTTLE_ERR_WOULD_BLOCK (-12)
#define SHUTTLE_ERR_PEER_DEAD (-13)
/* shuttle_create_ex with SHUTTLE_CREATE_HUGETLB_2MB/1GB: the explicit huge
 * pages could not be delivered (no hugetlbfs mount with that page size, no free
 * reserved pages, or a platform without hugetlbfs). The request NEVER falls
 * back to normal pages — this code is returned instead. */
#define SHUTTLE_ERR_NO_HUGEPAGES (-14)
/* shuttle_get_stats on a channel whose segment has no stats block (i.e. it was
 * not created with SHUTTLE_CREATE_STATS). */
#define SHUTTLE_ERR_NO_STATS (-15)

/* Per-op flags (IF-3): blocking is the default; OR in SHUTTLE_NONBLOCK for
 * try-semantics ("would block" instead of parking). Passed to the read/write
 * entry points. */
#define SHUTTLE_NONBLOCK 0x1

/* Opt-in LOSSY backpressure policy for shuttle_write (v1.3), and the only way
 * this library ever drops a message. "Backpressure, never drops" stays the
 * default and the guarantee: this bit changes behavior for exactly the one call
 * it is passed on, and there is no channel-wide, create-time, or fallback form
 * of it.
 *
 * With it, shuttle_write NEVER parks (it implies try-semantics) and:
 *   - message fits now      -> written; SHUTTLE_OK.
 *   - ring cannot take it   -> message DROPPED, nothing written, nothing
 *                              already queued disturbed; SHUTTLE_DROPPED (1).
 *                              The drop is counted in the segment's
 *                              msgs_dropped counter on a SHUTTLE_CREATE_STATS
 *                              segment; on a v1 segment the drop still happens
 *                              but there is nowhere to count it.
 *   - len > max_payload     -> SHUTTLE_ERR_MSG_TOO_LARGE, as always. A payload
 *                              that could never fit in ANY ring state is a
 *                              caller bug, not backpressure, and is not
 *                              silently swallowed as a drop.
 * Because it never parks, it never returns SHUTTLE_ERR_PEER_DEAD either: a
 * parked or dead consumer just means the ring stays full and writes drop.
 *
 * SHUTTLE_NONBLOCK|SHUTTLE_DROP_NEWEST is redundant but accepted — both ask for
 * try-semantics, and the drop policy decides what a full ring reports.
 *
 * Valid ONLY on shuttle_write. There is nothing to drop on a read or on an
 * acquire, so shuttle_read / shuttle_acquire_write / shuttle_acquire_read
 * reject the bit with SHUTTLE_ERR_INVALID_ARGS rather than ignore it. */
#define SHUTTLE_DROP_NEWEST 0x2

/* Create-flags (v1.1): a SEPARATE namespace from the per-op flags above —
 * these are passed only to shuttle_create_ex's create_flags word, never to
 * read/write. Opt-in and additive; unknown bits are masked off by the
 * implementation. SHUTTLE_CREATE_HUGEPAGES advises transparent huge pages on
 * the segment (advisory; effective only where the kernel THP policy permits). */
#define SHUTTLE_CREATE_HUGEPAGES 0x1
/* Explicit hugetlbfs-backed segment: the channel's bytes live in RESERVED huge
 * pages of the named size, not in normal pages the kernel may or may not
 * promote. Unlike SHUTTLE_CREATE_HUGEPAGES (advisory), this is a guarantee or
 * an error: if the pages cannot be obtained, shuttle_create_ex fails with
 * SHUTTLE_ERR_NO_HUGEPAGES and creates nothing. Requires an operator to have
 * reserved pages and mounted hugetlbfs (Linux only; always
 * SHUTTLE_ERR_NO_HUGEPAGES on macOS). Setting BOTH bits is
 * SHUTTLE_ERR_INVALID_ARGS — they name two different page sizes. Openers need
 * no flag and no special call: shuttle_open finds the segment either way. */
#define SHUTTLE_CREATE_HUGETLB_2MB 0x2
#define SHUTTLE_CREATE_HUGETLB_1GB 0x4
/* Create the segment with the statistics counters (layout version 2). See the
 * ABI note at the top of this file: this is the one create-flag that changes
 * the layout version, so pre-v1.2 openers get SHUTTLE_ERR_BAD_VERSION. Without
 * it the segment is the unchanged version-1 format and shuttle_get_stats
 * returns SHUTTLE_ERR_NO_STATS. */
#define SHUTTLE_CREATE_STATS 0x8
/* Page-aligned payload spans (v1.4). Every payload this channel carries starts
 * on a system-page boundary, so a pointer from shuttle_acquire_read (or
 * shuttle_acquire_write) can be handed straight to an API that requires
 * page-aligned host memory — Metal's newBufferWithBytesNoCopy, cudaHostRegister
 * — with no copy and no bounce buffer. No new function is needed: those calls
 * already return the span pointers; this flag only changes where they land.
 *
 * Framing becomes, per message:
 *     [8-byte length][pad to page][payload][pad to page]
 * so the payload sits one page into the frame and the frame stride is
 * page + round_up(len, page). The alignment unit is the SYSTEM page (4096 on
 * every supported target), including on a hugetlbfs-backed segment — rounding
 * small messages to 2 MB or 1 GB would be a different, much worse trade.
 *
 * COSTS, stated plainly:
 *   - internal fragmentation of at most 2*page - 9 bytes per message (8183 at
 *     page = 4096): the tail of the header page plus the payload's rounding.
 *     capacity is spent on padding, so size the channel for the stride, not the
 *     payload. SHUTTLE_ERR_CAPACITY_TOO_SMALL now means
 *     capacity < page + round_up(max_payload, page).
 *   - OLD BINARIES CANNOT OPEN THE SEGMENT. Unknown flag bits are normally
 *     ignored, but an ignorer would misparse every frame here, so this bit is
 *     backed by geometry: an aligned segment's data_offset is page-rounded,
 *     which a pre-v1.4 opener rejects with SHUTTLE_ERR_CORRUPT at its header
 *     check. Deliberate, and CORRUPT rather than BAD_VERSION is the accurate
 *     verdict — the offset really does disagree with the layout rule that
 *     binary knows, and no layout version was added.
 *
 * Composes with everything else: |SHUTTLE_CREATE_STATS (0x18) is an aligned v2
 * segment whose byte counters still count PAYLOAD bytes, never the padded
 * stride; |SHUTTLE_CREATE_HUGETLB_2MB and |SHUTTLE_CREATE_HUGEPAGES are legal
 * and leave the alignment unit at the system page. */
#define SHUTTLE_CREATE_ALIGNED_SPANS 0x10

/* The segment lives in a FILE rather than in a POSIX shm object (v1.4).
 *
 * ASYMMETRIC ON PURPOSE, and the one create-flag you do not pass to
 * shuttle_create_ex: choosing this backing means supplying a PATH, and
 * shuttle_create_ex has nowhere to put one — its `name` is an shm name, with
 * shm name rules. So the backing is selected by CALLING shuttle_create_file,
 * which sets this bit itself; shuttle_create_ex masks 0x20 off exactly like any
 * other bit it does not implement, and passing it there is silently ignored
 * (not an error), per the unknown-bit rule.
 *
 * Persisted but INFORMATIONAL: an opener takes no action on it — it had to name
 * the file to attach at all, and the framing and geometry are identical to an
 * shm segment's. The bit is here so tools and peers can see what the creator
 * asked for, and so `shuttle_get_stats`-style introspection can report it. */
#define SHUTTLE_CREATE_FILE_BACKED 0x20

typedef struct shuttle_channel shuttle_channel;

/* Counter snapshot (v1.2). Byte counts are PAYLOAD bytes — the 8-byte frame
 * header is transport overhead and excluded on both sides. msgs_dropped counts
 * messages thrown away by SHUTTLE_DROP_NEWEST writes (v1.3) and nothing else,
 * so it stays 0 unless the producer opts into that policy: a full ring
 * otherwise blocks or reports SHUTTLE_ERR_WOULD_BLOCK. */
typedef struct shuttle_stats {
    uint64_t msgs_written;
    uint64_t bytes_written;
    uint64_t msgs_dropped;
    uint64_t msgs_read;
    uint64_t bytes_read;
} shuttle_stats;

/* --- lifecycle (FR-1..FR-5) --- */
shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
/* v1.1 additive extension: as shuttle_create, plus a create_flags word
 * (SHUTTLE_CREATE_* bits). shuttle_create(name, cap, maxp, err) is exactly
 * shuttle_create_ex(name, cap, maxp, 0, err). */
shuttle_channel* shuttle_create_ex(const char* name, size_t capacity_bytes,
                                   size_t max_payload_bytes,
                                   uint32_t create_flags, int* err);
shuttle_channel* shuttle_open(const char* name, int* err);
void shuttle_close(shuttle_channel* ch);
int shuttle_unlink(const char* name);

/* --- file-backed lifecycle (v1.4 additive) ---
 * The same three operations against a segment that lives in a FILE at an
 * absolute `path` instead of in the POSIX shm namespace. Capacity is then
 * bounded by the filesystem rather than by RAM, and the OS page cache decides
 * which pages are resident — the point of the backing.
 *
 * The path IS the identifier. It must be absolute (`path[0] == '/'`); NULL, an
 * empty string, and a relative path are all SHUTTLE_ERR_INVALID_ARGS, because a
 * channel's identity must not depend on which directory each peer was started
 * in. There is no shm-style length limit: the filesystem's own limit applies
 * and surfaces as SHUTTLE_ERR_NAME_TOO_LONG.
 *
 * shuttle_create_file takes the same create_flags as shuttle_create_ex with one
 * exception: SHUTTLE_CREATE_HUGETLB_2MB/_1GB are REJECTED with
 * SHUTTLE_ERR_INVALID_ARGS (a hugetlbfs backing and a caller-chosen path name
 * two different segments; neither can be silently dropped).
 * SHUTTLE_CREATE_FILE_BACKED is set automatically. Errors otherwise mirror
 * shuttle_create_ex's, plus SHUTTLE_ERR_NOT_FOUND when the parent directory
 * does not exist, and SHUTTLE_ERR_EXISTS when the file is already there — that
 * refusal is the documented recovery point for a stale file (unlink it, then
 * recreate); nothing here ever truncates a file it did not create.
 *
 * shuttle_close is shared: a handle is a handle whatever backs it.
 *
 * PLATFORM: POSIX only this pass. On Windows the argument checks above still
 * apply (a malformed path is still SHUTTLE_ERR_INVALID_ARGS), and any call that
 * gets past them returns SHUTTLE_ERR_SYS — the platform seam reports ENOTSUP —
 * having created nothing. A documented parity gap, like the absent
 * robust-mutex recovery there. */
shuttle_channel* shuttle_create_file(const char* path, size_t capacity_bytes,
                                     size_t max_payload_bytes,
                                     uint32_t create_flags, int* err);
shuttle_channel* shuttle_open_file(const char* path, int* err);
int shuttle_unlink_file(const char* path);

/* --- copy convenience path (IF-2) --- */
/* Returns SHUTTLE_OK, a negative error code, or — only when the caller passed
 * SHUTTLE_DROP_NEWEST — the positive SHUTTLE_DROPPED. */
int shuttle_write(shuttle_channel* ch, const void* data, size_t len, int flags);
/* Returns payload length (>= 0) on success. If the waiting message is
 * larger than cap, returns SHUTTLE_ERR_MSG_TOO_LARGE and the message stays
 * queued. */
long shuttle_read(shuttle_channel* ch, void* out, size_t cap, int flags);

/* --- zero-copy borrow path (IF-2, the headline) --- */
int shuttle_acquire_write(shuttle_channel* ch, void** ptr, size_t len,
                          int flags);
int shuttle_commit_write(shuttle_channel* ch, size_t actual_len);
int shuttle_acquire_read(shuttle_channel* ch, const void** ptr, size_t* len,
                         int flags);
int shuttle_release_read(shuttle_channel* ch);

/* --- liveness (A3) --- */
void shuttle_keepalive(shuttle_channel* ch);

/* --- statistics (v1.2 additive) ---
 * Copy the segment's counters into *out. Either side of the channel may call
 * it, and so may a third process that merely opened the segment: the counters
 * live in the segment, not in the handle.
 * Returns SHUTTLE_OK, SHUTTLE_ERR_INVALID_ARGS (NULL argument), or
 * SHUTTLE_ERR_NO_STATS (segment created without SHUTTLE_CREATE_STATS).
 * Each field is individually exact and monotonic; the five together are not
 * sampled atomically, so a snapshot taken while traffic flows may show
 * msgs_read one behind msgs_written. */
int shuttle_get_stats(shuttle_channel* ch, shuttle_stats* out);

#ifdef __cplusplus
}
#endif

#endif /* SHUTTLE_C_H */
