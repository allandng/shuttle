# Shuttle Build Ledger

## Current objective: G0.1 — BLOCKED on environment (Docker not installed)

## Scheduled job: none (manually driven iterations so far)

## Gate status

| Gate | mac leg | linux leg | Verified by | Date | Notes |
|------|---------|-----------|-------------|------|-------|
| G0.1 | PENDING | PENDING | | | Empty test builds + runs clean under ASan, both legs |
| G0.2 | PENDING | PENDING | | | TSan preset links + runs empty test, both legs (linux: `setarch -R`) |
| G0.3 | PENDING | PENDING | | | 4 KB shm smoke in container with `--shm-size=512m` |
| G0.4 | PENDING | PENDING | | | Two-process pshared mutex+condvar smoke; mac leg is highest-risk unknown, gates Phase 4 |
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

## Environment verification (iteration zero, 2026-06-09)

| Check | Result |
|---|---|
| Xcode CLT | OK — `/Library/Developer/CommandLineTools`, Apple clang 17.0.0, target arm64-apple-darwin25.5.0 |
| cmake | OK — 4.3.1 |
| Docker | **MISSING** — no `docker` binary on PATH; no Docker Desktop in /Applications; no colima/podman/OrbStack alternatives found |
| glibc arm64 base image pull | NOT ATTEMPTED — no Docker runtime |

## Session notes (newest first)

- **2026-06-09 (iteration zero):** Read all docs. Verified environment: Xcode CLT and cmake OK; **Docker is not installed on this host** (not merely not running). Standing orders mandate STOP-and-tell-the-user when Docker is unavailable — the Linux leg can never be faked or skipped, and G0.1 cannot pass on both legs without it. Initialized git, created this ledger, ended the iteration with no Phase 0 code written (deliberate: every G0.x gate needs the Linux leg, so writing the CMake skeleton before the container runtime exists risks verifying against an assumption). **User action required:** install and start a Docker runtime (Docker Desktop for Mac, or colima/OrbStack — anything providing a `docker` CLI that runs linux/arm64 containers), able to `docker pull ubuntu:24.04` (glibc arm64, never Alpine) and `docker run --shm-size=512m`. Next iteration: re-verify Docker, pull base image, then begin Phase 0 work toward G0.1 (CMake skeleton with shuttle_core/shuttle_tests/shuttle_bench targets, platform.hpp seam, ASan+UBSan and separate TSan presets, Dockerfile, `make test-mac` / `make test-linux` single commands).
