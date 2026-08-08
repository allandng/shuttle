# Shuttle C ABI Reference

The frozen `extern "C"` surface declared in `include/shuttle/shuttle_c.h`. This
header is the single source of truth for foreign-language bindings; it is pure C
(fixed-width / standard types, no C++). No exception ever crosses the boundary —
every failure is an integer code. `SHUTTLE_ABI_VERSION` is `1`.

Two conventions for reporting failure:

- Functions returning `shuttle_channel*` report the code through an out-param
  `int* err` and return `NULL` on failure.
- Functions returning `int` return `SHUTTLE_OK` (0) or a negative error code.
- `shuttle_read` returns a `long`: the non-negative payload length on success,
  or a negative error code.

**Test errors as `< 0`, not as `!= SHUTTLE_OK`.** Since v1.3 there is exactly
one positive return in the ABI — `SHUTTLE_DROPPED` (1) from `shuttle_write`,
and only when the caller passed `SHUTTLE_DROP_NEWEST` on that very call. It is
not an error and it is not `SHUTTLE_OK`; code that tests `!= SHUTTLE_OK` will
report a successful drop as a failure. Code that tests `< 0` is correct for
every entry point here, before and after this addition.

The ten functions `shuttle_create` .. `shuttle_keepalive` are frozen v1.
`shuttle_create_ex` is an additive v1.1 symbol and `shuttle_get_stats` an
additive v1.2 symbol (new symbols only, no existing signature or semantic
touched); v1.3 adds only the `SHUTTLE_DROP_NEWEST` flag bit and the
`SHUTTLE_DROPPED` return, both opt-in. The ABI version stays `1`.

---

## Functions

### shuttle_create

```c
shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
```

Create and initialize a new channel: `shm_open(O_CREAT|O_EXCL)`, one-shot
`ftruncate`, `mmap`, header init, then a release-store that publishes readiness.
Owner-only permissions (`0600`). The caller of `create` is neither producer nor
consumer yet — the role is bound lazily on first use of the corresponding path.

- Blocking: no. Returns immediately.
- Segment size mapped: `data_offset + capacity_bytes`, where `data_offset` is
  the header size of the layout version written — the v1 layout here, always
  (only `shuttle_create_ex` with `SHUTTLE_CREATE_STATS` writes anything else).
- Errors (via `*err`): `INVALID_ARGS` (`name` NULL, `name[0] != '/'`,
  `capacity_bytes == 0`, or `max_payload_bytes == 0`), `NAME_TOO_LONG`,
  `CAPACITY_TOO_SMALL` (`capacity_bytes < max_payload_bytes + 8`), `EXISTS`
  (name already present), `SYS` (`ftruncate`/`mmap`/other syscall failure).

### shuttle_create_ex

```c
#define SHUTTLE_CREATE_HUGEPAGES   0x1
#define SHUTTLE_CREATE_HUGETLB_2MB 0x2
#define SHUTTLE_CREATE_HUGETLB_1GB 0x4
#define SHUTTLE_CREATE_STATS       0x8
shuttle_channel* shuttle_create_ex(const char* name, size_t capacity_bytes,
                                   size_t max_payload_bytes,
                                   uint32_t create_flags, int* err);
```

As `shuttle_create`, plus opt-in create-time behavior selected by
`create_flags`. `create_flags` is a **separate namespace** from the per-op
`flags` argument used by the read/write calls (`SHUTTLE_NONBLOCK`); the two must
not be mixed. Unknown flag bits are masked off and ignored (never persisted).

- `SHUTTLE_CREATE_HUGEPAGES`: advise transparent huge pages for the mapping via
  `madvise(MADV_HUGEPAGE)` on Linux. Purely advisory — it takes effect only
  where the kernel THP shmem policy permits, and a kernel that disallows it
  returns a harmless `EINVAL` that is dropped. No-op on macOS and on
  unsupported kernels; never a correctness dependency. The chosen flag is
  recorded in the segment header, so that `shuttle_open` re-advises the opener's
  independent mapping automatically.
- `SHUTTLE_CREATE_STATS`: allocate the statistics counters in the segment and
  keep them updated. See **Statistics** below — this is the one create-flag
  that changes the segment's **layout version** (1 → 2), and therefore which
  peers can open it.
- `SHUTTLE_CREATE_HUGETLB_2MB` / `SHUTTLE_CREATE_HUGETLB_1GB`: back the segment
  with **explicit, reserved huge pages** of that size. See **Explicit huge
  pages (hugetlbfs)** below — this is a guarantee-or-error flag, not advice,
  and it never falls back to normal pages.
- Errors: `shuttle_create`'s set, plus `INVALID_ARGS` if **both** hugetlb bits
  are set (they name two different page sizes; neither can be silently
  dropped), and `NO_HUGEPAGES` if a hugetlb request cannot be honored. Passing
  only unknown bits is not an error; they are silently masked.

#### Explicit huge pages (hugetlbfs)

`SHUTTLE_CREATE_HUGEPAGES` (THP) and `SHUTTLE_CREATE_HUGETLB_*` sound alike and
are opposites in the only way that matters:

| | `HUGEPAGES` (THP) | `HUGETLB_2MB` / `HUGETLB_1GB` |
|---|---|---|
| Mechanism | `madvise(MADV_HUGEPAGE)` on a normal shm mapping | segment object is a **file on a hugetlbfs mount** |
| If unavailable | silently proceeds on normal pages | **fails with `NO_HUGEPAGES`; creates nothing** |
| Platforms | Linux (no-op on macOS) | Linux only (always `NO_HUGEPAGES` on macOS) |
| Setup needed | none | operator must reserve pages and mount hugetlbfs |

**No silent fallback.** If the pages cannot be delivered — no hugetlbfs mount
with that page size, no free reserved pages, no permission on the mount, or a
platform without hugetlbfs — `shuttle_create_ex` returns `NULL` with
`SHUTTLE_ERR_NO_HUGEPAGES` and leaves nothing behind. A caller that would
rather have normal pages than an error must ask for them explicitly (retry
without the flag); the library will not decide that silently. This is the
entire difference from the advisory THP flag.

**Where the pages come from.** The implementation parses `/proc/mounts` for
`hugetlbfs` mounts and matches the `pagesize=` option against the requested
size; a mount with no `pagesize=` option uses the system default, resolved from
`/proc/meminfo`'s `Hugepagesize:`. The first matching mount wins. Reservation
happens at **`mmap` time**, not at create time, so an exhausted pool surfaces
as `NO_HUGEPAGES` from the mapping step — and the partially-created file is
removed before returning.

**Two namespaces.** A normal segment lives in the POSIX shm namespace
(`/name` → `/dev/shm/name` on Linux); a hugetlb segment is a file named
`shuttle_<name>` on the hugetlbfs mount. `shuttle_open` and `shuttle_unlink`
take no flag and need no knowledge of the backing: they probe shm first, then
every hugetlbfs mount. If a name somehow exists in **both** namespaces (which
requires two creators racing on one name — already a contract breach, since
create is `O_EXCL`), **shm wins** on open and on unlink, and the hugetlbfs file
is only reachable by unlinking a second time.

**Openers need nothing special.** No flag, no `MAP_HUGETLB` — that flag is for
*anonymous* mappings. A `MAP_SHARED` mapping of a hugetlbfs file is huge-page
backed because the file's filesystem says so, which is why an opener built
without any awareness of this feature still maps the segment correctly. The
hugetlb bits are persisted in the header purely so tools and peers can see what
the creator asked for.

**Size rounding.** hugetlbfs objects must be a whole number of huge pages, so
the segment is rounded **up** (a 1 MB channel on 2 MB pages occupies one full
page). `data_capacity` in the header still records the capacity you asked for;
the extra bytes are slack past the end of the data region, tolerated by the
same `>=` coverage checks that already tolerate macOS's shm page rounding.

**Combining flags.** `SHUTTLE_CREATE_STATS` is orthogonal — `0x8|0x2` is a
version-2 segment that happens to live on hugetlbfs. `SHUTTLE_CREATE_HUGEPAGES`
combined with a hugetlb bit is **not** an error: the explicit backing wins and
the THP advice is skipped as meaningless (both bits are still persisted).
Setting both hugetlb bits at once **is** an error (`INVALID_ARGS`).

**Operator recipe (Linux, root)** — without this, expect `NO_HUGEPAGES`:

```sh
sysctl -w vm.nr_hugepages=64                        # reserve 64 x 2 MB
mkdir -p /dev/hugepages
mount -t hugetlbfs -o pagesize=2M none /dev/hugepages
grep -i huge /proc/meminfo && grep hugetlbfs /proc/mounts
```

Make the reservation permanent via `vm.nr_hugepages` in `/etc/sysctl.conf` and
an `/etc/fstab` entry. A non-root process also needs write permission on the
mount point (`mount -o uid=,gid=,mode=`, or `chmod`). 1 GB pages generally
require the `hugepagesz=1G hugepages=N` kernel command line, since they cannot
usually be reserved at runtime.

### shuttle_open

```c
shuttle_channel* shuttle_open(const char* name, int* err);
```

Attach to an existing channel without re-initializing it. Waits (deadlined, 5 s)
for the creator's readiness publication, then validates magic, version, and
header geometry before trusting any field. If the creator opted into THP, the
opener's mapping is advised too.

The opener takes no flags and needs no knowledge of how the segment is backed:
it looks in the POSIX shm namespace first, then on hugetlbfs mounts (see
**Explicit huge pages** above), and a hugetlbfs-backed segment maps onto huge
pages with no `MAP_HUGETLB` and no special call.

Two layout versions are accepted: `1` (the default) and `2` (created with
`SHUTTLE_CREATE_STATS`). The version selects the only legal `data_offset`, so a
segment whose version and geometry disagree is `CORRUPT`, while a version this
binary does not know at all is `BAD_VERSION` — the two verdicts stay distinct.

- Blocking: bounded. Spins up to a 5 s deadline waiting for creator init.
- Errors (via `*err`): `INVALID_ARGS` (`name` NULL or `name[0] != '/'`),
  `NOT_FOUND` (no such segment), `CORRUPT` (`fstat` failure, segment smaller
  than a v1 header, or header geometry fails validation — including a
  `data_offset` that does not match the claimed version), `SYS` (`mmap`
  failure), `INIT_TIMEOUT` (creator never published readiness within 5 s),
  `BAD_MAGIC`, `BAD_VERSION` (layout version not 1 or 2).

### shuttle_close

```c
void shuttle_close(shuttle_channel* ch);
```

Unmap the segment and free the local handle (and any producer/consumer state it
created). The named shm object survives — use `shuttle_unlink` to remove it.
NULL-safe. Never fails.

### shuttle_unlink

```c
int shuttle_unlink(const char* name);
```

Remove the named object. Existing mappings remain valid until closed
(POSIX unlink semantics). Like `shuttle_open`, it probes both namespaces — the
shm object first, then the hugetlbfs file — so a hugetlb-backed channel is
removed by the same call, with the same name.

- Returns: `SHUTTLE_OK`, `INVALID_ARGS` (`name` NULL or `name[0] != '/'`),
  `NOT_FOUND` (no such object), `SYS` (other failure).

### shuttle_write

```c
int shuttle_write(shuttle_channel* ch, const void* data, size_t len, int flags);
```

Copy path: frame and enqueue a single message of `len` bytes. Binds the handle
as producer on first use.

- Blocking (`flags == 0`): brief adaptive spin, then park until space. Returns
  `SHUTTLE_OK` or `PEER_DEAD`.
- Non-blocking (`flags & SHUTTLE_NONBLOCK`): try-semantics; returns
  `WOULD_BLOCK` instead of parking.
- Lossy (`flags & SHUTTLE_DROP_NEWEST`): try-semantics *and* a drop policy;
  returns the positive `SHUTTLE_DROPPED` instead of `WOULD_BLOCK`. See
  **Drop-newest backpressure policy** below.
- Errors: `INVALID_ARGS` (`ch` NULL, or `data` NULL with `len != 0`),
  `MSG_TOO_LARGE` (`len > max_payload`; checked fail-fast, never parks),
  `WOULD_BLOCK` (non-blocking, no contiguous space now), `PEER_DEAD` (blocked
  wait aborted — consumer heartbeat went stale), `SYS`.

### shuttle_read

```c
long shuttle_read(shuttle_channel* ch, void* out, size_t cap, int flags);
```

Copy path: dequeue the next message into `out`, up to `cap` bytes. Binds the
handle as consumer on first use.

- Returns the payload length (`>= 0`) on success.
- Blocking / non-blocking as for `shuttle_write`. `SHUTTLE_DROP_NEWEST` is
  **not** valid here and returns `INVALID_ARGS` (there is nothing to drop on a
  read; see the policy section).
- If the waiting message is larger than `cap`, returns `MSG_TOO_LARGE` **and the
  message stays queued** (not consumed) — retry with a larger buffer.
- Errors: `INVALID_ARGS` (`ch` NULL, or `out` NULL with `cap != 0`),
  `MSG_TOO_LARGE`, `WOULD_BLOCK` (non-blocking, empty), `PEER_DEAD` (blocked
  wait aborted — producer heartbeat went stale), `CORRUPT` (framed length in
  the segment is impossible — never hands out an out-of-bounds span), `SYS`.

### shuttle_acquire_write

```c
int shuttle_acquire_write(shuttle_channel* ch, void** ptr, size_t len, int flags);
```

Zero-copy borrow, producer side: reserve a **contiguous** writable span of `len`
bytes and return a pointer to it in `*ptr`. Nothing is published until
`shuttle_commit_write`. At most one outstanding reservation per handle. The
reservation is process-local: a producer that dies mid-reservation leaves no
shared-state inconsistency.

- Blocking / non-blocking as for `shuttle_write`. `SHUTTLE_DROP_NEWEST` is
  **not** valid here and returns `INVALID_ARGS`: a reservation has no payload
  yet, so there is nothing a drop could discard.
- Errors: `INVALID_ARGS` (`ch` or `ptr` NULL, a reservation is already
  outstanding, or `flags` contains `SHUTTLE_DROP_NEWEST`), `MSG_TOO_LARGE`
  (`len > max_payload`), `WOULD_BLOCK` (non-blocking, no contiguous span now),
  `PEER_DEAD`, `SYS`.

### shuttle_commit_write

```c
int shuttle_commit_write(shuttle_channel* ch, size_t actual_len);
```

Publish `actual_len` bytes (`<= len` reserved) of the outstanding reservation.
This is the release edge that makes the payload visible to the consumer.

- Errors: `INVALID_ARGS` (`ch` NULL, no producer role, no active reservation, or
  `actual_len > len` reserved), `SYS`.

### shuttle_acquire_read

```c
int shuttle_acquire_read(shuttle_channel* ch, const void** ptr, size_t* len, int flags);
```

Zero-copy borrow, consumer side: borrow the next message in place. `*ptr` points
into the shared segment; `*len` is the payload length. At most one outstanding
borrow per handle; must be paired with `shuttle_release_read`.

- Blocking / non-blocking as for `shuttle_read`. `SHUTTLE_DROP_NEWEST` is
  **not** valid here and returns `INVALID_ARGS`.
- The borrowed payload is guaranteed contiguous (see Zero-copy borrow rules).
- Errors: `INVALID_ARGS` (`ch`, `ptr`, or `len` NULL, or `flags` contains
  `SHUTTLE_DROP_NEWEST`), `WOULD_BLOCK` (non-blocking, empty), `PEER_DEAD`,
  `CORRUPT`, `SYS`.

### shuttle_release_read

```c
int shuttle_release_read(shuttle_channel* ch);
```

Release the outstanding read borrow, freeing its bytes for producer reuse. This
is the release edge; the borrowed pointer is invalid afterward.

- Errors: `INVALID_ARGS` (`ch` NULL, no consumer role, or no active borrow),
  `SYS`.

### shuttle_keepalive

```c
void shuttle_keepalive(shuttle_channel* ch);
```

Bump this side's heartbeat without transferring data. Sparse-traffic peers must
call this (or raise the staleness threshold) so the other side does not declare
them dead. NULL-safe. Bumps whichever roles (producer/consumer) the handle has
already taken. Never fails.

### shuttle_get_stats

```c
typedef struct shuttle_stats {
    uint64_t msgs_written;
    uint64_t bytes_written;
    uint64_t msgs_dropped;
    uint64_t msgs_read;
    uint64_t bytes_read;
} shuttle_stats;

int shuttle_get_stats(shuttle_channel* ch, shuttle_stats* out);
```

Copy the segment's counters into `*out` (v1.2, additive). Any handle on the
segment may call it — producer, consumer, or a third process that merely opened
it — because the counters live in the segment, not in the handle.

- Returns `SHUTTLE_OK`, `INVALID_ARGS` (`ch` or `out` NULL), or `NO_STATS` (the
  segment was not created with `SHUTTLE_CREATE_STATS`).
- Never blocks and never touches the park mutex.
- See **Statistics** for what each field counts and for the exactness rules.

---

## Error codes

| Value | Name                          | Meaning / when it occurs |
|-------|-------------------------------|--------------------------|
| **1** | **`SHUTTLE_DROPPED`**         | **Not an error.** `shuttle_write` with `SHUTTLE_DROP_NEWEST`: the ring could not take the message, so it was dropped. The only positive return in the ABI; unreachable without the flag. |
| 0     | `SHUTTLE_OK`                  | Success. |
| -1    | `SHUTTLE_ERR_INVALID_ARGS`    | NULL handle/pointer, malformed name, zero size, misuse (double reservation/borrow, wrong role). |
| -2    | `SHUTTLE_ERR_NAME_TOO_LONG`   | Name exceeds the platform shm-name limit (macOS 30, Linux 254). |
| -3    | `SHUTTLE_ERR_EXISTS`          | `create`: a segment with that name already exists. |
| -4    | `SHUTTLE_ERR_NOT_FOUND`       | `open`/`unlink`: no such segment. |
| -5    | `SHUTTLE_ERR_SYS`             | Unexpected syscall failure (inspect `errno`) or a caught exception. |
| -6    | `SHUTTLE_ERR_BAD_MAGIC`       | `open`: segment magic word mismatch. |
| -7    | `SHUTTLE_ERR_BAD_VERSION`     | `open`: layout version mismatch (distinct from magic). |
| -8    | `SHUTTLE_ERR_CAPACITY_TOO_SMALL` | `create`: `capacity_bytes < max_payload_bytes + 8`. |
| -9    | `SHUTTLE_ERR_INIT_TIMEOUT`    | `open`: creator never published readiness within 5 s. |
| -10   | `SHUTTLE_ERR_CORRUPT`         | Header geometry fails validation, or a framed length in the data region is impossible. |
| -11   | `SHUTTLE_ERR_MSG_TOO_LARGE`   | Write payload `> max_payload`; or copy-read buffer smaller than the queued message (message stays queued). |
| -12   | `SHUTTLE_ERR_WOULD_BLOCK`     | Non-blocking op cannot proceed right now (full on write, empty on read). |
| -13   | `SHUTTLE_ERR_PEER_DEAD`       | A blocking wait aborted because the peer's heartbeat went stale. |
| -14   | `SHUTTLE_ERR_NO_HUGEPAGES`    | `create_ex` with `SHUTTLE_CREATE_HUGETLB_2MB`/`_1GB`: reserved huge pages could not be delivered (no hugetlbfs mount of that page size, no free pages, no permission, or a platform without hugetlbfs). Never a silent fallback to normal pages. |
| -15   | `SHUTTLE_ERR_NO_STATS`        | `shuttle_get_stats` on a segment created without `SHUTTLE_CREATE_STATS`. |

The C `#define`s are `static_assert`ed equal to the C++ `shuttle::Err` enum (and
`shuttle::kDropped` / the `shuttle::kOp*` flag bits) in the implementation, so
the two can never drift.

---

## Channel lifecycle

- **create / open**: exactly one creator (`shuttle_create[_ex]`, `O_CREAT|O_EXCL`)
  and any number of openers (`shuttle_open`). The channel is SPSC: one producer,
  one consumer. Roles are bound lazily — a handle becomes a producer on its
  first write/acquire-write, a consumer on its first read/acquire-read.
- **close**: unmaps and frees the local handle only; the named object persists.
- **unlink**: removes the named object; live mappings stay valid until closed.
- **Name constraints**: must begin with `/`, contain at least one further
  character (length `>= 2`), and not exceed the platform limit — **30 chars on
  macOS** (`PSHMNAMLEN` is 31 including the `/`; 30 avoids the documented
  off-by-one), **254 chars on Linux**. Over-limit names yield `NAME_TOO_LONG`.
- **Sizing rule**: `capacity_bytes >= max_payload_bytes + 8`. The `+8` is the
  frame header (an 8-byte little-endian length prefix on every message). This
  makes a permanently-unsatisfiable write impossible by construction, so
  blocking backpressure can never park the producer forever. Violation yields
  `CAPACITY_TOO_SMALL`.
- **One-shot sizing**: the segment is `ftruncate`d exactly once, at creation, on
  both platforms (macOS forbids re-truncating an shm object). It never grows.

---

## Backpressure

The default, and a v1 guarantee: **Shuttle applies backpressure and never drops
a message.** A full ring parks the producer (`flags == 0`) or reports
`WOULD_BLOCK` (`SHUTTLE_NONBLOCK`). Nothing queued is ever overwritten, and a
message the library accepted is a message the consumer will see.

### Drop-newest policy (`SHUTTLE_DROP_NEWEST`, v1.3)

```c
#define SHUTTLE_DROP_NEWEST 0x2   /* per-op flag, shuttle_write only */
#define SHUTTLE_DROPPED     1     /* positive, not an error, not SHUTTLE_OK */
```

For callers who would rather lose a sample than stall — telemetry, live video
frames, sensor feeds — `shuttle_write` accepts an opt-in lossy policy:

| `flags` on a **full** ring | Result |
|---|---|
| `0` | parks until space (or `PEER_DEAD`) — the default, unchanged |
| `SHUTTLE_NONBLOCK` | `SHUTTLE_ERR_WOULD_BLOCK`, message not written |
| `SHUTTLE_DROP_NEWEST` | `SHUTTLE_DROPPED`, message **discarded**, counted |
| `SHUTTLE_NONBLOCK \| SHUTTLE_DROP_NEWEST` | same as `SHUTTLE_DROP_NEWEST` (redundant, accepted) |

The rules, exactly:

- **Per call, never a mode.** The bit affects the one call it is passed on.
  There is no channel-wide setting, no create-flag, and no path by which a
  blocking or non-blocking write turns lossy on its own. Segments created by a
  dropping producer are ordinary segments.
- **It never parks.** `SHUTTLE_DROP_NEWEST` implies try-semantics, so it takes
  the same non-blocking path as `SHUTTLE_NONBLOCK` — including when the
  consumer is parked, wedged, or dead. It therefore never returns `PEER_DEAD`
  either: a dead consumer simply means the ring stays full and writes drop.
- **It drops the NEWEST — the message you just passed.** Nothing already queued
  is touched: no cursor moves, no bytes are overwritten, the consumer is not
  involved, and a drop is invisible to the peer. The consumer still sees a
  gap-free, in-order prefix of the messages that were accepted.
- **Oversize is still an error.** `len > max_payload` returns
  `MSG_TOO_LARGE`, not `SHUTTLE_DROPPED`. A payload that could never fit in any
  ring state is a caller bug, not backpressure, and is not swallowed silently.
- **Write-only.** `shuttle_read`, `shuttle_acquire_read` and
  `shuttle_acquire_write` return `INVALID_ARGS` if the bit is set — there is
  nothing to drop on those paths, so the flag is refused rather than ignored.
- **Counted where counters exist.** Each drop bumps `msgs_dropped` on a
  `SHUTTLE_CREATE_STATS` segment. On a v1 segment the drop still happens; there
  is simply nowhere to record it (`shuttle_get_stats` keeps returning
  `NO_STATS`). `msgs_written` / `bytes_written` never count a dropped message.

```c
int rc = shuttle_write(ch, frame, len, SHUTTLE_DROP_NEWEST);
if (rc < 0) return rc;                  /* real error */
if (rc == SHUTTLE_DROPPED) ++dropped;   /* consumer is behind; frame is gone */
```

### Freshness without dropping: drain-to-latest

`SHUTTLE_DROP_NEWEST` keeps the *oldest* queued messages and throws away the
newest. A consumer that wants the *newest* wants the opposite, and it does not
need a library feature — it needs a loop. Drain everything available and keep
the last message:

```c
const void* p; size_t n;
int have = 0;
while (shuttle_acquire_read(ch, &p, &n, SHUTTLE_NONBLOCK) == SHUTTLE_OK) {
    memcpy(latest, p, n);               /* copy: the borrow ends at release */
    latest_len = n; have = 1;
    shuttle_release_read(ch);           /* release each one, in order */
}
if (have) process(latest, latest_len);  /* only the freshest survives */
```

This is the **recommended freshness pattern**, and it is strictly better than
an overwrite-oldest ring would be: the consumer decides what is stale (it knows
what it is behind on; the producer does not), the ring keeps applying
backpressure if the consumer stops draining, and every memory-ordering
invariant is untouched — the consumer is only doing what a consumer always
does. See `docs/ROADMAP.md` for why overwrite-oldest is rejected on the
producer side.

---

## Zero-copy borrow rules

- **Pointer validity window**: a pointer from `shuttle_acquire_read` (or `*ptr`
  from `shuttle_acquire_write`) is valid only until the matching release
  (`shuttle_release_read` / `shuttle_commit_write`). After release the bytes may
  be reused by the peer; dereferencing the stale pointer is undefined.
- **Mandatory release**: every acquire must be paired with exactly one release.
  At most one borrow/reservation may be outstanding per handle; a second acquire
  before releasing the first returns `INVALID_ARGS`.
- **Contiguity guarantee**: the ring is a **BipBuffer** — reservations are
  whole-unit, so a payload is never split across the physical wrap. A borrowed or
  reserved span is always one contiguous run of bytes, safe to expose directly as
  a slice / `memoryview` / NumPy array with no copy and no stitching.

---

## Segment layout

A control header (a cache-line multiple), followed by the data region at byte
offset `data_offset`. Everything in the segment is fixed-width and referenced by
**byte offset from the segment base**, never by pointer — each process maps at a
different address.

Header, in order:

1. **Cold identity block** (written once by the creator, immutable after
   `init_state` is published): `magic`, `version`, `flags`, `data_offset`,
   `data_capacity`, `max_payload`, then the `init_state` publication atomic.
2. **BipBuffer cursors**, each on its own 128-byte line, each strictly
   single-writer: `write` (producer), `watermark` (producer), `read` (consumer).
   If `write >= read`, valid data is `[read, write)`; else `[read, watermark)`
   then `[0, write)`.
3. **Park flags**: `producer_waiting`, `consumer_waiting`.
4. **Heartbeats**: `producer_heartbeat`, `consumer_heartbeat`.
5. **Park/wake primitives** (off the hot path): a pshared mutex and two
   condvars. **End of layout version 1** — `data_offset` for a v1 segment is
   exactly the size of everything above.
6. **Statistics counters** — *layout version 2 only*: a producer-owned line
   (`msgs_written`, `bytes_written`, `msgs_dropped`) and a consumer-owned line
   (`msgs_read`, `bytes_read`). In a v1 segment these bytes are not header at
   all; they are the first bytes of the data region, and nothing ever touches
   them there.

**Flags-bits contract**: the creator writes the entire `flags` word **once**, in
the cold identity block, before the release-store that publishes `init_state`;
it is immutable thereafter. Openers must **ignore unknown bits** — `flags` is an
additive extension point, so new bits carry no version bump (an old opener
simply does not act on a bit it does not recognize). Correspondingly the creator
masks `create_flags` down to the bits it actually implements, so an unknown (or
merely reserved-and-unimplemented) bit is never persisted into a segment.
Recorded bits: `SHUTTLE_CREATE_HUGEPAGES` = `0x1`,
`SHUTTLE_CREATE_HUGETLB_2MB` = `0x2`, `SHUTTLE_CREATE_HUGETLB_1GB` = `0x4`,
`SHUTTLE_CREATE_STATS` = `0x8`. The hugetlb bits are persisted but
informational — an opener takes no action on them, because the segment's
location already determines its page size.

**Version gate**: `flags` is ignorable, `version` is not. A version selects the
physical size of the header, so an opener that does not recognize one cannot
know where the data region starts and must refuse the segment. That is why
`SHUTTLE_CREATE_STATS` is the one flag that also bumps the layout version — and
why a binary built before v1.2 reports `BAD_VERSION` when handed a v2 segment,
rather than silently ignoring the stats bit. Plain `shuttle_create` writes the
v1 layout, byte for byte as before.

---

## Statistics

Opt in at creation with `shuttle_create_ex(..., SHUTTLE_CREATE_STATS, ...)`;
read with `shuttle_get_stats`. Five counters live in the segment, so either peer
— or any other process that opens the segment — observes the same values.

| Field | Written by | Counts |
|-------|-----------|--------|
| `msgs_written`  | producer | messages published (counted at the commit that makes a message visible). |
| `bytes_written` | producer | **payload** bytes published. |
| `msgs_dropped`  | producer | messages discarded by a `SHUTTLE_DROP_NEWEST` write (v1.3), and nothing else. `0` unless the producer opts into that policy — a full ring otherwise blocks or returns `WOULD_BLOCK`. Dropped messages are **not** counted in `msgs_written` / `bytes_written`. |
| `msgs_read`     | consumer | messages released (counted at the release that frees the message's bytes). |
| `bytes_read`    | consumer | **payload** bytes released. |

- **Payload bytes, not frame bytes**: the 8-byte length prefix is transport
  overhead and is excluded on both sides, so `bytes_written` / `bytes_read` are
  directly comparable to the lengths the caller passed and to each other.
- **Cost**: each counter has exactly one writer and is updated with a relaxed
  load + relaxed store (never a read-modify-write) on a cache line that side
  already owns — the same discipline as the heartbeats. The producer's and
  consumer's counters sit on separate lines, so they never contend.
- **Exactness**: every field is individually exact and monotonic. The five are
  *not* sampled atomically, and the counters are deliberately not ordered
  against the data path, so a snapshot taken while traffic flows may show
  `msgs_read` trailing `msgs_written`, or a counter lagging a cursor. Once both
  sides are quiescent the totals are exact.
- **Without the flag**: `shuttle_get_stats` returns `NO_STATS`, and the data
  path never reads or writes those addresses (they are payload). This includes
  the drop counter: `SHUTTLE_DROP_NEWEST` works exactly the same on a v1
  segment, the drops are simply not counted anywhere. A producer that needs the
  drop rate must create the channel with `SHUTTLE_CREATE_STATS`.

---

## Liveness

Each side maintains a monotonically increasing heartbeat, bumped on every
successful operation, on every park iteration, and via `shuttle_keepalive`. A
**blocked** waiter samples the peer's heartbeat at each park timeout (100 ms
intervals); if it has not advanced within the staleness threshold (default 5 s,
process-local policy — not stored in the segment), the wait aborts with
`SHUTTLE_ERR_PEER_DEAD` instead of blocking forever.

**Documented caveat**: a peer that is alive but makes no Shuttle calls at all is
indistinguishable from a dead one. Applications with sparse traffic must call
`shuttle_keepalive` periodically (or raise the staleness threshold) to avoid a
spurious `PEER_DEAD`. The heartbeat only fires on the *blocking* paths;
non-blocking callers never see `PEER_DEAD`.

---

## Language bindings

### Python (cffi, ABI mode)

Open the library with `ffi.dlopen`, declare the subset you use with `ffi.cdef`,
then drive the borrow path. The payload is exposed **zero-copy** as a
`memoryview` over `ffi.buffer` wrapping the borrowed pointer — no `bytes()` copy
is ever taken; verification indexes the view in place:

```python
mv = memoryview(ffi.buffer(ptr, length))   # no copy
```

Because Python cannot enforce the borrow lifetime, the reference binding wraps
the view in a guard object that **invalidates on `release_read`**: the view is
`.release()`d and further access raises `RuntimeError`. Shuttle cannot prevent a
use-after-release, but it can refuse to serve a stale one.

For numeric payloads, wrap the same memoryview with NumPy — still zero-copy, no
intermediate buffer:

```python
arr = np.frombuffer(mv, dtype=np.uint8)    # a view over the borrow, no copy
```

Release before (or without) touching `arr` afterward; it aliases segment memory
that the producer may overwrite once released.

### Rust

The reference wrapper (`tests/ffi/rust/shuttle.rs`) hand-writes the `extern "C"`
declarations against the frozen header and encodes the borrow lifetime in the
type system. `acquire_read(&mut self)` borrows the `Consumer` mutably and returns
`Borrowed<'_>`; the slice from `as_slice()` is tied to the `Borrowed`'s lifetime,
and `Borrowed`'s `Drop` performs `shuttle_release_read`. Consequences enforced
**at compile time**:

- A slice cannot outlive the release — use-after-release is a borrow-check error
  (`E0597`), not a runtime fault.
- No second acquire while a borrow is outstanding — the `Consumer` stays mutably
  borrowed until the `Borrowed` drops.
