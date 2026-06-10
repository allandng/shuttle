# Software Requirements Specification

## Shuttle — A High-Performance, Zero-Copy Shared-Memory IPC Framework for Local AI Workloads

**Version:** 1.0 — Final design baseline
**Status:** Locked for implementation
**Orientation:** Production-grade portfolio piece. Pragmatics, optimized C++17, and a bulletproof benchmark are weighted above formal standards conformance; requirement IDs are retained for traceability.

> *Shuttle* — in a loom, the shuttle carries the thread back and forth across the warp. Here it carries data buffers back and forth between processes. (Fits the Loom / Whetstone family; rename freely.)

---

## Locked Architectural Decisions (v1.0)

These seven decisions are settled and drive every requirement below.

| # | Decision | Consequence |
|---|---|---|
| D1 | **Topology:** strictly SPSC, point-to-point, one-way per channel. Multi-process stacks instantiate multiple pairwise channels (Rust→Python, Python→C++, …). | Each channel has exactly one writer and one reader, enabling a lock-free data path. |
| D2 | **Buffer:** a **Bipartite Buffer (BipBuffer)**, not a plain circular ring. | Every reserved/borrowed block is guaranteed *physically contiguous* — no mid-payload wrap — so the zero-copy pointer handoff is always valid. |
| D3 | **Backpressure:** block the producer. Data integrity is non-negotiable for embeddings/LLM context. | Producer sleeps until enough *contiguous* space exists; nothing is ever dropped. |
| D4 | **Platforms:** develop on macOS (Apple silicon), ship on Linux. Full `PTHREAD_MUTEX_ROBUST` on Linux; documented timeout + heartbeat on macOS. | Crash recovery is a hard guarantee on Linux, a best-effort guarantee on macOS. |
| D5 | **Workload:** large variable-sized blobs (≈50 MB tensors / context windows) dominate; audio-frame streaming is secondary. | Capacity sizing and contiguous reservation are first-class concerns. |
| D6 | **Payload:** opaque bytes. The application agrees on layout (dtype, shape, sample rate) out-of-band. | Shuttle carries transport-level length framing only; it never interprets payload content. |
| D7 | **Benchmark baseline:** raw, uncompressed binary payloads over localhost HTTP. | The comparison is fair and resistant to "straw-man" objections. |

---

## 1. Introduction

### 1.1 Purpose

This document specifies **Shuttle**, a C++17 inter-process communication library that moves large binary payloads between cooperating processes on a single machine with near-zero copy overhead, for local AI workloads — audio streaming, vector embeddings, and LLM context windows — across a polyglot stack (a C++ execution engine, a Rust/Tauri frontend, Python sidecars).

### 1.2 Scope

Apps such as *Loom* and *Whetstone* currently move data over loopback networking (localhost HTTP). On that path a payload is copied into a kernel socket buffer, traverses the loopback stack, is copied into the receiver's socket buffer, and is framed/deframed by HTTP — several full copies plus protocol overhead per message. For a 50 MB payload this wastes both latency and CPU.

Shuttle replaces that path for **same-host** communication: one region of physical RAM is mapped into every participating process via POSIX shared memory, and access is coordinated with synchronization primitives stored *inside* that region. A producer writes a payload once; a consumer reads it in place. No copy crosses the network stack, and no per-message serialization is needed for payloads already in a flat binary layout (PCM audio, `float32` tensors, byte blobs).

**In scope (v1.0):** a single-producer/single-consumer, one-way streaming channel (D1) built on a BipBuffer (D2); channel lifecycle (create/open/close/unlink); a lock-free data path with a parking-lot blocking mechanism; producer backpressure (D3); a stable C ABI callable from C++, Rust (FFI), and Python (`cffi`/`ctypes`); a zero-copy borrow path plus a copy-out convenience path; per-platform crash resilience (D4); and a reproducible benchmark harness versus a raw-binary localhost-HTTP baseline (D7).

**Out of scope (v1.0):** cross-machine transport; Windows (different API — see App. C); multi-producer/multi-consumer and pub/sub (App. D); encryption of the segment; automatic serialization of structured objects (D6).

### 1.3 Definitions

| Term | Definition |
|---|---|
| **SPSC** | Single-Producer, Single-Consumer: exactly one writer and one reader per channel (D1). |
| **BipBuffer** | Bipartite buffer: a circular-buffer variant that serves allocations from at most two contiguous regions so that any single reserved block never wraps the physical end of the buffer. |
| **Zero-copy borrow** | The consumer (or producer) receives a pointer directly into the segment and reads/writes in place; the payload is never duplicated. |
| **Lock-free hot path** | Normal-case reads/writes coordinate via atomic cursors with acquire/release ordering and take no mutex. |
| **Parking-lot wake** | The mutex + condition variables are used *only* to sleep a process that must block and to wake it; they are off the hot path. |
| **Robust mutex** | `PTHREAD_MUTEX_ROBUST`: if the owner dies holding it, the next acquirer gets `EOWNERDEAD` and can repair state via `pthread_mutex_consistent` (Linux). |
| **Heartbeat** | A monotonic liveness counter each side updates, used on macOS to detect a dead peer where robust mutexes are unavailable (D4). |
| **Transport framing** | Shuttle's internal per-message length prefix, distinct from application payload semantics (D6). |
| **Backpressure** | Flow control that blocks the producer when the buffer cannot accept a write (D3). |
| **`shm_open` / `mmap`** | POSIX calls to create/open a named shared-memory object and map it into a process. |

### 1.4 References

POSIX.1-2017 (`shm_open`, `mmap`, `pthread_*`); Linux man-pages `shm_overview(7)`, `pthread_mutexattr_setpshared(3)`, `pthread_mutexattr_setrobust(3)`; Simon Cooke, "The Bip Buffer" (origin of the bipartite-buffer technique); ISO/IEC/IEEE 29148:2018 (structure reference only).

---

## 2. Architecture

This section is the heart of the document; §3 formalizes it into testable requirements.

### 2.1 Topology (D1)

A channel is a one-way pipe with one writer and one reader. The three-process stack is composed of several such channels:

```
   Rust / Tauri ──[chan: ui_to_py]──► Python sidecar ──[chan: py_to_cpp]──► C++ engine
        ▲                                                                        │
        └───────────────────────[chan: cpp_to_ui]───────────────────────────────┘
```

Each channel is independent: its own named segment, its own BipBuffer, its own primitives. Reverse or request/response flows are simply two channels. There is no shared global state across channels.

### 2.2 Segment and the BipBuffer (D2, D5)

A segment is one `mmap`'d region: a fixed **control header** followed by the **data region**. The data region is managed as a BipBuffer rather than a plain ring, because a plain ring lets a single logical payload straddle the wrap point — which would force either a two-part read or an internal copy to reassemble, breaking the zero-copy contract. The BipBuffer guarantees that every reserved write block and every readable block is one contiguous run of bytes.

**BipBuffer mechanics (single writer, single reader):** the buffer tracks a primary readable region **A** and, after a wrap, a secondary region **B** anchored at offset 0.

- **Reserve(n)** (producer): if space after A to the physical end is sufficient, reserve there (A will grow). Otherwise reserve at offset 0, beginning region B (the writer "wraps early" rather than splitting the block). Either way the reserved block is contiguous, or the reservation fails/blocks (D3).
- **Commit(n)** (producer): grows A's size (or B's size) and publishes it.
- **Read block** (consumer): region A, returned as a single contiguous `(ptr, len)`.
- **Release(n)** (consumer): advances A's start. When A is fully drained, **A := B** and B is cleared; subsequent reads continue from what was B.

Because the writer only ever wraps to B on a whole-message boundary (it reserves `8 + payload` as one unit — see §2.4), region A always ends on a message boundary, so message order and boundaries are preserved naturally.

**Sizing rule (D3 + D5):** with block-the-producer backpressure, a write that can never be satisfied would block forever. Therefore `shuttle_create` validates `capacity ≥ max_payload + framing`, and a write larger than the usable capacity fails fast with a distinct error rather than blocking. A capacity of roughly **2× the largest payload** is recommended so the producer can fill region B while the consumer drains region A (pipelining) instead of strict ping-pong.

### 2.3 Synchronization: lock-free hot path + parking-lot wake (D1, D4)

Strict SPSC means the producer is the sole writer of the write/reserve cursors and the consumer the sole writer of the read cursor. Each cursor therefore has a single writer and can be published with **release** stores and observed with **acquire** loads — no mutex is required on the normal path.

The mutex and the two condition variables (`not_empty`, `not_full`) exist solely to **park** a process that must block and to **wake** it:

- **Consumer, buffer empty:** set an atomic `consumer_waiting` flag; take the mutex; re-check emptiness (guard against lost wakeups); `pthread_cond_timedwait(not_empty)`; on wake, clear the flag and retry the lock-free read.
- **Producer, insufficient contiguous space:** symmetric, using `consumer`'s progress and `not_full`.
- **Wake path (kept off the hot path):** after a commit, the producer checks `consumer_waiting` atomically; only if set does it briefly take the mutex and signal `not_empty`. The consumer signals `not_full` after a release symmetrically. In the common case (peer not sleeping) neither side touches the mutex.

The critical section guarded by the mutex is thus tiny and bounded, which is what makes the crash story (§2.5) cheap.

> **Pragmatic fallback:** if verifying the fully lock-free cross-process ordering proves too costly for v1.0, a mutex-guarded data path is an acceptable degradation — it changes only the performance profile of small-message streaming, not correctness or the headline 50 MB benchmark. The lock-free path is the target.

### 2.4 Transport framing vs. payload semantics (D6)

Shuttle preserves message boundaries by prepending a fixed **8-byte little-endian length** to each reservation: a write of an `L`-byte payload reserves `8 + L` contiguous bytes, writes `L`, then the payload. The reader peeks the 8-byte length (a trivial read, never the payload) and hands back a contiguous pointer to the payload region with length `L`.

This length is *transport* metadata owned by Shuttle. The payload's meaning — dtype, tensor shape, sample rate, framing of sub-records — is **out-of-band** application convention (D6). Shuttle never reads or interprets payload bytes.

### 2.5 Crash resilience, per platform (D4)

Because the only lock is the brief park/wake critical section (§2.3), a peer dying mid-transfer cannot strand a lock held over a data copy.

- **Linux (production):** the park/wake mutex is `PTHREAD_MUTEX_ROBUST`. If a peer dies holding it, the survivor's `lock` returns `EOWNERDEAD`; the survivor restores the (small, well-defined) protected state and calls `pthread_mutex_consistent`. Combined with single-writer cursor ownership, the survivor can also detect that the peer is gone and tear the channel down cleanly.
- **macOS (development only):** robust mutexes and `pthread_mutex_timedlock` are **not** available. Instead, each side updates a monotonic **heartbeat** counter in the header; blocking waits use `pthread_cond_timedwait` and, on each timeout, check whether the peer's heartbeat has gone stale beyond a configurable threshold. A stale peer is declared dead and the wait aborts with an error rather than blocking indefinitely. This is explicitly a weaker, best-effort guarantee, documented as such.

### 2.6 Cross-language boundary

The core is C++17 exposing an `extern "C"` ABI (§3.1). Rust binds via FFI (`bindgen`); Python via `cffi`/`ctypes`. No C++ exceptions cross the boundary; all errors are integer codes. The header uses only fixed-width types and documented alignment so the same bytes are read identically by all three languages.

---

## 3. Specific Requirements

IDs are stable; priority is **M**ust / **S**hould / **C**ould; each item is individually testable.

### 3.1 Interface (C ABI)

Representative sketch (final signatures fixed during implementation):

```c
typedef struct shuttle_channel shuttle_channel;

/* lifecycle */
shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
shuttle_channel* shuttle_open  (const char* name, int* err);
void             shuttle_close (shuttle_channel* ch);   /* munmap + free local handle */
int              shuttle_unlink(const char* name);      /* shm_unlink the named object */

/* copy convenience path */
int  shuttle_write(shuttle_channel* ch, const void* data, size_t len, int flags);
long shuttle_read (shuttle_channel* ch, void* out, size_t cap, int flags);

/* zero-copy borrow path (headline) */
int  shuttle_acquire_write(shuttle_channel* ch, void** ptr, size_t len, int flags);
int  shuttle_commit_write (shuttle_channel* ch, size_t actual_len);
int  shuttle_acquire_read (shuttle_channel* ch, const void** ptr, size_t* len, int flags);
int  shuttle_release_read (shuttle_channel* ch);
```

| ID | Pri | Requirement |
|---|---|---|
| IF-1 | M | All public functions use `extern "C"` linkage and C-compatible types; no exception crosses the boundary; errors are returned as codes. |
| IF-2 | M | Both a copy path (`write`/`read`) and a zero-copy borrow path (`acquire`/`commit`/`release`) are provided. |
| IF-3 | M | Blocking vs. non-blocking ("would block") is selectable per call via `flags`. |
| IF-4 | S | A versioned header with stable struct/enum/error definitions is provided for `bindgen` / `cffi`. |

### 3.2 Functional Requirements

**Lifecycle**

| ID | Pri | Requirement |
|---|---|---|
| FR-1 | M | `shuttle_create` creates a named shared object (`shm_open` `O_CREAT`), sizes it (`ftruncate`), maps it (`mmap`, `MAP_SHARED`), and initializes the header, BipBuffer state, and process-shared primitives. |
| FR-2 | M | `shuttle_open` attaches to an existing object and maps it without re-initializing control structures. |
| FR-3 | M | On open, the library validates a magic number and version and returns an error on mismatch. |
| FR-4 | M | `shuttle_create` validates `capacity_bytes ≥ max_payload_bytes + framing`; otherwise it fails with a distinct error (prevents an unsatisfiable, permanently-blocking write — §2.2). |
| FR-5 | M | `shuttle_close` unmaps and releases per-process resources without destroying the named object; `shuttle_unlink` removes the named object so it does not leak across runs. |

**Topology (D1)**

| ID | Pri | Requirement |
|---|---|---|
| FR-6 | M | A channel supports exactly one producer and one consumer, one-way. Behavior with a second producer or consumer is undefined and need not be defended in v1.0. |

**Data transfer over the BipBuffer (D2, D6)**

| ID | Pri | Requirement |
|---|---|---|
| FR-7 | M | A producer can write a single payload of any length up to `max_payload_bytes`; message boundaries are preserved on the consumer side via internal 8-byte length framing (§2.4). |
| FR-8 | M | Payloads are delivered in FIFO order. |
| FR-9 | M | The zero-copy read path returns a pointer (resolved to the local mapping) into a **contiguous** region for the whole payload, valid until `release_read`; no payload copy occurs. |
| FR-10 | M | The zero-copy write path returns a writable **contiguous** pointer for the reservation; `commit_write` publishes the payload (allowing `actual_len ≤` reserved length). |
| FR-11 | M | The library never interprets payload bytes; only the internal length prefix is read/written by Shuttle (D6). |

**Backpressure (D3)**

| ID | Pri | Requirement |
|---|---|---|
| FR-12 | M | When no contiguous region large enough for the reservation exists, a blocking write parks the producer until the consumer frees sufficient contiguous space; a non-blocking write returns "would block". Nothing is dropped. |
| FR-13 | M | When no payload is available, a blocking read parks the consumer on `not_empty` (no busy-poll) until signaled; a non-blocking read returns "would block". |

**Synchronization (D1)**

| ID | Pri | Requirement |
|---|---|---|
| FR-14 | M | Primitives (mutex, `not_empty`, `not_full`) live inside the segment, initialized `PTHREAD_PROCESS_SHARED`. |
| FR-15 | M | The normal-case data path is lock-free: cursors are atomics published with release and observed with acquire; the mutex is taken only on the park/wake slow path (§2.3). (Mutex-guarded fallback per §2.3 note is acceptable.) |
| FR-16 | M | All in-segment references are byte **offsets** from the segment base, never absolute pointers (different processes map at different addresses — App. B). |
| FR-17 | M | The control structures are free of data races, verifiable under a multi-process ThreadSanitizer harness. |

**Crash resilience (D4)**

| ID | Pri | Requirement |
|---|---|---|
| FR-18 | M | **Linux:** the park/wake mutex is `PTHREAD_MUTEX_ROBUST`; on `EOWNERDEAD` the survivor restores consistent state and calls `pthread_mutex_consistent`; no permanent deadlock results from a peer crash. |
| FR-19 | M | **macOS:** with no robust mutex and no `pthread_mutex_timedlock`, blocking waits use `pthread_cond_timedwait` plus a heartbeat-staleness check; a stale peer aborts the wait with an error. Reduced guarantee is documented. |
| FR-20 | S | The channel exposes liveness/occupancy state (peer-attached, bytes pending, contiguous space free) for application-level dead-peer detection. |

**Cross-language (D6)**

| ID | Pri | Requirement |
|---|---|---|
| FR-21 | M | The same channel works end-to-end across languages: a C++ producer with a Python consumer, and a C++ producer with a Rust consumer, are each demonstrated. |
| FR-22 | S | Example Python (`cffi`) and Rust (FFI) wrappers are provided. |

### 3.3 Non-Functional Requirements

**Performance**

| ID | Pri | Requirement |
|---|---|---|
| NFR-P1 | M | For a 50 MB payload, end-to-end latency over Shuttle is ≥ **10×** lower than the raw-binary localhost-HTTP baseline (§4); stretch **50×**. |
| NFR-P2 | M | On the zero-copy path, CPU time spent copying/serializing the payload is effectively zero (consumer reads producer bytes in place), demonstrated by profiling. |
| NFR-P3 | M | Consumer wake latency after a commit is on the order of microseconds, not milliseconds, on target hardware. |
| NFR-P4 | S | Sustained streaming of small audio frames (e.g., 4–64 KB) exceeds the HTTP baseline throughput at a stated frame rate. |

> **Scope of the claim (state this in the writeup):** Shuttle eliminates kernel-stack copies and HTTP framing. "Zero serialization" holds for payloads already in flat binary layout (PCM, `float32` tensors, blobs). Any application-level structuring cost exists on *both* transports and is not what Shuttle removes.

**Reliability / Quality / Portability / Security**

| ID | Pri | Requirement |
|---|---|---|
| NFR-R1 | M | A peer crash never permanently deadlocks the survivor on Linux (FR-18); macOS degrades to timeout/heartbeat (FR-19). |
| NFR-R2 | M | No named shared-memory object leaks across normal start/stop (verified by inspecting `/dev/shm`). |
| NFR-M1 | M | The multi-process harness passes under AddressSanitizer + ThreadSanitizer with no reported errors. |
| NFR-M2 | S | API behavior, error codes, segment layout, and the memory-ordering contract are documented in one reference. |
| NFR-PO1 | M | Builds and passes its test suite on Linux (primary). |
| NFR-PO2 | S | Builds on macOS (Apple silicon) for development, with the documented robustness caveat. |
| NFR-S1 | M | Named objects are created owner-only by default; wider access is opt-in. |
| NFR-S2 | M | Lengths/offsets read from the header are validated to prevent out-of-bounds access if the segment is corrupted. |

---

## 4. Verification & Benchmark Plan

### 4.1 Functional verification

- Unit tests for BipBuffer reserve/commit/release, the A→B switch, and length-framing edge cases (payload exactly filling A; forced early wrap to B; max-size payload).
- Multi-process integration: producer and consumer processes exchange a known sequence; consumer asserts FIFO order and byte-exact content (FR-7, FR-8, FR-9).
- Cross-language: C++↔Python and C++↔Rust round trips (FR-21).
- Crash test: `SIGKILL` the producer while a reservation is in flight; assert the Linux survivor recovers via the robust path (FR-18) and the macOS survivor aborts via heartbeat (FR-19).
- Sanitizers: run the harness under TSan/ASan (NFR-M1).
- Leak check: `/dev/shm` clean after a graceful run (NFR-R2).

### 4.2 The headline benchmark (D7 → NFR-P1/P2/P3)

**Two transports, identical workload:** (a) Shuttle; (b) a localhost-HTTP baseline that moves the **raw, uncompressed binary payload as the HTTP body** — no JSON, no base64, keep-alive on, sensible socket/buffer sizes. This keeps the comparison bulletproof: the baseline is HTTP doing the least wasteful thing it can.

**Workloads:** (1) a single **50 MB** blob (context/tensor case); (2) a stream of small frames (e.g., 16 KB) at a fixed rate (audio case, D5 secondary).

**Metrics** (warm-up first, then many iterations; report median and p99): end-to-end latency (producer commit → consumer has the full payload); CPU time attributable to copy/serialize per side (profiler); streaming throughput (MB/s and frames/s).

**Controls:** identical hardware (document the spec — note Linux prod vs. macOS dev figures separately), identical payloads, warm caches.

**Pass criteria:** NFR-P1 (≥10× on 50 MB), NFR-P2 (negligible copy CPU on the borrow path), NFR-P3 (µs-scale wake).

---

## 5. Appendices

### Appendix A — Segment layout

Fixed-width fields; atomics where noted; all internal references are **offsets** (App. B). Pad the header so the data region begins on a cache-line boundary.

```
+--------------------------------------------------------------+ offset 0
| Control Header                                               |
|   uint64_t magic                                            |
|   uint32_t version,  uint32_t flags                          |
|   uint64_t data_capacity                                     |
|   -- BipBuffer state (atomics; single-writer ownership) --   |
|     atomic<uint64_t> a_start    (consumer-owned)            |
|     atomic<uint64_t> a_size     (producer grows, consumer drains)|
|     atomic<uint64_t> b_size     (producer grows, consumer clears)|
|     uint64_t         reserve_start, reserve_size (producer-private)|
|   -- parking-lot wake --                                     |
|     atomic<uint32_t> consumer_waiting, producer_waiting     |
|   -- liveness (macOS heartbeat) --                           |
|     atomic<uint64_t> producer_heartbeat, consumer_heartbeat |
|   pthread_mutex_t lock   (PROCESS_SHARED [+ ROBUST on Linux])|
|   pthread_cond_t  not_empty (PROCESS_SHARED)                |
|   pthread_cond_t  not_full  (PROCESS_SHARED)                |
+--------------------------------------------------------------+ data_offset
| Data region (BipBuffer): sequence of [u64 len | payload] ... |
+--------------------------------------------------------------+ end
```

The memory-ordering contract: producer is the only writer of `a_size`/`b_size`/`reserve_*`; consumer is the only writer of `a_start`. The **A→B handoff** (consumer setting `a_start=0`, adopting `b_size`, clearing `b_size`) is the single most delicate ordering point and must be specified precisely in the design doc with release/acquire fences.

### Appendix B — Implementation gotchas (read before coding)

1. **Never store raw pointers in the segment.** Each process maps it at a different base; store offsets, resolve as `base + offset`.
2. **Initialize pshared attributes explicitly** on both the mutex and the condvars; a default-initialized mutex only works within one process.
3. **Robust recovery is real code:** on `EOWNERDEAD`, repair the protected state *then* call `pthread_mutex_consistent` before unlocking, or the mutex is permanently dead (Linux only).
4. **macOS gaps:** no robust mutexes, no `pthread_mutex_timedlock`. Use `pthread_cond_timedwait` + heartbeat. macOS also imposes short `shm_open` name limits and shm size limits — keep names short and check `ftruncate`/`mmap` returns.
5. **Single init:** exactly one process initializes the header; the opener must not proceed until init is published (an init flag/seqlock in the header).
6. **BipBuffer contiguity is the whole point:** never split a payload across the wrap; reserve `8 + len` as one unit and wrap early to B if it won't fit after A.
7. **Lost-wakeup guard:** always re-check the predicate under the mutex after waking; pair the `*_waiting` flag with the signal so the wake path stays off the hot path without dropping wakeups.

### Appendix C — Platform notes

- **Linux (prod):** full feature set incl. robust pshared mutexes; objects under `/dev/shm`. Primary, fully-supported target.
- **macOS (dev, Apple silicon):** pshared mutexes yes; robust mutexes and `pthread_mutex_timedlock` no → heartbeat + `cond_timedwait`. Short shm name limits.
- **Windows (future):** different API (`CreateFileMapping`/`MapViewOfFile`, named events); a portability layer is future work.

### Appendix D — Future work

Multi-producer/multi-consumer and pub/sub fan-out; a typed schema layer atop the byte transport; a Windows portability layer; optional huge-page backing for very large buffers; an io_uring-style completion API for batched streaming.

---

*End of document.*
