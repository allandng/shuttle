# Shuttle — Implementation & Sprints Plan

**Companion to:** Shuttle SRS v1.0 (locked baseline)
**Author orientation:** dev on Apple M3 (macOS), ship on Linux (arm64 + x86_64)
**Guiding principle:** *isolate one variable per phase.* Pure data-structure logic is debugged before concurrency; concurrency before IPC; the lock-free data path before the wake mechanics; the wake mechanics before crash recovery. Never debug two of these at once — that is how shared-memory projects die.

---

## The build-order thesis (read first)

The single biggest risk in this project is conflating bugs. A wrong answer can come from (a) BipBuffer arithmetic, (b) a memory-ordering mistake, (c) a lost wakeup, (d) a pshared-init mistake, or (e) a platform difference. If you bring up everything at once and the consumer reads garbage, you cannot tell which of the five it is.

So the plan deliberately stages capability so that **at every phase, only one of those five can be the culprit:**

| Phase | New variable introduced | Everything before it is already trusted |
|---|---|---|
| 0 | toolchain, sanitizers, containers | — |
| 1 | segment lifecycle + header | toolchain |
| 2 | BipBuffer arithmetic (single thread, no shm) | nothing concurrent yet |
| 3 | atomics + cross-process ordering (busy-poll, **no mutex**) | BipBuffer logic is proven |
| 4 | parking-lot wake (mutex + condvars) | ordering is proven |
| 5 | crash recovery (heartbeat → robust) | wake is proven |
| 6 | C ABI freeze + FFI | core is proven |
| 7 | benchmark | everything is proven |

A corollary: **Phase 3 is the hard one.** Budget accordingly. If something is going to eat a week, it is cross-process release/acquire ordering on the A→B handoff, debugged under a multi-process ThreadSanitizer.

---

## Phase 0 — Toolchain, sanitizers, and the two-platform harness

**Goal:** a CMake project that compiles a stub library and runs an empty test on (1) macOS arm64 natively and (2) Linux arm64 inside Docker — *before any real code exists.* Establishing both targets now means every later phase is validated on both with one command.

### Work items (in order)
1. CMake skeleton, C++17, three targets: `shuttle_core` (static lib), `shuttle_tests`, `shuttle_bench`. Keep the core free of any test/bench dependency.
2. Create `platform.hpp` — the single seam where `#ifdef __linux__` / `#ifdef __APPLE__` lives. Nothing else in the codebase is allowed to `#ifdef` on platform. This file is the entire macOS-vs-Linux strategy in one place.
3. Sanitizer presets wired from day one: an ASan preset and a **separate** TSan preset (ASan and TSan are mutually exclusive — you cannot link both). Add a UBSan flag to the ASan build.
4. **Docker for the Linux target.** Docker Desktop on Apple Silicon runs `linux/arm64` containers natively in its lightweight VM — no QEMU emulation — so robust-mutex work in Phase 5 runs at near-native speed on your M3. Two concrete gotchas to bake into the Dockerfile/run now:
   - **Use a glibc base (Ubuntu/Debian arm64), not Alpine/musl.** musl's `PTHREAD_MUTEX_ROBUST` support is historically weaker and divergent; you do not want to discover that in Phase 5. glibc gives you the canonical `EOWNERDEAD` semantics the SRS assumes.
   - **`--shm-size` defaults to 64 MB inside containers.** With 50 MB payloads and the SRS's ~2× capacity rule, your segment is >100 MB and `ftruncate`/`mmap` will fail with a confusing `ENOSPC`/`EINVAL`. Set `docker run --shm-size=512m` (or mount a tmpfs) now and document it.
5. A trivial smoke target that maps and unmaps 4 KB of shm on both platforms, proving the toolchain end-to-end.

### Debugging strategy
- Make `make test-mac` and `make test-linux` (the latter wraps `docker run`) single commands. If running both targets is more than one keystroke, you will stop doing it, and the platform gap will bite you late.

### Verification gates
- **G0.1** Empty test binary builds and runs clean under ASan on macOS and under ASan in the arm64 container.
- **G0.2** TSan preset links and runs the empty test on both (proves TSan instrumentation is wired before you need it in Phase 3).
- **G0.3** The 4 KB smoke target maps/unmaps successfully in the container with a >100 MB `--shm-size`, proving the shm-size gotcha is handled.

---

## Phase 1 — Segment & header architecture

**Goal:** create, open, validate, close, and unlink a named segment with a correct, version-checked, properly-aligned header. No BipBuffer, no concurrency — just the memory object and its metadata.

### Work items (in order)
1. **Define the header struct** (App. A) using only fixed-width types (`uint64_t`, `uint32_t`, `std::atomic<uint64_t>`, etc.). Add explicit padding so `data_offset` lands on a cache-line boundary (64 B). `static_assert` the struct size and key field offsets so a layout change can't silently break the ABI or the other-language readers.
2. **Offsets, never pointers (CON-3 / FR-16).** Establish the discipline *now*, before there is anything to point at: a tiny `resolve(base, offset)` helper and a rule that nothing of pointer type is ever stored in the segment. This is cheap to honor from line one and expensive to retrofit.
3. **Lifecycle:** `shuttle_create` → `shm_open(O_CREAT|O_EXCL)` + `ftruncate` + `mmap(MAP_SHARED)` + header init. `shuttle_open` → `shm_open` (no create) + `mmap`, *no* re-init. `shuttle_close` → `munmap` + free local handle. `shuttle_unlink` → `shm_unlink`.
4. **Magic + version validation (FR-3)** on open; distinct error on mismatch.
5. **Capacity validation (FR-4):** reject `capacity < max_payload + framing` with a distinct error.
6. **The single-init problem (App. B #5):** the creator initializes the header; an opener must not read BipBuffer/primitive state until init is *published*. Use an `atomic<uint32_t> init_state` written last with a release store; the opener spins/acquire-loads until it sees `READY`. (A full seqlock is overkill for one-shot init.)
7. **Platform-specific seam:** macOS `shm_open` names are capped (~31 chars incl. leading `/`) and an shm object can effectively be `ftruncate`d only once at creation. Enforce a short-name check and one-shot truncate in `platform.hpp`.

### Debugging strategy
- On Linux, `ls -l /dev/shm` is your ground truth — every create should appear there, every unlink should remove it. On macOS there is no `/dev/shm`; rely on create/open succeeding from a second process and on clean teardown.
- Write a 5-line `shuttle_inspect` CLI that opens a segment and dumps the header. You will use it constantly through Phase 5.

### Verification gates
- **G1.1** Process A creates; process B opens, reads matching magic/version; B with a bumped version gets the mismatch error.
- **G1.2** `create` with `capacity < max_payload + 8` fails with the FR-4 error code, not a crash.
- **G1.3** Leak check (NFR-R2): after create→close→unlink, `/dev/shm` is clean on Linux; the name cannot be re-opened on either platform.

---

## Phase 2 — BipBuffer logic (single-threaded, no shared memory)

**Goal:** a provably-correct BipBuffer (D2) and 8-byte length framing (§2.4) running against a plain heap buffer in **one thread**. Zero concurrency, zero IPC. This is the phase where you nail the arithmetic that everything else assumes is correct.

### Work items (in order)
1. Implement `reserve(n) / commit(n) / read_block() / release(n)` over a `std::byte*` and plain `size_t` cursors (not atomics yet — concurrency is Phase 3). Model region A, region B, and the producer-private reserve cursor exactly as in App. A.
2. The **early-wrap rule (App. B #6):** reserve `8 + len` as one unit; if it doesn't fit after A, wrap to B at offset 0; never split a payload. Region A therefore always ends on a message boundary.
3. The **A→B handoff:** when A fully drains, `A := B`, clear B. Get this dead right here, with no memory-ordering noise to confuse you, because in Phase 3 this exact transition becomes the most delicate ordering point in the whole system.
4. Length framing: write `u64 len` then payload; reader peeks the length and returns `(ptr, len)` into the contiguous payload (FR-7).

### Debugging strategy
- This phase is pure logic, so it is the **one place you can use heavy fuzzing/property testing.** Drive thousands of random-sized writes interleaved with random-sized drains; after each operation assert the invariants: A and B never overlap, no reservation ever crosses the physical end, total bytes in == total bytes out, FIFO order preserved. A property test here is worth more than any amount of staring at Phase 3.

### Verification gates
- **G2.1** Byte-exact FIFO over ≥100k random write/read pairs of random sizes; invariants hold after every op.
- **G2.2** The three named edge cases: payload that *exactly* fills the space after A; a payload that forces an early wrap to B; a max-size payload (`= max_payload`).
- **G2.3** A write of `max_payload + 1` (logically, > usable capacity) fails fast rather than looping or wrapping incorrectly.

---

## Phase 3 — Lock-free hot path, cross-process, **busy-poll** (the hard phase)

**Goal:** move the BipBuffer state into the shared segment as atomics and make two *separate processes* exchange data correctly with the lock-free cursor protocol — **with no mutex and no condvars yet.** When the buffer is empty/full, the waiting side simply spins (busy-polls). This deliberately separates *memory-ordering correctness* from *wake mechanics*; you debug them one at a time.

### Work items (in order)
1. Promote the cursors to `std::atomic<uint64_t>` in the header. Enforce the **single-writer ownership** model from App. A: producer is the only writer of `a_size`, `b_size`, `reserve_*`; consumer is the only writer of `a_start`. Single-writer-per-atomic is what makes the lock-free path sound.
2. Apply the ordering contract: producer publishes a commit with a **release** store on the size it grew; consumer observes it with an **acquire** load before reading payload bytes. Symmetric for the consumer's `a_start` release and the producer's acquire. The release/acquire pair is what guarantees the payload bytes are visible before the cursor that advertises them.
3. **The A→B handoff is the ordering hotspot.** The consumer setting `a_start = 0`, adopting `b_size` as the new `a_size`, and clearing `b_size` must be ordered so the producer never sees a torn intermediate state. Specify the exact fence sequence in the design doc and comment it inline; this is the line of code most likely to harbor a 1-in-10⁶ bug.
4. Empty/full handling: **spin** (optionally with a `pause`/`yield` hint). No blocking primitives yet. This is intentionally CPU-wasteful and temporary.

### Debugging strategy
- **Multi-process ThreadSanitizer is the whole game this phase.** TSan across processes requires the shared segment be mapped in both and both binaries built with TSan; run producer and consumer as separate TSan'd processes against the same segment. A clean TSan run over a long stress test is your proof of the ordering contract — far stronger than "it didn't crash."
- Asymmetric-speed stress: run producer fast / consumer slow (forces full → backpressure-by-spin), then the reverse (forces empty → spin), then both flat-out (maximizes handoff contention). The handoff bug, if present, surfaces under flat-out.
- Keep a `--single-process` mode (producer thread + consumer thread, same address space) as a faster inner-loop check, but **the gate is the two-process run** — same-process testing hides pshared/mapping bugs.

### Verification gates
- **G3.1** Two separate processes exchange ≥1 GB of data in random-sized messages, byte-exact, FIFO — **clean under multi-process TSan.**
- **G3.2** Producer-fast and consumer-fast asymmetric stress both pass byte-exact (proves spin-based backpressure and spin-based empty handling are correct).
- **G3.3** A targeted A→B-handoff stress (capacity sized so wraps happen every few messages) passes byte-exact under TSan.

---

## Phase 4 — Parking-lot wake (mutex + condvars, off the hot path)

**Goal:** replace the busy-poll from Phase 3 with real blocking, so an idle peer consumes ~0% CPU — while keeping the mutex strictly off the normal path (§2.3). Because Phase 3 already proved ordering, any bug here is *by construction* a wake bug (lost wakeup, missed signal), which is a much smaller search space.

### Work items (in order)
1. Add the pshared `pthread_mutex_t lock` and the two `pthread_cond_t` (`not_empty`, `not_full`) to the segment. **Initialize the pshared attribute explicitly** on all three (App. B #2) — a default-initialized mutex silently "works" within one process and fails across processes, which is a nasty false-positive in same-process tests.
2. Add the `consumer_waiting` / `producer_waiting` atomic flags.
3. Block path (consumer empty): set `consumer_waiting`; lock; **re-check emptiness under the lock** (lost-wakeup guard, App. B #7); `pthread_cond_wait(not_empty)`; on wake clear the flag, unlock, retry the Phase-3 lock-free read. Symmetric for producer-full on `not_full`.
4. Wake path (kept off hot path): after a commit, the producer does an atomic load of `consumer_waiting`; only if set does it briefly lock + `signal(not_empty)`. In the common case (peer not sleeping) neither side ever touches the mutex. Symmetric on release.

### Debugging strategy
- The failure mode here is the **lost wakeup**: peer checks "empty", then the other side commits + signals *before* the peer is waiting, peer then waits forever. The flag-then-recheck-under-lock dance prevents it. Stress it by inserting artificial scheduling delays (a `sleep(0)`/jitter) between the predicate check and the wait, which widens the race window and makes a missing guard reproduce quickly.
- CPU usage is now a *correctness* signal: an idle blocked peer must sit near 0% CPU. If it's spinning, your block path isn't engaging.

### Verification gates
- **G4.1** Idle consumer (no data for several seconds) shows ~0% CPU — proves true blocking, not residual spin.
- **G4.2** "Trickle" stress: producer sends one message every random interval; consumer receives every message with no lost/extra wakeups over ≥100k messages.
- **G4.3** Wake latency: p99 from commit→consumer-has-payload is microsecond-scale on target hardware (NFR-P3); the hot path still touches no mutex when the peer isn't parked (verify by counting lock acquisitions).

---

## Phase 5 — Crash resilience: macOS heartbeat first, then Linux robust

**Goal:** a peer that dies mid-transfer never permanently strands the survivor. Per D4 this is a *hard* guarantee on Linux and a *best-effort* guarantee on macOS. **Build the macOS path first** because it's your dev machine and lets you exercise the abstraction immediately; then move into the arm64 Linux container for the robust path.

### Step 5a — macOS heartbeat (on the M3, dev-only guarantee — FR-19)
1. Add `producer_heartbeat` / `consumer_heartbeat` monotonic atomics to the header; each side bumps its counter on activity.
2. macOS lacks `pthread_mutex_timedlock` and robust mutexes. So all blocking waits become `pthread_cond_timedwait`; on each timeout, check whether the peer's heartbeat has gone stale past a configurable threshold. Stale ⇒ declare the peer dead, abort the wait with a distinct error rather than blocking forever.
3. Put **all** of this behind the `platform.hpp` seam — a `wait_blocking()` and a `recover_if_peer_dead()` interface with two implementations. The core code above this seam must not know which platform it's on.

### Step 5b — Linux robust mutex (in the arm64 glibc container — FR-18)
1. Initialize the segment mutex with `PTHREAD_MUTEX_ROBUST` (in addition to pshared).
2. On `lock()` returning `EOWNERDEAD`: **repair the small, well-defined protected state, *then* call `pthread_mutex_consistent`, *then* unlock** — in that order (App. B #3). Get it backwards and the mutex is permanently dead. Because the critical section guards only park/wake bookkeeping (not a data copy), "repair" is genuinely small.
3. Combine with single-writer cursor ownership: the survivor can also conclude the peer is gone and tear the channel down cleanly.

### Debugging strategy
- The crash test is `SIGKILL` (not `SIGTERM` — you want no cleanup) on the producer while a reservation is in flight. Script it so it's repeatable: producer reserves, raises a flag, sleeps; harness kills it at that exact point.
- On Linux, deliberately write a *buggy* recovery once (call `consistent` before repairing) to confirm your test actually detects a permanently-dead mutex — i.e., verify the test can fail.

### Verification gates
- **G5.1 (macOS)** `SIGKILL` the producer mid-reservation; the consumer's blocked wait aborts via heartbeat staleness with the documented error within ~the configured threshold (FR-19).
- **G5.2 (Linux container)** Same kill; the survivor's `lock` returns `EOWNERDEAD`, recovers via `pthread_mutex_consistent`, and the channel never permanently deadlocks (FR-18 / NFR-R1).
- **G5.3** No shm object leaks after a crashed run on either platform once the survivor tears down (NFR-R2).

---

## Phase 6 — C ABI freeze + Rust/Python FFI

**Goal:** lock the `extern "C"` surface and prove the same segment is driven byte-exact from C++, Rust, and Python. The core is already trusted, so any failure here is a *binding/marshalling* bug, not a core bug.

### Work items (in order)
1. Freeze the signatures from SRS §3.1, the integer error-code enum, and the flags. **No C++ exception may cross the boundary (IF-1):** wrap every entry point in a `try/catch` that converts to an error code.
2. Ship a single versioned C header (IF-4) with the struct/enum/error definitions — this is the source of truth for both `bindgen` and `cffi`.
3. Rust: generate FFI with `bindgen`, then a thin safe wrapper. The borrow path (`acquire_read` → pointer → `release_read`) is where Rust lifetimes earn their keep — model the borrowed slice's lifetime so it cannot outlive `release_read`.
4. Python: `cffi` (preferred over `ctypes` for header-driven binding). Expose the borrowed payload as a zero-copy `memoryview` over the returned pointer so you don't accidentally copy on the Python side and silently lose the headline benefit.

### Debugging strategy
- Test cross-language pairs against the *same running segment*, not language-internal mocks: a C++ producer process with a Python consumer process, and a C++ producer with a Rust consumer (FR-21). A round trip that's byte-exact across a language boundary is the real proof.
- Watch for accidental copies in the dynamic languages — a `bytes(...)` in Python or a `.to_vec()` in Rust on the borrow path defeats the zero-copy contract. Assert pointer identity / use a profiler to confirm no copy.

### Verification gates
- **G6.1** C++ producer ↔ Python consumer: byte-exact round trip over the borrow path (FR-21).
- **G6.2** C++ producer ↔ Rust consumer: byte-exact, and the Rust wrapper *fails to compile* if borrowed data is used after `release_read` (lifetime correctness).
- **G6.3** An induced error (e.g., open a nonexistent segment) surfaces as the correct integer code in all three languages, with no exception/panic escaping the ABI.

---

## Phase 7 — The headline benchmark

**Goal:** prove NFR-P1/P2/P3 against a *fair* baseline (D7): raw uncompressed binary payload as the HTTP body, keep-alive on, sensible socket buffers — HTTP doing the least wasteful thing it can.

### Work items (in order)
1. Two transports, identical workload: (a) Shuttle borrow path; (b) raw-binary localhost HTTP.
2. Two workloads: a single **50 MB** blob (tensor/context case) and a **16 KB** frame stream at a fixed rate (audio case, D5 secondary).
3. Methodology: warm-up iterations discarded; then many iterations; report **median and p99**. Measure end-to-end latency (producer commit → consumer holds the full payload), per-side CPU attributable to copy/serialize (profiler), and streaming throughput (MB/s, frames/s).
4. **Where to run it:** correctness across Phases 3–6 is fully valid in the arm64 container, but the *performance numbers* should be taken on real Linux hardware where possible — a virtualized container kernel adds overhead that can muddy the ratio. Report macOS-dev and Linux-prod figures separately and label them (D4 / §4 controls).

### Verification gates (these are the SRS pass criteria)
- **G7.1** 50 MB end-to-end latency ≥ **10×** lower than the HTTP baseline (NFR-P1; stretch 50×).
- **G7.2** Profiler shows negligible copy/serialize CPU on the borrow path (NFR-P2) — the consumer is genuinely reading the producer's bytes in place.
- **G7.3** Microsecond-scale wake latency under load (NFR-P3), consistent with the G4.3 measurement.

---

## Cross-cutting reminders (pin these above your desk)

- **One variable per phase.** If a phase's gate fails, the bug is almost certainly in *that* phase's new variable — don't go spelunking in already-gated code.
- **Offsets, never pointers**, from Phase 1 line one.
- **Two-process TSan run is the real gate**, not the same-process convenience test — same-process testing hides pshared and mapping bugs.
- **The A→B handoff (Phase 3) and the EOWNERDEAD recovery order (Phase 5b)** are the two places a subtle, rare bug will hide. Over-comment both.
- **Glibc container + `--shm-size`** are settled in Phase 0 so they never surprise you later.
- Re-run **every** phase's gates on **both** platforms before declaring it done — the whole point of the Phase 0 two-command harness.
