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

## v1.x candidates

Plausible next steps; each carries a caveat that keeps it out of v1.

- **Explicit hugetlbfs pages.** Reserved 2 MB / 1 GB pages (optionally
  `MADV_COLLAPSE`) for guaranteed large-page backing rather than THP's advisory
  best-effort. Needs dedicated hardware with reserved huge pages to test — CI
  shared runners cannot provide it.
- **Stats counters in the shared header.** Message/byte counters surfaced
  through the inspect tool. Touches the frozen segment layout, so it needs
  versioning care (the layout freeze is an ABI contract).
- **Configurable backpressure policies.** Lossy drop/overwrite modes for callers
  that prefer freshness over completeness. Must be **strictly opt-in**:
  never-drops is a v1 guarantee, so any lossy mode is an explicit,
  separately-selected policy — never a default or a silent fallback.
- **Bare-metal Linux benchmark run.** Convert the provisional headline latency
  claim into a final one by measuring on controlled hardware (shared CI runners
  produce arbitrary numbers and cannot settle a percentile claim).

## Exploratory / v2

Larger directions that would reshape the design; captured for direction, not
committed.

- **CUDA IPC / GPU-direct interop.** Let GPU-bound payloads move without a
  CPU-RAM round-trip, for pipelines where both ends already live on the device.
- **Windows named shared memory.** A `CreateFileMapping`-based platform seam
  alongside the current POSIX one. v1 is Linux + macOS only; Windows is a whole
  new backend, not a port.
