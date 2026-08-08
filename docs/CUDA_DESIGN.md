# CUDA IPC interop — design notes (EXPERIMENTAL)

> **Status: experimental, unproven, not part of the supported v1 surface.**
> Nothing in this module has run on a GPU. The host-side descriptor codec is
> unit-tested and is the *only* part proven anywhere; the device glue compiles
> against the CUDA headers and has never executed. Do not build a product on
> this. The proven-vs-unproven ledger is spelled out at the bottom — read it
> before trusting any sentence above it.

## The problem

Shuttle moves bytes between same-host processes with no copy. But a producer
whose data already lives in GPU memory has no cheap way to share it: to put it
on the byte channel it must first copy device -> host, and the consumer copies
host -> device again. For a pipeline where both ends already run on the device,
those two copies are the entire cost, and they defeat the point.

CUDA already solves the sharing primitive. `cudaIpcGetMemHandle` turns a device
allocation into an opaque handle a *different process on the same host* can pass
to `cudaIpcOpenMemHandle` to map the same physical device memory into its own
address space. What CUDA does not provide is a transport for the handle. That is
exactly what Shuttle is.

## The idea in one line

**The GPU data never touches the channel. A small descriptor does.**

The producer packs a fixed-layout descriptor — the 64-byte `cudaIpcMemHandle_t`,
the owning device ordinal, a byte offset+length into the allocation, and an
optional `cudaIpcEventHandle_t` — and sends it as an ordinary Shuttle message.
The consumer reads that message, unpacks the descriptor, and calls
`cudaIpcOpenMemHandle` to get a device pointer it can read directly. The bytes
that cross the channel are ~160, regardless of whether the shared allocation is
16 KB or 16 GB.

```
producer                                   consumer
--------                                    --------
have dptr (device memory)
cudaIpcGetMemHandle(&h, base)
pack descriptor -> 160 bytes
shuttle_write(ch, desc_bytes, 160) ───────► shuttle_read(ch, buf, ...)
                                            shuttle_cuda_desc_unpack(buf,...)
                                            cudaIpcOpenMemHandle(&p, h, ...)
                                            (optional) cudaStreamWaitEvent(evt)
                                            <read device memory at p+offset>
                                            cudaIpcCloseMemHandle(base)
```

## Why this is testable at all

The descriptor is **just bytes**. The two CUDA handles are opaque 64-byte blobs
that the codec copies verbatim and never interprets — it does not call CUDA, it
does not dereference a device pointer, it does not even need CUDA headers. So the
codec is pure host code, and `tests/cuda_desc_test.cpp` exercises it on every
platform with no GPU: roundtrip, byte-exact wire layout, rejection of every
malformed image, and a fuzz-lite pass that feeds random and truncated buffers
through `unpack` under AddressSanitizer to prove it never accepts garbage and
never reads out of bounds.

That clean split — pure host codec on one side, all CUDA behind a compile guard
on the other — is the whole reason any part of this module can be proven in an
environment without a GPU.

## The descriptor

Defined in `include/shuttle/shuttle_cuda.h`:

| offset | size | field          | notes                                        |
|-------:|-----:|----------------|----------------------------------------------|
|   0    |  4   | `magic`        | `0x53435544` ('S''C''U''D'), little-endian   |
|   4    |  4   | `version`      | `1`; checked exactly, unknown version rejected |
|   8    |  4   | `device`       | `int32`, owning CUDA device ordinal, `>= 0`  |
|  12    |  4   | `flags`        | only `HAS_EVENT` (0x1) defined; other bits must be 0 |
|  16    | 64   | `mem_handle`   | `cudaIpcMemHandle_t`, opaque                 |
|  80    |  8   | `offset`       | byte offset of payload within the allocation |
|  88    |  8   | `len`          | payload length, `> 0`                        |
|  96    | 64   | `event_handle` | `cudaIpcEventHandle_t`, all-zero unless `HAS_EVENT` |
| **160**|      | **total**      | `SHUTTLE_CUDA_DESC_WIRE_SIZE`                 |

`validate()` enforces: correct magic and version, `len > 0`, `device >= 0`, no
unknown flag bits, and event-handle consistency (`HAS_EVENT` set **iff** the
event handle is nonzero). It **cannot** check that the handle names a live
allocation — that is a device-runtime question, not a host one, and no host code
can answer it.

### Endianness decision

**The wire format is fixed little-endian, packed field by field** — not a
`memcpy` of the struct. Two reasons, and I considered the alternative:

- The *simpler* option was to declare "CUDA IPC is same-host by construction, so
  producer and consumer share an architecture and endianness; just `memcpy` the
  struct and `static_assert(sizeof == 160)`." That is genuinely sound for the
  intended use — `cudaIpcOpenMemHandle` only works within one physical machine,
  so a big-endian producer and little-endian consumer can never be a real pair.
- I chose **explicit little-endian packing anyway** because it decouples the
  wire bytes from struct padding and from the host's native byte order, which
  makes the codec a *real, testable function* instead of a `memcpy`. The
  endianness test in `cuda_desc_test.cpp` pins exact bytes at exact offsets, so
  a silent layout change is caught. The opaque 64-byte handles have no
  endianness — they are copied verbatim; only the six scalar fields are byte-
  ordered. `sizeof(shuttle_cuda_desc) == 160` is still asserted, but only as a
  canary: the wire format would survive even if a compiler added padding.

The cost of the explicit packing is nil on any little-endian host (the common
case): it is a handful of byte moves per scalar, dwarfed by the memcpy of the
128 handle bytes.

## How it rides the existing channel

Nothing about the byte channel changes. The descriptor is a normal payload:

```c
/* producer */
shuttle_cuda_desc d;
shuttle_cuda_desc_init(&d);                 /* stamps magic + version */
/* ... fill device/offset/len/mem_handle, e.g. via the device-glue helper ... */
uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
int n = shuttle_cuda_desc_pack(&d, buf, sizeof buf);   /* 160 or negative */
shuttle_write(ch, buf, (size_t)n, 0);

/* consumer */
uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
long n = shuttle_read(ch, buf, sizeof buf, 0);
shuttle_cuda_desc d;
if (shuttle_cuda_desc_unpack(buf, (size_t)n, &d) == SHUTTLE_CUDA_OK) {
    /* d is guaranteed structurally valid here */
    void* p; int cuda_err;
    shuttle_cuda_open(&d, &p, &cuda_err);   /* device glue; CUDA build only */
    /* read device memory at p ... */
    shuttle_cuda_close(p, &cuda_err);
}
```

The descriptor is small enough that the copy path (`shuttle_write` /
`shuttle_read`) is the natural fit — there is no reason to reach for the
zero-copy borrow path for 160 bytes.

## Lifetime hazard analysis — the part that actually matters

This is where the design earns its "experimental" label, and where the honest
caveats live.

**Shuttle's SPSC borrow semantics govern the DESCRIPTOR bytes, not the device
memory.** When the consumer holds a borrow of the message, the zero-copy
contract guarantees *those 160 descriptor bytes* stay valid until release. It
guarantees **nothing** about the GPU allocation the descriptor points at. The
device memory is owned by the producer's CUDA context and lives entirely outside
Shuttle's knowledge. This is the single most important thing to understand about
the module, and the easiest to get wrong.

Concretely, three races that are the application's responsibility, not
Shuttle's:

1. **Producer frees the allocation while the consumer is still reading it.**
   Once the descriptor is sent, Shuttle is done. If the producer calls
   `cudaFree` (or lets the allocation go out of scope) before the consumer
   finishes, the consumer's mapped pointer dangles — a use-after-free in device
   memory that no channel rule can prevent. The application must keep the
   allocation alive across the whole borrow, by a protocol Shuttle does not
   provide (e.g. an ack message on a return channel).

2. **In-flight kernels vs. the read.** Even if the allocation is alive, the
   producer's writes to it may still be queued on a CUDA stream when the
   descriptor arrives. The consumer reading "now" can read *stale or partially
   written* data. This is what the optional **event handle** is for: the
   producer records a `cudaEvent_t` (created `cudaEventInterprocess |
   cudaEventDisableTiming`) after its writes, ships the event handle in the
   descriptor, and the consumer does `cudaStreamWaitEvent` on the opened event
   before launching its own read. That makes the cross-process, cross-stream
   ordering explicit. **This event path is entirely unproven** — it has never
   run — and getting it right (and proving it) is most of the real work this
   module has not done.

3. **The borrow-vs-kernel race in practice.** Releasing the Shuttle borrow says
   "I am done with the descriptor bytes," which is *not* the same as "my GPU
   read kernel has finished." A consumer that releases the borrow and then lets
   the producer proceed, while its own read kernel is still queued, reintroduces
   race (2) from the other side. The correct discipline (event round-trip, or a
   completion ack) is application-level and, again, unproven here.

### The handle cache

`cudaIpcOpenMemHandle` refuses to map the same handle twice in one process. If a
consumer legitimately receives the same allocation over two descriptors, it must
open once and share the pointer. The glue keeps a small per-process refcounted
cache keyed by the raw 64 handle bytes (see `src/shuttle_cuda.cpp`). This is a
**sketch**: the exact error code, whether "same handle bytes" matches CUDA's
notion of allocation identity across driver versions, and the thread-safety of
the cache against concurrent real opens are all unverified. The `close` path
even carries a documented heuristic for recovering the cache entry from a biased
pointer that a production version should replace with an opaque handle type. None
of it has run.

## What is PROVEN vs UNPROVEN

**PROVEN** (in this environment, no GPU):

- The host descriptor codec — `pack` / `unpack` / `validate` / `init` — is
  correct: byte-exact roundtrip (including event-carrying and extreme-value
  descriptors), the fixed little-endian wire layout matches exact expected
  bytes, `sizeof == SHUTTLE_CUDA_DESC_WIRE_SIZE == 160`, every malformed image
  is rejected with the matching error, and 200k random + all sub-size truncated
  buffers through `unpack` never yield acceptance and never over-read (checked
  under ASan/UBSan). This is `tests/cuda_desc_test.cpp`, run on every CI leg.
- The device glue is **well-formed**: it compiles with `-DSHUTTLE_WITH_CUDA
  -Wall -Wextra -Werror` against the CUDA runtime header surface it uses. In CI
  this is checked against a minimal stub `cuda_runtime.h` (declarations only),
  because there is no CUDA toolkit on the runners; the guarantee is "the C++
  parses and the `cudaIpc*` calls type-check," and nothing more. *If you have a
  real toolkit,* `-DSHUTTLE_CUDA=ON` compiles the same source against the real
  headers and links `CUDA::cudart` — but that path is not exercised by CI.

**UNPROVEN** (requires hardware this project has not run on):

- That cross-process device memory actually becomes visible through the opened
  handle. **Never tested.**
- That the event handle synchronizes producer and consumer streams correctly —
  the entire ordering story in hazard (2)/(3) above. **Never tested.**
- The handle cache's refcount behavior against real `cudaIpcOpenMemHandle`,
  including the twice-open error and the close-by-biased-pointer heuristic.
- Any performance claim whatsoever. There is no benchmark, and none would be
  meaningful without a GPU. Whether this is faster than a device-host-device
  round-trip for a given size is an open question, not a result.
- Multi-GPU and MIG behavior: whether the `device` field is sufficient, whether
  peer access must be enabled, whether MIG partitions can share handles at all.
- The borrow-vs-kernel lifetime race in practice — the hardest part, and the one
  that most needs a real device and a falsifiable test to settle.

## Scope statement

This module is **not** part of the frozen v1 ABI. It adds no symbol to
`shuttle_c.h`, changes no segment layout, and does not move
`SHUTTLE_ABI_VERSION`. It is opt-in at build time (`-DSHUTTLE_CUDA=ON`, default
OFF) and is a research sketch of the v2 "CUDA IPC / GPU-direct interop"
direction in [ROADMAP.md](ROADMAP.md) — captured so the descriptor protocol and
its hazards are written down, not so anyone ships it. When it graduates, it will
need: a machine with GPUs, a two-process byte-exact device-memory transfer test,
an event-synchronization test that can *fail*, and a benchmark against the naive
copy path. Until then, treat every unproven bullet above as an open risk.
