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
`SHUTTLE_DROPPED` return, both opt-in. v1.4 adds the
`SHUTTLE_CREATE_ALIGNED_SPANS` flag, the three path-typed lifecycle symbols
`shuttle_create_file` / `shuttle_open_file` / `shuttle_unlink_file`, and the
read-only lookahead `shuttle_peek_next`, all additive. The ABI version stays
`1`.

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
#define SHUTTLE_CREATE_HUGEPAGES     0x1
#define SHUTTLE_CREATE_HUGETLB_2MB   0x2
#define SHUTTLE_CREATE_HUGETLB_1GB   0x4
#define SHUTTLE_CREATE_STATS         0x8
#define SHUTTLE_CREATE_ALIGNED_SPANS 0x10
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
- `SHUTTLE_CREATE_ALIGNED_SPANS`: make every payload span start on a system
  page. See **Page-aligned payload spans** below — this one changes the
  **framing**, which raises the `CAPACITY_TOO_SMALL` floor and means older
  binaries cannot open the segment.
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

#### Page-aligned payload spans (`SHUTTLE_CREATE_ALIGNED_SPANS`, v1.4)

Every payload the channel carries starts on a **system-page boundary**, so the
pointer from `shuttle_acquire_read` — or from `shuttle_acquire_write` — can be
handed straight to an API that demands page-aligned host memory:
`cudaHostRegister`, Metal's `newBufferWithBytesNoCopy`, a driver ioctl that maps
a user range. Without the flag those calls need a bounce buffer, which is
exactly the copy the rest of this library exists to avoid.

No new function comes with it. The acquire/read entry points already return the
span pointers; the flag only changes where they land.

**Framing.** Each message becomes:

```
[8-byte length][pad to page][payload][pad to page]
 ^ frame start                ^ frame start + page = the pointer you get
```

so the frame stride is `page + round_up(len, page)`. Frame starts stay
page-aligned across wraps because every stride is a whole number of pages, the
data region itself starts page-aligned (`data_offset` is rounded up to a page),
and an early wrap restarts at offset 0.

**The alignment unit is the system page** — `sysconf(_SC_PAGESIZE)`, 4096 on
x86-64 and 16384 on Apple Silicon. Because Shuttle is same-host IPC, that is a
host constant: every process mapping a given segment computes the same value, so
neither side has to be told. It stays the system page even on a hugetlbfs-backed
segment (`0x10|0x2` is legal): the APIs above want ordinary page granularity, and
rounding every message to 2 MB or 1 GB would be a far worse trade.

**Cost: internal fragmentation, stated as a number.** Padding per message is
`(page - 8)` for the tail of the header page, plus up to `page - 1` for the
payload's rounding — a worst case of **`2*page - 8 - 1` = 8183 bytes** at a
4 KiB page, hit when `len % page == 1`. That is 0.016% of a 50 MB payload and
100x the size of a 64-byte one, so the flag is for channels carrying pages, not
packets. Capacity is spent on padding too, which is why
`CAPACITY_TOO_SMALL` now means `capacity < page + round_up(max_payload, page)`
on an aligned channel.

**Cost: older binaries cannot open the segment.** Unknown flag bits are normally
ignored (see **Segment layout**), but an ignorer would misparse every frame
here, so this bit is deliberately backed by geometry: an aligned segment's
`data_offset` is page-rounded (4096, not 1280/1536), and a binary built before
v1.4 rejects that at its geometry check with **`SHUTTLE_ERR_CORRUPT`**.

`CORRUPT` rather than `BAD_VERSION` is the accurate verdict and is chosen on
purpose. No layout *version* was added — the header's shape and every field
offset are unchanged — so there is nothing for a version check to report. What
genuinely disagrees is the offset, against the layout rule that binary knows,
and `CORRUPT` is what this library has always returned for that. Deciding to set
the flag is therefore a decision about **which peers can attach**, exactly like
`SHUTTLE_CREATE_STATS` (which reports `BAD_VERSION` for the same class of
reason: there, the header shape really did change).

**Combining flags.** All orthogonal:

| Combination | Result |
|---|---|
| `0x10` | v1 header, `data_offset` = `round_up(1280, page)` = 4096 |
| `0x18` (`\|STATS`) | v2 header, `data_offset` = `round_up(1536, page)` = 4096; byte counters still count **payload** bytes, never the padded stride |
| `0x12` / `0x14` (`\|HUGETLB_*`) | hugetlbfs-backed, alignment unit stays the **system** page |
| `0x11` (`\|HUGEPAGES`) | THP advice, unchanged and still advisory |

### shuttle_create_file / shuttle_open_file / shuttle_unlink_file

```c
#define SHUTTLE_CREATE_FILE_BACKED 0x20   /* persisted, informational */
shuttle_channel* shuttle_create_file(const char* path, size_t capacity_bytes,
                                     size_t max_payload_bytes,
                                     uint32_t create_flags, int* err);
shuttle_channel* shuttle_open_file(const char* path, int* err);
int shuttle_unlink_file(const char* path);
```

The same three lifecycle operations against a segment that lives in a **file**
instead of in the POSIX shm namespace (v1.4, additive: three new symbols, no
existing signature or semantic touched). Capacity is then bounded by the
filesystem rather than by RAM or by `/dev/shm`'s tmpfs limit, and the OS page
cache decides which pages are resident — a 256 GB channel on a 16 GB box is an
ordinary thing to create. See **File-backed channels** below for the backing's
own rules: the crash story, stale files, and the durability non-goal.

`shuttle_close`, and every read/write/stats entry point, are shared: a handle is
a handle whatever backs it.

- **The path is the identifier.** It must be **absolute** (`path[0] == '/'`);
  NULL, `""`, and relative paths are all `INVALID_ARGS`, because a channel's
  identity must not depend on which directory each peer was started in. Paths
  are not decorated in any way — the file is exactly where you said.
- **No shm name limit.** The 30/254-character shm rules do not apply; the
  filesystem's own limit does, and it surfaces as `NAME_TOO_LONG`.
- **`create_flags`** takes the same bits as `shuttle_create_ex` — `STATS`,
  `ALIGNED_SPANS`, `HUGEPAGES` are all legal and compose — with one exception:
  `SHUTTLE_CREATE_HUGETLB_2MB` / `_1GB` are **rejected** with `INVALID_ARGS`. A
  hugetlbfs backing and a caller-chosen path name two different segments;
  honoring one would mean ignoring the other, and neither may be dropped
  silently. `SHUTTLE_CREATE_FILE_BACKED` (`0x20`) is set for you.
- **Errors** (`shuttle_create_file`, via `*err`): `INVALID_ARGS` (path rules
  above, zero size, a hugetlb bit), `CAPACITY_TOO_SMALL` (unchanged — the FR-4
  rule is not relaxed by the backing), `EXISTS` (the file is already there; it
  is **never** truncated — see the stale-file recipe), `NOT_FOUND` (the parent
  directory does not exist), `NAME_TOO_LONG`, `SYS`.
- **Errors** (`shuttle_open_file`): `INVALID_ARGS`, `NOT_FOUND`, then exactly
  what `shuttle_open` reports — `INIT_TIMEOUT`, `BAD_MAGIC`, `BAD_VERSION`,
  `CORRUPT`, `SYS`. The validation is not weakened for files.
- **Errors** (`shuttle_unlink_file`): `SHUTTLE_OK`, `INVALID_ARGS`, `NOT_FOUND`,
  `SYS`. Live mappings survive, as with `shuttle_unlink`.

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
`SHUTTLE_CREATE_STATS`). The version **and the flags** select the only legal
`data_offset` — `SHUTTLE_CREATE_ALIGNED_SPANS` rounds it up to a page — so a
segment whose header and geometry disagree is `CORRUPT`, while a version this
binary does not know at all is `BAD_VERSION`; the two verdicts stay distinct.
This is the check that makes an aligned segment unopenable by a pre-v1.4 binary,
which is intended: see **Page-aligned payload spans**.

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

### shuttle_peek_next

```c
int shuttle_peek_next(shuttle_channel* ch, size_t* len_out);
```

**Peeking at the next message** (v1.4, additive). Report whether the next
**un-borrowed** message is committed, and its payload length in `*len_out`.
Read-only: it never blocks, never copies, moves no cursor, and neither creates
nor disturbs a borrow. There is no `flags` word — peeking is a question, and the
answer is always available immediately.

Valid with **0 or 1 borrows outstanding**, and the second case is why it exists.
Borrows here are strictly release-before-acquire: while you hold message N,
`shuttle_acquire_read` hands you N again, not N+1 (it is idempotent). So without
this call there is no way to learn that N+1 has arrived — or how big it is —
until you have already committed to releasing N:

```c
const void* p; size_t n;
shuttle_acquire_read(ch, &p, &n, 0);
size_t next = 0;
if (shuttle_peek_next(ch, &next) == SHUTTLE_OK) {
    /* N+1 is committed and is `next` bytes: size the destination now, decide
       whether to batch, keep a pipeline full, or start a DMA for it. */
}
consume(p, n);
shuttle_release_read(ch);
```

- `SHUTTLE_OK` — `*len_out` is the next message's payload length.
- `SHUTTLE_ERR_WOULD_BLOCK` — nothing beyond the current borrow is committed.
  **Not an error**: it is the ordinary "not here yet". The bindings map it to
  `None` (Python) and `Ok(None)` (Rust) for exactly that reason.
- Errors: `INVALID_ARGS` (`ch` or `len_out` NULL), `CORRUPT` (the frame header
  holds a length the producer could never have written — the same verdict the
  acquire path reaches on the same bytes, reached through the same guards).

**Freshness.** A length answer stays true: the producer never un-publishes. A
`WOULD_BLOCK` answer may be stale the instant it is given, because the consumer
may have missed a just-landed publication. That is one-sided by construction —
peek can only under-report, never over-report, since `write` is the only
publisher of committed data — and it is the same staleness every non-blocking
call in this ABI already has.

Calling it binds this handle's **consumer** role, exactly as `shuttle_read` and
`shuttle_acquire_read` do.

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
  first write/acquire-write, a consumer on its first read/acquire-read. The
  path-typed trio (`shuttle_create_file` / `shuttle_open_file` /
  `shuttle_unlink_file`, v1.4) is the same lifecycle against a segment that
  lives in a file; everything in this section applies to it except the shm
  name rules, which a path replaces. See **File-backed channels**.
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

## File-backed channels (v1.4)

`shuttle_create_file` / `shuttle_open_file` / `shuttle_unlink_file` put a
channel's segment in an ordinary file at an absolute path instead of a POSIX shm
object. One sentence for why: **an shm segment is bounded by RAM (and by
`/dev/shm`), a file is bounded by the filesystem**, so a channel can be far
larger than physical memory and the OS page cache decides what is resident. That
is the shape a weights-streaming or KV-cache workload wants — a large backing
store consumed through a small resident window.

Everything else is deliberately identical. The segment a file-backed create
writes is byte-for-byte the segment an shm create writes: same header, same
`data_offset`, same framing, same cursors, same park block. Both peers `mmap`
the object `MAP_SHARED` and everything above this section applies unchanged.

**Three namespaces now.** A default segment is an shm object (`/name`), a
hugetlb segment is a file the library names on a hugetlbfs mount, and a
file-backed segment is a file **you** named. The first two are found by
`shuttle_open`'s probe; the third is not part of that probe and never will be —
its identifier is a path, not a name, and the two cannot be told apart by
inspection. That is why these are separate symbols rather than a flag on
`shuttle_create_ex`: the caller always knows which kind of identifier it holds,
and saying so by calling a different function makes it a compile-time fact.

### Crash story — re-argued for this backing, and measured

The three mechanisms, each re-examined rather than assumed to carry over:

- **Heartbeats work identically, by construction.** They are two atomics in the
  segment, and the segment is a `MAP_SHARED` mapping either way. Nothing about
  a heartbeat touches the object it is backed by. A blocking wait therefore
  still aborts with `PEER_DEAD` when the peer's heartbeat goes stale, at both
  crash kill points. **Observed** (`tests/filebacked_test.cpp`, cases c and d):
  a producer SIGKILLed mid-reservation, and one SIGKILLed while holding the park
  mutex, each left the survivor's blocked read returning `SHUTTLE_ERR_PEER_DEAD`
  ~2.5 s after parking against a 1.5 s threshold. No deadlock, and no phantom
  data from the dead peer's uncommitted reservation.

- **Robust-mutex recovery works identically on Linux — measured, not inferred.**
  The concern was real: `PTHREAD_MUTEX_ROBUST` is implemented by a per-thread
  robust list the kernel walks at task exit, and it was worth confirming that
  the list works on a mutex living in a *file* mapping rather than a tmpfs one.
  It does. With a peer SIGKILLed while owning the park mutex, a raw
  `pthread_mutex_lock` on that mutex **returns `EOWNERDEAD` (130)** — observed
  directly in `tests/filebacked_test.cpp` case (e), which takes the lock raw
  precisely so the seam's recovery cannot hide the code being measured. After
  the standard repair (`pthread_mutex_consistent`, then unlock) the mutex is
  fully serviceable — lock/unlock cycles, a condvar `timedwait`, and further
  transfers over the same mapping all behave normally. The same test also runs
  the deliberately buggy recovery (unlock *without* `pthread_mutex_consistent`)
  and confirms it leaves the mutex `ENOTRECOVERABLE`, so the positive result
  cannot be a test that passes for the wrong reason.

  **The file-backed crash story on Linux is therefore the shm crash story, with
  nothing subtracted.**

- **macOS is unchanged and still best-effort.** There are no robust mutexes
  there for any backing, and the park path holds nothing
  (`os_sync_wait_on_address`), so the heartbeat is the whole guarantee — exactly
  as for shm. Nothing about file backing makes that better or worse.

### Stale files, and the recovery recipe

This is the one genuinely new failure mode, and it comes from the good property:
**a file survives a reboot; an shm object does not.** So a file-backed segment
can be left over from a previous boot, complete with a valid header, plausible
cursors, and heartbeats from processes that no longer exist.

What the library does about it:

- `shuttle_open_file` applies **exactly** the checks `shuttle_open` applies —
  the size floor, the bounded (5 s) wait for the creator's readiness
  publication, then full header validation. A file holding anything else fails
  one of them: garbage that never publishes readiness costs 5 s and returns
  `INIT_TIMEOUT`; a wrong or damaged header returns `BAD_MAGIC`,
  `BAD_VERSION`, or `CORRUPT`. Nothing is trusted because it was in a file you
  named.
- `shuttle_create_file` **refuses an existing file** with `EXISTS` and leaves it
  untouched. It never truncates: silently reusing a file could pull a live
  peer's segment out from under it, and after a reboot there is no way to tell
  the two cases apart from inside the process.

The recipe, therefore — an explicit operator action, not an automatic one:

```c
int rc = shuttle_unlink_file("/var/lib/app/chan.seg");  /* deliberate */
if (rc != SHUTTLE_OK && rc != SHUTTLE_ERR_NOT_FOUND) return rc;
ch = shuttle_create_file("/var/lib/app/chan.seg", cap, maxp, 0, &err);
```

Startup code that wants to be self-healing should unlink-then-create as above,
and should do it only where it *owns* the path. A supervisor that cannot be sure
no peer is running must not: the unlink is what makes it safe, and unlinking a
file another process still maps leaves that process running against a segment
nobody else can reach.

### Durability is an explicit non-goal

**The library never calls `msync`, and never will on this path.** The file is a
transport medium, not a database. What is on disk after a crash is whatever the
kernel had written back, at whatever granularity it chose, with no ordering
guarantee of any kind between the payload bytes and the cursors that describe
them. A segment recovered off disk and reopened is *not* a queue you can resume
— it is bytes whose consistency nobody promised. Treat a file-backed segment
exactly like an shm one: the channel's contents mean something only while both
peers are alive.

If a persistence use case ever lands, it needs its own design (ordering,
barriers, a recovery protocol), not an `msync` call bolted onto this one.

### Residency, honestly

The page cache owns residency, which cuts both ways: pages are faulted in on
first touch and written back and reclaimed under pressure, so a channel far
larger than RAM works — but a ring the producer has just marched through is
*dirty file pages*, and those stay resident until the kernel decides otherwise.
Measured while streaming 512 MB through a 256 MB file-backed channel with at
most 8 messages in flight, this process's RSS went from 8 MB to 265 MB (ASan
test build, `tests/filebacked_test.cpp` case b, on an otherwise idle box): the
whole ring, because the whole ring had just been written. The guarantee is that
the kernel *can* reclaim it, not that it will have done so at any given moment.
Size the channel for the working set you want, not for the largest file you can
create.

### Prefetch on file-backed channels (`MADV_WILLNEED`, automatic)

There is nothing to switch on and nothing to call. On a **file-backed** channel
— and only there — the consumer advises the kernel about the pages it is about
to read: `posix_madvise(POSIX_MADV_WILLNEED)` on Linux, `madvise(MADV_WILLNEED)`
on macOS, over the **committed-but-unread** region beyond the borrow it is
currently holding. Two points, both consumer-side: on a successful acquire (so
the next message's pages are on their way in while the current one is being
worked on) and immediately before parking.

The reason it is scoped to this backing: on an shm segment there is nothing to
bring in — the pages are already memory. On a file, an unread page can be a disk
read, and paying for it synchronously in the middle of a borrow is exactly the
stall this hint exists to avoid. That shape is the point of the backing: a large
store on disk, consumed through a small resident window.

Properties worth being precise about:

- **Advisory, and the return value is deliberately ignored.** WILLNEED is a hint
  the kernel may refuse, defer, or complete asynchronously. Nothing in the
  library makes a decision from it, and a channel behaves identically if every
  call is a no-op. Same contract as `SHUTTLE_CREATE_HUGEPAGES`.
- **Zero-cost on the default path.** The gate is a `bool` resolved once at
  consumer construction from the persisted `0x20` flag, so an shm channel pays
  one perfectly-predictable branch per acquire and never reaches the advisory
  code (`Consumer::prefetching()` exposes the gate, and the test suite asserts
  it is `false` for shm and `true` for a file).
- **Only committed data is ever named.** The free part of the ring is never
  advised — asking the kernel to fetch pages the producer is about to overwrite
  would be work for nothing. In the wrapped state that means two runs:
  `[read+borrow, watermark)` and `[0, write)`.
- **No new ordering.** The hooks read the same cursors with the same memory
  orders the read path already uses and store nothing; the
  memory-ordering contract in `include/shuttle/spsc.hpp` says why that adds no
  edge.
- **Windows:** no-op, like the file backing itself.

### Combining flags

| Combination | Result |
|---|---|
| file only (`0x20`) | v1 header, ordinary framing, segment in your file |
| `\|STATS` (`0x28`) | v2 header with counters; identical semantics |
| `\|ALIGNED_SPANS` (`0x30`) | page-aligned payload spans out of a file mapping |
| `\|HUGEPAGES` (`0x21`) | THP advice, still advisory and still a harmless no-op where the kernel declines |
| `\|HUGETLB_2MB` / `_1GB` | **`INVALID_ARGS`** — a hugetlbfs backing and a path name two different segments |

### The `0x20` asymmetry

`SHUTTLE_CREATE_FILE_BACKED` is the one create-flag you never pass to
`shuttle_create_ex`. Selecting this backing means supplying a *path*, and
`shuttle_create_ex`'s parameter is an shm *name* with shm name rules — there is
nowhere to put one. So the backing is chosen by **calling
`shuttle_create_file`**, which sets the bit itself.

Passed to `shuttle_create_ex` anyway, `0x20` is masked off and ignored — not an
error — exactly like any other bit that entry point does not implement, per the
unknown-bit rule in **Segment layout**. The bit is persisted and
**informational**: an opener takes no action on it (it had to name the file to
attach at all), and it exists so tools and peers can see what the creator asked
for.

### Platform support

POSIX only this pass. On **Windows** the argument checks still apply (a
malformed path is still `INVALID_ARGS`), and any call that gets past them
returns `SHUTTLE_ERR_SYS` — the platform seam reports `ENOTSUP` — having created
nothing. A **documented parity gap**, in the same class as the absent
robust-mutex recovery there. The Windows form would be `CreateFileW` plus a `CreateFileMappingW` on
that handle instead of on `INVALID_HANDLE_VALUE`; it is not attempted here
because the experimental Windows backend has no crash-recovery story to test it
against. See the README's Scope section and `docs/ROADMAP.md`.

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
- **Release before acquire, and the window through it**: a consumer holding
  message N cannot acquire N+1 — the read cursor does not move until the
  release. Use `shuttle_peek_next` to learn that N+1 exists, and how large it
  is, while still holding N; it is read-only and cannot invalidate the borrow
  you are holding.
- **Contiguity guarantee**: the ring is a **BipBuffer** — reservations are
  whole-unit, so a payload is never split across the physical wrap. A borrowed or
  reserved span is always one contiguous run of bytes, safe to expose directly as
  a slice / `memoryview` / NumPy array with no copy and no stitching.
- **Alignment**: by default a payload lands 8 bytes past its frame start and has
  no alignment guarantee beyond that. Create the channel with
  `SHUTTLE_CREATE_ALIGNED_SPANS` to get every span page-aligned, which is what
  `cudaHostRegister` and `newBufferWithBytesNoCopy` require — see
  **Page-aligned payload spans**.

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
`SHUTTLE_CREATE_STATS` = `0x8`, `SHUTTLE_CREATE_ALIGNED_SPANS` = `0x10`,
`SHUTTLE_CREATE_FILE_BACKED` = `0x20`. The hugetlb and file-backed bits are
persisted but informational — an opener takes no action on them, because the
segment's location already determines its page size (hugetlb) or was named by
the opener itself (file). `0x20` is also the one bit `shuttle_create_ex` cannot
set: it is selected by calling `shuttle_create_file`, and is masked off like any
unknown bit if passed to `shuttle_create_ex` (see **File-backed channels**).

**Version gate**: `flags` is ignorable, `version` is not. A version selects the
physical size of the header, so an opener that does not recognize one cannot
know where the data region starts and must refuse the segment. That is why
`SHUTTLE_CREATE_STATS` is the one flag that also bumps the layout version — and
why a binary built before v1.2 reports `BAD_VERSION` when handed a v2 segment,
rather than silently ignoring the stats bit. Plain `shuttle_create` writes the
v1 layout, byte for byte as before.

**Geometry gate**: `SHUTTLE_CREATE_ALIGNED_SPANS` is the second exception, by a
different mechanism. It adds no header field, so there is no version to bump —
but it changes the **framing**, and an opener that ignored it would misparse
every message. So the flag also selects the segment's `data_offset`
(`round_up(header_size, page)`), and `data_offset` is validated exactly. A
binary that does not know the bit computes the unaligned offset, finds a
mismatch, and returns `CORRUPT`. In other words: **a flag that changes how bytes
are laid out must be backed by something version- or geometry-checked, never
left to the ignore-unknown-bits rule.** That rule is for bits an old reader can
safely do nothing about.

The two exceptions and their verdicts:

| Flag | What it changes | Old opener sees |
|---|---|---|
| `SHUTTLE_CREATE_STATS` (`0x8`) | header SIZE (layout version 1 → 2) | `BAD_VERSION` |
| `SHUTTLE_CREATE_ALIGNED_SPANS` (`0x10`) | `data_offset` + frame layout | `CORRUPT` |
| `SHUTTLE_CREATE_FILE_BACKED` (`0x20`) | nothing on the segment — only where it lives | opens normally (it named the file) |
| everything else | nothing an opener must act on | opens normally |

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
  directly comparable to the lengths the caller passed and to each other. The
  same holds on a `SHUTTLE_CREATE_ALIGNED_SPANS` channel: the page padding is
  transport overhead too and is **not** counted, so the counters do not silently
  change meaning when the flag is added. (Ring occupancy on such a channel is
  therefore *larger* than `bytes_written - bytes_read` suggests — that is the
  fragmentation, and it is measured by the stride, not by these counters.)
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

The distributable `shuttle` crate keeps that shape, which decides where the
v1.4 lookahead lives. A live `Borrowed` holds the `Consumer` exclusively, so
*no* method on the consumer — `&self` or `&mut self` — is callable while a
message is borrowed; weakening that to let a peek through would give up both
guarantees above. So the peek is offered on **both** types:
`Consumer::peek_next(&self)` when nothing is borrowed, and
`Borrowed::peek_next(&self)` when something is (the case that matters). Both
return `Result<Option<u64>>`, with `Ok(None)` for `WOULD_BLOCK`. `&self` rather
than `&mut self` because peeking observes and leaves nothing behind. A
`compile_fail` doctest in the crate pins the reasoning: calling
`consumer.peek_next()` with a borrow alive is `E0502`, by design, and
`msg.peek_next()` is the spelling that works.

In Python the same call is `Channel.peek_next()`, returning the length or
`None`; it is safe to call with a `BorrowedMessage` outstanding and does not
invalidate its view.
