//! Tell the linker where libshuttle_c lives, and link it.
//!
//! There is nothing to compile here: this crate is declarations only, and the
//! C library is built by the repo's CMake, not by cargo. `SHUTTLE_LIB_DIR`
//! points at the directory holding `libshuttle_c.so` / `.dylib`; without it we
//! emit only the link request and let the platform's own search find the
//! library (an installed prefix, `LD_LIBRARY_PATH`, the ld.so cache).
//!
//! Because the package declares `links = "shuttle_c"`, the `cargo:lib_dir=`
//! line below reaches dependent build scripts as `DEP_SHUTTLE_C_LIB_DIR` —
//! that is how the safe wrapper's build.rs gets the same directory for an
//! rpath, without re-reading the environment.
//!
//! This crate needs the same rpath for itself: `cargo test` builds a unit-test
//! binary from `src/lib.rs` even though there is no `#[cfg(test)]` code here,
//! and that binary links `libshuttle_c` and so aborts at dyld/ld.so load
//! without one. A build-script directive applies to its own package's units
//! only, so this cannot be delegated to the wrapper's build script.

fn main() {
    println!("cargo:rerun-if-env-changed=SHUTTLE_LIB_DIR");

    if let Ok(dir) = std::env::var("SHUTTLE_LIB_DIR") {
        if !dir.is_empty() {
            println!("cargo:rustc-link-search=native={}", dir);
            println!("cargo:lib_dir={}", dir);
            if !cfg!(windows) {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir);
            }
        }
    }

    // dylib: the C ABI is only ever shipped as a shared library, and a static
    // link would drag the C++ runtime in behind it.
    println!("cargo:rustc-link-lib=dylib=shuttle_c");
}
