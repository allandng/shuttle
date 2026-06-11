// G6.2 driver, three stages:
//   1. rustc compiles the safe wrapper + consumer — must SUCCEED (proves
//      the wrapper itself is valid, so stage 2's failure is meaningful).
//   2. rustc compiles compile_fail.rs — must FAIL with E0597: the borrowed
//      slice used after release_read is rejected AT COMPILE TIME.
//   3. End-to-end: C++ producer (via the frozen C ABI) -> Rust consumer
//      process, byte-exact FIFO over the zero-copy borrow path (FR-21).
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/platform.hpp"
#include "shuttle/shuttle_c.h"

#ifndef SHUTTLE_C_LIBDIR
#error "SHUTTLE_C_LIBDIR must be defined"
#endif
#ifndef RUST_SRC_DIR
#error "RUST_SRC_DIR must be defined"
#endif
#ifndef RUST_OUT_DIR
#error "RUST_OUT_DIR must be defined"
#endif

namespace {

constexpr uint64_t kMsgs = 1500;
constexpr uint64_t kMaxPayload = 4096;
constexpr uint64_t kSeed = 0x50570007;
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
uint64_t msg_len(uint64_t i) { return splitmix(kSeed ^ i) % (kMaxPayload + 1); }
unsigned char fill_byte(uint64_t msg, uint64_t j) {
    return static_cast<unsigned char>((msg * 1315423911ull) + j * 151ull +
                                      (j >> 8));
}

// Run argv to completion with stderr redirected to err_path. Returns the
// exit code, or -1 on spawn/abnormal-exit, -2 on timeout.
int run_capture(char* const argv[], const char* err_path) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 2, err_path,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid = 0;
    const int rc =
        posix_spawnp(&pid, argv[0], &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        std::fprintf(stderr, "spawn %s failed: %s\n", argv[0],
                     std::strerror(rc));
        return -1;
    }
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
        if (shuttle::monotonic_ns() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -2;
        }
        usleep(20000);
    }
}

void dump_file(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        std::fwrite(buf, 1, n, stderr);
    std::fclose(f);
}

bool file_contains(const char* path, const char* needle) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    std::string all;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    std::fclose(f);
    return all.find(needle) != std::string::npos;
}

int rustc_build(const char* src, const char* out, const char* err_path) {
    std::string s = std::string(RUST_SRC_DIR) + "/" + src;
    std::string rpath = std::string("-Clink-args=-Wl,-rpath,") +
                        SHUTTLE_C_LIBDIR;
    char* argv[] = {const_cast<char*>("rustc"),
                    const_cast<char*>("--edition=2021"),
                    const_cast<char*>(s.c_str()),
                    const_cast<char*>("-L"),
                    const_cast<char*>(SHUTTLE_C_LIBDIR),
                    const_cast<char*>("-lshuttle_c"),
                    const_cast<char*>(rpath.c_str()),
                    const_cast<char*>("-o"),
                    const_cast<char*>(out),
                    nullptr};
    return run_capture(argv, err_path);
}

}  // namespace

int main() {
    int fails = 0;
    const std::string out_bin = std::string(RUST_OUT_DIR) + "/rust_consumer";
    const std::string cf_bin = std::string(RUST_OUT_DIR) + "/rust_cf";
    const std::string err1 = std::string(RUST_OUT_DIR) + "/rustc_ok.err";
    const std::string err2 = std::string(RUST_OUT_DIR) + "/rustc_cf.err";

    // Stage 1: the wrapper + consumer must compile.
    if (rustc_build("consumer.rs", out_bin.c_str(), err1.c_str()) != 0) {
        std::fprintf(stderr, "FAIL: rust consumer did not compile:\n");
        dump_file(err1.c_str());
        return 1;  // nothing else is meaningful
    }

    // Stage 2: use-after-release must NOT compile, specifically E0597.
    const int cfrc = rustc_build("compile_fail.rs", cf_bin.c_str(),
                                 err2.c_str());
    if (cfrc == 0) {
        std::fprintf(stderr,
                     "FAIL: compile_fail.rs COMPILED — the wrapper does not"
                     " enforce the borrow lifetime\n");
        ++fails;
    } else if (!file_contains(err2.c_str(), "E0597")) {
        std::fprintf(stderr,
                     "FAIL: compile_fail.rs failed for the wrong reason"
                     " (no E0597):\n");
        dump_file(err2.c_str());
        ++fails;
    }

    // Stage 3: end-to-end byte-exact exchange.
    char name[32];
    std::snprintf(name, sizeof name, "/shrs.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle_unlink(name);
    int err = 0;
    shuttle_channel* ch = shuttle_create(name, 1u << 20, kMaxPayload, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }
    char nmsgs[16], seed[24], maxp[16];
    std::snprintf(nmsgs, sizeof nmsgs, "%llu", (unsigned long long)kMsgs);
    std::snprintf(seed, sizeof seed, "%llu", (unsigned long long)kSeed);
    std::snprintf(maxp, sizeof maxp, "%llu", (unsigned long long)kMaxPayload);
    pid_t pid = 0;
    char* cargv[] = {const_cast<char*>(out_bin.c_str()), name, nmsgs, seed,
                     maxp, nullptr};
    if (posix_spawn(&pid, out_bin.c_str(), nullptr, nullptr, cargv,
                    environ) != 0) {
        std::fprintf(stderr, "driver: spawn rust_consumer failed\n");
        shuttle_close(ch);
        shuttle_unlink(name);
        return 1;
    }
    std::vector<unsigned char> tmp(kMaxPayload);
    for (uint64_t i = 0; i < kMsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        if (shuttle_write(ch, tmp.data(), len, 0) != SHUTTLE_OK) {
            std::fprintf(stderr, "driver: write %llu failed\n",
                         (unsigned long long)i);
            ++fails;
            break;
        }
    }
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "driver: rust consumer failed (0x%x)\n",
                             st);
                ++fails;
            }
            break;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "driver: TIMEOUT on rust consumer\n");
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            ++fails;
            break;
        }
        usleep(10000);
    }
    shuttle_close(ch);
    shuttle_unlink(name);
    if (fails == 0)
        std::printf("cabi_rust ok: byte-exact exchange; use-after-release"
                    " rejected at compile time (E0597)\n");
    return fails == 0 ? 0 : 1;
}
