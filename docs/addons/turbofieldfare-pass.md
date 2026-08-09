# Addon plan: TurboFieldfare-inspired feature pass

> **Status:** Approved (scope fixed by the commissioning prompt; fence confirmed there)
> **Owner:** orchestrator session (claude-fable-5), implementation on claude-opus-5
> **Updated:** 2026-08-08
> **Related:** docs/API.md, docs/ROADMAP.md, CONTRIBUTING.md

Extends Shuttle's zero-copy contract in three directions a
TurboFieldfare-style runtime (small resident core, weights streamed from disk
in kernel-ready layout) exposes: to the GPU boundary (page-aligned spans), to
disk-backed segments (file-backed channels), and to pipelined consumption
(prefetch + peek). Docs pass at the end.

## Evidence ledger

VERIFIED (read directly in this session):

- Borrow semantics are strictly release-before-acquire.
  `include/shuttle/spsc.hpp` lines 409-428: `Consumer::try_read` parses at the
  un-advanced `read` cursor; `read` moves only in `release()` (lines 446-453).
  `src/shuttle_c.cpp` lines 85-97: `ensure_borrow` reuses the active borrow.
  A consumer holding message N cannot acquire N+1 today. Decides P3: peek is
  a new API, not documentation.
- Backend seam exists and was built for this. `include/shuttle/platform.hpp`:
  `SegBacking` enum (`kShm`, `kHugeTLB2M`, `kHugeTLB1G`) with
  `seg_create/seg_open/seg_size/seg_map/seg_unmap/seg_close/seg_unlink/seg_map_len`;
  adding `kFile` is an enum value plus branches in the seam, no signature moves.
- Flags namespace: `include/shuttle/header.hpp` — 0x1 THP advisory, 0x2/0x4
  hugetlb, 0x8 stats (implies layout v2). Next free bits: 0x10, 0x20. Known-bits
  mask lives in `shuttle::create` (`src/shuttle.cpp`), currently 0xF.
- Geometry validation is centralized: `validate_header(base, map_len)` in
  `src/shuttle.cpp`, overflow-safe subtraction form, fuzzed by
  `fuzz/header_fuzz.cpp` whose oracle recomputes geometry independently.
  Any geometry rule change must update BOTH.
- `data_offset` is 1280 (v1) / 1536 (v2) — NOT page-aligned. Aligned-span
  segments need a page-rounded data_offset, so `validate_header` must accept
  a flags-dependent offset (see P1 design).
- Framing: 8-byte little-endian length header immediately before payload
  (`include/shuttle/bipbuffer.hpp`, `kFrameHeader = 8`); consumer parses at
  `read` (`spsc.hpp` `parse()`). Payload today lands at `read + 8` — never
  page-aligned. Aligned mode therefore changes the frame STRIDE, and parse
  must know (P1 design below).
- Error codes -1..-15 assigned; per-op flags 0x1 NONBLOCK, 0x2 DROP_NEWEST;
  positive return 1 = SHUTTLE_DROPPED (`include/shuttle/shuttle_c.h`).
- Bindings lag the ABI: `bindings/python/shuttle_ipc/_ffi.py` and
  `bindings/rust/shuttle-sys/src/lib.rs` carry the v1.1 surface only (no
  stats/hugetlb/drop symbols). The per-phase "bindings track the ABI"
  invariant requires a catch-up, scheduled in P1.
- Crash story today: heartbeat liveness primary on both platforms; Linux adds
  robust pshared mutex EOWNERDEAD recovery; SIGKILL tests at two kill points
  (`tests/crash_heartbeat_test.cpp`, `tests/crash_mutex_test.cpp`).

INFERRED (hedged; implementing agent must verify):

- Robust pthread mutexes appear to work identically in a file-backed
  MAP_SHARED mapping (the robust list is address-based, not shm-specific);
  P2's crash tests must prove it, not assume it.
- `os_sync_wait_on_address` on macOS appears backing-agnostic; the mac CI leg
  is the proof point for file-backed parking there.

UNKNOWN:

- Q-01: does the in-flight Windows backend (WP8, running now) land before P1
  dispatch? P1 is blocked on it either way — it owns platform.hpp/header.hpp.
  Answered by: this session, before P1 dispatch.

## Scope fence

IN SCOPE
  1. P1 `SHUTTLE_CREATE_ALIGNED_SPANS` (0x10): page-aligned payload spans.
  2. P2 file-backed channels: `SegBacking::kFile`, `shuttle_create_file` /
     `shuttle_open_file` / `shuttle_unlink_file` (path-typed symbols; v1
     name-typed symbols untouched), flag 0x20 persisted informational.
  3. P3 `MADV_WILLNEED` prefetch of committed-but-unread region (advisory,
     seam-gated, zero-cost on default path) + `shuttle_peek_next`.
  4. P4 docs: `docs/EXPERIMENTS.md` (new file; dated, labeled measurements), worked
     KV-cache handoff example, README/API.md/ROADMAP.md/CHANGELOG updates.
  5. Bindings catch-up to the full current ABI (v1.2/v1.3 debt) in P1, then
     per-phase parity for every new symbol.

OUT OF SCOPE
  - Overwrite-oldest backpressure (already rejected in ROADMAP) — revisit v2.
  - Durability guarantees on file-backed segments (msync is advisory,
    explicit non-goal) — revisit if a persistence use case lands.
  - GPU code of any kind (the CUDA module is separate and untouched).
  - Any second writer to any cursor; any change to v1 symbol signatures.
  - Windows implementations of the new backings/APIs beyond compiling
    (aligned spans use the same portable logic; kFile Windows backend is a
    stub returning ENOTSUP-equivalent this pass) — revisit with WP8 parity.

MAY TOUCH (allowlist; per-phase subsets in the phase contracts)
  include/shuttle/header.hpp        - flag bits 0x10/0x20; aligned data_offset note
  include/shuttle/platform.hpp      - SegBacking::kFile, advise_willneed, page_size
  include/shuttle/bipbuffer.hpp     - aligned reservation/parse stride (P1)
  include/shuttle/spsc.hpp          - aligned parse, peek_next, prefetch hook
  include/shuttle/shuttle.hpp       - C++ mirrors (create_file/open_file, peek)
  src/shuttle.cpp                   - create/open/validate_header flag logic
  include/shuttle/shuttle_c.h       - additive v1.4 symbols/defines
  src/shuttle_c.cpp                 - impls + static_assert mirrors
  bindings/python/shuttle_ipc/*.py  - ABI catch-up + new symbols
  bindings/python/tests/*.py        - coverage
  bindings/rust/shuttle-sys/src/lib.rs, bindings/rust/shuttle/src/*.rs,
  bindings/rust/shuttle/tests/*.rs  - ABI catch-up + new symbols
  fuzz/header_fuzz.cpp              - oracle update for flags-dependent geometry
  tests/hugepage_test.cpp           - known-bits mask probe update only
  CMakeLists.txt                    - new test targets only
  docs/API.md, docs/ROADMAP.md, README.md, CHANGELOG.md, docs/EXPERIMENTS.md (new)
  tests/aligned_spans_test.cpp, tests/filebacked_test.cpp,
  tests/peek_prefetch_test.cpp      - new
  Nothing outside this list. A phase needing another file stops and amends
  this plan first.

WILL NOT
  - add any dependency (none exist today; none arrive)
  - rename or move existing symbols or files
  - reformat code outside the diff (CONTRIBUTING rule)
  - change the meaning of any existing error code or flag bit
  - introduce a second writer to write/watermark/read (SPSC invariant)
  - touch tests/ffi/* (frozen ABI conformance layer)
  - alter existing tests except the named mask-probe extension

## Key designs (the parts that must not be improvised)

P1 aligned spans. Alignment unit = system page (`sysconf(_SC_PAGESIZE)`;
4096 on every supported target; same-host IPC makes it a host constant —
document). Frame layout in aligned mode: header page + rounded payload:
`[8B length header][pad to page]` then `[payload][pad to page]`. Payload
therefore starts at frame_start + page, page-aligned whenever frame_start is
page-aligned and the data region starts page-aligned. Consequences, all
mandatory: (a) `data_offset` becomes `round_up(kDataOffsetVx, page)` when the
flag is set — `validate_header` accepts the flags-dependent offset and the
fuzz oracle mirrors it; (b) BipBuffer reservation in aligned mode rounds every
reserve to `page + round_up(len, page)` so frame starts stay page-aligned
across wraps (the never-straddle invariant is untouched: rounding only grows
the contiguous block); (c) `parse()` reads the header at `read`, payload at
`read + page`, `borrowed_ = page + round_up(len, page)`; (d) FR-4 capacity
check uses the aligned stride. Worst-case internal fragmentation, stated as a
number: `2*page - 8 - 1` bytes per message (4088 header-page waste + up to
4095 payload rounding at page=4096) = at most 8183 bytes; 0.016% of a 50 MB
payload. Mode is a channel property: creator sets 0x10, opener reads it from
flags and configures its Consumer identically — both sides branch once at
construction, zero cost when off.

P2 file-backed. `SegBacking::kFile`; the path IS the identifier — new
path-typed symbols rather than overloading shm names (rejected alternative:
`SHUTTLE_CREATE_FILEBACKED` on `create_ex` reusing `name` as a path — it
collides with shm-name rules and recreates the dual-namespace ambiguity the
hugetlb work had to document). `seg_create(kFile)`: `open(path,
O_CREAT|O_EXCL|O_RDWR, 0600)` + one-shot `ftruncate`; `seg_map` unchanged.
Crash story, re-argued not assumed: heartbeats live in the mapping and work
identically; Linux robust mutex must be PROVEN on a file mapping by running
both SIGKILL kill-point tests against a file-backed channel; a file (unlike
shm on reboot) can be a STALE segment from a previous boot — open() already
handles arbitrary garbage via validate_header + init-spin timeout, and
create() refuses an existing file (EEXIST), which is the documented recovery
path (unlink stale file, recreate). msync: advisory only, never called by the
library; durability is an explicit non-goal (document in API.md). Capacity may
exceed RAM; the page cache handles residency — the memory-ceiling test proves
streaming 2x a constrained ceiling works (container leg).

P3 prefetch + peek. `advise_willneed(addr, len)` in the seam
(`posix_madvise(POSIX_MADV_WILLNEED)` / macOS `madvise(MADV_WILLNEED)`;
no-op stub on Windows). Called from the consumer only, on the
committed-but-unread region, at two points: immediately before parking and on
successful acquire (prefetch the region beyond the borrow). Gated by a bool
resolved at Consumer construction — true only for kFile-backed segments (flag
0x20) — so the default path pays one predictable branch (provably zero-cost:
the existing perf tests are the guard). `shuttle_peek_next(ch, size_t*
len_out)`: with 0 or 1 borrows outstanding, reports whether the NEXT
un-borrowed message is committed and its payload length. Read-only: computes
at `read + borrowed_` against `write`/`watermark` with the same P1/P2 acquire
edges `try_read` uses; stores nothing, so no new writer and no new
happens-before obligation beyond a documented reuse of the existing edges —
extend the spsc.hpp contract comment inline, same style. Returns OK |
WOULD_BLOCK | INVALID_ARGS(null args). Wrap subtlety the implementation must
handle: at `read + borrowed_ == watermark` with `write < read`, the next
message is at offset 0 (do NOT perform the handoff store from peek — peek
must not write `read`; it reports the wrapped message's length by reading at
0 without moving anything).

## Blast radius (actionable items only)

- `validate_header` + `fuzz/header_fuzz.cpp` oracle change together (P1) —
  the fuzzer is the regression net for exactly this file pair.
- Known-bits mask 0xF -> 0x3F: `tests/hugepage_test.cpp` mask probe asserts
  today's mask; extend it, do not weaken it (P1).
- Aligned mode changes on-disk framing INSIDE a segment but only when 0x10 is
  set at create; default segments are byte-identical. Old binaries opening an
  aligned segment: flags contract says unknown bits are ignored — but 0x10
  changes framing, which an ignorer would misparse. RESOLUTION (mandatory):
  like kFlagStats, 0x10 gates on something version-checked — an aligned
  segment writes data_offset rounded to page, which an old binary REJECTS at
  the geometry check (offset mismatch -> kErrCorrupt). Verify this rejection
  in the P1 test and document the deliberate choice (corrupt-not-badversion is
  acceptable: the offset genuinely disagrees with the old layout rule).
- File-backed + SIGKILL tests double the crash matrix; keep runtime bounded.
- Bindings catch-up touches the Python guard class — its 28 tests are the net.
- Rollback: every phase is a commit; each reverts independently (new flags
  are opt-in, new symbols additive, docs last).

## Phases

P-01 aligned spans
  Does        0x10 flag end-to-end: create/open/validate, BipBuffer stride,
              parse, C ABI, bindings (incl. v1.2/v1.3 catch-up), tests
              (>=10k-op alignment property incl. wrap-heavy, flag-off
              negative, byte-exact FIFO stress under flag, old-binary
              rejection), fuzz oracle, mask probe.
  Touches     header.hpp, bipbuffer.hpp, spsc.hpp, shuttle.hpp, shuttle.cpp,
              shuttle_c.h/.cpp, bindings/*, fuzz/header_fuzz.cpp,
              tests/hugepage_test.cpp, tests/aligned_spans_test.cpp (new),
              CMakeLists.txt, docs/API.md
  Verified by ASan+TSan full suite green; alignment property test; bindings
              pytest+cargo green
  Reverts by  reverting the commit (flag opt-in, nothing default changes)
  Blocked by  WP8 (Windows backend) landing — same files

P-02 file-backed channels
  Does        SegBacking::kFile; shuttle_create_file/open_file/unlink_file
              (+C++ mirrors); flag 0x20; crash story re-argued in code+docs;
              tests: roundtrip both platforms (CI), streamed-bytes > memory
              ceiling, SIGKILL at both kill points on file-backed.
  Touches     platform.hpp, shuttle.hpp, shuttle.cpp, shuttle_c.h/.cpp,
              bindings/*, tests/filebacked_test.cpp (new), CMakeLists.txt,
              docs/API.md
  Verified by ASan+TSan suites; the file-backed crash tests
  Reverts by  reverting the commit
  Blocked by  P-01 (shared files, serial by contract)

P-03 prefetch + peek
  Does        advise_willneed seam + consumer hooks (kFile-gated);
              shuttle_peek_next + C++ peek_next + bindings; spsc.hpp contract
              comment extension; trickle-variant test with peek in the loop
              proving no lost wakeups; TSan clean.
  Touches     platform.hpp, spsc.hpp, shuttle.hpp (if needed), shuttle_c.h/.cpp,
              bindings/*, tests/peek_prefetch_test.cpp (new), CMakeLists.txt,
              docs/API.md
  Verified by ASan+TSan suites incl. new trickle variant
  Reverts by  reverting the commit
  Blocked by  P-02

P-04 docs + experiments
  Does        docs/EXPERIMENTS.md (numbered, dated: bench rerun; aligned vs
              unaligned throughput; file-backed vs shm latency; prefetch
              on/off cold-cache — all labeled virtualized); KV-cache worked
              example; README/API.md/ROADMAP.md/CHANGELOG sweep (incl. the
              outstanding Unreleased backfill for this whole branch).
  Touches     docs/EXPERIMENTS.md (new), README.md, docs/API.md,
              docs/ROADMAP.md, CHANGELOG.md
  Verified by prose review; numbers reproduced from actual runs
  Reverts by  reverting the commit
  Blocked by  P-03

## NOTICED (logged, not fixed here)

- docs/API.md vs implementation: second `shuttle_acquire_read` is documented
  INVALID_ARGS but implemented idempotent (found during bindings work).
  Owner decision needed; P3's peek makes the idempotent behavior MORE useful,
  so recommend documenting idempotence rather than changing code — but that
  is a doc decision for P4, flagged not assumed.
