//! Bake an rpath into the binaries cargo builds for this crate.
//!
//! Linking is `shuttle-sys`'s job; this exists only so that `cargo test` finds
//! `libshuttle_c` at *run* time without the caller also exporting
//! `LD_LIBRARY_PATH`. The directory arrives as `DEP_SHUTTLE_C_LIB_DIR`, which
//! cargo sets from the `cargo:lib_dir=` line in shuttle-sys's build script
//! (that crate declares `links = "shuttle_c"`).
//!
//! `rustc-link-arg`, not `rustc-link-arg-tests`: the `-tests` form reaches only
//! targets of kind `Test` — the `tests/*.rs` integration binaries — and NOT the
//! unit-test binary cargo builds from `src/lib.rs`, which is a `Lib`-kind
//! target with `test = true`. That binary links `libshuttle_c` like any other
//! and aborts at dyld/ld.so load with no rpath, even though this crate declares
//! no `#[cfg(test)]` code for it to run. Any build-script directive applies to
//! this package's own units only, so the unscoped form still imposes nothing on
//! a downstream binary that links this crate.

fn main() {
    println!("cargo:rerun-if-env-changed=DEP_SHUTTLE_C_LIB_DIR");
    println!("cargo:rerun-if-env-changed=SHUTTLE_LIB_DIR");

    let dir = std::env::var("DEP_SHUTTLE_C_LIB_DIR")
        .or_else(|_| std::env::var("SHUTTLE_LIB_DIR"))
        .unwrap_or_default();
    if dir.is_empty() || cfg!(windows) {
        return;
    }

    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir);
}
