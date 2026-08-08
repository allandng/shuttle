/* Shuttle CUDA IPC interop — EXPERIMENTAL, and NOT part of the frozen v1 ABI.
 *
 * ==========================================================================
 *  EXPERIMENTAL / UNPROVEN. This header is a proposal, not a supported
 *  surface. Nothing in this module has been exercised on a GPU. Only the
 *  host-side descriptor CODEC (pack / unpack / validate below) is tested;
 *  the device glue in src/shuttle_cuda.cpp is compile-guarded, compile-only,
 *  and has never run. Do NOT build a product on this. See docs/CUDA_DESIGN.md
 *  for the precise proven-vs-unproven ledger.
 * ==========================================================================
 *
 * The idea, in one paragraph. A producer that already holds GPU-resident data
 * does not want to stage it back through CPU RAM to share it. CUDA IPC lets a
 * second process on the SAME host map that device allocation directly. All the
 * second process needs is a small, fixed-layout DESCRIPTOR: the opaque 64-byte
 * cudaIpcMemHandle_t, the owning device id, a byte offset+length into the
 * allocation, and an optional cudaIpcEventHandle_t for cross-stream ordering.
 * That descriptor is just BYTES, so it rides an ordinary Shuttle message over
 * the existing SPSC byte channel — producer packs it and shuttle_write()s it;
 * consumer shuttle_read()s it, unpacks, and calls cudaIpcOpenMemHandle().
 *
 * The split that makes this testable. Everything in THIS header — the struct
 * and the three codec functions — is pure host code with NO CUDA dependency:
 * the handles are opaque byte blobs we neither interpret nor dereference. That
 * codec is unit-tested (tests/cuda_desc_test.cpp) and is the only part of the
 * module proven anywhere. The functions that actually touch a device pointer
 * live in src/shuttle_cuda.cpp behind #ifdef SHUTTLE_WITH_CUDA and are declared
 * at the bottom of this header behind the same guard, so a non-CUDA build never
 * sees them.
 *
 * Relationship to the frozen ABI. This is a SEPARATE, additive, opt-in module.
 * It does not touch shuttle_c.h, does not change the segment layout, and does
 * not change SHUTTLE_ABI_VERSION. Shuttle's SPSC borrow semantics govern the
 * DESCRIPTOR bytes only — NOT the device memory the descriptor points at, whose
 * lifetime the application must manage with the CUDA event (see the design
 * doc).
 */
#ifndef SHUTTLE_CUDA_H
#define SHUTTLE_CUDA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire identity. magic tags the blob; version gates the layout exactly (an
 * unknown version is rejected, not ignored — unlike a Shuttle create-flag). */
#define SHUTTLE_CUDA_MAGIC \
    0x53435544u /* 'S''C''U''D' little-endian on the wire */
#define SHUTTLE_CUDA_VERSION 1u

/* The two opaque CUDA handle blobs are 64 bytes each in every CUDA release this
 * targets. We never interpret them; we copy them verbatim. If a future CUDA
 * changed the size, that would be a new descriptor version, not a silent
 * reinterpretation — which is exactly why the size is a named constant. */
#define SHUTTLE_CUDA_HANDLE_BYTES 64u

/* Exact serialized size, in bytes, of a packed descriptor. This is the wire
 * contract: pack() always writes this many bytes, unpack() requires at least
 * this many. Derivation: 4(magic)+4(version)+4(device)+4(flags)
 *  + 64(mem_handle) + 8(offset) + 8(len) + 64(event_handle) = 160. */
#define SHUTTLE_CUDA_DESC_WIRE_SIZE 160u

/* flags bits. Only HAS_EVENT is defined at v1; every other bit is reserved and
 * MUST be zero — validate() rejects unknown bits so a malformed or
 * forward-versioned descriptor cannot masquerade as valid at this version. */
#define SHUTTLE_CUDA_FLAG_HAS_EVENT 0x1u
#define SHUTTLE_CUDA_FLAG_ALL_KNOWN 0x1u /* OR of every defined bit above */

/* Codec return codes. Distinct namespace from the SHUTTLE_ERR_* codes in
 * shuttle_c.h: this is a separate module and its errors describe the
 * DESCRIPTOR, not the channel. All failures are negative; success is 0 (or, for
 * pack, the positive byte count). */
#define SHUTTLE_CUDA_OK 0
#define SHUTTLE_CUDA_ERR_INVALID_ARGS (-1) /* NULL pointer argument */
#define SHUTTLE_CUDA_ERR_BUF_TOO_SMALL \
    (-2) /* buffer < SHUTTLE_CUDA_DESC_WIRE_SIZE */
#define SHUTTLE_CUDA_ERR_BAD_MAGIC (-3)   /* magic != SHUTTLE_CUDA_MAGIC */
#define SHUTTLE_CUDA_ERR_BAD_VERSION (-4) /* version != SHUTTLE_CUDA_VERSION \
                                           */
#define SHUTTLE_CUDA_ERR_BAD_LEN (-5)     /* len == 0 */
#define SHUTTLE_CUDA_ERR_BAD_DEVICE (-6)  /* device < 0 */
#define SHUTTLE_CUDA_ERR_BAD_FLAGS (-7)   /* an unknown flag bit is set */
#define SHUTTLE_CUDA_ERR_EVENT_MISMATCH \
    (-8) /* HAS_EVENT clear but event nonzero, or set but zero */
/* Device-glue codes (only returned by the SHUTTLE_WITH_CUDA functions). A CUDA
 * runtime failure is surfaced as this single code; the underlying cudaError_t
 * is reported through the out-parameter of those functions. */
#define SHUTTLE_CUDA_ERR_CUDA (-9) /* a cuda*  call failed */
#define SHUTTLE_CUDA_ERR_ALREADY_OPEN \
    (-10) /* handle already mapped in this process */
#define SHUTTLE_CUDA_ERR_NOT_OPEN \
    (-11) /* close of a pointer this process did not open */

/* The wire descriptor. Field order here IS the wire order (see the codec), but
 * the codec serializes each scalar field explicitly little-endian rather than
 * memcpy'ing the struct, so the wire format does not depend on this struct's
 * padding or the host's endianness. The struct is a same-host in-memory
 * convenience; the packed bytes are the contract. See the endianness note in
 * docs/CUDA_DESIGN.md. */
typedef struct shuttle_cuda_desc {
    uint32_t magic;   /* SHUTTLE_CUDA_MAGIC */
    uint32_t version; /* SHUTTLE_CUDA_VERSION */
    int32_t device;   /* owning CUDA device ordinal, >= 0 */
    uint32_t flags;   /* SHUTTLE_CUDA_FLAG_* */
    uint8_t
        mem_handle[SHUTTLE_CUDA_HANDLE_BYTES]; /* cudaIpcMemHandle_t, opaque */
    uint64_t
        offset;   /* byte offset of the payload within the mapped allocation */
    uint64_t len; /* payload length in bytes, > 0 */
    uint8_t
        event_handle[SHUTTLE_CUDA_HANDLE_BYTES]; /* cudaIpcEventHandle_t;
                                                    all-zero unless HAS_EVENT */
} shuttle_cuda_desc;

/* --- pure host codec (NO CUDA dependency; the tested surface) ---------------
 */

/* Zero *d and stamp the current magic + version. Convenience so a caller fills
 * only device/offset/len/handle and cannot forget the identity fields. */
void shuttle_cuda_desc_init(shuttle_cuda_desc* d);

/* Structural validation, independent of any wire buffer. Checks magic, version,
 * len > 0, device >= 0, no unknown flag bits, and event-handle consistency
 * (HAS_EVENT set iff event_handle is nonzero). Returns SHUTTLE_CUDA_OK or a
 * negative SHUTTLE_CUDA_ERR_*. Does NOT and CANNOT check that the handle refers
 * to a live allocation — that is a device-runtime question, not a host one. */
int shuttle_cuda_desc_validate(const shuttle_cuda_desc* d);

/* Serialize *d into buf (little-endian, exactly SHUTTLE_CUDA_DESC_WIRE_SIZE
 * bytes). The descriptor is validate()d first, so a malformed descriptor is
 * never emitted. Returns the number of bytes written
 * (SHUTTLE_CUDA_DESC_WIRE_SIZE) on success, or a negative SHUTTLE_CUDA_ERR_*
 * (INVALID_ARGS for NULL, BUF_TOO_SMALL if buflen is short, or whatever
 * validate() rejected). */
int shuttle_cuda_desc_pack(const shuttle_cuda_desc* d, void* buf,
                           size_t buflen);

/* Deserialize the first SHUTTLE_CUDA_DESC_WIRE_SIZE bytes of buf into *out and
 * validate() the result, so unpack NEVER yields an invalid descriptor and NEVER
 * reads past buflen. Returns SHUTTLE_CUDA_OK or a negative SHUTTLE_CUDA_ERR_*.
 * Safe to call on arbitrary/adversarial bytes: a short buffer is BUF_TOO_SMALL,
 * a wrong magic/version/len/device/flags is the matching error, and no read
 * ever exceeds buflen. */
int shuttle_cuda_desc_unpack(const void* buf, size_t buflen,
                             shuttle_cuda_desc* out);

/* --- device glue (COMPILE-GUARDED, COMPILE-ONLY, NEVER RUN HERE) -----------
 * Present only in a build configured with -DSHUTTLE_CUDA=ON, which defines
 * SHUTTLE_WITH_CUDA and links the CUDA runtime. These are the ONLY functions in
 * the module that depend on CUDA, and none of them has ever executed. Their
 * signatures take/return void* and int so the header needs no CUDA types; the
 * cudaError_t detail is passed back through cuda_err out-params. See
 * docs/CUDA_DESIGN.md for the lifetime hazards these do NOT solve. */
#ifdef SHUTTLE_WITH_CUDA

/* Producer side: fill *out from an existing device allocation. base_ptr must be
 * the BASE pointer returned by cudaMalloc (cudaIpcGetMemHandle requires the
 * allocation base); offset/len name the sub-range being shared. Calls
 * cudaIpcGetMemHandle. On CUDA failure returns SHUTTLE_CUDA_ERR_CUDA and writes
 * the cudaError_t to *cuda_err (if non-NULL). */
int shuttle_cuda_desc_from_devptr(shuttle_cuda_desc* out, const void* base_ptr,
                                  uint64_t offset, uint64_t len, int device,
                                  int* cuda_err);

/* Producer side, optional: attach an event so the consumer can wait on the
 * producer's stream before reading. Sets HAS_EVENT and fills event_handle via
 * cudaIpcGetEventHandle. The event must have been created with
 * cudaEventInterprocess | cudaEventDisableTiming. */
int shuttle_cuda_desc_attach_event(shuttle_cuda_desc* out, void* event,
                                   int* cuda_err);

/* Consumer side: cudaIpcOpenMemHandle the descriptor's handle and return the
 * mapped pointer BIASED BY offset in *out_ptr (i.e. already pointing at the
 * shared sub-range). Opening the same handle twice in one process is a CUDA
 * error, so this maintains a per-process refcounted cache keyed by the 64
 * handle bytes: a second open of the same handle bumps a refcount and returns
 * the same base. Returns SHUTTLE_CUDA_OK, SHUTTLE_CUDA_ERR_CUDA (with
 * *cuda_err), or a codec error if the descriptor is invalid. */
int shuttle_cuda_open(const shuttle_cuda_desc* d, void** out_ptr,
                      int* cuda_err);

/* Consumer side: release a pointer obtained from shuttle_cuda_open. Decrements
 * the cache refcount and calls cudaIpcCloseMemHandle on the last reference.
 * out_ptr may be the biased pointer shuttle_cuda_open returned. Returns
 * SHUTTLE_CUDA_OK, SHUTTLE_CUDA_ERR_NOT_OPEN, or SHUTTLE_CUDA_ERR_CUDA. */
int shuttle_cuda_close(void* out_ptr, int* cuda_err);

#endif /* SHUTTLE_WITH_CUDA */

#ifdef __cplusplus
}
#endif

#endif /* SHUTTLE_CUDA_H */
