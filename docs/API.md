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

The ten functions `shuttle_create` .. `shuttle_keepalive` are frozen v1.
`shuttle_create_ex` is an additive v1.1 symbol (new symbol only, no existing
signature or semantic touched), so the ABI version stays `1`.

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
- Segment size mapped: `sizeof(ChannelHeader) + capacity_bytes`.
- Errors (via `*err`): `INVALID_ARGS` (`name` NULL, `name[0] != '/'`,
  `capacity_bytes == 0`, or `max_payload_bytes == 0`), `NAME_TOO_LONG`,
  `CAPACITY_TOO_SMALL` (`capacity_bytes < max_payload_bytes + 8`), `EXISTS`
  (name already present), `SYS` (`ftruncate`/`mmap`/other syscall failure).

### shuttle_create_ex

```c
#define SHUTTLE_CREATE_HUGEPAGES 0x1
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
- Errors: identical set to `shuttle_create`. Passing only unknown bits is not
  an error; they are silently masked.

### shuttle_open

```c
shuttle_channel* shuttle_open(const char* name, int* err);
```

Attach to an existing channel without re-initializing it. Waits (deadlined, 5 s)
for the creator's readiness publication, then validates magic, version, and
header geometry before trusting any field. If the creator opted into huge pages,
the opener's mapping is advised too.

- Blocking: bounded. Spins up to a 5 s deadline waiting for creator init.
- Errors (via `*err`): `INVALID_ARGS` (`name` NULL or `name[0] != '/'`),
  `NOT_FOUND` (no such segment), `CORRUPT` (`fstat` failure, segment smaller
  than a header, or header geometry fails validation), `SYS` (`mmap` failure),
  `INIT_TIMEOUT` (creator never published readiness within 5 s), `BAD_MAGIC`,
  `BAD_VERSION`.

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

Remove the named shm object. Existing mappings remain valid until closed
(POSIX unlink semantics).

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
- Blocking / non-blocking as for `shuttle_write`.
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

- Blocking / non-blocking as for `shuttle_write`.
- Errors: `INVALID_ARGS` (`ch` or `ptr` NULL, or a reservation is already
  outstanding), `MSG_TOO_LARGE` (`len > max_payload`), `WOULD_BLOCK`
  (non-blocking, no contiguous span now), `PEER_DEAD`, `SYS`.

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

- Blocking / non-blocking as for `shuttle_read`.
- The borrowed payload is guaranteed contiguous (see Zero-copy borrow rules).
- Errors: `INVALID_ARGS` (`ch`, `ptr`, or `len` NULL), `WOULD_BLOCK`
  (non-blocking, empty), `PEER_DEAD`, `CORRUPT`, `SYS`.

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

---

## Error codes

| Value | Name                          | Meaning / when it occurs |
|-------|-------------------------------|--------------------------|
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

The C `#define`s are `static_assert`ed equal to the C++ `shuttle::Err` enum in
the implementation, so the two can never drift.

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

`sizeof(ChannelHeader)` (a cache-line multiple) of control header, followed by
the data region at byte offset `data_offset`. Everything in the segment is
fixed-width and referenced by **byte offset from the segment base**, never by
pointer — each process maps at a different address.

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
5. **Park/wake primitives** (off the hot path): a pshared mutex and two condvars.

**Flags-bits contract**: the creator writes the entire `flags` word **once**, in
the cold identity block, before the release-store that publishes `init_state`;
it is immutable thereafter. Openers must **ignore unknown bits** — `flags` is an
additive extension point, so new bits carry no version bump (an old opener
simply does not act on a bit it does not recognize). `SHUTTLE_CREATE_HUGEPAGES`
is recorded here as bit `0x1`.

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
