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

The production target is Linux, and that path needs nothing exotic: a C++17 compiler, CMake (>= 3.25), and the standard build tools. Build and run the full correctness suite under AddressSanitizer/UBSan:

```sh
cmake -B build -DSHUTTLE_SAN=asan
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This is the path CI proves on `ubuntu-24.04` (g++, cmake). The cross-language FFI tests additionally need `python3` + `cffi` and `rustc`. Swap `-DSHUTTLE_SAN=asan` for `-DSHUTTLE_SAN=tsan` to get the ThreadSanitizer build — a separate tree, since ASan and TSan cannot be linked into the same binary. A handful of tests measure latency percentiles and CPU ratios and are only meaningful on quiet, controlled hardware; CI excludes them on shared runners with `ctest -E "bench_g71|park_latency|wake_under_load|nocopy_cpu|trickle"`, and you should too on a busy machine.

The project also ships a two-platform harness driven by `make`, which builds and runs the same suite natively on macOS (Apple silicon) *and* inside a glibc Linux container under both sanitizers:

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

Full function-by-function reference: [docs/API.md](docs/API.md).

The benchmark harness (`shuttle_bench`, built unsanitized at `-O2`) runs all three transports over identical workloads and prints the table above, labeling container runs as virtualized.

## Using Shuttle in your project

Shuttle is dependency-free (no third-party libraries), so consuming it is deliberately plain. Point a CMake project at this tree with `add_subdirectory` (or `FetchContent`) and link one of two targets:

- **`shuttle_c`** — the shared library exposing the frozen C ABI (`shuttle/shuttle_c.h`). This is the boundary the Python and Rust bindings link against.
- **`shuttle_core`** — the static library for the C++ API (headers under `include/shuttle/`).

Both targets declare their headers as `PUBLIC` include directories, so a linking consumer inherits the `include/` path automatically — no manual `target_include_directories` needed:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_app LANGUAGES C CXX)

add_subdirectory(path/to/shuttle shuttle_build)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE shuttle_c)   # or shuttle_core for the C++ API
```

```c
#include <shuttle/shuttle_c.h>   /* resolved via the inherited include path */
```

Because the library has no external dependencies, vendoring `include/` + `src/` straight into your own build is an equally legitimate option; if you go that route (or build without CMake), add `-Iinclude` and compile `src/shuttle.cpp` (plus `src/shuttle_c.cpp` for the C ABI) yourself.

For opt-in transparent huge pages on the segment, create the channel with `shuttle_create_ex(name, cap, maxp, SHUTTLE_CREATE_HUGEPAGES, &err)` — an additive C ABI v1.1 symbol (advisory `madvise` on Linux, no-op elsewhere); see [docs/API.md](docs/API.md).

## Verification

The build was driven gate-by-gate with one rule: **one new variable per phase** — data-structure logic proven before concurrency, concurrency before IPC, ordering before wake mechanics, wake before crash recovery. The test suite is the standing evidence, and it stands alone.

Highlights of what the suite (29 tests, ASan + TSan clean on both legs) actually proves:

- 200k-pair randomized property test of the BipBuffer with invariants checked after every operation (19k+ wraps in the tight configuration).
- ≥1 GiB two-process byte-exact FIFO stress; asymmetric-speed stress with the spin paths *proven engaged*; a wrap-heavy stress that fires the delicate A→B handoff 57k times.
- 100k trickle park/wake cycles with zero lost wakeups; hot path verified to take **zero** locks when the peer isn't parked.
- SIGKILL crash tests at both kill points (mid-transfer, and while holding the park mutex), on both platforms, including proof that the *test can fail* (a deliberately buggy recovery leaves the mutex `ENOTRECOVERABLE`).
- Cross-language byte-exact runs (C++→Python, C++→Rust) over the borrow path, and an induced-error sweep showing every failure surfaces as the right integer in all three languages.

## Benchmark honesty

- Numbers above are from a native Apple M-series host (macOS) — **development figures**. Container (Docker on the same host) figures are 24 µs median for the 50 MB blob — still 482×/541× over UDS/HTTP — but are labeled *virtualized, not headline*.
- There is now also a **virtualized Linux x86_64 (cloud container, 4 vCPU)** data point, glibc, unsanitized `-O2`, 20 iterations after 3 warmups (2026-08-08): **62.3 µs median** for the 50 MB blob (p99 97.1 µs) — **101× over UDS, 355× over HTTP** — and **5.5 GB/s** on the 16 KB stream throughput case. The harness prints `(linux, native)` for this run only because it cannot detect virtualization from inside; it is a shared cloud container, and a cloud container is still not bare metal.
- Those ratios (101×/355×) sit well below the macOS-native ones (1,857×/1,699×), and the two causes are worth separating. Part of it is the baselines: UDS is genuinely faster on that box (6.3 ms for the 50 MB blob, against 9.3 ms on macOS), which shrinks its ratio without Shuttle changing at all — though HTTP is not (22.1 ms, against 8.5 ms). The rest is Shuttle itself being slower in absolute terms on shared cloud vCPUs: 62.3 µs, against 5 µs native and 24 µs in the macOS container. Both effects move the ratios, and only bare metal will separate the virtualization tax from the platform.
- The production target is Linux; the headline claim is **provisional until the harness runs on bare-metal Linux** (`make test-linux` on any glibc box, or run `shuttle_bench` directly). The virtualized Linux run above does not settle it.
- The HTTP baseline is deliberately fair: raw uncompressed body, keep-alive, TCP_NODELAY, 4 MB socket buffers — HTTP doing the least wasteful thing it can. A Unix-domain-socket baseline is included as the stronger comparator.
- "Zero serialization" applies to payloads already in flat binary layout (PCM, `float32` tensors, blobs). Application-level structuring costs exist on every transport and are not what Shuttle removes.

## Scope (v1.0)

Same-host, single-producer/single-consumer, one-way channels. Cross-machine transport, MPMC/pub-sub, and payload schemas are explicitly out of scope. macOS crash recovery is best-effort by design (no robust mutexes exist there); Linux is the hard-guarantee platform.

**Windows is an experimental third backend, not a supported platform.** A `CreateFileMappingW` named-section + `WaitOnAddress` implementation lives behind the same platform seam as Linux/macOS and is **compile- and smoke-tested in CI** (a `windows-latest`/MSVC job that builds the seam, runs the pure-logic BipBuffer tests, and runs a threads-plus-two-process named-section round-trip). It is **not at parity**: there is no robust-mutex crash recovery (heartbeat liveness is the crash story, as on macOS), the multi-process SIGKILL gate suite and the Python/Rust FFI gates are POSIX-only, and no performance is claimed. Treat it as a reviewable starting point, not a production target. Proposed directions past v1 — and why each stays where it is — are triaged in [docs/ROADMAP.md](docs/ROADMAP.md).

## Repository layout

```
include/shuttle/   header.hpp (segment layout), bipbuffer.hpp (core logic),
                   spsc.hpp (lock-free path + parking), platform.hpp (the ONLY
                   file allowed to #ifdef on platform), shuttle_c.h (C ABI v1)
src/               lifecycle (shm_open/mmap/validate) + C ABI implementation
tests/             29 gate tests; tests/ffi/ holds the Python + Rust bindings
bench/             three-transport benchmark harness
docs/              API.md (frozen C ABI reference), ROADMAP.md (post-v1 triage)
```
