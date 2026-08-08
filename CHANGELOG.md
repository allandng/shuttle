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

Everything since 1.1.0, in one section because none of it has been tagged. It
spans **three additive C ABI steps** — v1.2 (stats), v1.3 (drop-newest), v1.4
(aligned spans, file-backed channels, peek) — and under the policy above each
of those is a minor release, so this would tag as **1.4.0**:
`SHUTTLE_ABI_VERSION` is still `1`, no frozen signature moved, and no header
field moved. The one segment-layout addition (`SHUTTLE_CREATE_STATS`) is
version-gated (layout 1 → 2) rather than silent, which is what keeps it minor.

### Added

- **Opt-in statistics counters (C ABI v1.2).** `shuttle_create_ex(...,
  SHUTTLE_CREATE_STATS, ...)` allocates five counters in the segment —
  `msgs_written`, `bytes_written`, `msgs_dropped`, `msgs_read`, `bytes_read` —
  read back with the new `shuttle_get_stats(ch, shuttle_stats*)` by either peer
  or any third process that opens the segment. Payload bytes only; the frame
  header is transport overhead and is excluded on both sides. Each counter has
  exactly one writer and is updated with a relaxed load + relaxed store on a
  cache line that side already owns, with the producer's and consumer's
  counters on separate lines. This is the one create-flag that also selects a
  new **segment layout version** (1 → 2), because it changes the physical size
  of the header — so a pre-v1.2 binary handed a v2 segment reports
  `BAD_VERSION` rather than silently ignoring the bit. New error
  `SHUTTLE_ERR_NO_STATS` (-15) for reading counters off a v1 segment.
  `shuttle_inspect` dumps them.
- **Explicit hugetlbfs backing.** `SHUTTLE_CREATE_HUGETLB_2MB` / `_1GB` put the
  segment in *reserved* huge pages: the object becomes a file on a hugetlbfs
  mount of that page size, discovered by parsing `/proc/mounts` (a mount with
  no `pagesize=` option resolved against `/proc/meminfo`). Unlike the advisory
  THP flag this is a guarantee or an error — **never a silent fallback to
  normal pages** — and a request that cannot be honored returns the new
  `SHUTTLE_ERR_NO_HUGEPAGES` (-14) having created nothing. Setting both bits is
  `INVALID_ARGS`. Openers need no flag and no `MAP_HUGETLB`: `shuttle_open` and
  `shuttle_unlink` probe shm first, then every hugetlbfs mount.
- **Opt-in drop-newest backpressure (C ABI v1.3).** The per-op flag
  `SHUTTLE_DROP_NEWEST` (`0x2`) on `shuttle_write` discards the message it was
  passed instead of blocking on a full ring, returning the new positive
  `SHUTTLE_DROPPED` (`1`) — the first non-zero non-negative return in the ABI,
  which is why the docs now say to test errors as `rc < 0`. Strictly per call:
  no create-flag, no channel mode. It never parks (so never returns
  `PEER_DEAD`), never touches anything already queued, is refused with
  `INVALID_ARGS` on the read/acquire paths, and increments `msgs_dropped` where
  counters exist. **"Backpressure, never drops" remains the default and the
  guarantee.**
- **Page-aligned payload spans (C ABI v1.4).** `SHUTTLE_CREATE_ALIGNED_SPANS`
  (`0x10`) makes every payload span start on a system page, so a borrowed or
  reserved pointer can be handed to `cudaHostRegister` or
  `newBufferWithBytesNoCopy` with no bounce buffer. Framing becomes
  `[8-byte length][pad to page][payload][pad to page]`, so the stride is
  `page + round_up(len, page)` and the `CAPACITY_TOO_SMALL` floor rises to
  match. Worst-case internal fragmentation is `2*page - 8 - 1` = 8183 bytes per
  message at a 4 KiB page. Because the flag changes byte layout it is
  deliberately **not** left to the ignore-unknown-bits rule: an aligned
  segment's `data_offset` is page-rounded, and a pre-v1.4 binary refuses it
  with `CORRUPT` at the geometry check.
- **File-backed channels (C ABI v1.4).** Three additive path-typed symbols —
  `shuttle_create_file`, `shuttle_open_file`, `shuttle_unlink_file` — put the
  segment in an ordinary file at an absolute path, so capacity is bounded by
  the filesystem rather than by RAM or `/dev/shm`, and the page cache owns
  residency. The segment written is byte-for-byte the segment an shm create
  writes; `shuttle_close` and every read/write/stats entry point are shared.
  `SHUTTLE_CREATE_FILE_BACKED` (`0x20`) is persisted and informational, and is
  the one bit `shuttle_create_ex` cannot set. Hugetlb bits are rejected on this
  path (`INVALID_ARGS`) because a hugetlbfs mount and a caller-chosen path name
  two different segments. The crash story was **re-argued and measured**, not
  assumed to carry over: robust-mutex `EOWNERDEAD` recovery is observed
  directly on a file mapping, and SIGKILL at both kill points leaves the
  survivor with `PEER_DEAD`. **Durability is an explicit non-goal** — the
  library never calls `msync` on this path, and a segment recovered off disk is
  not a resumable queue. Stale files (a file survives a reboot; an shm object
  does not) are handled by refusing to truncate: `create_file` returns `EXISTS`,
  and the documented recovery is an explicit unlink-then-create.
- **Read-only lookahead and prefetch (C ABI v1.4).** `shuttle_peek_next(ch,
  size_t* len_out)` reports whether the next **un-borrowed** message is
  committed and how long it is — valid with a borrow outstanding, which is the
  case that matters, since borrows are strictly release-before-acquire. It
  moves no cursor, stores nothing, and adds no memory-ordering obligation
  beyond a documented reuse of the read path's existing edges;
  `SHUTTLE_ERR_WOULD_BLOCK` is the ordinary "not here yet", mapped to `None` /
  `Ok(None)` in the bindings. Separately, on **file-backed channels only**, the
  consumer now advises the kernel about the committed-but-unread region
  (`posix_madvise(POSIX_MADV_WILLNEED)`) on successful acquire and before
  parking. Advisory, return value deliberately ignored, gated on a `bool`
  resolved once at consumer construction so the default path never reaches it,
  and never naming the free part of the ring. No new symbol.
- **Distributable Python and Rust binding packages.** `bindings/python`
  (`shuttle-ipc`, cffi in ABI mode, zero-copy `memoryview` with a guard that
  invalidates on release) and `bindings/rust` (`shuttle-sys` raw externs plus
  the safe `shuttle` crate, whose borrow lifetimes make use-after-release a
  compile error). Both track the full current ABI, new symbols included; the
  Rust crate offers the peek on **both** `Consumer` and `Borrowed`, with a
  `compile_fail` doctest pinning why.
- **Install and packaging.** `cmake --install` ships the headers, a versioned
  `SONAME` for `libshuttle_c`, a `find_package(shuttle)` config package
  exporting `shuttle::c` / `shuttle::core`, and a `shuttle.pc` for pkg-config.
- **libFuzzer harnesses.** `fuzz/bipbuffer_fuzz.cpp` (operation tape against a
  deque reference model, guard canaries, wrap invariant) and
  `fuzz/header_fuzz.cpp` (arbitrary, seeded, and field-directed segments with
  an overflow-safe soundness oracle that recomputes geometry independently).
  `-DSHUTTLE_FUZZ=ON` is clang-only; a CI job runs each for 60 s from a cold
  corpus and uploads reproducers on failure.
- **Experimental Windows backend (compile + smoke, NOT parity).** A third
  branch inside `platform.hpp` — `CreateFileMappingW` named sections,
  `MapViewOfFile`, `WaitOnAddress`/`WakeByAddressAll` — with every Win32
  `#ifdef` confined to the seam. The park block became a seam-defined
  `ParkArea`, keeping the POSIX header offsets and `data_offset` byte-for-byte
  unchanged under a compile-time tripwire. A `windows-latest` MSVC job builds
  the libraries and the pure-logic tests and runs a threads-plus-two-process
  named-section round-trip. Not at parity: no robust-mutex crash recovery, no
  `unlink` (a named section dies with its last handle), the POSIX SIGKILL gate
  suite and the FFI gates do not run, and no performance is claimed.
- **Experimental CUDA IPC descriptor module.** `include/shuttle/shuttle_cuda.h`
  + `src/shuttle_cuda.cpp` and `docs/CUDA_DESIGN.md`: GPU data never rides the
  channel; a small fixed-layout descriptor (the opaque `cudaIpcMemHandle_t`,
  device id, offset, length, optional event handle) travels as an ordinary
  message. The codec is pure host code and is unit-tested on every platform
  with no CUDA present; the device glue is compile-guarded behind
  `-DSHUTTLE_CUDA=ON` and **has never run**. Not part of the v1 ABI.
- **macOS CI legs.** ASan and TSan on `macos-latest`, so the two-platform
  claim is enforced by CI rather than by local runs.
- **Contributor hygiene.** `.clang-format` derived from the existing tree (not
  imposed on it), `CONTRIBUTING.md`, and this changelog.
- **Docs.** `docs/EXPERIMENTS.md` — a dated, curated measurement record for
  this host class, including a worked KV-cache handoff example and an honest
  **null result** for the prefetch hint. `docs/API.md` grew sections for every
  addition above; `docs/ROADMAP.md` gained a "Shipped in v1.4" note; the README
  now lists the post-v1.0 capabilities and carries a re-measured benchmark data
  point.

### Fixed

- **Security-relevant: integer overflow in segment header validation.**
  `shuttle_open`'s geometry check computed the `uint64_t` sum
  `max_payload + kFrameHeader > data_capacity`, which **wraps** for a
  `max_payload` near 2^64. A segment claiming an 18-exabyte `max_payload` was
  therefore accepted, which disarmed `Consumer::parse`'s length guard and let
  the library hand a forged **out-of-bounds span** to the caller — reachable by
  anyone who can write the segment a process opens. Found by `header_fuzz` in
  about two seconds. The magic/version/geometry checks were extracted into a
  pure `validate_header()` with every bound written as subtraction against a
  checked floor, and the same overflow shape was closed in `create()`'s FR-4
  check and in `parse()`'s backstop, so the whole class is gone rather than the
  one reachable instance. No legitimate geometry changes verdict; 9.7M fuzz
  executions clean after the fix.
- **`docs/API.md` contradicted the implementation on `shuttle_acquire_read`.**
  The borrow rules claimed a second acquire before releasing returns
  `INVALID_ARGS`; the consumer side has always been **idempotent** (it returns
  the same pointer and length and moves no cursor), while only the *producer*
  side rejects a double reservation. The documentation was corrected to match
  the implementation — no code changed, so no caller's behavior changed. The
  idempotent behavior is kept deliberately: it composes with
  `shuttle_peek_next`. Recorded as E5 in `docs/EXPERIMENTS.md`.
- Stale test count in the README (29 → 36, per `ctest -N` on a default build).

### Changed

- **Segment-backend seam in `platform.hpp`.** The lifecycle syscalls moved
  behind a `SegBacking`-parameterized interface
  (`seg_create` / `seg_open` / `seg_size` / `seg_map` / `seg_unmap` /
  `seg_close` / `seg_unlink` / `seg_map_len`), so `src/shuttle.cpp` no longer
  names `shm_open` directly. Pure refactor at the time; it is what let
  hugetlbfs, the Windows section backend, and `kFile` land afterward as new
  enum values plus branches rather than as signature churn.
- **Test errors as `rc < 0`, not `rc != SHUTTLE_OK`.** Documented across
  `docs/API.md` now that `SHUTTLE_DROPPED` (1) exists. Correct before and after
  the addition; the `!= SHUTTLE_OK` form reports a successful drop as a
  failure.
- The virtualized Linux x86_64 benchmark data point recorded in the README and
  ROADMAP; the **headline claim stays provisional** until the harness runs on
  bare-metal Linux. A shared cloud container is not bare metal, and the re-run
  against this tree (E1) showed the p99 on that host is too noisy to quote.

### Notes

- Every feature here is opt-in and additive. A channel created by plain
  `shuttle_create` is byte-for-byte the v1 segment, on the v1 code path; the
  gates for stats, aligned framing, and prefetch are each resolved once at
  construction, so the default path pays predictable branches and nothing else.
  E1 in `docs/EXPERIMENTS.md` is the standing evidence.
- Two flags are exceptions to "openers ignore unknown bits", each backed by
  something checkable rather than by convention: `SHUTTLE_CREATE_STATS` bumps
  the layout **version** (old opener: `BAD_VERSION`), and
  `SHUTTLE_CREATE_ALIGNED_SPANS` changes `data_offset` **geometry** (old
  opener: `CORRUPT`). A flag that changes how bytes are laid out must never be
  left to the ignore rule.

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
