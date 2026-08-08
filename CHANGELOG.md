# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Versioning applies to the library's public surfaces: the frozen C ABI
(`include/shuttle/shuttle_c.h`) and the shared-segment layout
(`ChannelHeader`). Additive-only changes to either are minor releases; anything
that moves a header field or alters an existing ABI signature is a major
release and bumps `kVersion`.

## [Unreleased]

## [1.1.0] - 2026-07-24

### Added

- Opt-in transparent huge pages via `shuttle_create_ex(name, capacity,
  max_payload, create_flags, err)` and the `SHUTTLE_CREATE_HUGEPAGES` flag bit.
  A new symbol only — every frozen v1 signature and semantic is untouched, so
  `SHUTTLE_ABI_VERSION` stays `1` and the ABI is described as v1.1.
- `shuttle::create` gained a defaulted `create_flags` parameter, keeping
  source compatibility with pre-flags callers.
- `ChannelHeader::flags` is now a documented additive extension point: the
  creator writes the word once in the cold identity block before the
  `init_state` release-store, and openers must ignore bits they do not know.
  New bits therefore carry no `kVersion` bump.
- `advise_huge_pages` in the platform seam — `madvise(MADV_HUGEPAGE)` on Linux,
  a no-op on macOS. Both creator and opener advise their own mapping, since the
  two mappings are independent.
- `tests/hugepage_test.cpp`: create/open round-trip with the flag set, byte-
  exact transfer, and proof that unknown create-flag bits are masked off and
  never persisted into the segment.

### Notes

- THP is advisory and never a correctness dependency. It takes effect only
  where the kernel THP shmem policy permits; a kernel that refuses returns an
  `EINVAL` that is deliberately dropped.
- `create_flags` is a separate namespace from the per-operation `flags`
  argument used by the read/write calls; the two must not be mixed.

## [1.0.0] - 2026-07-24

First tagged release: a zero-copy shared-memory IPC framework for local AI
workloads. C++17 core, lock-free SPSC data path, crash-resilient, with a frozen
C ABI driven from Python and Rust.

### Added

- **Segment lifecycle.** `shm_open(O_CREAT|O_EXCL)` + one-shot `ftruncate` +
  `mmap` + header init, published last with a release store; owner-only `0600`
  permissions. Openers acquire-spin on `init_state` under a deadline, then
  validate magic, version, and header geometry as distinct errors.
- **BipBuffer core.** Bipartite ring on the A1 cursor model — three absolute
  offsets (`read` / `write` / `watermark`), each strictly single-writer — so
  every reserved block is physically contiguous and the zero-copy pointer
  handoff is always valid. Oversize writes fail fast with zero state mutation.
- **Lock-free SPSC data path.** Cursors are atomics published with release
  stores and observed with acquire loads; the hot path takes zero locks while
  the peer is not parked. The full happens-before argument is written inline in
  `include/shuttle/spsc.hpp`.
- **Parking, not polling.** Brief adaptive spin, then park: a seq_cst Dekker
  protocol (amendment A4) closes the store→load race, and every wait is a
  bounded timedwait, so nothing can sleep forever. Idle peer cost measured at
  0.05% CPU. Linux parks on a robust pshared mutex + `CLOCK_MONOTONIC` condvar;
  macOS parks on `os_sync_wait_on_address`, which holds nothing a dying process
  could orphan.
- **Crash resilience.** Heartbeat liveness is the primary mechanism on both
  platforms: a peer SIGKILLed mid-transfer — even while holding the park mutex
  — leaves the survivor with a clean `PEER_DEAD` error rather than a deadlock.
  Linux adds robust-mutex `EOWNERDEAD` recovery in the seam, with the repair
  order (repair, then `pthread_mutex_consistent`, then unlock) documented.
- **Backpressure, never drops.** A full buffer blocks the producer; payloads
  larger than `max_payload` are refused at the call rather than blocking
  forever, and the FR-4 capacity rule makes an unsatisfiable write impossible
  by construction at channel creation.
- **Frozen C ABI v1.** Ten `extern "C"` functions, integer error codes, no
  exception crossing the boundary; every `SHUTTLE_*` constant is
  `static_assert`ed against its `shuttle::Err` counterpart.
- **Bindings.** Python via cffi with a zero-copy `memoryview` that invalidates
  on release; hand-written Rust externs whose borrow lifetimes make
  use-after-release a compile error (E0597).
- **`shuttle_inspect`.** Read-only segment introspection — dumps header
  geometry, cursors, flags, and heartbeats without validating or mutating.
- **Benchmark harness.** `shuttle_bench` runs Shuttle, Unix-domain sockets, and
  raw HTTP over identical workloads and labels container runs as virtualized.
- **Two-platform test harness.** `make test-mac` / `test-linux` / `tsan-mac` /
  `tsan-linux`; 29 gate tests, ASan+UBSan and TSan clean on both legs.
- **CI.** Linux x86_64 ASan and TSan correctness suites on push, with the
  performance-measurement tests excluded on shared runners; a version-tag push
  publishes a GitHub Release from versioned notes.
- **Docs.** `README.md`, `docs/API.md` (function-by-function C ABI reference),
  `docs/ROADMAP.md` (post-v1 triage), MIT license.

### Scope

Same-host, single-producer/single-consumer, one-way channels. Cross-machine
transport, Windows, MPMC/pub-sub, and payload schemas are explicitly out of
scope. Linux is the hard-guarantee platform; macOS crash recovery is
best-effort by design, because robust mutexes do not exist there.

### Benchmark honesty

Headline figures (50 MB end-to-end in ~5 µs, versus ~9 ms over Unix sockets)
are development measurements on native Apple M-series hardware. The production
target is Linux, and the headline claim stays provisional until the harness
runs on bare metal Linux. See the README's "Benchmark honesty" section.

---

Historical note: this file was seeded retrospectively from
`.github/releases/v1.0.0.md` and the commit history. No git tags existed at the
time of writing, and the v1.0.0 release notes bundle the huge-pages work into
the first release; it is broken out as 1.1.0 here because it is an additive ABI
change (C ABI v1.1) and deserves its own semantic-versioning line. Both entries
carry the date of the corresponding commits.
