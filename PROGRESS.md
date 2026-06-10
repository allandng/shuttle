# Shuttle Build Ledger

## Current objective: G2.2

## Scheduled job: id `2fdb3d70`, hourly at :23 (cron `23 * * * *`), created 2026-06-10, auto-expires 2026-06-17 (~13:45 ET)

Caveats: the job is **session-only** — it lives in the current Claude Code session and dies if that session exits (the scheduler declined the durable flag). It fires only while the session REPL is idle. Per hygiene rules: if expiry is within 24 h at the start of an iteration, re-register and update this line; if the driving session is gone, re-create the job; cancel it (CronDelete) when all gates are PASS.

## Gate status

| Gate | mac leg | linux leg | Verified by | Date | Notes |
|------|---------|-----------|-------------|------|-------|
| G0.1 | PASS | PASS | `make test-mac` / `make test-linux` (ctest 1/1 Passed, ASan+UBSan) | 2026-06-10 | mac leg required CLT 26.5 update — see decisions log |
| G0.2 | PASS | PASS | `make tsan-mac` / `make tsan-linux` (ctest 1/1 Passed under TSan) | 2026-06-10 | linux leg needed seccomp=unconfined for setarch — see decisions log |
| G0.3 | PASS | PASS | `make test-mac` / `make test-linux` (shuttle_shm_smoke: 4 KB + 128 MB page-touched) | 2026-06-10 | Negative control verified: same binary SIGBUSes in container at default 64 MB /dev/shm |
| G0.4 | PASS | PASS | `make test-mac`+`tsan-mac` / `make test-linux`+`tsan-linux` (shuttle_pshared_smoke) | 2026-06-10 | macOS pshared condvar park/wake WORKS on 26.5 — no os_sync_wait_on_address fallback needed. posix_spawn role-arg pattern per amendment; also clean under TSan both legs |
| G1.1 | PASS | PASS | `make test-mac`+`tsan-mac` / `make test-linux`+`tsan-linux` (shuttle_lifecycle_test) | 2026-06-10 | Driver creates; spawned child opens + verifies magic/version; bumped version → distinct kErrBadVersion. macOS rounds shm st_size to page size — see decisions log |
| G1.2 | PASS | PASS | `make test-mac`+`tsan-mac` / `make test-linux`+`tsan-linux` (shuttle_capacity_test) | 2026-06-10 | Three too-small shapes → kErrCapacityTooSmall; failed create leaves no object; boundary cap == maxp+8 accepted |
| G1.3 | PASS | PASS | `make test-mac`+`tsan-mac` / `make test-linux`+`tsan-linux` (shuttle_leak_test) | 2026-06-10 | Linux: object visible in /dev/shm while alive, gone after unlink. Both: survives close (FR-5), unlinked name → kErrNotFound, double unlink distinct |
| G2.1 | PASS | PASS | `make test-mac`+`tsan-mac` / `make test-linux`+`tsan-linux` (shuttle_bipbuffer_test) | 2026-06-10 | 2×100k pairs (roomy 64KB: 1496 wraps; tight 4KB: 19267 wraps), byte-exact FIFO, size/cursor invariants after every op |
| G2.2 | PENDING | PENDING | | | Edge cases: exact-fill after A, forced early wrap, max-size payload |
| G2.3 | PENDING | PENDING | | | Oversized write fails fast |
| G3.1 | PENDING | PENDING | | | ≥1 GB two-process byte-exact FIFO; TSan-clean on single-process dual-thread config (A2) |
| G3.2 | PENDING | PENDING | | | Asymmetric-speed stress both directions |
| G3.3 | PENDING | PENDING | | | Wrap-heavy A→B handoff stress |
| G4.1 | PENDING | PENDING | | | Idle blocked peer ~0% CPU |
| G4.2 | PENDING | PENDING | | | Trickle stress ≥100k messages, no lost/extra wakeups |
| G4.3 | PENDING | PENDING | | | µs-scale p99 wake latency; zero mutex touches when peer not parked |
| G5.1 | PENDING | PENDING | | | SIGKILL mid-reservation → heartbeat-staleness abort (both platforms, per A3) |
| G5.2 | N/A | PENDING | | | Linux robust mutex: EOWNERDEAD → repair → consistent → unlock |
| G5.3 | PENDING | PENDING | | | No shm leak after crashed run once survivor tears down |
| G5.4 | PENDING | PENDING | | | Kill-while-holding-park-mutex: robust recovery (linux) / trylock-loop escape (mac) |
| G6.1 | PENDING | PENDING | | | C++ producer ↔ Python consumer, byte-exact borrow path |
| G6.2 | PENDING | PENDING | | | C++ producer ↔ Rust consumer; use-after-release fails to compile |
| G6.3 | PENDING | PENDING | | | Induced error → correct integer code in all three languages |
| G7.1 | PENDING | PENDING | | | 50 MB ≥10× vs HTTP AND vs UDS baselines; Docker numbers labeled "virtualized — not headline"; headline NFR-P1 provisional until bare-metal Linux |
| G7.2 | PENDING | PENDING | | | Profiler: negligible copy/serialize CPU on borrow path |
| G7.3 | PENDING | PENDING | | | µs-scale wake latency under load, consistent with G4.3 |

## Decisions log

- 2026-06-09 — Repo `git init` (branch `main`) at `/Users/allannguyen/shuttle`; `.gitignore` covers `.DS_Store` and `build*/`. Removed an empty stray `shuttle/docs/` directory that predated the repo.
- 2026-06-09 — Design amendments A1–A4 + minor amendments in `docs/SHUTTLE_AGENT_PROMPT.md` are binding and override the SRS/plan where they conflict (cursor model = read/write/watermark; TSan gates restructured per A2; heartbeat+timedwait primary on both platforms; seq_cst parking protocol; CLOCK_MONOTONIC condvars; 128-byte padding for hot atomics; G0.4 and G5.4 added; UDS baseline added to Phase 7).

- 2026-06-10 — **Host-wide install: Command Line Tools for Xcode 26.5** (via `softwareupdate -i`). Reason: the prior CLT (Apple clang 17, clang-1700.4.4.1) shipped an ASan runtime incompatible with macOS 26.5 — ANY ASan-instrumented binary (even a one-line C `main`) spun forever pre-`main` inside `wrap_malloc_default_zone` ← `__malloc_init` (confirmed by `sample`; `MallocNanoZone=0` did not help). CLT 26.5 (Apple clang 21, clang-2100.1.1.101) fixes it. If macOS updates again and ASan binaries start hanging at startup with 100% CPU, suspect this same runtime/OS mismatch first and check `softwareupdate --list` for a newer CLT.
- 2026-06-10 — "Sanitizer presets" realized as Makefile targets + a `SHUTTLE_SAN=off|asan|tsan` CMake cache var with separate build trees (`build/{mac,linux}-{asan,tsan}`), not CMakePresets.json — one command per leg either way, fewer moving parts. ASan build includes UBSan per plan; TSan strictly separate (cannot link both).
- 2026-06-10 — Linux leg = `ubuntu:24.04` glibc 2.39 arm64 image (`docker/Dockerfile`, built as `shuttle-linux-dev`), run with `--shm-size=512m`, repo bind-mounted at `/work`. `tsan-linux` target pre-wires the `setarch -R` ASLR workaround.

- 2026-06-10 — **Amendment to docs/SHUTTLE_AGENT_PROMPT.md (user-directed, binding):** the G0.4 smoke test and every other multi-process test must launch processes via fork+exec or `posix_spawn` — the driver runs the test binary twice with role arguments. Plain fork without exec is forbidden: TSan on macOS does not support fork-without-exec and the child inherits a broken runtime.

- 2026-06-10 — **TSan linux leg runs with `--security-opt seccomp=unconfined`.** Docker's default seccomp profile blocks `personality(2)` with ADDR_NO_RANDOMIZE, so the pre-approved `setarch -R` workaround failed with "Operation not permitted". Note: on Docker Desktop's LinuxKit kernel (`vm.mmap_rnd_bits=18`) gcc-13 TSan happens to work even without `setarch`, but we keep the documented workaround functional (via unconfined seccomp on the TSan target only) so the harness also works on stock Ubuntu kernels (`mmap_rnd_bits=32`) where TSan crashes at startup without it. Dev container running our own code; acceptable.

- 2026-06-10 — **macOS rounds shm object `st_size` up to page size (16 KB on Apple Silicon).** An opener's `fstat` can legitimately see a larger size than the creator's exact `ftruncate` length. Open-time geometry validation (NFR-S2) therefore checks the mapping *covers* the claimed `data_offset + data_capacity` (`>=`), never equality. Any future code deriving capacity from `st_size` instead of the header would be wrong on macOS.

## Environment verification (iteration zero, 2026-06-09; Docker re-verified 2026-06-10)

| Check | Result |
|---|---|
| Xcode CLT | OK — `/Library/Developer/CommandLineTools`, Apple clang 17.0.0, target arm64-apple-darwin25.5.0 |
| cmake | OK — 4.3.1 |
| Docker | OK (2026-06-10) — Docker Desktop 29.5.3, server arch arm64 |
| glibc arm64 base image pull | OK (2026-06-10) — `ubuntu:24.04` pulls and runs natively: `uname -m` = aarch64, glibc 2.39 |

## Session notes (newest first)

- **2026-06-10 (iteration 8 — G2.1 PASS both legs):** Phase 2 BipBuffer landed in `include/shuttle/bipbuffer.hpp`: pure single-threaded logic, amendment-A1 cursor model (read/write/watermark absolute offsets, single-writer each; regions A/B derived only; reserve state producer-private). Key encoded rules: strict `write < read` in all wrapped-space checks (wrapped-full must never alias linear-empty `write == read`), whole-unit early wrap to offset 0, A→B handoff = consumer storing `read = 0` on observing `read == watermark && write < read`, explicit little-endian u64 framing. `kFrameHeader` moved from header.hpp into bipbuffer.hpp (constant relocation only; header.hpp now includes bipbuffer.hpp). `tests/bipbuffer_test.cpp`: model-based property test, payload bytes regenerated from msg index (nothing stored), 100k pairs roomy (64 KB cap, 1496 wraps) + 100k pairs tight (4 KB cap, 19267 wraps), invariants (cursors ≤ cap; wrapped ⇒ read ≤ watermark; derived size == modeled in-flight bytes) after every op, byte-exact FIFO. 7/7 under ASan+TSan, both legs. Next objective: **G2.2** — the three named edge cases: payload exactly filling the space after A; forced early wrap to B; max-size payload.

- **2026-06-10 (iteration 7 — G1.3 PASS both legs; PHASE 1 COMPLETE):** Added `shm_object_exists_fs` to the platform seam (Linux: stat of /dev/shm/<name>; macOS: -1 = no filesystem view, callers fall back to behavioral proof) and `tests/leak_test.cpp`: live object visible in /dev/shm (ground truth the check can fail), survives close() per FR-5, gone from /dev/shm after unlink, not re-openable (kErrNotFound) on both platforms, double unlink reports kErrNotFound. 6/6 tests pass under ASan and TSan on both legs. Phase 1 done: lifecycle, header, validation, leak hygiene all gated. Next objective: **G2.1** — Phase 2 BipBuffer logic, single-threaded over a heap buffer, NO shared memory: reserve/commit/read_block/release with the amendment-A1 read/write/watermark invariant (regions A/B strictly derived, never stored), early-wrap rule, 8-byte framing; gate = byte-exact FIFO over ≥100k random write/read pairs with invariants asserted after every op. This is the phase for heavy property testing — invest in the fuzz harness, it is what de-risks Phase 3.

- **2026-06-10 (iteration 6 — G1.2 PASS both legs):** Added `tests/capacity_test.cpp` (single-process; FR-4 needs no second process): capacities of maxp+7, maxp, and 1 against a 64 KB max_payload all fail with the distinct kErrCapacityTooSmall; a failed create leaves no shm object behind (open afterward → kErrNotFound); the exact boundary capacity == max_payload + kFrameHeader succeeds, then closes and unlinks cleanly. No library-code changes needed — the validation landed with G1.1. 5/5 tests pass under ASan and TSan on both legs. Next objective: **G1.3** — leak check (NFR-R2): after create→close→unlink, /dev/shm is clean on Linux and the name cannot be re-opened on either platform.

- **2026-06-10 (iteration 5 — G1.1 PASS both legs):** Phase 1 lifecycle landed: `include/shuttle/header.hpp` (full ChannelHeader per amendment A1 — read/write/watermark single-writer cursors, parking flags, heartbeats, pshared primitives; every hot atomic on its own 128-byte line, offsets static_asserted, layout change = version bump), `include/shuttle/shuttle.hpp` (Err codes + create/open/close/unlink), `src/shuttle.cpp` (O_CREAT|O_EXCL + one-shot ftruncate + mmap + init, release-publish of init_state; opener acquire-spins with 5 s deadline → kErrInitTimeout; magic then version then geometry validation with distinct errors), `shm_name_ok` in the platform seam (30-char macOS cap), `tools/inspect.cpp` (shuttle_inspect header-dump CLI), `tests/proc_util.hpp` (posix_spawn child helper per fork-ban amendment), `tests/lifecycle_test.cpp` (the G1.1 test). One real bug found by the mac leg: geometry equality check vs macOS page-size-rounded `st_size` (decisions log). All 4 tests pass under ASan and TSan on both legs. Next objective: **G1.2** — `create` with `capacity < max_payload + 8` fails with the FR-4 error code, not a crash (validation already implemented in create(); needs its gate test).

- **2026-06-10 (iterations 2–4 — G0.2, G0.3, G0.4 all PASS; PHASE 0 COMPLETE):**
  - **G0.2:** TSan legs pass. Container hiccup: Docker's default seccomp blocks `personality(2)`, so `setarch -R` failed; fixed with `--security-opt seccomp=unconfined` on the TSan target only (see decisions log — Docker Desktop's kernel has `mmap_rnd_bits=18` so TSan happens to work without setarch, but we keep the workaround functional for stock-Ubuntu kernels).
  - **G0.3:** `shuttle_shm_smoke` maps/touches/unmaps 4 KB and 128 MB segments on both legs. Negative control verified: the same binary SIGBUSes in a container with the default 64 MB /dev/shm, proving the test actually guards the `--shm-size` gotcha.
  - **G0.4:** `shuttle_pshared_smoke` — driver posix_spawn's the binary as `waiter`/`signaler` (fork-without-exec forbidden per amendment); waiter parks on a pshared condvar in shm, signaler wakes it; per-run nonce; every wait deadlined (timeout=failure; ctest TIMEOUT 60 as backstop). **PASS on macOS 26.5 natively** — the highest-risk Phase 0 unknown resolved positively; pshared mutex+condvar work cross-process on this host, no `os_sync_wait_on_address` fallback needed. Also PASS in container, and clean under TSan on both legs. `platform.hpp` gained its first real seam functions: `mutex_init_pshared`, `cond_init_pshared_monotonic` (CLOCK_MONOTONIC on Linux), `cond_timedwait_rel` (timedwait vs `pthread_cond_timedwait_relative_np`), `monotonic_ns`.
  - Next objective: **G1.1** — Phase 1 segment lifecycle: header struct with static_asserts, offsets-not-pointers discipline, create/open/close/unlink, magic+version validation, single-init publication, short-name + one-shot-ftruncate enforcement in the platform seam, `shuttle_inspect` CLI.

- **2026-06-10 (iteration 1 — G0.1 PASS both legs):** Docker now installed and verified (pull + native arm64 run of ubuntu:24.04). Built the Phase 0 skeleton: `CMakeLists.txt` (C++17, three targets `shuttle_core`/`shuttle_tests`/`shuttle_bench`, core free of test/bench deps, `SHUTTLE_SAN` sanitizer config), `include/shuttle/platform.hpp` (the single platform seam, stub), empty test + bench stub, `docker/Dockerfile` (ubuntu:24.04 glibc arm64), `Makefile` with one-command legs `test-mac`/`test-linux`/`tsan-mac`/`tsan-linux` (tsan targets wired but NOT yet verified — that is G0.2). Linux leg passed first try. Mac leg initially HUNG: ASan runtime spin pre-`main` (see decisions log) — fixed by CLT 26.5 update, then `ctest` 1/1 Passed under ASan+UBSan. Both legs re-verified on the final tree. Next objective: **G0.2** — TSan preset links and runs the empty test on both legs (`make tsan-mac`, `make tsan-linux`; linux leg already wired with `setarch -R`).

- **2026-06-09 (iteration zero):** Read all docs. Verified environment: Xcode CLT and cmake OK; **Docker is not installed on this host** (not merely not running). Standing orders mandate STOP-and-tell-the-user when Docker is unavailable — the Linux leg can never be faked or skipped, and G0.1 cannot pass on both legs without it. Initialized git, created this ledger, ended the iteration with no Phase 0 code written (deliberate: every G0.x gate needs the Linux leg, so writing the CMake skeleton before the container runtime exists risks verifying against an assumption). **User action required:** install and start a Docker runtime (Docker Desktop for Mac, or colima/OrbStack — anything providing a `docker` CLI that runs linux/arm64 containers), able to `docker pull ubuntu:24.04` (glibc arm64, never Alpine) and `docker run --shm-size=512m`. Next iteration: re-verify Docker, pull base image, then begin Phase 0 work toward G0.1 (CMake skeleton with shuttle_core/shuttle_tests/shuttle_bench targets, platform.hpp seam, ASan+UBSan and separate TSan presets, Dockerfile, `make test-mac` / `make test-linux` single commands).
