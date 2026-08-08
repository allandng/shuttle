# Shuttle Roadmap

Honest triage of proposed upgrade directions. No dates, no promises — just
direction and rationale. Entries are grouped by how settled they are.

## Already in v1

Shipped and tested; listed here because they are frequently proposed as
"future work" when they already exist.

- **Lock-free acquire/release SPSC data path.** Single-writer atomic cursors, no
  mutex on the hot path; the park mutex guards only the park/wake handshake.
  See `include/shuttle/spsc.hpp` (memory-ordering contract in the header
  comment).
- **Zero-copy Python memoryview.** The borrowed payload is a `memoryview` over
  the shared segment — no copy — with a guard that invalidates on release.
  See `tests/ffi/py_consumer.py`.
- **Frozen C ABI with `#[repr(C)]` layout contracts.** A stable `extern "C"`
  surface consumed by hand-written Rust externs whose types encode the borrow
  lifetime. See `tests/ffi/rust/shuttle.rs` and `include/shuttle/shuttle_c.h`.
- **Read-only segment introspection tool.** Dumps the header (cursors, flags,
  heartbeats, geometry) without validating or mutating. See `tools/inspect.cpp`.

## Landing in this change

- **Opt-in transparent huge pages (THP).** `shuttle_create_ex` with
  `SHUTTLE_CREATE_HUGEPAGES` advises the mapping via `madvise(MADV_HUGEPAGE)` on
  Linux. Advisory only: a no-op on macOS and where the kernel THP policy
  disallows it, never a correctness dependency. The flag is recorded in the
  segment header so the opener advises its own mapping too. Unknown create-flag
  bits are masked.
- **Explicit hugetlbfs pages.** `shuttle_create_ex` with
  `SHUTTLE_CREATE_HUGETLB_2MB` / `_1GB` puts the segment in *reserved* huge
  pages: the object becomes a file on a hugetlbfs mount of that page size,
  discovered by parsing `/proc/mounts` (a mount without `pagesize=` is resolved
  against `/proc/meminfo`'s `Hugepagesize`). Unlike THP this is a guarantee or
  an error — no mount, no free pages, or macOS all yield
  `SHUTTLE_ERR_NO_HUGEPAGES` (-14) and create nothing. **Never a silent
  fallback to normal pages**; that distinction is the entire point of the flag.
  Openers need no flag and no `MAP_HUGETLB`: `shuttle_open` probes both
  namespaces, and a `MAP_SHARED` mapping of a hugetlbfs file is huge-page
  backed because the file says so.

  Honest caveat about testing: the **positive** path runs only on a host where
  an operator has reserved pages and mounted hugetlbfs, so CI (and any shared
  runner) exercises the **error** path instead — `tests/hugetlb_test.cpp`
  asserts the exact `-14`, the both-bits-set rejection, and that a failed
  create leaves nothing in either namespace, then prints `SKIP` and exits 0.
  The positive path was verified on a host with `vm.nr_hugepages=64` and a
  2 MB hugetlbfs mount (cross-process open, byte-exact transfer,
  `KernelPageSize: 2048 kB` observed in `/proc/self/smaps` on both sides);
  it is not verified by every run. `MADV_COLLAPSE` was deliberately **not**
  implemented: it collapses normal pages into THP, which is redundant once the
  mapping is on explicitly reserved huge pages.

- **Configurable backpressure: the drop-newest policy.** `shuttle_write` accepts
  the per-op flag `SHUTTLE_DROP_NEWEST` (`0x2`): on a full ring the message is
  discarded instead of blocking, and the call returns the new positive
  `SHUTTLE_DROPPED` (`1`) — the first non-negative-non-zero return in the ABI,
  which is why the documentation now says to test errors as `rc < 0`. Strictly
  **per call**: no create-flag, no channel mode, no fallback path, so
  "backpressure, never drops" remains the default and the guarantee. It implies
  try-semantics (it can never park, even against a dead consumer), it never
  touches anything already queued, oversize payloads still fail with
  `MSG_TOO_LARGE` rather than being disguised as backpressure, and it is refused
  with `INVALID_ARGS` on the read/acquire paths where there is nothing to drop.
  Drops increment `msgs_dropped` on a `SHUTTLE_CREATE_STATS` segment; on a v1
  segment the drop still happens, uncounted. See `tests/drop_policy_test.cpp`.

  **Overwrite-oldest is rejected for v1.x**, and not on taste. Making the
  producer discard the oldest queued message would require it to:

  1. **advance `read`** — the consumer's cursor. Strict single-writer ownership
     of `write`/`watermark` (producer) and `read` (consumer) is the premise
     every memory-ordering proof in `spsc.hpp` rests on: it is what lets each
     store be a plain store rather than an RMW, and what makes a stale read of
     the peer's cursor merely conservative instead of unsound. A second writer
     to `read` invalidates all of it, C2's handoff argument first.
  2. **invalidate a borrow that is legally outstanding.** `[read, read +
     borrowed)` is a live pointer the consumer is allowed to be dereferencing;
     the zero-copy contract says those bytes stay valid until release. A
     producer reclaiming them turns the headline feature into a use-after-free
     that no API rule could make the consumer's fault.
  3. **parse frame headers it does not own,** racing the consumer, to find where
     the oldest message ends — reading data-region bytes whose framing is only
     stable because exactly one side advances each cursor.

  A sound freshness story needs none of that, and needs no library change:
  **consumer drain-to-latest** — read and release in a loop while messages are
  available, keep the last one. The consumer is the side that knows what it is
  behind on, it is only doing what a consumer always does, and backpressure
  still protects the ring if it stops draining. It is documented as the
  recommended pattern in `docs/API.md`.

## v1.x candidates

Plausible next steps; each carries a caveat that keeps it out of v1.
- **Stats counters in the shared header.** Message/byte counters surfaced
  through the inspect tool. Touches the frozen segment layout, so it needs
  versioning care (the layout freeze is an ABI contract).
- **Bare-metal Linux benchmark run.** Convert the provisional headline latency
  claim into a final one by measuring on controlled hardware (shared CI runners
  produce arbitrary numbers and cannot settle a percentile claim). A virtualized
  Linux x86_64 data point now exists — cloud container, 4 vCPU, glibc,
  unsanitized `-O2`, 2026-08-08: 62.3 µs median (p99 97.1 µs) for the 50 MB
  blob, 101×/355× over UDS/HTTP, 5.5 GB/s on the 16 KB stream. That is a shared
  virtual machine, so it does not settle the claim; bare metal remains the
  missing piece.

## Exploratory / v2

Larger directions that would reshape the design; captured for direction, not
committed.

- **CUDA IPC / GPU-direct interop.** Let GPU-bound payloads move without a
  CPU-RAM round-trip, for pipelines where both ends already live on the device.
  An **experimental, opt-in module now exists in the tree** as a first sketch of
  this direction — `include/shuttle/shuttle_cuda.h`, `src/shuttle_cuda.cpp`,
  design in [docs/CUDA_DESIGN.md](CUDA_DESIGN.md). The idea: GPU data never
  rides the channel; instead the producer sends a small fixed-layout
  **descriptor** (the opaque 64-byte `cudaIpcMemHandle_t`, device id, offset,
  length, optional event handle) as an ordinary Shuttle message, and the
  consumer opens it with `cudaIpcOpenMemHandle` and reads device memory
  directly. The descriptor codec is **pure host code** (the handle is just
  bytes), so it is fully unit-tested here — `tests/cuda_desc_test.cpp`, built and
  run on every platform, no CUDA required. The device glue is compile-guarded
  behind `-DSHUTTLE_CUDA=ON` and has **never run**: there is no GPU in CI, so a
  compile-only job proves only that the glue is well-formed against the CUDA
  headers. **Nothing about actual cross-process device visibility, event
  synchronization, or performance is proven.** This is a research sketch, not a
  supported surface, and explicitly not part of the v1 ABI — the design doc
  keeps the precise proven-vs-unproven ledger. It stays in v2 exactly because
  the hard part (the borrow-vs-kernel lifetime race, real multi-GPU behavior)
  cannot be settled without hardware.
- **Windows named shared memory.** A `CreateFileMapping`-based platform seam
  alongside the current POSIX one. v1 is Linux + macOS only; Windows is a whole
  new backend, not a port.
