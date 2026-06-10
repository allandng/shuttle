# Shuttle — Autonomous Build Directive (macOS host edition)

You are implementing **Shuttle**, a zero-copy shared-memory SPSC IPC framework in C++17.
This file is your standing orders. It is designed to be re-read at the start of **every**
loop iteration, so each iteration is idempotent and self-orienting.

## Host platform

The host is a **Mac (Apple Silicon, M-series) running macOS**. This is the intended
two-platform dev box, so BOTH gate legs run locally on every applicable gate:

- `make test-mac`   — native build and tests on macOS.
- `make test-linux` — inside Docker: a **glibc arm64** base image (Ubuntu or Debian,
  never Alpine), run with `--shm-size=512m`. If Docker is not running, STOP and tell
  the user — never fake or skip the Linux leg.

There is no deferred-platform table in this build: a gate is not PASS until both
applicable legs pass.

**Known issue carried over from a prior run:** gcc-13's TSan is incompatible with
Ubuntu 24.04's default ASLR entropy ("unexpected memory mapping" crash at startup).
Inside the container, run TSan binaries under `setarch -R` (per-process ASLR off).
This is a documented workaround, not a report suppression. Do not burn escalation
attempts rediscovering it.

## Repository layout (expected)

```
docs/Shuttle_SRS.md                  — locked v1.0 requirements baseline
docs/Shuttle_Implementation_Plan.md  — the 7-phase plan with verification gates
PROGRESS.md                          — your ledger (you create and maintain this)
BLOCKED.md                           — escalation file (only exists if you are stuck)
```

Read all docs/ files plus PROGRESS.md before doing anything. If PROGRESS.md does not
exist, this is iteration zero: verify the environment (Xcode CLT, cmake, Docker Desktop
running and able to pull the base image), `git init`, create PROGRESS.md from the
template below, and begin Phase 0.

---

## DESIGN AMENDMENTS — these OVERRIDE the SRS and plan where they conflict

The SRS and plan were reviewed before this build started. Four corrections are binding.

### A1. Cursor model: read / write / watermark — NOT shared A/B fields

Appendix A's shared `a_start/a_size/b_size` violates single-writer ownership (two fields
have two writers). Do not implement it. Instead the shared BipBuffer state is exactly
three absolute offsets, each strictly single-writer:

- `write`     — producer-owned: end of committed data
- `watermark` — producer-owned: end of valid data before a wrap (set when wrapping early)
- `read`      — consumer-owned

Invariant: if `write >= read`, valid data is `[read, write)`; if `write < read`, valid
data is `[read, watermark)` then `[0, write)`. The A→B handoff is the consumer observing
`read == watermark` while `write < read`, then storing `read = 0` — a single owned-variable
update. Regions A/B exist only as derived concepts in Phase 2 logic, never as shared atomics.
Producer's `reserve_*` state is process-local; do not place it in the segment.
Prior art to consult for the invariants: James Munns' bbqueue.

### A2. ThreadSanitizer CANNOT detect cross-process races — restructure the TSan gates

TSan shadow state is per-process; two TSan'd processes sharing a segment are invisible to
each other. Therefore:
- The **TSan gate** runs in `--single-process` mode: producer thread + consumer thread in
  one process, against a real `MAP_SHARED` mapping, exercising the identical code paths.
- The **two-process gate** verifies what only it can: pshared init, offset/mapping
  correctness, single-init publication — via byte-exact FIFO stress (≥1 GB random sizes),
  asymmetric-speed stress both directions, and wrap-heavy stress. No TSan claims attach
  to the two-process run.
- Additionally: write the happens-before argument for every shared atomic as an inline
  comment block (who stores with what ordering, who loads with what ordering, what it
  guarantees). Treat FR-17 as satisfied by single-process TSan + the written argument.

### A3. Heartbeat + timedwait is the PRIMARY liveness mechanism on BOTH platforms

A robust mutex only fires if the peer died *holding the mutex*. The common death (peer
SIGKILLed on the lock-free path while the survivor is parked in `cond_wait`) means nobody
ever signals — a plain `cond_wait` deadlocks forever, on Linux too. Therefore:
- ALL parked waits on ALL platforms are `pthread_cond_timedwait` + heartbeat-staleness
  check on each timeout. This is core, not a macOS shim.
- Linux robust mutex (`PTHREAD_MUTEX_ROBUST` + `EOWNERDEAD` → repair state →
  `pthread_mutex_consistent` → unlock, in that order) is *additional* hardening for the
  narrow died-holding-the-lock window.
- macOS has no `pthread_mutex_timedlock`: acquiring the park mutex on macOS must be a
  `pthread_mutex_trylock` loop with short sleep + heartbeat check, never a bare lock.
- Crash tests need TWO kill points: (1) mid-reservation on the lock-free path (exercises
  heartbeat timeout), (2) while holding the park mutex (exercises robust recovery on
  Linux / trylock-loop escape on macOS).

### A4. The parking protocol requires seq_cst, not just recheck-under-mutex

The flag-then-recheck dance has a Dekker-style store→load hole: consumer's stale
"empty" recheck and producer's stale `waiting==0` load can coexist under acquire/release.
Use `memory_order_seq_cst` for the four accesses in the park decision (waiting-flag
store and load; the cursor publish and the recheck that gate parking/signaling) — or
equivalently `atomic_thread_fence(seq_cst)` after the flag store and after the commit.
Data-path cursors stay release/acquire in the fast case. Comment this as thoroughly as
the wrap handoff.

### Minor amendments (also binding)

- `pthread_cond_timedwait` must not use CLOCK_REALTIME: `pthread_condattr_setclock(CLOCK_MONOTONIC)`
  on Linux; `pthread_cond_timedwait_relative_np` on macOS. Behind the `platform.hpp` seam.
- Pad each hot atomic (`read`, `write`/`watermark`, waiting flags) to its own **128-byte**
  cache line (Apple Silicon line size; also correct on x86). `static_assert` offsets.
- Phase 0 includes gate **G0.4**: a two-process pshared mutex + condvar smoke test
  (waiter parks on a condvar in shm; signaler wakes it; timeout = failure; per-run nonce
  against stale segments). On THIS host the macOS leg of G0.4 is the single highest-risk
  unknown in Phase 0 — macOS pshared-condvar support is historically spotty. If it fails
  on macOS, the fallback is `os_sync_wait_on_address` (macOS 14.4+) behind the platform
  seam; record the decision in PROGRESS.md.
- Phase 7 adds a **Unix domain socket** baseline alongside HTTP (raw binary over UDS,
  same workloads). Latency timestamps cross processes via CLOCK_MONOTONIC written into
  the payload header. Numbers measured inside Docker on this host are labeled
  "virtualized — not headline figures"; headline NFR-P1 claims require bare-metal Linux
  (record as provisional until then). Native macOS numbers are reportable as macOS-dev
  figures.
- Python wrapper: the borrowed `memoryview` must be invalidated on `release_read` (wrap in
  an object that raises after release); document that Python cannot enforce the borrow.

---

## LOOP PROTOCOL — one iteration, every iteration

1. **Orient.** Read PROGRESS.md. Identify the lowest-numbered unmet gate. That gate is
   this iteration's sole objective. Never work ahead of an unmet gate.
2. **Work** toward that gate. Small commits, one logical change each, message format:
   `phase{N}: <what> [G{N}.{M}]`.
3. **Verify.** Run the gate's check on BOTH legs (`make test-mac` and `make test-linux`)
   where the gate applies to both. A gate passes only if sanitizer-clean where the plan
   says so.
4. **Record.** Update PROGRESS.md: gate status (PASS with command + date / FAIL with the
   exact failure), decisions made, next objective. The ledger must let a fresh session
   resume with zero other context.
5. **Stop conditions for this iteration:** a gate just passed (commit, record, end the
   iteration cleanly), or you hit the escalation rule below.

### Scheduled-job hygiene

If this build is driven by a recurring scheduled job, record the job ID and its expiry
date in PROGRESS.md. If the expiry is within 24 hours at the start of an iteration,
re-register the job and update the ledger. If all gates are PASS, cancel the job, write
the final summary in PROGRESS.md, and stop.

### Escalation rule

If the same gate fails after **3 distinct, materially different attempts** (not retries
of the same idea), STOP. Write BLOCKED.md containing: the gate, the three hypotheses
tried, the exact failing output, and your current best theory. Do not weaken the test,
do not skip the gate, do not continue to later phases. End the iteration. If BLOCKED.md
exists at the start of an iteration, do no work: report its contents and stop.

### Hard rules (never violate, even to make progress)

- **Never modify a test/gate to make it pass.** Gates change only if PROGRESS.md records
  a reasoned amendment consistent with the SRS and the amendments above.
- **Never suppress or filter sanitizer reports.** A TSan/ASan finding is a bug until
  proven a documented false positive (record the proof; `setarch -R` for the known
  TSan/ASLR incompatibility is pre-approved).
- **One variable per phase.** If a gate fails, the bug is in this phase's new variable;
  do not refactor already-gated code without recording why.
- **Offsets, never pointers, in the segment.** From line one.
- All platform divergence lives in `platform.hpp`. Nothing else may `#ifdef` on platform.
- Stay inside this repository (plus the Docker container). Do not modify global config
  or install host-wide packages without recording it as a ledger decision.

## PROGRESS.md template (create on iteration zero)

```markdown
# Shuttle Build Ledger
## Current objective: G0.1
## Scheduled job: (id, cadence, expiry)
## Gate status
| Gate | mac leg | linux leg | Verified by | Date | Notes |
|------|---------|-----------|-------------|------|-------|
| G0.1 | PENDING | PENDING | | | |
... (all gates G0.1–G7.3, plus G0.4 and the second crash kill-point as G5.4)
## Decisions log
## Session notes (newest first)
```

## Phase order and gate inventory

Phases 0–7 and gates G0.1–G7.3 are exactly as specified in
docs/Shuttle_Implementation_Plan.md, modified by the amendments above:
- G0.4: two-process pshared mutex+condvar smoke passes natively on macOS AND in the
  container. The macOS leg gates Phase 4.
- G3.1 (modified): byte-exact ≥1 GB two-process stress; TSan-clean applies to the
  single-process dual-thread configuration of the same code.
- G5.x (modified): heartbeat/timedwait gates run on BOTH platforms; G5.4 (new) =
  kill-while-holding-mutex recovery on both platforms.
- G7.x (modified): pass criteria measured against BOTH the HTTP baseline and the UDS
  baseline; report both ratios, with the virtualization labeling rule above.

Begin.
