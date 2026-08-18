# Shuttle experiment record

A curated log of measurements, not a benchmark suite. Each entry says what was
run, the exact command, the numbers that came out, what they establish, and —
the part that makes the entry worth keeping — what they do **not** establish.
An experiment that answered nothing is still recorded, labeled inconclusive.

Related: [API.md](API.md) for what each feature is, [ROADMAP.md](ROADMAP.md)
for what is still open, and the README's **Benchmark honesty** section for the
rules these entries are held to.

Every number in **E1–E5** was produced by a run on the machine described below;
**E6** and **E7** ran on a different host and each carries its own environment
table. Entries are not automatically comparable to one another: E7 changed the
16 KB stream metric, so a stream figure from E1 or E6 cannot be read as a trend
against one from E7, and each entry says so where it matters. The
README's **Benchmark honesty** rules bind this file: nothing measured on a
virtualized host is a headline figure, and each entry says so on its own rather
than relying on the reader remembering the header.

---

## Environment (E1–E5, 2026-08-08)

| | |
|---|---|
| Host | **virtualized Linux x86_64 cloud container — not bare metal** |
| CPU | Intel Xeon @ 2.10 GHz, **4 vCPU**, L2 8 MiB, **L3 260 MiB** |
| Memory | 16 GB, ~14 GB free at test time |
| Kernel / libc | Linux 6.18.5, glibc 2.39 (Ubuntu 24.04) |
| Compiler | g++ 13.3.0, **unsanitized, `-O2`** |
| Page size | 4096 bytes |
| Segment store | `/dev/vda` ext4 (a real block device, not tmpfs); device read-ahead **8192 KiB** |
| Build | `cmake -B build -DSHUTTLE_SAN=off && cmake --build build -j4` |
| Tree | branch `claude/shuttle-updates-m6u870`, HEAD = the v1.4 feature pass (aligned spans, file-backed channels, peek/prefetch) |

Two properties of this host shape several results below and are called out
where they matter:

- **L3 is 260 MiB.** A 64 MiB ring is entirely cache-resident, so throughput
  figures for rings that size measure a cache-resident pipeline, not DRAM.
- **Block-device read-ahead is 8 MiB** (the default on this virtio device is
  far above the usual 128 KiB), and the host hypervisor has its own cache that
  a guest cannot drop. Both blunt E4.

Harnesses live outside the repository (a scratchpad directory) except for E1,
which is the repository's own `shuttle_bench`. Raw output for every run was
retained; the tables quote medians from it faithfully.

---

## E1 — Three-transport benchmark on current HEAD

**Why.** The README publishes a virtualized Linux data point taken before this
branch's features landed. All of them are opt-in, so the default path should be
unchanged; that is a claim, and a claim about performance is worth re-running
rather than asserting.

**How.**

```sh
cmake -S . -B build-off -DSHUTTLE_SAN=off && cmake --build build-off -j4
./build-off/shuttle_bench          # x3, back to back
```

`shuttle_bench` is the repository harness: 50 MB blob, 20 iterations after 3
warmups, end-to-end producer-commit → consumer-holds-payload, plus a 16 KB
frame stream for throughput.

**Numbers.** Three consecutive runs, plus the README's pre-branch figure:

| Run | Shuttle median | Shuttle p99 | UDS median | HTTP median | uds/shuttle | http/shuttle | 16 KB stream |
|---|---|---|---|---|---|---|---|
| README (pre-branch) | 62.3 µs | 97.1 µs | — | — | 101× | 355× | 5.5 GB/s |
| 1 | 72.7 µs | 127.5 µs | 7.42 ms | 27.9 ms | 102.1× | 384.4× | 3.98 GB/s |
| 2 | 62.6 µs | 137.7 µs | 8.33 ms | 21.0 ms | 132.9× | 335.6× | 5.46 GB/s |
| 3 | 63.5 µs | 252.3 µs | 7.94 ms | 33.3 ms | 125.0× | 524.8× | 5.34 GB/s |
| **median of 3** | **63.5 µs** | **137.7 µs** | 7.94 ms | 27.9 ms | 125.0× | 384.4× | **5.34 GB/s** |

**What it shows.** The headline median reproduces: 63.5 µs against the README's
62.3 µs, a 2% difference on a host whose own run-to-run spread is 16%
(62.6–72.7 µs across three identical runs). The 16 KB stream reproduces too
(5.34 GB/s vs 5.5 GB/s). **The v1.4 features cost the default path nothing
measurable** — which is what the design intends, since every one of them is
gated on a create-flag resolved once at construction.

The transport ratios moved *up* (125×/384× against 101×/355×) and that is not
Shuttle getting faster: it is the two baselines being slower on this run
(UDS 7.9 ms against 6.3 ms, HTTP 27.9 ms against 22.1 ms). The README already
makes this point about ratios; it holds in the other direction too.

**What it does NOT show.**

- Nothing about bare metal. This is the same virtualized host class the README
  already labels *not headline*, and the headline claim stays provisional.
- **The p99 is not reproducible enough to quote.** 127.5 / 137.7 / 252.3 µs
  across three identical runs, against 97.1 µs previously. On a shared 4-vCPU
  VM the tail measures the hypervisor's scheduler as much as Shuttle's. Treat
  the README's 97.1 µs p99 as one sample of a noisy quantity, not a bound.
- It does not prove the new code paths are correct or fast — only that they are
  absent from the default path. The opt-in paths are E2–E4.
- **The 16 KB stream column is on the old driver-timed metric** and is not
  comparable to E7's consumer-timed figures. Both numbers in that column
  (5.5 and 5.34 GB/s) remain a faithful record of what the harness printed at
  the time; they are simply measuring a different quantity.

---

## E2 — Page-aligned spans vs classic framing: throughput and fragmentation

**Why.** `SHUTTLE_CREATE_ALIGNED_SPANS` buys page-aligned payload pointers and
pays for them in internal fragmentation. `docs/API.md` states the worst case
arithmetically (`2*page - 8 - 1` = 8183 bytes per message). This measures both
sides: what the padding actually costs in ring occupancy, and what the flag
does to throughput.

**How.** A two-process harness (`posix_spawn`, same shape as `shuttle_bench`):
producer `acquire_write` → `memset` the whole payload → `commit_write`;
consumer `read` → touch one byte per 4 KiB page of the payload → `release`.
64 MiB shm channel, classic and aligned interleaved, 7 repetitions each,
10% of iterations discarded as warmup. Fragmentation is measured separately by
filling a channel with non-blocking writes and no consumer, counting how many
messages fit.

```sh
g++ -std=c++17 -O2 -Iinclude -o p4x p4x.cpp src/shuttle.cpp -pthread
for rep in 1..7; do
  ./p4x e2 shm {0,1} 4096    200000 20000
  ./p4x e2 shm {0,1} 65536    40000  4000
  ./p4x e2 shm {0,1} 1048576   4000   400
done
./p4x e2frag {0,1} <size> 67108864
```

**Fragmentation (exact, deterministic).** Messages resident in a 64 MiB
capacity, and the payload fraction of that capacity:

| Payload | Classic stride | msgs | payload/capacity | Aligned stride | msgs | payload/capacity |
|---|---|---|---|---|---|---|
| 4 KiB | 4104 | 16352 | 99.8% | 8192 | 8192 | **50.0%** |
| 64 KiB | 65544 | 1023 | 99.9% | 69632 | 963 | 94.0% |
| 1 MiB | 1048584 | 63 | 98.4% | 1052672 | 63 | 98.4% |

Measured occupancy matched the predicted `capacity / stride` exactly in all six
cases. The cost curve is the documented one, made concrete: **a page-sized
message loses half the ring; a 1 MiB message loses nothing worth counting.**

**Throughput.** Median of 7 interleaved runs, with the observed range:

| Payload | Mode | msgs/s (median) | range | MB/s (median) |
|---|---|---|---|---|
| 4 KiB | classic | 1,503,598 | 1.20 M – 1.67 M | 6,159 |
| 4 KiB | **aligned** | **2,283,234** | 2.13 M – 2.70 M | 9,352 |
| 64 KiB | classic | 306,636 | 262 k – 326 k | 20,096 |
| 64 KiB | aligned | 319,934 | 229 k – 329 k | 20,967 |
| 1 MiB | classic | 17,849 | 13.9 k – 19.0 k | 18,716 |
| 1 MiB | aligned | 17,570 | 15.6 k – 18.8 k | 18,424 |

**What it shows.**

- At **64 KiB and 1 MiB the flag is throughput-neutral**: the medians differ by
  4% and 2% while the run-to-run ranges overlap heavily. Turning it on to get
  page-aligned pointers costs nothing measurable at the sizes it is meant for.
- At **4 KiB the aligned channel was consistently ~50% faster**, and that is
  not a noise artifact: the seven aligned runs (min 2.13 M msgs/s) do not
  overlap the seven classic runs (max 1.67 M msgs/s) at all, despite the
  aligned channel consuming twice the ring bytes per message.
- A follow-up decomposition (3 reps, 4 KiB, toggling the producer's payload
  `memset` and the consumer's page touch) locates the effect on the producer's
  fill: with the fill removed the ordering **reverses** (classic median 3.55 M
  msgs/s vs aligned 3.14 M — the aligned ring simply wraps twice as often).

| 4 KiB variant | classic msgs/s | aligned msgs/s |
|---|---|---|
| producer fills, consumer touches | 1,691,800 | 2,297,005 |
| producer fills, no consumer touch | 1,920,813 | 2,581,306 |
| no fill, consumer touches | 3,832,438 | 3,423,520 |
| neither | 3,546,359 | 3,135,780 |

**What it does NOT show.**

- **The 4 KiB result is measured but not explained.** An isolated microbenchmark
  of the same two `memset` shapes (4096 bytes at a page boundary vs at page+8,
  same buffer, same loop) shows only an 8% difference — 18.4 vs 17.0 GB/s — far
  short of the ~50% seen through the channel. The mechanism (cache-line split
  stores, store-buffer behavior, TLB, something else) was not established.
  **Do not sell aligned spans as a throughput optimization on this evidence.**
  Its documented purpose — a pointer you can hand to `cudaHostRegister` without
  a bounce buffer — is unaffected either way.
- **These are cache-resident numbers.** The 64 MiB ring fits inside this host's
  260 MiB L3, so 18–21 GB/s reflects L3, not memory. A host with a normal L3
  will produce lower absolute figures; the aligned-vs-classic comparison should
  survive, but that is untested.
- Nothing about aligned spans on a **hugetlbfs** or **file** backing, and
  nothing about the actual GPU-registration path the flag exists for — no GPU
  was involved.

---

## E3 — File-backed vs shm latency

**Why.** File-backed channels put the segment on a filesystem instead of in
`/dev/shm`. Once pages are resident the two should be indistinguishable —
both are `MAP_SHARED` mappings and the data path never knows the difference.
That is a prediction, and it is cheap to check.

**How.** Same two-process harness, latency mode: the producer stamps
`CLOCK_MONOTONIC` into the payload head immediately before `commit_write`; the
consumer takes the delta the moment `read` returns the borrow. Identical
payload sizes on both backings, interleaved, 7 repetitions.

The ring is deliberately held to **two frames** (`capacity = 2 * (size + 8)`).
That matters, and the first attempt at this experiment got it wrong: with a
64 MiB ring the producer runs far ahead, messages queue, and the
commit→hold delta becomes queueing delay rather than latency — the discarded
run reported a *median of 787 seconds* for 4 KiB file-backed messages, which is
a backlog measurement, not a latency one. A two-frame ring also keeps a
file-backed segment's dirty set to a few pages, so kernel writeback is not
silently inside the sample.

```sh
for rep in 1..7; do
  ./p4x e3 {shm,file} 4096     20000 2000
  ./p4x e3 {shm,file} 65536    10000 1000
  ./p4x e3 {shm,file} 1048576   3000  300
  ./p4x e3 {shm,file} 50000000    23    3
done
```

**Numbers.** Median of the 7 per-run medians, with the spread across runs:

| Payload | shm median | shm range | file median | file range | shm p99 | file p99 |
|---|---|---|---|---|---|---|
| 4 KiB | 12.28 µs | 2.83 – 15.37 | 10.56 µs | 1.19 – 12.69 | 39.5 µs | 36.0 µs |
| 64 KiB | 1.84 µs | 0.47 – 14.11 | 0.81 µs | 0.64 – 11.34 | 37.7 µs | 33.4 µs |
| 1 MiB | 22.41 µs | 20.16 – 25.83 | 22.79 µs | 22.01 – 25.37 | 61.3 µs | 56.6 µs |
| 50 MB | 70.25 µs | 60.52 – 75.20 | 80.35 µs | 59.30 – 131.90 | 134 µs | 155 µs |

**What it shows.** At the two sizes where the measurement is stable — 1 MiB
(shm range 20.2–25.8 µs, file 22.0–25.4 µs) and 50 MB (60.5–75.2 vs
59.3–131.9) — **file-backed and shm are at parity with warm pages**, exactly as
predicted. At 1 MiB the two medians differ by 1.7% and each backing's own
spread is larger than the gap. The 50 MB file-backed figure sits inside its own
shm spread on 4 of 7 runs and above it on 3; the median difference (10 µs on
70 µs) is not separable from run-to-run noise at n=20 samples per run.

This is the expected result and it is worth stating plainly: **choosing the
file backing is a capacity and lifetime decision, not a latency decision** —
provided the pages are resident. E4 is where that proviso gets tested.

**What it does NOT show.**

- **The 4 KiB and 64 KiB rows are inconclusive, on both backings.** Per-run
  medians there are bimodal — a run lands either near 0.5–3 µs (the consumer's
  adaptive spin catches the commit) or near 12–15 µs (it parks and pays a
  wakeup) — and which mode a run falls into is decided by the hypervisor's
  scheduler, not by the backing. Both backings show both modes. No difference
  between them can be extracted at these sizes from this data.
- Nothing about **cold** pages: every entry here ran against a file whose ring
  had just been written and was still resident.
- Nothing about durability or crash consistency of the file — see the
  non-goal in `docs/API.md`; the file is a transport, not a store.
- macOS is untested here (this host is Linux only).

---

## E4 — `MADV_WILLNEED` prefetch on a cold page cache

**Why.** The v1.4 consumer advises the kernel about committed-but-unread pages
on a file-backed channel, at acquire and before parking. On resident pages that
is pure overhead; the hint exists for the case where an unread page is a disk
read. This is the experiment that should decide whether it earns its place.

**Cold cache: what was possible.** This container runs as root and
`sync && echo 3 > /proc/sys/vm/drop_caches` **works** — it was used, and it
demonstrably evicted the guest page cache (a 32 MiB drain that takes
0.001–0.002 s warm takes 0.012–0.017 s immediately after a drop, an 8–12× jump,
with per-message p99 rising from ~70 µs to 3–4 ms). So the guest cache really
was cold.

It is still **not a cold disk**, and that is the entry's central caveat: the
guest's block device is virtio-backed, the hypervisor has a cache a guest
cannot drop, and this device is configured with **8 MiB of read-ahead**. A
"cold" read here still delivers 2–3 GB/s. Writing a file larger than RAM was
rejected as an alternative — it would defeat the *guest* cache no better than
`drop_caches` already does, and still not defeat the host's.

**Control: how prefetch was turned off.** The gate is not an API surface — it
is a `bool` resolved at `Consumer` construction from the persisted `0x20`
(`SHUTTLE_CREATE_FILE_BACKED`) flag, and there is no per-channel way to
disable it. The control therefore **clears that bit in the consumer's mapped
header immediately before constructing the `Consumer`**, and asserts
`Consumer::prefetching() == false` afterward. This is the minimal possible
intervention: `0x20` is documented as informational — it selects no geometry,
no framing, no other code path, and an opener takes no other action on it — so
clearing it changes exactly one thing, which is the thing under test. Both arms
are otherwise the same binary reading byte-identical copies of the same
segment. (This is a measurement hack, not a supported way to configure a
channel.)

**How.** A producer process fills a file-backed ring with N messages and exits
(unmapping first). The segment is copied twice, so both arms read
byte-identical files with identical cursor state. Caches are dropped before
each arm, and the arm order is swapped between trials so disk-layout and
ordering effects cannot masquerade as the result. A warm pair runs after each
cold pair as a control on the control.

```sh
./p4x e4write seg-master.seg 1048576 400      # 400 x 1 MiB, then exit
cp seg-master.seg a.seg; cp seg-master.seg b.seg
sync; echo 3 > /proc/sys/vm/drop_caches
./p4x e4read a.seg 1 1048576 400              # prefetch ON
sync; echo 3 > /proc/sys/vm/drop_caches
./p4x e4read b.seg 0 1048576 400              # prefetch OFF (bit cleared)
```

Per message the consumer times `read` → touch one byte per page of the payload
→ `release`, so a page fault taken anywhere in the message lands in the sample.

**Numbers — 400 × 1 MiB (400 MiB backlog), 3 trials in each order.**

| Trial | Order | prefetch | total | MB/s | per-msg median | per-msg p99 |
|---|---|---|---|---|---|---|
| 1 | first | **on** | 0.834 s | 503 | 931 µs | 12.0 ms |
| 1 | second | off | 0.191 s | 2198 | 14 µs | 5.1 ms |
| 1 | first | off | 0.155 s | 2706 | 12 µs | 5.5 ms |
| 1 | second | **on** | 0.405 s | 1035 | 463 µs | 6.7 ms |
| 2 | first | **on** | 0.199 s | 2103 | 445 µs | 2.9 ms |
| 2 | second | off | 0.125 s | 3362 | 13 µs | 4.7 ms |
| 2 | first | off | 0.099 s | 4251 | 12 µs | 3.6 ms |
| 2 | second | **on** | 0.298 s | 1408 | 423 µs | 27.4 ms |
| 3 | first | **on** | 0.171 s | 2459 | 456 µs | 1.5 ms |
| 3 | second | off | 0.138 s | 3043 | 13 µs | 5.4 ms |
| 3 | first | off | 0.152 s | 2767 | 12 µs | 4.9 ms |
| 3 | second | **on** | 0.187 s | 2243 | 502 µs | 1.0 ms |

**Numbers — 32 × 1 MiB (32 MiB backlog)**, same protocol (3 trials × 2 orders =
6 cold runs per arm), with a warm control pair after each cold pair:

| Condition | prefetch | total (median) | per-msg median | per-msg p99 |
|---|---|---|---|---|
| cold | on | 0.017 s | ~540 µs | 0.7 – 1.1 ms |
| cold | off | 0.012 s | ~50 µs | 3.1 – 4.3 ms |
| warm | on | 0.002 s | 58 µs | ~115 µs |
| warm | off | 0.001 s | 33 µs | ~65 µs |

**What it shows.**

1. **The hint costs about 25–29 µs per acquire on this host, and that cost is
   real and repeatable.** Warm-page medians: 58 µs with the hint against 33 µs
   without, in every one of six warm pairs. Notably the overhead is the *same*
   whether the unread backlog is 32 MiB or 400 MiB, which implies the kernel is
   capping the work per `posix_madvise` call rather than walking the whole
   named region — an inference from the data, not something measured directly.
2. **On this host it did not pay for itself.** In all six cold pairs the
   prefetching arm took longer in total (e.g. trial 3: 0.171 s vs 0.152 s in
   one order, 0.187 s vs 0.138 s in the other). The per-message median is 30–40×
   worse because *every* message pays the advisory cost, while only some
   messages would have faulted.
3. **It does change the shape of the distribution, in the direction it is
   supposed to.** Without the hint, most messages are fast and a few stall
   badly: median ~12 µs but p99 of 3.6–5.5 ms. With it, the median rises to
   ~450 µs but the tail comes in — p99 of 1.0–2.9 ms in trials 2 and 3, against
   3.6–5.4 ms for the corresponding control. The hint trades median for tail.
   Trial 1 contradicts even this (12.0 ms and 6.7 ms p99 with the hint on); it
   is the script's first cold pair, and the first run of a series is the slow
   outlier in E1 and E2 as well on this host.

**What it does NOT show — and this is the point of the entry.**

- **It does not settle whether the prefetch hook is worth having.** The
  scenario it targets — a consumer stalling on genuine disk reads — was not
  reproduced. With 8 MiB of device read-ahead already configured and a host
  cache underneath, sequential cold reads here run at 2–3 GB/s, which is
  precisely the regime in which an *additional* readahead hint has nothing left
  to contribute. On a host where a cold page costs milliseconds rather than
  microseconds, the balance could invert entirely. **Inconclusive, by
  environment.**
- **A cold cache implies a deep backlog, structurally.** For a file's pages to
  be cold, the producer must have written and left; that means the entire ring
  is committed-and-unread when the consumer starts. So the case that motivates
  the hint is also its worst case for cost, and the pipelined shallow-backlog
  shape the feature was designed around (producer and consumer running
  together, small resident window) *cannot* have cold pages in the first place.
  This experiment cannot construct the favourable case; it is not clear that
  the favourable case exists on Linux.
- The measured overhead is **per acquire on a file-backed channel only**. It
  says nothing about the default shm path, where the gate is `false` and the
  advisory code is never reached — E1 is the evidence there, and E1 shows no
  regression.
- No macOS run (`madvise(MADV_WILLNEED)` there is a different implementation),
  and no NVMe/spinning-disk run.

**Standing recommendation from this entry:** leave the hook as it is — it is
advisory, correctly scoped to committed data, and off by construction on shm —
but do not document it as a speedup, because on the only host that has been
measured it is not one. If a future host shows a genuine disk-stall regime,
re-run this entry before claiming anything.

---

## E5 — `shuttle_acquire_read` idempotence (observation, not a benchmark)

**Why.** `docs/API.md` claimed in its borrow rules that a second
`shuttle_acquire_read` before releasing returns `INVALID_ARGS`, while its
`shuttle_peek_next` section described the same call as idempotent. Both cannot
be true; the code decides.

**How.** A 20-line C program against the built `libshuttle_c.so`: write two
messages, open a second handle, acquire twice without releasing, peek, release,
acquire again. The producer's `shuttle_acquire_write` is exercised the same way
for contrast.

**Result.**

```
first  rc=0 len=5 data=hello
second rc=0 len=5 data=hello  same_ptr=1
peek_next rc=0 len=7
after release rc=0 len=7 data=world!!
acquire_write first rc=0 second rc=-1 (INVALID_ARGS=-1)
```

**What it shows.** The read side is **idempotent**: a second acquire returns
`SHUTTLE_OK` with the identical pointer and length, and the cursor does not
move. The write side is **not**: a second `shuttle_acquire_write` returns
`INVALID_ARGS`, as documented. The asymmetry is deliberate in the
implementation (`ensure_borrow` in `src/shuttle_c.cpp` reuses the active
borrow; `Producer::try_acquire_write` rejects on `res_active_`), and the
documentation has been corrected to match — see the note in `docs/API.md`.

**What it does NOT show.** Nothing about thread safety: a handle is still
single-threaded per role, and idempotence is not a licence to acquire from two
threads.

---

## E6 — Three-transport benchmark, native Apple M3 (2026-08-17)

> **Partly superseded by E7 (same day, commit `d07996e`).** Two findings below
> no longer describe the harness: the 1 µs clock quantization was fixed, and
> the "bimodal" stream row turned out to be a driver-side measurement artifact.
> The entry is kept as measured — it is the record of what the old harness
> produced, and the reason the new one exists.

**Why.** The README's headline table is the one set of numbers in this project
that is *not* virtualized, and it had been carried forward unchanged across
several feature passes. Two things were worth re-checking against the current
tree: whether the macOS-native figures still reproduce at `9509d82`, and — the
question that turned out to matter more — what the harness's clock resolution
does to a five-microsecond median.

**Environment.** Different host from E1–E5; this entry does not inherit that
header.

| | |
|---|---|
| Host | **native Apple M3 Mac — not virtualized, not a container** |
| OS | macOS 26.5.1 |
| Compiler | AppleClang 21.0.0.21000101, **unsanitized, `-O2`**; cmake 4.3.1 |
| Binary | `build/mac-asan/shuttle_bench` — the bench target is defined *above* `link_libraries(shuttle_san)` in `CMakeLists.txt`, so it is unsanitized regardless of `SHUTTLE_SAN`; confirmed with `otool -L` (no `libclang_rt.asan`) |
| Tree | commit `9509d82`, branch `main` |
| Load | machine otherwise idle |
| Harness label | printed `(macos, native)` — `in_container()` is a `stat("/.dockerenv")` probe, and it is correct here |

`shuttle_bench` unchanged from the repository: 50 MB blob, 20 iterations after
3 warmups, end-to-end producer-commit → consumer-holds-payload across
`posix_spawn`'d processes, plus a 16 KB frame stream for throughput (the MB/s
figure includes the 500 warmup frames in its wall time).

**How.**

```sh
./build/mac-asan/shuttle_bench          # x3, back to back
./build/mac-asan/shuttle_nocopy_cpu_test
./build/mac-asan/shuttle_park_idle_test
```

**Numbers — 50 MB blob, three consecutive runs.**

| Run | Shuttle median | Shuttle p99 | UDS median | UDS p99 | HTTP median | HTTP p99 | uds/shuttle | http/shuttle |
|---|---|---|---|---|---|---|---|---|
| 1 | 5.0 µs | 8.0 µs | 8.54 ms | 10.17 ms | 7.30 ms | 9.31 ms | 1708.6× | 1460.0× |
| 2 | 6.0 µs | 8.0 µs | 8.17 ms | 9.93 ms | 7.24 ms | 7.46 ms | 1361.3× | 1206.7× |
| 3 | 5.0 µs | 9.0 µs | 8.50 ms | 9.11 ms | 7.31 ms | 48.90 ms | 1700.2× | 1461.8× |
| **median of 3** | **5.0 µs** | **8.0 µs** | **8.50 ms** | 9.93 ms | **7.30 ms** | 9.31 ms | **1700×** | **1460×** |

The 48.90 ms HTTP p99 in run 3 is a single-sample outlier in a 20-sample set
and is why the p99 columns are recorded but not promoted anywhere.

**Numbers — 16 KB stream throughput (MB/s).**

| Run | shuttle | uds | http |
|---|---|---|---|
| 1 | 13734 | 7004 | 967 |
| 2 | **6717** | 7066 | 963 |
| 3 | 13291 | 7027 | 897 |
| median | **13291 (≈13.3 GB/s)** | 7027 | 963 |

Run 2 is almost exactly half of the other two on the Shuttle row while the two
baselines stay flat to within 1%, so this is a property of the Shuttle
measurement (or of what the scheduler did to it), not of the host being busy.
**Bimodal; treat 13.3 GB/s as a median, not a level.** *(Superseded by E7: the
bimodality was the driver's `waitpid` poll loop quantizing the denominator. The
metric has since changed to a consumer-timed window, so these MB/s figures are
not comparable to E7's.)*

**Numbers — CPU accounting.**

| Measurement | Value | Build |
|---|---|---|
| Consumer CPU, 2 GB over the borrow path | **0.29 ms** (7.3 µs/msg) — **0.04%** of the 699.27 ms UDS copy baseline | unsanitized `-O2` |
| Idle blocked peer | 1.4 ms CPU over 2.98 s blocked — **0.05%** | **ASan-instrumented** |

The park-idle figure matches the README's 0.05% but comes from a sanitized
binary, which is the wrong way round for a CPU claim: instrumentation can only
add cost, so the figure is an upper bound rather than a like-for-like
reproduction. It is recorded that way deliberately.

**Test suites, same host and commit.** ASan+UBSan 36/36 (two consecutive runs,
~67 s), TSan 36/36 (~149 s). Zero sanitizer reports on either leg, zero
compiler warnings, no suppressions. Three expected macOS skips (robust mutex
×2, hugetlb) occur *inside* passing tests.

**The finding: the Shuttle column is five clock ticks.** *(Superseded by E7,
which replaced the macOS clock; `monotonic_ns()` now ticks at ~41.7 ns and the
±20% below no longer applies to any figure taken after `d07996e`.)*
`shuttle::monotonic_ns()` is `clock_gettime(CLOCK_MONOTONIC)`, which on macOS
quantizes to **1 µs**. Every archived sample across all nine latency files is
an exact multiple of 1000 ns — zero exceptions. The 20 post-warmup Shuttle blob
samples from run 1, verbatim (ns):

```
4000 8000 5000 7000 4000 6000 4000 4000 5000 4000
4000 6000 5000 5000 4000 5000 5000 4000 3000 8000
```

That is a distribution with **six distinct values in it**, spanning 3–8 ticks.
A 5 µs median is 5 ticks; ±1 tick is ±20% on the median and therefore on both
headline ratios, which at 4 µs and 6 µs against the same 8.50 ms UDS baseline
span roughly **1,417×–2,125×**. The UDS and HTTP medians are mid-millisecond,
where a 1 µs tick is four orders of magnitude below the signal, so the
baselines are unaffected.

**What this establishes.**

- The macOS-native headline **reproduces on an M3 at the clock's resolution**:
  5.0 µs median, the same figure the README has carried, measured at
  `9509d82` with every v1.2–v1.4 feature in the tree.
- **The default path is unchanged**, on this platform as well as on the
  virtualized Linux host of E1 — which is the claim every opt-in create-flag
  rests on.
- The ratio movement against the README's previous table (1,857×/1,699× →
  1,700×/1,460×) is **attributable to the baselines**, not to Shuttle: UDS came
  in at 8.50 ms against 9.3 ms and HTTP at 7.30 ms against 8.5 ms, while
  Shuttle's own median reproduced exactly.

**What this does NOT establish.**

- **Nothing about the ratios beyond ±20% on the Shuttle side.** "1,700×" is a
  faithful quotient of the numbers measured; it is not a three-significant-
  figure result, and this harness on this OS cannot make it one. Sub-microsecond
  resolution would need `mach_absolute_time` or a busy-wait calibration, neither
  of which the harness does.
- **Nothing about Linux, and nothing about bare metal.** This is the second
  non-bare-metal-Linux entry in a row; the README's headline claim stays
  provisional for exactly the reason it already says.
- **Nothing about stream throughput stability.** One of three runs came in at
  half the median with the baselines unmoved, and the cause was not
  investigated. A 13.3 GB/s number should not be quoted without the outlier
  next to it. *(E7 investigated it: the cause was the harness's driver-timed
  denominator, and the metric has been replaced.)*
- Nothing about the ASan build's CPU cost being separable from the park-idle
  figure above, since no unsanitized park-idle run was taken.

---

## E7 — Nanosecond clock, consumer-timed stream, and re-measurement (2026-08-17, native Apple M3)

**Why.** E6 closed with two unfinished pieces of its own, and this entry is what
happened when both were chased.

1. **The headline was five clock ticks.** E6 established that the harness's
   `monotonic_ns()` quantized to 1 µs on macOS, so the 5.0 µs median carried
   ±20% that no amount of re-running could remove. That is a *harness* limit,
   and harness limits are fixable.
2. **The stream row was called bimodal and the cause was "not investigated."**
   One of three runs at half the median, with both baselines flat to within 1%.
   E6 said explicitly that 13.3 GB/s should not be quoted without the outlier
   next to it. That is an admission of not knowing, and it stayed on the list.

Fixing (1) surfaced something neither question was looking for: a latent
correctness bug on macOS, reachable by any laptop that suspends while a peer is
parked. Investigating (2) found no scheduler effect at all — the bimodality was
in the denominator.

**Environment.** Same host class as E6, one commit later; this entry does not
inherit the E1–E5 header.

| | |
|---|---|
| Host | **native Apple M3 Mac — not virtualized, not a container**; 4 P-core + 4 E-core, 8 logical |
| OS | macOS 26.5.1 |
| Compiler | AppleClang 21.0.0.21000101, **unsanitized, `-O2`**; cmake 4.3.1 |
| Binary | `build/mac-asan/shuttle_bench` — defined *above* `link_libraries(shuttle_san)` in `CMakeLists.txt`, so unsanitized regardless of `SHUTTLE_SAN` |
| Tree | commit `d07996e`, branch `main` |
| Load | **NOT quiet — see the caveat below.** `spotlightknowledged` held 79–97% of a core throughout; 1-min load average ~3.5 of 8 |
| Harness label | printed `(macos, native)` |

Raw output lives outside the repository, in the same scratchpad convention the
header describes: `bench_runs2/` (the diagnostic session — clock probes, QoS
probe, sleep-quantum probe, the instrumented harness, and a `PROVENANCE.txt`)
and `bench_runs3/` (the ten final runs, the nocopy run, and the load logs).

---

### Part 1 — the clock

**How.** A standalone probe (`clockprobe.cpp`) reads each candidate clock in a
tight loop and records the non-zero deltas, then times 200 ms of wall clock
against each and measures per-call cost.

**Numbers — resolution.**

```
timebase numer=125 denom=3                    (125/3 = 41.667 ns per tick, 24 MHz)
CLOCK_MONOTONIC  deltas: 1000 1000 1000 1000 1000 1000 1000 1000
CLOCK_UPTIME_RAW deltas:   41  167  125  125  125  125   83  125
200ms: MONOTONIC=202670000 ns  UPTIME_RAW=202670250 ns  skew=250 ns
```

Every `CLOCK_MONOTONIC` delta is exactly 1000 ns; every `CLOCK_UPTIME_RAW`
delta is a multiple of 41.67 ns. The two clocks agree on elapsed time to
250 ns over 200 ms — a rate difference of 1.2 ppm, i.e. the same clock read at
different granularity, not a different time base.

The new read is also **cheaper**, though by how much is not stable: the
archived probe measured 14.6 ns/call for
`clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` against 35.0 ns for
`clock_gettime(CLOCK_MONOTONIC)`, and a repeat measured 15.5 against 28.4.
The direction reproduces; the ratio does not, and neither figure is quoted
anywhere as a result.

**Numbers — the resolution actually reaching the harness.** Post-change, 400
pooled 50 MB blob samples contain **157 distinct values**, of which **3.5%**
are multiples of 1000 ns — against the ~4% expected by chance on a 41.67 ns
grid, and against **100%** before the change. This is the whole point of the
entry stated as a measurement: the samples are no longer on a microsecond grid.

**Numbers — the latent bug.** Darwin's `CLOCK_MONOTONIC` **keeps advancing
while the machine is suspended**. The macOS park timeout does not: it uses
`OS_CLOCK_MACH_ABSOLUTE_TIME`, which freezes. So a laptop that slept with a
peer parked burned heartbeat-staleness budget for the whole suspend while the
park timeout stood still, and could wake up and declare a **live peer dead** —
a spurious `PEER_DEAD`. `CLOCK_UPTIME_RAW` freezes across suspend, so both
budgets now freeze together, and all three platforms agree that suspended time
does not elapse.

**The contract argument for why this swap is safe.** Changing a clock under a
concurrency library is exactly the kind of edit that silently desynchronizes a
timed wait, so the reason it cannot here is worth writing down rather than
assuming. **No `monotonic_ns()` value is ever handed to the kernel as a
deadline.** The macOS park path takes a *relative* timeout
(`os_sync_wait_on_address_with_timeout` /
`pthread_cond_timedwait_relative_np`), and the Linux condvar builds its
absolute deadline from its own `clock_gettime(CLOCK_MONOTONIC)` inside
`cond_timedwait_rel()`. `monotonic_ns()` is used only for differences —
heartbeat staleness, spin budgets, and the bench harness's cross-process
stamp — so the clock it reads is free to change as long as it stays monotonic,
system-wide, and frozen across suspend. That three-clause contract is now
written above the function in `include/shuttle/platform.hpp`, together with the
rule that keeps it true: a caller who ever needs an *absolute* deadline for a
wait primitive must read the clock that primitive names, not this function.

---

### Part 2 — the stream metric

**Hypothesis 1: the low run landed on E-cores (QoS).** Tested and **refuted**.
A QoS probe reports the class of the bench process and of a `posix_spawn`'d
child:

```
parent pid=40295 qos=USER_INTERACTIVE rel=0
child  pid=40296 qos=USER_INTERACTIVE rel=0
```

Children inherit `QOS_CLASS_USER_INTERACTIVE`; nothing demotes them. And an
instrumented harness showed the low run's **consumer-side window was normal** —
whatever was slow, it was not the transfer.

**Hypothesis 2: the denominator.** The old figure was 5,500 frames — including
the 500 warmups — divided by the *driver's* wall clock around the spawned pair.
That denominator contains process setup, two `posix_spawn`s, teardown, the
warmup frames, and a `waitpid` poll loop. The poll loop sleeps `usleep(5000)`,
which on this host is not 5 ms:

```
usleep(5000) actual: min 5.074 ms  mean 7.107 ms  max 8.053 ms  (n=200)
```

A ~7 ms granule on a ~8 ms measurement quantizes it. An instrumented harness
reporting **both** metrics from the same run, 20 runs, shows exactly that: the
consumer window varies smoothly (post-warmup 0.00156–0.00251 s, i.e.
32.7–52.4 GB/s), while the driver-timed figure sits pinned near 11.0–11.4 GB/s
and jumps to discrete outliers — one run at 14,789 MB/s (driver 0.006093 s) and
one at 9,939 MB/s (driver 0.009067 s) — as the poll loop lands one sleep early
or late.

**Causal check.** The diagnostic harness exposes the poll interval as
`SHB_POLL_US`. Dropping it from 5000 to 50 moves the driver-timed figure and
leaves the consumer-timed window where it was. The quantity that changes when
you change the poll loop is the one the poll loop was measuring.

**Verdict.** E6's "bimodal 13.7 / 6.7 / 13.3 GB/s" Shuttle row was a harness
artifact, not a property of Shuttle or of the scheduler. The figure is now
**5,000 timed frames over a consumer-timed steady-state window**, applied
symmetrically to all three transports.

**The consequence, stated plainly: the metric changed, so the numbers are not
comparable across the change.** Every stream figure in this file taken before
this entry — E1's 5.5 and 5.34 GB/s on virtualized Linux, E6's 13.3 GB/s
median — was driver-timed and **cannot** be compared to a figure below.
Within a single run the three transports remain comparable to each other,
because all three moved to the new window together.

---

### Part 3 — re-measurement (`bench_runs3`)

**How.** Ten consecutive runs, back to back, plus the CPU-accounting tests:

```sh
./build/mac-asan/shuttle_bench          # x10, back to back
./build/mac-asan/shuttle_nocopy_cpu_test
./build/mac-asan/shuttle_park_idle_test
```

**Numbers — 50 MB blob, ten consecutive runs.** All times µs.

| Run | Shuttle median | Shuttle p99 | UDS median | HTTP median | uds/shuttle | http/shuttle |
|---|---|---|---|---|---|---|
| 1 | 4.4 | 7.7 | 8265.0 | 7384.5 | 1871.2× | 1671.8× |
| 2 | 4.0 | 6.2 | 6889.2 | 7366.0 | 1722.3× | 1841.5× |
| 3 | 4.2 | 7.2 | 6668.7 | 7412.5 | 1569.1× | 1744.1× |
| 4 | 3.7 | 7.1 | 6748.0 | 7503.7 | 1819.8× | 2023.7× |
| 5 | 3.5 | 6.5 | 6729.6 | 7316.2 | 1945.5× | 2115.1× |
| 6 | 4.5 | **32.7** | 6493.4 | 7537.9 | 1443.0× | 1675.1× |
| 7 | 4.2 | 7.1 | 6860.2 | 7433.6 | 1646.3× | 1783.9× |
| 8 | 3.8 | 7.0 | 6732.2 | 7359.7 | 1795.3× | 1962.6× |
| 9 | 4.1 | 7.6 | 6747.4 | 7390.0 | 1652.1× | 1809.5× |
| 10 | 3.6 | 6.9 | 6600.2 | 7457.4 | 1820.7× | 2057.2× |
| **median of 10** | **4.0** | **7.1** | **6739.8** | **7401.2** | **1758.8×** | **1825.5×** |

The ratio columns are medians of the per-run ratios, not a quotient of the
median columns; they differ by less than a percent here but the former is the
honest aggregation.

Three things in that table are deliberately not promoted. **Run 6's 32.7 µs
p99** is one interference sample in a 20-sample set — the highest of the other
nine runs is 7.7 µs — so it is recorded and not headlined, the same treatment
E6 gave its 48.90 ms HTTP outlier. **Run 1 is cold**: its 8,265 µs UDS median
is 20% above every other run; excluding it, runs 2–10 span 6,493.4–6,889.2 µs.
The published 6,739.8 µs median is the median of all ten *including* run 1,
because dropping a run for being inconvenient is not a policy this file has.
**HTTP is the tighter baseline**: 7,316.2–7,537.9 µs, a 3.0% spread against the
median, with no cold-run effect.

**Numbers — 16 KB stream throughput (MB/s), consumer-timed.**

| Run | shuttle | uds | http |
|---|---|---|---|
| 1 | 45321 | 12018 | 1079 |
| 2 | 58677 | 12796 | 1087 |
| 3 | 62065 | 12708 | 1083 |
| 4 | 67840 | 12891 | 1098 |
| 5 | 60450 | 12796 | 1080 |
| 6 | 50235 | 11483 | 1072 |
| 7 | 63210 | 12052 | 1068 |
| 8 | 65171 | 11469 | 1100 |
| 9 | 65457 | 11606 | 1049 |
| 10 | 65014 | 12554 | 1051 |
| **median** | **62638 (≈62.6 GB/s)** | **12303** | **1080** |

Runs 2–10 cluster near 63 GB/s with a coefficient of variation of 8.4%. The
distribution is **unimodal**: the two low values are run 1 (cold) and run 6 (the
same run whose blob p99 spiked at 32.7 µs) — identifiable interference on both,
so this is a tail, not a second level. Contrast E6, where a single low run sat
at exactly half the median with no explanation available.

**What the stream number compares, honestly.** It compares *unlike work*, and
always did. Shuttle's consumer borrows the frame in place and touches only an
8-byte stamp; the UDS and HTTP consumers `read()` all 16 KB into their own
buffer. That asymmetry is the point of the library — not copying is the
feature — but it means the ratio is "borrow versus copy", not "one transport
versus another at equal work". This was equally true of E6's 13.3 GB/s; it is
called out here because it matters more at tens of GB/s.

**Numbers — CPU accounting.**

| Measurement | Value | Build |
|---|---|---|
| Consumer CPU, 2 GB over the borrow path | **0.28 ms** (6.9 µs/msg) — **0.04%** of the 731.86 ms UDS copy baseline | unsanitized `-O2` |
| Idle blocked peer | 1.5 ms CPU over 2.98 s blocked — **0.05%**, now **enforced** at ≤ 30 ms (1%) | **ASan-instrumented** |

The park-idle figure still comes from a sanitized binary, which remains the
wrong way round for a CPU claim — instrumentation can only add cost, so it is
an upper bound, not a like-for-like reproduction. What changed since E6 is that
the bound is now a **gate** rather than a printed line: `park_idle_test` used to
pass anything under 250 ms (8.3% of the block) against a measured 0.6–1.6 ms;
calibrated across 28 runs on both sanitizer legs, the ceiling is now 30 ms (1%).

**Test suites, same host and commit.** ASan+UBSan 37/37, TSan 37/37. Zero
sanitizer reports on either leg, zero compiler warnings, no suppressions. Three
expected macOS skips (robust mutex ×2, hugetlb) occur *inside* passing tests.
The suite grew from 36 to 37 with `shuttle_cabi_threads_test` (G6.4), which
compiles `src/shuttle_c.cpp` directly into a sanitized binary — the shipped
`libshuttle_c` stays unsanitized because foreign runtimes `dlopen` it.

---

### The host-load caveat, twice

Neither of today's measurement sessions ran on a quiet machine, and both are
labeled as floors rather than as results.

- **`bench_runs2`** (the diagnostic session) ran with `mediaanalysisd` at 190%,
  `corespotlightd` at 144%, `spotlightknowledged` at 41% and a
  Virtualization.framework VM alongside; load average 2.7–5.0 on 8 logical
  cores. Its `PROVENANCE.txt` estimates this depressed every absolute figure by
  roughly **15–20%** against E6, measured on the identical driver-timed metric
  (UDS stream 5,860 MB/s here against E6's 7,004). **No absolute figure from
  that session is published.** Its results are the ratios, the distributions,
  and the diagnosis — all of which survive a loaded host.
- **`bench_runs3`** (the ten runs above) is the cleaner session and strictly
  dominates `bench_runs2` on every figure, which is why it replaces it. It was
  still **not quiet**: a 15-minute poll waiting for an idle machine timed out
  (`TIMEOUT_NOT_QUIET`), with `spotlightknowledged` at 79–97% of a core the
  entire time and 1-min load ~3.5 of 8 both before run 1 and after run 10.
  These figures are therefore a **loaded-host floor**. A quiet host should
  produce equal or better numbers; it will not produce worse ones.

Publishing a floor and saying so is the honest option here. Waiting for a quiet
machine that this host would not provide, and publishing nothing, is not.

**What this establishes.**

- **The macOS quantization caveat is resolved.** The harness clock ticks at
  ~41.7 ns rather than 1 µs, proven by both a direct delta probe and the
  distribution of the samples themselves (157 distinct values across 400
  samples; 3.5% on the microsecond grid, i.e. chance). **This supersedes E6's
  "the Shuttle column is five clock ticks" finding**, which was correct about
  the old harness and no longer describes this one.
- **E6's stream bimodality is explained and gone.** It was the driver's
  `waitpid` poll loop quantizing the denominator, not the E-cores, not QoS, and
  not Shuttle. The QoS hypothesis was tested and refuted rather than dropped.
- **A latent macOS correctness bug is fixed**: heartbeat-staleness budget and
  park timeout now read clocks that freeze together across system suspend, so a
  laptop waking from sleep cannot report a live peer as `PEER_DEAD`.
- **The clock swap cannot desynchronize a wait**, because no `monotonic_ns()`
  value is ever handed to the kernel as a deadline — an argument from the code's
  structure, now pinned in a comment above the function so it stays true.
- **New headline figures at nanosecond resolution**, on a loaded host: 4.0 µs
  median-of-ten for the 50 MB blob, 1,759× over UDS and 1,826× over HTTP, and
  62.6 GB/s on the consumer-timed 16 KB stream.

**What this does NOT establish.**

- **Nothing about a quiet host.** Every figure here was taken under sustained
  Spotlight indexing. They are a floor, and the floor has not been checked
  against a ceiling.
- **Nothing that makes the ratios three-significant-figure results.** The clock
  is no longer the limit, but the millisecond baselines still jitter: uds/shuttle
  spans 1,443×–1,946× and http/shuttle 1,672×–2,115× across ten identical runs.
  The finer clock removed a *systematic* ±20%; it did not remove run-to-run
  variance, and the ratios remain order-of-magnitude statements.
- **Nothing comparable to any pre-change stream figure.** The metric changed.
  E1's 5.5 / 5.34 GB/s and E6's 13.3 GB/s are driver-timed numbers; the
  62.6 GB/s above is a consumer-timed number. Quoting a trend across them would
  be a measurement artifact, not a result.
- **The suspend fix is not covered by a test.** It was found by reading the two
  clocks' documented sleep semantics against each other, not by a failing gate,
  and it has never been observed in the wild. Suspending a machine is not
  something the suite can do, so the evidence is the argument plus the platform
  documentation — weaker than every other claim in this file, and labeled that
  way.
- **Nothing about Linux, and nothing about bare metal.** Third non-bare-metal-
  Linux entry in a row; the README's headline claim stays provisional for the
  reason it already gives.
- **Nothing about the stream figure as a like-for-like transport comparison.**
  Borrow-in-place versus `read()`-the-frame is the comparison being made, on
  purpose.

---

## Worked example: handing a large KV / prefix cache to an inference sidecar

Placed here rather than in `docs/API.md` on purpose: the reference documents
one function at a time, while this is a *design* — which flags, which sizes,
which loop, and what to expect when a process dies — and every number in it is
justified by an entry above (E2 for the padding arithmetic, E3 for the parity
claim, E4 for what not to expect from prefetch).

### The shape

An orchestrator holds a prefix (KV) cache for a long prompt and an inference
sidecar needs it, without a copy through a socket. Concretely, for a 32-layer
model with 8 KV heads of head-dim 128 in FP16:

```
per token, per layer, K and V:  8 heads x 128 dims x 2 bytes x 2 (K,V) = 4096 B
per token, all 32 layers:                                   4096 x 32 = 128 KiB
4096-token prefix:                                        128 KiB x 4096 = 512 MiB
```

512 MiB of flat FP16 — no serialization to do, which is the case Shuttle is
for. Send it as **one message per layer**: 4096 tokens × 4096 B = **16 MiB per
message**, 32 messages.

### Flags

```c
/* orchestrator */
int err = 0;
shuttle_channel* ch = shuttle_create_file(
    "/var/lib/infer/kv.seg",
    67125248,                                        /* capacity, see below */
    16777216,                                        /* max_payload = 16 MiB */
    SHUTTLE_CREATE_ALIGNED_SPANS | SHUTTLE_CREATE_STATS,
    &err);
```

- **File-backed** (`shuttle_create_file`, which sets `0x20` itself): capacity is
  bounded by the filesystem, not by `/dev/shm`, and the page cache owns
  residency. E3 says this costs nothing in latency while pages are warm.
- **`SHUTTLE_CREATE_ALIGNED_SPANS` (`0x10`)**: every borrowed pointer is
  page-aligned, so the sidecar can `cudaHostRegister` the span (or hand it to a
  driver ioctl) with no bounce buffer — the copy this whole library exists to
  avoid. At 16 MiB messages the padding is 0.024% (E2 measured the cost curve;
  it only bites at page-sized messages).
- **`SHUTTLE_CREATE_STATS` (`0x8`)**: `msgs_written` / `bytes_written` /
  `msgs_read` / `bytes_read` in the segment, readable by either peer or by a
  third process — the cheapest way to answer "is the sidecar keeping up".
  Counts payload bytes, never the padded stride.
- **Not** `SHUTTLE_CREATE_HUGETLB_*`: rejected with `INVALID_ARGS` on the
  path-typed create, because a hugetlbfs mount and a caller-chosen path name
  two different segments.

### Sizing arithmetic

The channel is a **window**, not the store. Four slabs in flight is plenty to
keep an upload pipeline fed:

```
page                     = 4096
max_payload              = 16 MiB          = 16,777,216
aligned stride           = page + round_up(max_payload, page)
                         = 4096 + 16,777,216   = 16,781,312   (16 MiB + 4 KiB)
FR-4 floor (aligned)     = capacity >= 16,781,312
capacity (4 slabs)       = 4 x 16,781,312     = 67,125,248    (64 MiB + 16 KiB)
data_offset (aligned)    = round_up(1536, 4096)    = 4096      (STATS => v2 header)
file size on disk        = 4096 + 67,125,248  = 67,129,344    (~64 MiB)
padding overhead         = 4096 / 16,781,312  = 0.024%
```

Note the two places the aligned flag changes the arithmetic: `CAPACITY_TOO_SMALL`
is now `capacity < page + round_up(max_payload, page)` (not
`max_payload + 8`), and `data_offset` is page-rounded, which is what makes the
segment unopenable — deliberately, with `CORRUPT` — by a pre-v1.4 binary.

512 MiB streams through the 64 MiB window under ordinary backpressure. If you
would rather hold the whole cache at once, set `capacity = 32 * stride` ≈
512 MiB; the file may exceed RAM, and the page cache decides what is resident —
but see **Residency, honestly** in `docs/API.md` before assuming a ring you just
wrote is not resident.

### The consumption loop

`shuttle_peek_next` is what makes this loop different from a v1.3 loop: while
the sidecar holds layer *N*, it can learn that layer *N+1* has landed and how
big it is — and therefore size a destination buffer, pin memory, or queue a DMA
descriptor — without releasing *N* first.

```c
/* sidecar */
int err = 0;
shuttle_channel* ch = shuttle_open_file("/var/lib/infer/kv.seg", &err);
if (ch == NULL) return err;

for (int layer = 0; layer < n_layers; ++layer) {
    const void* p = NULL;
    size_t n = 0;

    int rc = shuttle_acquire_read(ch, &p, &n, 0);      /* blocks; page-aligned */
    if (rc == SHUTTLE_ERR_PEER_DEAD) break;            /* orchestrator is gone */
    if (rc < 0) return rc;                             /* test < 0, never != OK */

    size_t next = 0;
    if (shuttle_peek_next(ch, &next) == SHUTTLE_OK) {
        /* Layer N+1 is committed and is `next` bytes. Do the work that has to
           happen BEFORE we can accept it: allocate/pin the destination, or
           enqueue its H2D descriptor. WOULD_BLOCK here is not an error — it
           just means the orchestrator has not committed N+1 yet. */
        stage_destination(layer + 1, next);
    }

    upload_layer(layer, p, n);   /* cudaHostRegister(p, n, ...) needs the page
                                    alignment ALIGNED_SPANS just guaranteed */
    shuttle_release_read(ch);    /* the borrow ends here; p is dead after this */
}
```

Three rules this loop is obeying, each of which has a reason in `docs/API.md`:

- **`rc < 0` is the error test**, not `rc != SHUTTLE_OK` — `SHUTTLE_DROPPED` (1)
  exists.
- **`p` is valid only until `shuttle_release_read`.** `upload_layer` must finish
  reading (or the DMA must have completed) before the release, or the
  orchestrator may overwrite those bytes.
- **Peek is read-only and cannot disturb the borrow.** It moves no cursor and
  stores nothing; a `WOULD_BLOCK` answer may go stale the instant it is given,
  but it can only under-report, never over-report.

Prefetch needs no code: the consumer's `MADV_WILLNEED` hooks are on
automatically because the channel is file-backed. Per E4, **do not budget for a
speedup from them** — treat them as a hint that costs a fixed ~25 µs per
acquire on the measured host and may help on hosts with slower storage.

### What the application can rely on when something dies

- **Orchestrator SIGKILLed mid-slab.** The sidecar's blocking
  `shuttle_acquire_read` returns `SHUTTLE_ERR_PEER_DEAD` once the producer's
  heartbeat goes stale, rather than hanging. Measured on a **file-backed**
  channel in `tests/filebacked_test.cpp` (cases c and d, at both kill points —
  mid-reservation and while holding the park mutex): ~2.5 s against a 1.5 s
  staleness threshold. A half-written slab is never visible, because the commit
  is the release edge: the sidecar sees a gap-free prefix of the layers that
  were committed, and nothing else.
- **Sidecar SIGKILLed.** Symmetric: the orchestrator's blocking
  `shuttle_acquire_write` returns `PEER_DEAD` instead of parking forever.
- **On Linux, robust-mutex recovery works on a file mapping** — measured, not
  assumed (`tests/filebacked_test.cpp` case e observes `EOWNERDEAD` from a raw
  lock and full serviceability after `pthread_mutex_consistent`). The
  file-backed crash story is the shm crash story with nothing subtracted.
  macOS remains best-effort for every backing.
- **The segment is not resumable.** The library never calls `msync`; what is on
  disk after a crash is whatever the kernel happened to write back, with no
  ordering between payload bytes and cursors. On restart, re-stream from the
  source of truth. Do not treat `kv.seg` as a cache.
- **Stale files are the one new failure mode**, because a file survives a reboot
  and an shm object does not. `shuttle_create_file` refuses an existing file
  with `EXISTS` and never truncates it. Startup code that owns the path should
  be explicit:

  ```c
  int rc = shuttle_unlink_file("/var/lib/infer/kv.seg");
  if (rc != SHUTTLE_OK && rc != SHUTTLE_ERR_NOT_FOUND) return rc;
  ch = shuttle_create_file("/var/lib/infer/kv.seg", cap, maxp, flags, &err);
  ```

  A supervisor that cannot be certain no peer is running must **not** do this:
  unlinking a file another process still maps leaves that process alive on a
  segment nobody can reach.
- **Sparse traffic needs `shuttle_keepalive`.** If the orchestrator pauses
  between prompts for longer than the staleness threshold without making any
  Shuttle call, the sidecar cannot distinguish it from a dead peer.
