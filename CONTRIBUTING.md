# Contributing

Shuttle is a small, deliberately narrow library: same-host SPSC channels over
POSIX shared memory, with a frozen C ABI. Most of the rules below exist because
something in the segment or the ABI is a published contract that other people's
processes depend on. Read them before touching `include/shuttle/`.

## Build and test

```sh
cmake -B build -DSHUTTLE_SAN=asan
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DSHUTTLE_SAN=tsan` gives the ThreadSanitizer build; it needs its own build
directory, since ASan and TSan cannot be linked into the same binary. A change
is not done until it is green under **both**.

A handful of tests measure latency percentiles and CPU ratios. They are only
meaningful on quiet, controlled hardware and will flap on a laptop with a
browser open or on a shared CI runner. Exclude them the way CI does:

```sh
ctest --test-dir build --output-on-failure \
  -E "bench_g71|park_latency|wake_under_load|nocopy_cpu|trickle"
```

The cross-language FFI tests additionally need `python3` + `cffi` and `rustc`.

### The two-platform harness

`make` wraps the four legs so each is one command — native macOS plus a glibc
Linux container, under each sanitizer:

```sh
make test-mac      # native ASan/UBSan
make test-linux    # same suite in the container (--shm-size=512m)
make tsan-mac
make tsan-linux
```

Linux is the hard-guarantee platform; macOS crash recovery is best-effort by
design. A change that touches parking, crash recovery, or the platform seam
needs all four legs, not just the one you develop on.

## Formatting

`.clang-format` describes the style already in the tree — it was derived from
the source, not imposed on it, and running it over the existing files is close
to a no-op. Format the code you touch:

```sh
clang-format -i <files you changed>
```

Do not sweep the whole tree. Wholesale reformatting buries real history.

## The rules that are not negotiable

**The C ABI is frozen, and changes are additive only.** The ten functions
`shuttle_create` .. `shuttle_keepalive` in `include/shuttle/shuttle_c.h` keep
their signatures and their semantics forever. New capability arrives as a *new
symbol* — that is what `shuttle_create_ex` is — never as a changed parameter
list, a repurposed argument, or a tightened precondition. Errors are plain
integers; no exception may cross the boundary, which is why every ABI entry
point wraps its body in `try`. Every `SHUTTLE_*` constant mirrors a
`shuttle::Err` / flag value in `src/shuttle_c.cpp` under a `static_assert`; if
you add a code or a flag bit, add the mirroring assert in the same commit so
the two can never drift.

**The segment layout is frozen.** Field offsets in `ChannelHeader` are an ABI
contract between processes that may be built from different versions of this
library. Moving, resizing, or reordering a field is a break and must bump
`kVersion`. The `static_assert(offsetof(...))` block in `header.hpp` is the
enforcement — keep it in step with any field you add.

**Unknown flag bits must be ignored by openers.** `ChannelHeader::flags` is the
additive extension point: the creator writes the whole word once, in the cold
identity block, before the release-store that publishes `init_state`, and it is
immutable after. An opener acts only on bits it knows and silently ignores the
rest, so a new bit does *not* need a `kVersion` bump. Correspondingly, the
creator masks `create_flags` down to the known set — an unknown bit must never
be persisted into the segment.

**`platform.hpp` is the only file allowed to `#ifdef` on platform.** Every
macOS-vs-Linux divergence — robust mutexes, timedwait clocks, shm name limits,
one-shot `ftruncate`, wait-on-address vs condvar, THP advice — gets an interface
in the seam with two implementations behind it. If you find yourself reaching
for `__APPLE__` anywhere else, the seam is missing a function; add it there.

## Testing philosophy

The build was driven gate by gate under one rule: **one new variable per
phase**. Data-structure logic was proven before concurrency was introduced,
concurrency before IPC, memory ordering before wake mechanics, wake mechanics
before crash recovery. When something failed, there was exactly one new thing
it could be.

Contributions are expected to hold that line. A patch that adds a feature *and*
a new concurrency mechanism *and* a new platform path in one step is not
reviewable in this codebase — split it. Prefer tests that verify an invariant
after every operation over tests that check a final answer, and where a test
claims to prove something subtle, make it demonstrate that it *can fail*: the
robust-mutex test ships a deliberately buggy recovery path that leaves the
mutex `ENOTRECOVERABLE`, because a crash-recovery test that always passes
proves nothing. See the README's "Verification" section for what the existing
suite establishes.

Multi-process tests use `posix_spawn`, never `fork` without `exec` — forking a
process with a locked pshared mutex or a sanitizer runtime attached produces
failures that are not the bug you are hunting.
