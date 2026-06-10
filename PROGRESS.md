# Shuttle Build Ledger

## Current objective: G1.1 (Phase 0 complete)

## Scheduled job: id `2fdb3d70`, hourly at :23 (cron `23 * * * *`), created 2026-06-10, auto-expires 2026-06-17 (~13:45 ET)

Caveats: the job is **session-only** — it lives in the current Claude Code session and dies if that session exits (the scheduler declined the durable flag). It fires only while the session REPL is idle. Per hygiene rules: if expiry is within 24 h at the start of an iteration, re-register and update this line; if the driving session is gone, re-create the job; cancel it (CronDelete) when all gates are PASS.

## Gate status

| Gate | mac leg | linux leg | Verified by | Date | Notes |
|------|---------|-----------|-------------|------|-------|
| G0.1 | PASS | PASS | `make test-mac` / `make test-linux` (ctest 1/1 Passed, ASan+UBSan) | 2026-06-10 | mac leg required CLT 26.5 update — see decisions log |
| G0.2 | PASS | PASS | `make tsan-mac` / `make tsan-linux` (ctest 1/1 Passed under TSan) | 2026-06-10 | linux leg needed seccomp=unconfined for setarch — see decisions log |
| G0.3 | PASS | PASS | `make test-mac` / `make test-linux` (shuttle_shm_smoke: 4 KB + 128 MB page-touched) | 2026-06-10 | Negative control verified: same binary SIGBUSes in container at default 64 MB /dev/shm |
| G0.4 | PASS | PASS | `make test-mac`+`tsan-mac` / `make test-linux`+`tsan-linux` (shuttle_pshared_smoke) | 2026-06-10 | macOS pshared condvar park/wake WORKS on 26.5 — no os_sync_wait_on_address fallback needed. posix_spawn role-arg pattern per amendment; also clean under TSan both legs |
| G1.1 | PENDING | PENDING | | | Create/open, magic+version validation, version-mismatch error |
| G1.2 | PENDING | PENDING | | | FR-4 capacity validation error, no crash |
| G1.3 | PENDING | PENDING | | | Leak check: /dev/shm clean (linux); name not re-openable (both) |
| G2.1 | PENDING | PENDING | | | ≥100k random write/read pairs, byte-exact FIFO, invariants every op |
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

## Environment verification (iteration zero, 2026-06-09; Docker re-verified 2026-06-10)

| Check | Result |
|---|---|
| Xcode CLT | OK — `/Library/Developer/CommandLineTools`, Apple clang 17.0.0, target arm64-apple-darwin25.5.0 |
| cmake | OK — 4.3.1 |
| Docker | OK (2026-06-10) — Docker Desktop 29.5.3, server arch arm64 |
| glibc arm64 base image pull | OK (2026-06-10) — `ubuntu:24.04` pulls and runs natively: `uname -m` = aarch64, glibc 2.39 |

## Session notes (newest first)

- **2026-06-10 (iterations 2–4 — G0.2, G0.3, G0.4 all PASS; PHASE 0 COMPLETE):**
  - **G0.2:** TSan legs pass. Container hiccup: Docker's default seccomp blocks `personality(2)`, so `setarch -R` failed; fixed with `--security-opt seccomp=unconfined` on the TSan target only (see decisions log — Docker Desktop's kernel has `mmap_rnd_bits=18` so TSan happens to work without setarch, but we keep the workaround functional for stock-Ubuntu kernels).
  - **G0.3:** `shuttle_shm_smoke` maps/touches/unmaps 4 KB and 128 MB segments on both legs. Negative control verified: the same binary SIGBUSes in a container with the default 64 MB /dev/shm, proving the test actually guards the `--shm-size` gotcha.
  - **G0.4:** `shuttle_pshared_smoke` — driver posix_spawn's the binary as `waiter`/`signaler` (fork-without-exec forbidden per amendment); waiter parks on a pshared condvar in shm, signaler wakes it; per-run nonce; every wait deadlined (timeout=failure; ctest TIMEOUT 60 as backstop). **PASS on macOS 26.5 natively** — the highest-risk Phase 0 unknown resolved positively; pshared mutex+condvar work cross-process on this host, no `os_sync_wait_on_address` fallback needed. Also PASS in container, and clean under TSan on both legs. `platform.hpp` gained its first real seam functions: `mutex_init_pshared`, `cond_init_pshared_monotonic` (CLOCK_MONOTONIC on Linux), `cond_timedwait_rel` (timedwait vs `pthread_cond_timedwait_relative_np`), `monotonic_ns`.
  - Next objective: **G1.1** — Phase 1 segment lifecycle: header struct with static_asserts, offsets-not-pointers discipline, create/open/close/unlink, magic+version validation, single-init publication, short-name + one-shot-ftruncate enforcement in the platform seam, `shuttle_inspect` CLI.

- **2026-06-10 (iteration 1 — G0.1 PASS both legs):** Docker now installed and verified (pull + native arm64 run of ubuntu:24.04). Built the Phase 0 skeleton: `CMakeLists.txt` (C++17, three targets `shuttle_core`/`shuttle_tests`/`shuttle_bench`, core free of test/bench deps, `SHUTTLE_SAN` sanitizer config), `include/shuttle/platform.hpp` (the single platform seam, stub), empty test + bench stub, `docker/Dockerfile` (ubuntu:24.04 glibc arm64), `Makefile` with one-command legs `test-mac`/`test-linux`/`tsan-mac`/`tsan-linux` (tsan targets wired but NOT yet verified — that is G0.2). Linux leg passed first try. Mac leg initially HUNG: ASan runtime spin pre-`main` (see decisions log) — fixed by CLT 26.5 update, then `ctest` 1/1 Passed under ASan+UBSan. Both legs re-verified on the final tree. Next objective: **G0.2** — TSan preset links and runs the empty test on both legs (`make tsan-mac`, `make tsan-linux`; linux leg already wired with `setarch -R`).

- **2026-06-09 (iteration zero):** Read all docs. Verified environment: Xcode CLT and cmake OK; **Docker is not installed on this host** (not merely not running). Standing orders mandate STOP-and-tell-the-user when Docker is unavailable — the Linux leg can never be faked or skipped, and G0.1 cannot pass on both legs without it. Initialized git, created this ledger, ended the iteration with no Phase 0 code written (deliberate: every G0.x gate needs the Linux leg, so writing the CMake skeleton before the container runtime exists risks verifying against an assumption). **User action required:** install and start a Docker runtime (Docker Desktop for Mac, or colima/OrbStack — anything providing a `docker` CLI that runs linux/arm64 containers), able to `docker pull ubuntu:24.04` (glibc arm64, never Alpine) and `docker run --shm-size=512m`. Next iteration: re-verify Docker, pull base image, then begin Phase 0 work toward G0.1 (CMake skeleton with shuttle_core/shuttle_tests/shuttle_bench targets, platform.hpp seam, ASan+UBSan and separate TSan presets, Dockerfile, `make test-mac` / `make test-linux` single commands).
