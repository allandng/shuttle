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
- **Backpressure, never drops.** A full buffer blocks the producer; data integrity is non-negotiable for embeddings and context windows. Oversized writes fail fast instead of blocking forever (validated at channel creation). *Never drops* remains the default and the guarantee — a caller who would rather lose a sample than stall opts in **per call** with `SHUTTLE_DROP_NEWEST` (v1.3), which is not a channel mode and never applies on its own.
- **Crash resilience.** Heartbeat liveness is the primary mechanism on both platforms: a peer SIGKILLed mid-transfer — even while *holding the park mutex* — leaves the survivor with a clean `PEER_DEAD` error, never a deadlock. Linux adds robust-mutex (`EOWNERDEAD`) recovery; macOS parks on `os_sync_wait_on_address`, which holds nothing a dying process could orphan.
- **Frozen C ABI.** Ten v1 functions whose signatures and semantics never change; new capability arrives only as a new symbol or a new flag bit (the surface is at v1.4 and `SHUTTLE_ABI_VERSION` is still `1`). Integer error codes, no exception ever crosses the boundary ([`include/shuttle/shuttle_c.h`](include/shuttle/shuttle_c.h)). Python binds via cffi with a zero-copy `memoryview` that invalidates on release; the Rust wrapper makes use-after-release a **compile error** (E0597) via borrow lifetimes.

### Since v1.1 — all opt-in, all additive to the frozen ABI

The v1 default path is byte-for-byte what it was. Each of these is a new symbol or a new create-flag bit, resolved once at channel construction, so a channel that does not ask for one never pays for it. Full reference in [docs/API.md](docs/API.md); dated measurements in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).

- **Stats counters** (v1.2) — `SHUTTLE_CREATE_STATS` puts five message/byte counters in the segment, readable by either peer or any process that opens it via `shuttle_get_stats`. The one flag that also bumps the segment layout version (1 → 2).
- **Explicit huge pages** (v1.2) — `SHUTTLE_CREATE_HUGETLB_2MB` / `_1GB` back the segment with *reserved* hugetlbfs pages. Unlike the advisory THP flag this is a guarantee or an error: no mount, no free pages, or macOS all yield `NO_HUGEPAGES` and create nothing. **Never a silent fallback to normal pages.**
- **Drop-newest backpressure** (v1.3) — the per-call `SHUTTLE_DROP_NEWEST` flag on `shuttle_write`, returning the positive `SHUTTLE_DROPPED`. Nothing already queued is touched, and the default stays blocking.
- **Page-aligned payload spans** (v1.4) — `SHUTTLE_CREATE_ALIGNED_SPANS` makes every borrowed payload pointer start on a system page, so it can go straight to `cudaHostRegister` or `newBufferWithBytesNoCopy` with no bounce buffer. Paid for in internal fragmentation (a page-sized message loses half the ring; a 1 MiB message loses 0.4%).
- **File-backed channels** (v1.4) — `shuttle_create_file` / `shuttle_open_file` / `shuttle_unlink_file` put the segment in a file at a path you choose, so capacity is bounded by the filesystem rather than by RAM or `/dev/shm`. Durability is an explicit **non-goal**: the library never calls `msync`, and a segment recovered off disk is not a resumable queue.
- **Peek and prefetch** (v1.4) — `shuttle_peek_next` reports whether the *next* message has been committed, and how long it is, **while a borrow is still outstanding**; it is read-only and moves no cursor. On file-backed channels the consumer additionally advises the kernel (`MADV_WILLNEED`) about committed-but-unread pages, automatically and with no symbol of its own. That hint is advisory and, on the only host it has been measured on, **not a speedup** — see E4 in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).
- **Experimental, not supported:** a Windows backend behind the platform seam, and a CUDA IPC descriptor module. Both are covered honestly under **Scope** below.

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

**Installed builds.** `cmake --install` is supported: it ships the headers, a versioned `SONAME` for `libshuttle_c`, a `find_package(shuttle)` config package, and a `shuttle.pc` for pkg-config — so a consumer that does not vendor the tree can use whichever of the three it prefers:

```cmake
find_package(shuttle REQUIRED)
target_link_libraries(my_app PRIVATE shuttle::c)   # or shuttle::core
```

**From Python and Rust.** `bindings/` holds distributable packages over the same frozen C ABI — a pip-installable `shuttle-ipc` (cffi, ABI mode, zero-copy `memoryview`) and the `shuttle-sys` / `shuttle` Rust crates (raw externs, and a safe wrapper whose borrow lifetimes make use-after-release a compile error). They track the ABI, new symbols included.

**Create-time options** — transparent huge pages, reserved hugetlbfs pages, stats counters, page-aligned spans — are create-flag bits on `shuttle_create_ex(name, cap, maxp, flags, &err)`, an additive C ABI v1.1 symbol; a file-backed channel is `shuttle_create_file(path, ...)` instead. All of them are covered in [docs/API.md](docs/API.md).

## Verification

The build was driven gate-by-gate with one rule: **one new variable per phase** — data-structure logic proven before concurrency, concurrency before IPC, ordering before wake mechanics, wake before crash recovery. The test suite is the standing evidence, and it stands alone.

Highlights of what the suite (**36 tests**, ASan + TSan clean on both legs; `ctest -N` on a default build is the count) actually proves:

- 200k-pair randomized property test of the BipBuffer with invariants checked after every operation (19k+ wraps in the tight configuration).
- ≥1 GiB two-process byte-exact FIFO stress; asymmetric-speed stress with the spin paths *proven engaged*; a wrap-heavy stress that fires the delicate A→B handoff 57k times.
- 100k trickle park/wake cycles with zero lost wakeups; hot path verified to take **zero** locks when the peer isn't parked.
- SIGKILL crash tests at both kill points (mid-transfer, and while holding the park mutex), on both platforms, including proof that the *test can fail* (a deliberately buggy recovery leaves the mutex `ENOTRECOVERABLE`).
- Cross-language byte-exact runs (C++→Python, C++→Rust) over the borrow path, and an induced-error sweep showing every failure surfaces as the right integer in all three languages.
- The v1.4 additions carry their own gates: a ≥10k-operation alignment property test (wrap-heavy, plus proof that a pre-v1.4 binary *rejects* an aligned segment), file-backed SIGKILL tests at both kill points including a raw `EOWNERDEAD` observation on a file mapping, and a trickle variant that keeps `peek_next` in the park/wake loop with zero lost wakeups.
- `fuzz/` holds two libFuzzer harnesses: one over the BipBuffer's operation sequences, one over header/geometry validation — the untrusted-input surface, with an oracle that recomputes the geometry independently. The header harness found a real integer overflow in `validate_header`, which was fixed in the same commit.

Measurements — the three-transport benchmark, aligned-vs-classic throughput, file-backed-vs-shm latency, and a recorded **null result** for the prefetch hint — are kept as a dated experiment log in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).

## Benchmark honesty

- Numbers above are from a native Apple M-series host (macOS) — **development figures**. Container (Docker on the same host) figures are 24 µs median for the 50 MB blob — still 482×/541× over UDS/HTTP — but are labeled *virtualized, not headline*.
- There is now also a **virtualized Linux x86_64 (cloud container, 4 vCPU)** data point, glibc, unsanitized `-O2`, 20 iterations after 3 warmups (2026-08-08): **62.3 µs median** for the 50 MB blob (p99 97.1 µs) — **101× over UDS, 355× over HTTP** — and **5.5 GB/s** on the 16 KB stream throughput case. The harness prints `(linux, native)` for this run only because it cannot detect virtualization from inside; it is a shared cloud container, and a cloud container is still not bare metal.
- Those ratios (101×/355×) sit well below the macOS-native ones (1,857×/1,699×), and the two causes are worth separating. Part of it is the baselines: UDS is genuinely faster on that box (6.3 ms for the 50 MB blob, against 9.3 ms on macOS), which shrinks its ratio without Shuttle changing at all — though HTTP is not (22.1 ms, against 8.5 ms). The rest is Shuttle itself being slower in absolute terms on shared cloud vCPUs: 62.3 µs, against 5 µs native and 24 µs in the macOS container. Both effects move the ratios, and only bare metal will separate the virtualization tax from the platform.
- That data point was **re-measured on 2026-08-08 against the current tree**, after the v1.4 features landed: 63.5 µs median (median of three consecutive runs; individual runs 62.6 / 63.5 / 72.7 µs) and 5.34 GB/s on the 16 KB stream. The default path is unchanged, as the design intends — every new capability is gated on a create-flag. The p99 did **not** reproduce as tightly (127.5 / 137.7 / 252.3 µs against the 97.1 µs above), so treat that percentile as one sample of a noisy quantity on a shared vCPU, not as a bound. Full run log: [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md), E1.
- The production target is Linux; the headline claim is **provisional until the harness runs on bare-metal Linux** (`make test-linux` on any glibc box, or run `shuttle_bench` directly). The virtualized Linux run above does not settle it.
- The HTTP baseline is deliberately fair: raw uncompressed body, keep-alive, TCP_NODELAY, 4 MB socket buffers — HTTP doing the least wasteful thing it can. A Unix-domain-socket baseline is included as the stronger comparator.
- "Zero serialization" applies to payloads already in flat binary layout (PCM, `float32` tensors, blobs). Application-level structuring costs exist on every transport and are not what Shuttle removes.

## Scope (unchanged since v1.0)

Same-host, single-producer/single-consumer, one-way channels. Cross-machine transport, MPMC/pub-sub, and payload schemas are explicitly out of scope. macOS crash recovery is best-effort by design (no robust mutexes exist there); Linux is the hard-guarantee platform. The v1.4 additions widen what a channel can be *backed by* and how its spans are laid out; they do not widen this scope.

**Windows is an experimental third backend, not a supported platform.** A `CreateFileMappingW` named-section + `WaitOnAddress` implementation lives behind the same platform seam as Linux/macOS and is **compile- and smoke-tested in CI** (a `windows-latest`/MSVC job that builds the seam, runs the pure-logic BipBuffer tests, and runs a threads-plus-two-process named-section round-trip). It is **not at parity**: there is no robust-mutex crash recovery (heartbeat liveness is the crash story, as on macOS), the multi-process SIGKILL gate suite and the Python/Rust FFI gates are POSIX-only, and no performance is claimed. Treat it as a reviewable starting point, not a production target.

**CUDA IPC is a research sketch, not a feature.** An opt-in module (`include/shuttle/shuttle_cuda.h`, design in [docs/CUDA_DESIGN.md](docs/CUDA_DESIGN.md)) sends a small fixed-layout **descriptor** — the opaque `cudaIpcMemHandle_t`, device id, offset, length — as an ordinary Shuttle message, so GPU data never rides the channel. The descriptor codec is pure host code and is unit-tested everywhere, with no CUDA required; the device glue is compile-guarded behind `-DSHUTTLE_CUDA=ON` and **has never run**, because there is no GPU in CI. Nothing about cross-process device visibility, event synchronization, or performance is proven. It is not part of the v1 ABI.

Proposed directions past v1 — and why each stays where it is — are triaged in [docs/ROADMAP.md](docs/ROADMAP.md).

## Repository layout

```
include/shuttle/   header.hpp (segment layout), bipbuffer.hpp (core logic),
                   spsc.hpp (lock-free path + parking), platform.hpp (the ONLY
                   file allowed to #ifdef on platform), shuttle_c.h (C ABI v1.4)
src/               lifecycle (shm/hugetlbfs/file + mmap/validate), C ABI impl,
                   and the opt-in CUDA descriptor module
tests/             36 gate tests; tests/ffi/ holds the frozen-ABI conformance
                   layer driven from Python and Rust
bindings/          distributable packages over that ABI: python/ (shuttle-ipc,
                   cffi) and rust/ (shuttle-sys + the safe shuttle crate)
fuzz/              libFuzzer harnesses over the untrusted-input surface
bench/             three-transport benchmark harness
tools/             shuttle_inspect (read-only segment introspection)
cmake/             install/packaging inputs (find_package config, shuttle.pc)
docs/              API.md (C ABI reference), EXPERIMENTS.md (dated measurements),
                   ROADMAP.md (post-v1 triage), CUDA_DESIGN.md (the sketch)
```
