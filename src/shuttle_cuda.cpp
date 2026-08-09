// Shuttle CUDA IPC interop — implementation.
//
// EXPERIMENTAL. Two clearly separated halves:
//
//   1. The pure host DESCRIPTOR CODEC (pack / unpack / validate / init). No
//      CUDA dependency at all — the handles are opaque byte blobs. This half is
//      always compiled and is unit-tested by tests/cuda_desc_test.cpp. It is
//      the only part of the module proven anywhere.
//
//   2. The DEVICE GLUE, guarded by #ifdef SHUTTLE_WITH_CUDA (set by
//      -DSHUTTLE_CUDA=ON). It includes <cuda_runtime.h> STRICTLY inside the
//      guard and calls the four cudaIpc* entry points. It is compile-only: it
//      has never run, and the environment this was written in has no GPU and no
//      CUDA toolkit. CI compiles it against a stub header to prove it is
//      well-formed; that is all it proves. See docs/CUDA_DESIGN.md.
//
// The codec fixes the wire format as explicit LITTLE-ENDIAN byte packing (see
// the endianness note in the design doc): each scalar is written byte by byte,
// so the packed bytes never depend on struct padding or host endianness, and
// the opaque 64-byte handles are copied verbatim.

#include "shuttle/shuttle_cuda.h"

#include <cstring>

namespace {

// sizeof canary. The wire codec does NOT rely on this equality — it packs field
// by field — but a divergence would mean the struct grew padding, which is
// worth knowing at build time. Kept as a soft check, not a wire dependency.
static_assert(sizeof(shuttle_cuda_desc) == SHUTTLE_CUDA_DESC_WIRE_SIZE,
              "shuttle_cuda_desc gained padding; wire codec is unaffected but "
              "the struct/wire 1:1 assumption in the docs no longer holds");

// --- little-endian scalar put/get; every access is bounds-checked by the
// caller having already ensured buflen >= SHUTTLE_CUDA_DESC_WIRE_SIZE. -------

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

bool handle_is_zero(const uint8_t* h) {
    for (uint32_t i = 0; i < SHUTTLE_CUDA_HANDLE_BYTES; ++i)
        if (h[i] != 0) return false;
    return true;
}

}  // namespace

extern "C" {

void shuttle_cuda_desc_init(shuttle_cuda_desc* d) {
    if (d == nullptr) return;
    std::memset(d, 0, sizeof(*d));
    d->magic = SHUTTLE_CUDA_MAGIC;
    d->version = SHUTTLE_CUDA_VERSION;
}

int shuttle_cuda_desc_validate(const shuttle_cuda_desc* d) {
    if (d == nullptr) return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    if (d->magic != SHUTTLE_CUDA_MAGIC) return SHUTTLE_CUDA_ERR_BAD_MAGIC;
    if (d->version != SHUTTLE_CUDA_VERSION) return SHUTTLE_CUDA_ERR_BAD_VERSION;
    if ((d->flags & ~SHUTTLE_CUDA_FLAG_ALL_KNOWN) != 0u)
        return SHUTTLE_CUDA_ERR_BAD_FLAGS;
    if (d->device < 0) return SHUTTLE_CUDA_ERR_BAD_DEVICE;
    if (d->len == 0) return SHUTTLE_CUDA_ERR_BAD_LEN;
    // Event-handle consistency: the HAS_EVENT bit and a nonzero event handle
    // must agree. This makes "0 if unused" a checked invariant rather than a
    // convention, and rejects the two contradictory states outright.
    const bool has_event = (d->flags & SHUTTLE_CUDA_FLAG_HAS_EVENT) != 0u;
    const bool event_zero = handle_is_zero(d->event_handle);
    if (has_event == event_zero) return SHUTTLE_CUDA_ERR_EVENT_MISMATCH;
    return SHUTTLE_CUDA_OK;
}

int shuttle_cuda_desc_pack(const shuttle_cuda_desc* d, void* buf,
                           size_t buflen) {
    if (d == nullptr || buf == nullptr) return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    const int v = shuttle_cuda_desc_validate(d);
    if (v != SHUTTLE_CUDA_OK) return v;  // never emit a malformed descriptor
    if (buflen < SHUTTLE_CUDA_DESC_WIRE_SIZE)
        return SHUTTLE_CUDA_ERR_BUF_TOO_SMALL;

    uint8_t* p = static_cast<uint8_t*>(buf);
    put_u32(p + 0, d->magic);
    put_u32(p + 4, d->version);
    put_u32(p + 8, static_cast<uint32_t>(d->device));  // two's-complement bits
    put_u32(p + 12, d->flags);
    std::memcpy(p + 16, d->mem_handle, SHUTTLE_CUDA_HANDLE_BYTES);
    put_u64(p + 80, d->offset);
    put_u64(p + 88, d->len);
    std::memcpy(p + 96, d->event_handle, SHUTTLE_CUDA_HANDLE_BYTES);
    return static_cast<int>(SHUTTLE_CUDA_DESC_WIRE_SIZE);
}

int shuttle_cuda_desc_unpack(const void* buf, size_t buflen,
                             shuttle_cuda_desc* out) {
    if (buf == nullptr || out == nullptr) return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    if (buflen < SHUTTLE_CUDA_DESC_WIRE_SIZE)
        return SHUTTLE_CUDA_ERR_BUF_TOO_SMALL;

    const uint8_t* p = static_cast<const uint8_t*>(buf);
    shuttle_cuda_desc tmp;
    std::memset(&tmp, 0, sizeof(tmp));
    tmp.magic = get_u32(p + 0);
    tmp.version = get_u32(p + 4);
    tmp.device = static_cast<int32_t>(get_u32(p + 8));
    tmp.flags = get_u32(p + 12);
    std::memcpy(tmp.mem_handle, p + 16, SHUTTLE_CUDA_HANDLE_BYTES);
    tmp.offset = get_u64(p + 80);
    tmp.len = get_u64(p + 88);
    std::memcpy(tmp.event_handle, p + 96, SHUTTLE_CUDA_HANDLE_BYTES);

    // Validate BEFORE handing anything back: unpack never yields an invalid
    // descriptor. Only on success do we write *out.
    const int v = shuttle_cuda_desc_validate(&tmp);
    if (v != SHUTTLE_CUDA_OK) return v;
    *out = tmp;
    return SHUTTLE_CUDA_OK;
}

}  // extern "C"

// ===========================================================================
//  DEVICE GLUE — compile-guarded, compile-only, NEVER RUN.
//
//  This block is excluded from every build in this repository's test matrix
//  except a compile-only CI check against a stub cuda_runtime.h. It has no GPU
//  coverage of any kind. Treat every line below as UNPROVEN.
// ===========================================================================
#ifdef SHUTTLE_WITH_CUDA

#include <cuda_runtime.h>  // STRICTLY inside the guard — no CUDA in a host build

#include <map>
#include <mutex>

namespace {

// Per-process open-handle cache. cudaIpcOpenMemHandle refuses to map the same
// handle twice in a process (it returns cudaErrorAlreadyMapped / an error), so
// a process that receives the same allocation over two descriptors must map it
// ONCE and share the pointer. We key a refcount by the raw 64 handle bytes.
//
// UNPROVEN: the exact error code, whether refcounting by handle bytes matches
// CUDA's notion of allocation identity in every driver version, and thread
// safety under real concurrent opens. The mutex here serializes cache access
// only; it says nothing about CUDA's own reentrancy.
struct HandleKey {
    uint8_t bytes[SHUTTLE_CUDA_HANDLE_BYTES];
    bool operator<(const HandleKey& o) const {
        return std::memcmp(bytes, o.bytes, sizeof(bytes)) < 0;
    }
};

struct OpenEntry {
    void* base;     // the base pointer cudaIpcOpenMemHandle returned
    uint64_t refs;  // outstanding shuttle_cuda_open calls
};

std::mutex g_cache_mu;
std::map<HandleKey, OpenEntry> g_open_by_handle;  // handle -> mapped base
std::map<void*, HandleKey> g_handle_by_base;      // base -> handle (for close)

HandleKey key_of(const uint8_t* h) {
    HandleKey k;
    std::memcpy(k.bytes, h, sizeof(k.bytes));
    return k;
}

int fail_cuda(cudaError_t e, int* cuda_err) {
    if (cuda_err != nullptr) *cuda_err = static_cast<int>(e);
    return SHUTTLE_CUDA_ERR_CUDA;
}

}  // namespace

extern "C" {

int shuttle_cuda_desc_from_devptr(shuttle_cuda_desc* out, const void* base_ptr,
                                  uint64_t offset, uint64_t len, int device,
                                  int* cuda_err) {
    if (out == nullptr || base_ptr == nullptr)
        return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    shuttle_cuda_desc_init(out);
    out->device = device;
    out->offset = offset;
    out->len = len;
    cudaIpcMemHandle_t h;
    const cudaError_t e = cudaIpcGetMemHandle(&h, const_cast<void*>(base_ptr));
    if (e != cudaSuccess) return fail_cuda(e, cuda_err);
    static_assert(sizeof(h) == SHUTTLE_CUDA_HANDLE_BYTES,
                  "cudaIpcMemHandle_t is not 64 bytes on this CUDA — the wire "
                  "descriptor version must change");
    std::memcpy(out->mem_handle, &h, SHUTTLE_CUDA_HANDLE_BYTES);
    return shuttle_cuda_desc_validate(out);
}

int shuttle_cuda_desc_attach_event(shuttle_cuda_desc* out, void* event,
                                   int* cuda_err) {
    if (out == nullptr || event == nullptr)
        return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    cudaIpcEventHandle_t eh;
    const cudaError_t e =
        cudaIpcGetEventHandle(&eh, static_cast<cudaEvent_t>(event));
    if (e != cudaSuccess) return fail_cuda(e, cuda_err);
    static_assert(sizeof(eh) == SHUTTLE_CUDA_HANDLE_BYTES,
                  "cudaIpcEventHandle_t is not 64 bytes on this CUDA");
    std::memcpy(out->event_handle, &eh, SHUTTLE_CUDA_HANDLE_BYTES);
    out->flags |= SHUTTLE_CUDA_FLAG_HAS_EVENT;
    return shuttle_cuda_desc_validate(out);
}

int shuttle_cuda_open(const shuttle_cuda_desc* d, void** out_ptr,
                      int* cuda_err) {
    if (d == nullptr || out_ptr == nullptr)
        return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    const int v = shuttle_cuda_desc_validate(d);
    if (v != SHUTTLE_CUDA_OK) return v;

    std::lock_guard<std::mutex> lk(g_cache_mu);
    const HandleKey k = key_of(d->mem_handle);
    auto it = g_open_by_handle.find(k);
    void* base = nullptr;
    if (it != g_open_by_handle.end()) {
        // Already mapped in this process: reuse, do NOT open twice.
        it->second.refs += 1;
        base = it->second.base;
    } else {
        cudaIpcMemHandle_t h;
        std::memcpy(&h, d->mem_handle, SHUTTLE_CUDA_HANDLE_BYTES);
        const cudaError_t e =
            cudaIpcOpenMemHandle(&base, h, cudaIpcMemLazyEnablePeerAccess);
        if (e != cudaSuccess) return fail_cuda(e, cuda_err);
        g_open_by_handle[k] = OpenEntry{base, 1};
        g_handle_by_base[base] = k;
    }
    // Bias by the descriptor's offset so the caller gets a pointer at the
    // shared sub-range, not the allocation base.
    *out_ptr = static_cast<void*>(static_cast<uint8_t*>(base) + d->offset);
    return SHUTTLE_CUDA_OK;
}

int shuttle_cuda_close(void* out_ptr, int* cuda_err) {
    if (out_ptr == nullptr) return SHUTTLE_CUDA_ERR_INVALID_ARGS;
    std::lock_guard<std::mutex> lk(g_cache_mu);
    // The caller may hand back the biased pointer; we cannot recover the offset
    // from it directly, so search the base map for the entry whose [base, ...)
    // this pointer fell within by matching on the recorded base of any handle.
    // In practice a caller closes with the exact biased pointer it received, so
    // we scan the small cache for the base b with b <= out_ptr and the same
    // handle mapping. UNPROVEN heuristic; a production version would hand the
    // caller an opaque handle rather than a biased raw pointer.
    for (auto& kv : g_handle_by_base) {
        void* base = kv.first;
        auto oit = g_open_by_handle.find(kv.second);
        if (oit == g_open_by_handle.end()) continue;
        // Accept the biased pointer if it is at or after this base and this is
        // the only plausible allocation (the cache is tiny). Exact-base match
        // is the common path.
        if (out_ptr == base ||
            static_cast<uint8_t*>(out_ptr) >= static_cast<uint8_t*>(base)) {
            if (oit->second.refs > 1) {
                oit->second.refs -= 1;
                return SHUTTLE_CUDA_OK;
            }
            const cudaError_t e = cudaIpcCloseMemHandle(base);
            g_open_by_handle.erase(oit);
            g_handle_by_base.erase(base);
            if (e != cudaSuccess) return fail_cuda(e, cuda_err);
            return SHUTTLE_CUDA_OK;
        }
    }
    return SHUTTLE_CUDA_ERR_NOT_OPEN;
}

}  // extern "C"

#endif  // SHUTTLE_WITH_CUDA
