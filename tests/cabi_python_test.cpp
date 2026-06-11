// G6.1 driver: C++ producer feeding a PYTHON consumer process over the
// frozen C ABI — the cross-language gate (FR-21). The producer side here
// deliberately uses the C ABI too (not the C++ classes), so this test
// exercises the exact surface foreign bindings see, from creation to
// teardown. Python is spawned via posix_spawnp (fork+exec rule).
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "proc_util.hpp"
#include "shuttle/platform.hpp"
#include "shuttle/shuttle_c.h"

#ifndef SHUTTLE_C_LIB
#error "SHUTTLE_C_LIB must be defined (path to libshuttle_c)"
#endif
#ifndef PY_CONSUMER
#error "PY_CONSUMER must be defined (path to py_consumer.py)"
#endif

namespace {

constexpr uint64_t kMsgs = 1500;
constexpr uint64_t kMaxPayload = 4096;
constexpr uint64_t kSeed = 0xFF100006;
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

}  // namespace

int main() {
    char name[32];
    std::snprintf(name, sizeof name, "/shpy.%d",
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
    std::snprintf(maxp, sizeof maxp, "%llu",
                  (unsigned long long)kMaxPayload);
    char* argv[] = {const_cast<char*>("python3"),
                    const_cast<char*>(PY_CONSUMER),
                    const_cast<char*>(SHUTTLE_C_LIB),
                    name,
                    nmsgs,
                    seed,
                    maxp,
                    nullptr};
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "python3", nullptr, nullptr, argv, environ);
    if (rc != 0) {
        std::fprintf(stderr, "driver: spawn python3 failed: %s\n",
                     std::strerror(rc));
        shuttle_close(ch);
        shuttle_unlink(name);
        return 1;
    }

    // Produce through the C ABI: blocking writes, seeded random sizes.
    int fails = 0;
    std::vector<unsigned char> tmp(kMaxPayload);
    for (uint64_t i = 0; i < kMsgs; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) tmp[j] = fill_byte(i, j);
        const int wrc = shuttle_write(ch, tmp.data(), len, 0);
        if (wrc != SHUTTLE_OK) {
            std::fprintf(stderr, "driver: write %llu rc=%d\n",
                         (unsigned long long)i, wrc);
            ++fails;
            break;
        }
    }

    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "driver: python consumer failed"
                             " (0x%x)\n", st);
                ++fails;
            }
            break;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "driver: TIMEOUT on python consumer\n");
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
        std::printf("cabi_python ok: C++ producer -> Python consumer,"
                    " byte-exact over the borrow path\n");
    return fails == 0 ? 0 : 1;
}
