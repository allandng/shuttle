# shuttle-ipc

Python bindings for [Shuttle](../../README.md), a zero-copy shared-memory SPSC
IPC library. The package binds the frozen C ABI (`include/shuttle/shuttle_c.h`,
v1.4) through cffi in **ABI mode**: nothing is compiled at install time, the
wheel is pure Python, and no C++ header is involved.

The reference bindings under `tests/ffi/` remain the ABI conformance tests. This
package is the distributable layer built on the same surface.

## Install

```sh
pip install ./bindings/python          # or: pip install -e ./bindings/python
```

The only runtime dependency is `cffi`. The shared library itself is **not**
bundled — build it from this repo:

```sh
cmake -B build -S .
cmake --build build --target shuttle_c
```

## Finding libshuttle_c

Resolved in this order, first hit wins:

1. **An explicit path** passed to the API — `Channel.create(..., library="/path/to/libshuttle_c.so")`,
   or `shuttle_ipc.load_library("/path/to/libshuttle_c.so")` if you want to hold
   the handle yourself and pass it around.
2. **`SHUTTLE_C_LIB`** in the environment — the whole path, not a directory:

   ```sh
   export SHUTTLE_C_LIB=$PWD/build/libshuttle_c.so
   ```

3. **`ctypes.util.find_library("shuttle_c")`** — the platform's own search
   (`ld.so` cache, `LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, the usual prefixes).
   This is the path that works after a `cmake --install`.

If all three come up empty, `shuttle_ipc.LibraryNotFoundError` is raised. The
default library is loaded once and cached; an explicit path always produces a
fresh handle, so two builds can coexist in one process.

## Usage

```python
from shuttle_ipc import Channel

producer = Channel.create("/demo", capacity=1 << 20, max_payload=1 << 16)
consumer = Channel.open("/demo")

producer.write(b"hello")                    # copy path
with consumer.acquire_read() as payload:    # zero-copy path
    assert bytes(payload) == b"hello"       # bytes() here is *your* copy

consumer.close()
producer.close()
Channel.unlink("/demo")
```

`capacity` must be at least `max_payload + 8` (the 8 bytes are the frame
header); a smaller one raises `CapacityTooSmall`. Names start with `/` and are
capped at 30 characters on macOS, 254 on Linux.

`huge_pages=True` on `create` routes through the v1.1 `shuttle_create_ex` with
`SHUTTLE_CREATE_HUGEPAGES`. It is advisory — effective only where the kernel THP
policy permits, a silent no-op elsewhere, never a correctness dependency.

### Zero-copy, both directions

Producer side — reserve, fill in place, publish:

```python
with producer.acquire_write(4096) as buf:   # writable memoryview into the segment
    n = fill(buf)                           # no intermediate bytes object
# clean exit commits the full 4096 bytes; commit fewer explicitly:

res = producer.acquire_write(4096)
n = fill(res.view)
res.commit(n)
```

Consumer side — borrow in place, release:

```python
with consumer.acquire_read() as payload:    # read-only memoryview, no copy
    total = sum(payload)                    # indexed in the segment itself
```

For numeric payloads, NumPy wraps the same view without an intermediate buffer:

```python
import numpy as np
with consumer.acquire_read() as payload:
    arr = np.frombuffer(payload, dtype=np.uint8)   # still zero-copy
    result = arr.mean()                            # consume it inside the block
```

### The borrow guard

A borrowed payload is valid only until release; afterwards the producer may
overwrite those bytes. Python cannot enforce a borrow lifetime the way Rust's
`Borrowed<'a>` does — but it can refuse to serve a stale view. On release the
`memoryview` is invalidated:

```python
borrow = consumer.acquire_read()
view = borrow.view
borrow.release()

borrow.view      # ShuttleError: borrowed payload used after release_read
view[0]          # ValueError: operation forbidden on released memoryview object
```

One caveat this cannot cover: if you export the view to something that holds its
own reference (`np.frombuffer`, a `bytearray` view, a C extension), that object
still aliases segment memory after the release. Consume it inside the block, or
take a copy with `bytes(view)` / `borrow.tobytes()` if it must outlive the
borrow.

At most one borrow and one reservation may be outstanding per handle. The C
layer enforces that itself on the write side; on the read side a repeat
`shuttle_acquire_read` is idempotent there, which would silently orphan the
first guard, so this package raises `InvalidArgs` instead and holds the
documented contract on both sides.

The same guard applies to a write reservation: after `commit`, the reservation's
view is dead. The C ABI has no cancel — a reservation ends only at
`shuttle_commit_write` — so this package does not invent one. Dropping a
reservation without committing publishes nothing and leaves it outstanding; the
next `acquire_write` on that handle then fails with `InvalidArgs`. Used as a
context manager, a `Reservation` commits the full span on a clean exit and
leaves it uncommitted if the block raised.

### Blocking, non-blocking, timeouts

Blocking is the default: the C layer spins briefly, then parks (it does not
poll). `nonblock=True` sets `SHUTTLE_NONBLOCK` and raises `WouldBlock` instead:

```python
try:
    producer.write(payload, nonblock=True)
except shuttle_ipc.WouldBlock:
    ...
```

`timeout=<seconds>` is a **convenience layered on top, not an ABI feature** —
the C surface has only "park" and "fail now", no deadline variant. A timeout is
therefore emulated by repeating the non-blocking call with a backing-off sleep
(50 µs to 1 ms). That is polling. On any hot path, pass `timeout=None` and let
the C layer park. `nonblock=True` and `timeout=` together raise `ValueError`.

`PeerDead` only ever arrives on a blocking call: it means the peer's heartbeat
went stale. A peer that is alive but idle looks the same as a dead one, so
sparse-traffic processes should call `channel.keepalive()` periodically.

### Copy reads

`read()` uses `shuttle_read`, which needs a destination buffer, and the ABI has
no way to query `max_payload` on an *opened* handle. So `read()` sizes itself:
it starts from `max_payload` when the handle created the channel, otherwise
64 KiB, and doubles on `MSG_TOO_LARGE`. That retry is well-defined because an
oversized copy-read leaves the message queued. Pass `max_len=` to bound it
yourself — then `TooBig` propagates and the message stays queued for a
larger-buffered retry.

## Errors

Every failure the C layer reports as a negative integer is raised as a
`ShuttleError` subclass carrying that integer as `.code`:

| Code | Exception | |
|---|---|---|
| -1 | `InvalidArgs` | NULL/malformed args, misuse (double borrow, wrong role) |
| -2 | `NameTooLong` | over the platform shm-name limit |
| -3 | `Exists` | `create`: name already present |
| -4 | `NotFound` | `open`/`unlink`: no such segment |
| -5 | `SysError` | syscall failure |
| -6 | `BadMagic` | segment magic mismatch |
| -7 | `BadVersion` | layout version mismatch |
| -8 | `CapacityTooSmall` | `capacity < max_payload + 8` |
| -9 | `InitTimeout` | creator never published readiness within 5 s |
| -10 | `Corrupt` | header geometry or framed length impossible |
| -11 | `TooBig` | `SHUTTLE_ERR_MSG_TOO_LARGE` (alias: `MsgTooLarge`) |
| -12 | `WouldBlock` | non-blocking op cannot proceed now |
| -13 | `PeerDead` | blocking wait aborted, peer heartbeat stale |

Catch the family with `ShuttleError`, or one failure by name. An unrecognized
code yields a plain `ShuttleError` carrying it rather than a `KeyError`, so a
future additive code degrades gracefully. `LibraryNotFoundError` is also a
`ShuttleError` but its `.code` is `None` — it never reached the ABI.

## Tests

```sh
cmake -B build -S . && cmake --build build --target shuttle_c
pip install -e ./bindings/python
SHUTTLE_C_LIB=$PWD/build/libshuttle_c.so pytest bindings/python/tests -q
```

The suite creates and opens channels in one process, checks byte-exact
roundtrips over both paths in both directions, and pins the error codes
(`NotFound` on a missing open, `TooBig` on an oversized write, and the rest).
It skips itself, rather than failing, if no `libshuttle_c` can be found.
