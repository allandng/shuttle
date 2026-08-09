# Rust bindings

Two crates in one workspace, both over the frozen C ABI
(`include/shuttle/shuttle_c.h`, v1.1). No C++ header is involved, and neither
crate is published — they are built from this repo by path.

| Crate | What it is |
|---|---|
| [`shuttle-sys`](shuttle-sys) | Raw `extern "C"` declarations: the ten frozen v1 functions plus the additive v1.1 `shuttle_create_ex`, the error codes, and the two flag words. |
| [`shuttle`](shuttle) | The safe wrapper: `Channel` → `Producer` / `Consumer`, copy and zero-copy paths, errors as an enum, `Drop` closing the handle. |

The reference bindings under `tests/ffi/rust/` remain the ABI conformance
tests. These crates are the distributable layer over the same surface.

## No bindgen

`shuttle-sys` hand-writes its declarations. bindgen would pull libclang into
every build of a crate whose entire job is eleven signatures that cannot change
without an ABI break — the same call the reference bindings made. The cost is
that a drift between `shuttle-sys/src/lib.rs` and the header would not be
caught by the compiler; it is caught by the repo's byte-exact FFI integration
tests instead.

## Building

The C library is built by CMake, not by cargo. Point `SHUTTLE_LIB_DIR` at the
directory holding `libshuttle_c.so` (or `.dylib`):

```sh
cmake -B build -S . && cmake --build build --target shuttle_c
SHUTTLE_LIB_DIR=$PWD/build cargo test --manifest-path bindings/rust/Cargo.toml
```

`shuttle-sys`'s build script emits `rustc-link-search` for that directory and
links `shuttle_c` as a dylib. It also republishes the directory as
`DEP_SHUTTLE_C_LIB_DIR` (the package declares `links = "shuttle_c"`), which the
`shuttle` build script turns into an rpath on the **test** binaries only — so
`cargo test` finds the library at run time without `LD_LIBRARY_PATH`. Set
`LD_LIBRARY_PATH` anyway if you run the test binaries directly, or drop
`SHUTTLE_LIB_DIR` entirely once the library is installed somewhere the
platform's own search covers.

## Usage

```rust
use shuttle::Channel;

let mut producer = Channel::create("/demo", 1 << 20, 1 << 16)?.into_producer();
producer.write(b"hello")?;

let mut consumer = Channel::open("/demo")?.into_consumer();
{
    let msg = consumer.acquire_read()?;      // zero-copy borrow
    assert_eq!(msg.as_slice(), b"hello");    // read in place
}                                            // release on drop

Channel::unlink("/demo")?;
```

A `Channel` has no role yet. `into_producer` / `into_consumer` consume it and
hand back the half you asked for, which makes the ABI's lazy role binding
explicit and irreversible — a handle can then only be used one way, which is
what the SPSC contract wants anyway.

Blocking is the default: the C layer spins briefly, then parks. The `try_*`
methods set `SHUTTLE_NONBLOCK` and return `Error::WouldBlock` instead. There is
no timeout variant, because the ABI has no deadline and emulating one would
mean polling — which is exactly what the parking design exists to avoid.

## The borrow contract

This is the reason the wrapper exists. `Consumer::acquire_read` borrows the
consumer mutably and returns `Borrowed<'_>`; the slice from `as_slice()` is
tied to that value's lifetime, and its `Drop` performs `shuttle_release_read`.
Two things are therefore compile errors, not runtime faults:

```rust
let stale;
{
    let msg = consumer.acquire_read()?;
    stale = msg.as_slice();
}                                // release runs here
println!("{}", stale[0]);        // E0597: `msg` does not live long enough
```

```rust
let first = consumer.acquire_read()?;
let second = consumer.acquire_read()?;   // E0499: `consumer` already borrowed
```

Both are `compile_fail` doc-tests on `Borrowed` (annotated with those exact
error codes), so `cargo test` fails if either ever starts compiling.
`Reservation<'_>` has the same shape on the producer side.

One asymmetry, deliberate: `Reservation` does **not** implement `Drop`. The C
ABI has no cancel — a reservation ends only at `shuttle_commit_write` — so this
crate does not invent one. Dropping a reservation without committing publishes
nothing and leaves it outstanding; the next `acquire_write` on that producer
returns `Error::InvalidArgs`. Commit zero bytes to release a span you decided
not to use.

## Threading

`Producer` and `Consumer` hold a raw handle and are deliberately **not** `Send`
or `Sync`. The C header documents no thread affinity either way, so the wrapper
does not assert one. Keep a handle on the thread that made it, or add the
`unsafe impl Send` yourself if your own build guarantees it.

## Errors

`Error` is `#[non_exhaustive]` — the ABI is additive, so match with a `_` arm.
Every variant maps to exactly one code (`Error::code()` gives it back), and a
code this crate does not name becomes `Error::Unknown(i32)` rather than a panic
or a silent success. See docs/API.md for the full table.
