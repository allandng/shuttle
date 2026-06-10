# Linux leg of the two-platform harness.
# glibc base (never Alpine/musl): musl's PTHREAD_MUTEX_ROBUST support is
# divergent; glibc gives the canonical EOWNERDEAD semantics the SRS assumes.
# Runs linux/arm64 natively on Apple Silicon Docker Desktop (no QEMU).
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        util-linux \
    && rm -rf /var/lib/apt/lists/*

# Known issue: gcc-13 TSan is incompatible with Ubuntu 24.04's default ASLR
# entropy ("unexpected memory mapping" at startup). TSan binaries must run
# under `setarch -R` (per-process ASLR off) — wired in the Makefile.
