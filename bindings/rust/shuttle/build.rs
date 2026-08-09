//! Bake an rpath into this crate's test and example binaries.
//!
//! Linking is `shuttle-sys`'s job; this exists only so that `cargo test` finds
//! `libshuttle_c` at *run* time without the caller also exporting
//! `LD_LIBRARY_PATH`. The directory arrives as `DEP_SHUTTLE_C_LIB_DIR`, which
//! cargo sets from the `cargo:lib_dir=` line in shuttle-sys's build script
//! (that crate declares `links = "shuttle_c"`).
//!
//! `rustc-link-arg-tests` is scoped to this crate's test targets, so nothing is
//! imposed on a downstream binary that links this crate.

fn main() {
    println!("cargo:rerun-if-env-changed=DEP_SHUTTLE_C_LIB_DIR");
    println!("cargo:rerun-if-env-changed=SHUTTLE_LIB_DIR");

    let dir = std::env::var("DEP_SHUTTLE_C_LIB_DIR")
        .or_else(|_| std::env::var("SHUTTLE_LIB_DIR"))
        .unwrap_or_default();
    if dir.is_empty() || cfg!(windows) {
        return;
    }

    println!("cargo:rustc-link-arg-tests=-Wl,-rpath,{}", dir);
}
