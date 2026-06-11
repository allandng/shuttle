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
 * requires bumping SHUTTLE_ABI_VERSION.
 */
#ifndef SHUTTLE_C_H
#define SHUTTLE_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHUTTLE_ABI_VERSION 1

/* Error codes (mirror shuttle::Err; static_asserted in the implementation). */
#define SHUTTLE_OK 0
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

/* Flags (IF-3): blocking is the default; OR in SHUTTLE_NONBLOCK for
 * try-semantics ("would block" instead of parking). */
#define SHUTTLE_NONBLOCK 0x1

typedef struct shuttle_channel shuttle_channel;

/* --- lifecycle (FR-1..FR-5) --- */
shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
shuttle_channel* shuttle_open(const char* name, int* err);
void shuttle_close(shuttle_channel* ch);
int shuttle_unlink(const char* name);

/* --- copy convenience path (IF-2) --- */
int shuttle_write(shuttle_channel* ch, const void* data, size_t len,
                  int flags);
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

#ifdef __cplusplus
}
#endif

#endif /* SHUTTLE_C_H */
