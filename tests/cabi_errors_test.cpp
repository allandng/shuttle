// G6.3: induced errors surface as the correct integer codes in all three
// languages (C++ via the C ABI, Python via cffi, Rust via the safe
// wrapper); no exception or panic escapes the ABI anywhere.
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "proc_util.hpp"
#include "shuttle/platform.hpp"
#include "shuttle/shuttle_c.h"

#ifndef SHUTTLE_C_LIB
#error "SHUTTLE_C_LIB must be defined"
#endif
#ifndef SHUTTLE_C_LIBDIR
#error "SHUTTLE_C_LIBDIR must be defined"
#endif
#ifndef PY_ERR_PROBE
#error "PY_ERR_PROBE must be defined"
#endif
#ifndef RUST_SRC_DIR
#error "RUST_SRC_DIR must be defined"
#endif
#ifndef RUST_OUT_DIR
#error "RUST_OUT_DIR must be defined"
#endif

namespace {

constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

int run_to_completion(char* const argv[]) {
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv,
                                environ);
    if (rc != 0) {
        std::fprintf(stderr, "spawn %s failed: %s\n", argv[0],
                     std::strerror(rc));
        return -1;
    }
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (shuttle::monotonic_ns() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -2;
        }
        usleep(20000);
    }
}

// C++ leg: induced errors through the C ABI, breadth across the codes.
int cxx_leg() {
    int fails = 0;
    int err = 0;

    if (shuttle_open("/shnx.does-not-exist", &err) != nullptr ||
        err != SHUTTLE_ERR_NOT_FOUND) {
        std::fprintf(stderr, "c++: open-nonexistent err=%d\n", err);
        ++fails;
    }
    if (shuttle_create("/shnx.badcap", 64, 1u << 16, &err) != nullptr ||
        err != SHUTTLE_ERR_CAPACITY_TOO_SMALL) {
        std::fprintf(stderr, "c++: bad-capacity err=%d\n", err);
        ++fails;
    }
    if (shuttle_create("no-leading-slash", 1u << 20, 1u << 16, &err) !=
            nullptr ||
        err != SHUTTLE_ERR_INVALID_ARGS) {
        std::fprintf(stderr, "c++: bad-name err=%d\n", err);
        ++fails;
    }
    if (shuttle_unlink("/shnx.does-not-exist") != SHUTTLE_ERR_NOT_FOUND) {
        std::fprintf(stderr, "c++: unlink-nonexistent wrong code\n");
        ++fails;
    }

    // Nonblocking ops report WOULD_BLOCK as a value, never an exception.
    char name[32];
    std::snprintf(name, sizeof name, "/shnx.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle_unlink(name);
    shuttle_channel* ch = shuttle_create(name, 1u << 16, 1u << 10, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "c++: setup create err=%d\n", err);
        return fails + 1;
    }
    const void* p = nullptr;
    size_t len = 0;
    if (shuttle_acquire_read(ch, &p, &len, SHUTTLE_NONBLOCK) !=
        SHUTTLE_ERR_WOULD_BLOCK) {
        std::fprintf(stderr, "c++: empty nonblocking read wrong code\n");
        ++fails;
    }
    char big[8];
    if (shuttle_write(ch, big, 2048, 0) != SHUTTLE_ERR_MSG_TOO_LARGE) {
        std::fprintf(stderr, "c++: oversize write wrong code\n");
        ++fails;
    }
    shuttle_close(ch);
    shuttle_unlink(name);
    if (fails == 0) std::printf("c++: all induced errors correct\n");
    return fails;
}

}  // namespace

int main() {
    int fails = cxx_leg();

    // Python leg.
    {
        char* argv[] = {const_cast<char*>("python3"),
                        const_cast<char*>(PY_ERR_PROBE),
                        const_cast<char*>(SHUTTLE_C_LIB), nullptr};
        if (run_to_completion(argv) != 0) {
            std::fprintf(stderr, "FAIL: python error probe\n");
            ++fails;
        }
    }

    // Rust leg: compile the probe against the wrapper, then run it.
    const std::string bin = std::string(RUST_OUT_DIR) + "/rust_err_probe";
    {
        std::string src = std::string(RUST_SRC_DIR) + "/err_probe.rs";
        std::string rpath =
            std::string("-Clink-args=-Wl,-rpath,") + SHUTTLE_C_LIBDIR;
        char* argv[] = {const_cast<char*>("rustc"),
                        const_cast<char*>("--edition=2021"),
                        const_cast<char*>(src.c_str()),
                        const_cast<char*>("-L"),
                        const_cast<char*>(SHUTTLE_C_LIBDIR),
                        const_cast<char*>("-lshuttle_c"),
                        const_cast<char*>(rpath.c_str()),
                        const_cast<char*>("-o"),
                        const_cast<char*>(bin.c_str()),
                        nullptr};
        if (run_to_completion(argv) != 0) {
            std::fprintf(stderr, "FAIL: rust error probe did not compile\n");
            return fails + 1;
        }
    }
    {
        char* argv[] = {const_cast<char*>(bin.c_str()), nullptr};
        if (run_to_completion(argv) != 0) {
            std::fprintf(stderr, "FAIL: rust error probe\n");
            ++fails;
        }
    }

    if (fails == 0)
        std::printf("cabi_errors ok: correct integer codes in C++, Python,"
                    " Rust; nothing thrown or panicked across the ABI\n");
    return fails == 0 ? 0 : 1;
}
