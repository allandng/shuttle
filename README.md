# Shuttle

[![CI](https://github.com/allandng/shuttle/actions/workflows/ci.yml/badge.svg)](https://github.com/allandng/shuttle/actions/workflows/ci.yml)

**A zero-copy shared-memory IPC framework for local AI workloads.** C++17 core, lock-free SPSC data path, crash-resilient, with a frozen C ABI driven from Python and Rust.

In a loom, the shuttle carries the thread back and forth across the warp. Here it carries 50 MB tensors between processes in microseconds.

```
50 MB payload, end-to-end (producer commit → consumer holds the payload):

  transport     median        vs Shuttle
  ─────────────────────────────────────────
  Shuttle         5 µs             —
  Unix socket   9.3 ms         1,857× slower
  HTTP (raw)    8.5 ms         1,699× slower

  (native Apple M-series, macOS — dev figures; see "Benchmark honesty" below)
```

## Why

Local AI stacks are polyglot: a Rust/Tauri frontend, Python sidecars, a C++ inference engine — all on one machine, shoveling large binary payloads (audio frames, embeddings, LLM context windows) between processes over localhost HTTP. On that path a 50 MB tensor is copied into a kernel socket buffer, through the loopback stack, into the receiver's socket buffer, and framed/deframed by HTTP — several full copies plus protocol overhead, per message.

Shuttle replaces that path for same-host communication: one region of physical RAM is mapped into both processes via POSIX shared memory. The producer writes a payload once; the consumer reads it **in place**. Measured consumer-side cost of receiving 2 GB over the borrow path: **0.22 ms of CPU — 0.03% of what the same bytes cost over a Unix socket**.

## Design

- **Strictly SPSC, point-to-point.** One writer, one reader per channel; multi-process stacks compose pairwise channels. This is what makes the lock-free data path sound: every shared cursor has exactly one writer.
- **Bipartite buffer (BipBuffer), not a plain ring.** Every reserved block is physically contiguous — a payload never straddles the wrap point, so the zero-copy pointer handoff is always valid. Cursor model is three absolute offsets (`read` / `write` / `watermark`, bbqueue-style), each strictly single-writer.
- **Lock-free hot path.** Cursors are atomics published with release stores and observed with acquire loads. The full happens-before argument for every shared atomic is written inline in [`include/shuttle/spsc.hpp`](include/shuttle/spsc.hpp).
- **Parking, not polling.** A blocked peer sleeps (idle cost measured at 0.05% CPU) and wakes in microseconds. The park decision uses a seq_cst Dekker protocol to close the classic store→load race; every wait is a bounded timedwait — nothing can sleep forever.
- **Backpressure, never drops.** A full buffer blocks the producer; data integrity is non-negotiable for embeddings and context windows. Oversized writes fail fast instead of blocking forever (validated at channel creation).
- **Crash resilience.** Heartbeat liveness is the primary mechanism on both platforms: a peer SIGKILLed mid-transfer — even while *holding the park mutex* — leaves the survivor with a clean `PEER_DEAD` error, never a deadlock. Linux adds robust-mutex (`EOWNERDEAD`) recovery; macOS parks on `os_sync_wait_on_address`, which holds nothing a dying process could orphan.
- **Frozen C ABI.** Ten functions, integer error codes, no exception ever crosses the boundary ([`include/shuttle/shuttle_c.h`](include/shuttle/shuttle_c.h)). Python binds via cffi with a zero-copy `memoryview` that invalidates on release; the Rust wrapper makes use-after-release a **compile error** (E0597) via borrow lifetimes.

## Quick start

Requirements: macOS (Apple silicon) with Xcode CLT + CMake, and Docker Desktop for the Linux leg. The FFI tests additionally use `python3` + `cffi` and `rustc` (both preinstalled in the provided container image).

```sh
make test-mac     # native build + full test suite under ASan/UBSan
make test-linux   # the same, inside a glibc arm64 container (--shm-size=512m)
make tsan-mac     # ThreadSanitizer legs (separate build trees)
make tsan-linux
```

Minimal producer/consumer over the C ABI:

```c
#include <shuttle/shuttle_c.h>

/* producer process */
int err;
shuttle_channel* ch = shuttle_create("/my-chan", 128u << 20, 64u << 20, &err);
void* span;
shuttle_acquire_write(ch, &span, payload_len, 0);   /* contiguous, in-segment */
fill_tensor(span, payload_len);                     /* write the payload ONCE */
shuttle_commit_write(ch, payload_len);

/* consumer process */
shuttle_channel* ch = shuttle_open("/my-chan", &err);
const void* p; size_t len;
shuttle_acquire_read(ch, &p, &len, 0);              /* zero-copy borrow */
run_inference(p, len);                              /* read in place */
shuttle_release_read(ch);
```

The benchmark harness (`shuttle_bench`, built unsanitized at `-O2`) runs all three transports over identical workloads and prints the table above, labeling container runs as virtualized.

## Verification

The build was driven gate-by-gate through an 8-phase plan ([docs/Shuttle_Implementation_Plan.md](docs/Shuttle_Implementation_Plan.md)) with one rule: **one new variable per phase** — data-structure logic proven before concurrency, concurrency before IPC, ordering before wake mechanics, wake before crash recovery. All 27 gates passed on both platforms; the complete ledger with per-gate evidence, dated decisions, and the failures encountered along the way is in [PROGRESS.md](PROGRESS.md).

Highlights of what the suite (28 tests, ASan + TSan clean on both legs) actually proves:

- 200k-pair randomized property test of the BipBuffer with invariants checked after every operation (19k+ wraps in the tight configuration).
- ≥1 GiB two-process byte-exact FIFO stress; asymmetric-speed stress with the spin paths *proven engaged*; a wrap-heavy stress that fires the delicate A→B handoff 57k times.
- 100k trickle park/wake cycles with zero lost wakeups; hot path verified to take **zero** locks when the peer isn't parked.
- SIGKILL crash tests at both kill points (mid-transfer, and while holding the park mutex), on both platforms, including proof that the *test can fail* (a deliberately buggy recovery leaves the mutex `ENOTRECOVERABLE`).
- Cross-language byte-exact runs (C++→Python, C++→Rust) over the borrow path, and an induced-error sweep showing every failure surfaces as the right integer in all three languages.

## Benchmark honesty

- Numbers above are from a native Apple M-series host (macOS) — **development figures**. Container (Docker on the same host) figures are 24 µs median for the 50 MB blob — still 482×/541× over UDS/HTTP — but are labeled *virtualized, not headline*.
- The production target is Linux; the headline claim is **provisional until the harness runs on bare-metal Linux** (`make test-linux` on any glibc box, or run `shuttle_bench` directly).
- The HTTP baseline is deliberately fair: raw uncompressed body, keep-alive, TCP_NODELAY, 4 MB socket buffers — HTTP doing the least wasteful thing it can. A Unix-domain-socket baseline is included as the stronger comparator.
- "Zero serialization" applies to payloads already in flat binary layout (PCM, `float32` tensors, blobs). Application-level structuring costs exist on every transport and are not what Shuttle removes.

## Scope (v1.0)

Same-host, single-producer/single-consumer, one-way channels. Cross-machine transport, Windows, MPMC/pub-sub, and payload schemas are explicitly out of scope (see [docs/Shuttle_SRS.md](docs/Shuttle_SRS.md)). macOS crash recovery is best-effort by design (no robust mutexes exist there); Linux is the hard-guarantee platform.

## Repository layout

```
include/shuttle/   header.hpp (segment layout), bipbuffer.hpp (core logic),
                   spsc.hpp (lock-free path + parking), platform.hpp (the ONLY
                   file allowed to #ifdef on platform), shuttle_c.h (C ABI v1)
src/               lifecycle (shm_open/mmap/validate) + C ABI implementation
tests/             28 gate tests; tests/ffi/ holds the Python + Rust bindings
bench/             three-transport benchmark harness
docs/              SRS, implementation plan, build directive
PROGRESS.md        the complete build ledger: every gate, decision, and dead end
```
