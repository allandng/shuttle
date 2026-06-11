#pragma once

// Multi-process test helper: spawn THIS test binary with role arguments via
// posix_spawn and wait with a deadline. Plain fork without exec is forbidden
// (binding amendment: TSan on macOS cannot survive fork-without-exec).
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "shuttle/platform.hpp"

extern char** environ;

namespace shuttle_test {

// Runs `self role a1 a2` to completion. Returns the child's exit code,
// -1 on spawn failure, -2 on timeout (child is SIGKILLed), -3 on signal.
inline int run_child_sync(const char* self, const char* role, const char* a1,
                          const char* a2, uint64_t timeout_ns) {
    if (std::strchr(self, '/') == nullptr) {
        std::fprintf(stderr, "run_child_sync: argv[0] not a path: %s\n", self);
        return -1;
    }
    char* argv[5];
    int n = 0;
    argv[n++] = const_cast<char*>(self);
    argv[n++] = const_cast<char*>(role);
    if (a1 != nullptr) argv[n++] = const_cast<char*>(a1);
    if (a2 != nullptr) argv[n++] = const_cast<char*>(a2);
    argv[n] = nullptr;

    pid_t pid = 0;
    int rc = posix_spawn(&pid, self, nullptr, nullptr, argv, environ);
    if (rc != 0) {
        std::fprintf(stderr, "posix_spawn(%s) failed: %s\n", role,
                     std::strerror(rc));
        return -1;
    }
    const uint64_t deadline = shuttle::monotonic_ns() + timeout_ns;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(st)) return WEXITSTATUS(st);
            return -3;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "run_child_sync(%s): TIMEOUT, killing\n",
                         role);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -2;
        }
        usleep(2000);
    }
}

// Spawns `self role_a a1` and `self role_b a1` CONCURRENTLY and waits for
// both with one deadline. Returns 0 iff both exit 0; stragglers are
// SIGKILLed on timeout.
inline int run_two_children_sync(const char* self, const char* role_a,
                                 const char* role_b, const char* a1,
                                 uint64_t timeout_ns) {
    const char* roles[2] = {role_a, role_b};
    pid_t pids[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>(roles[i]),
                        const_cast<char*>(a1), nullptr};
        int rc = posix_spawn(&pids[i], self, nullptr, nullptr, argv, environ);
        if (rc != 0) {
            std::fprintf(stderr, "posix_spawn(%s) failed: %s\n", roles[i],
                         std::strerror(rc));
            if (i == 1) {
                kill(pids[0], SIGKILL);
                waitpid(pids[0], nullptr, 0);
            }
            return 1;
        }
    }
    const uint64_t deadline = shuttle::monotonic_ns() + timeout_ns;
    bool done[2] = {false, false};
    int fails = 0;
    while (!(done[0] && done[1])) {
        for (int i = 0; i < 2; ++i) {
            if (done[i]) continue;
            int st = 0;
            if (waitpid(pids[i], &st, WNOHANG) == pids[i]) {
                done[i] = true;
                if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                    std::fprintf(stderr, "child %s failed (status 0x%x)\n",
                                 roles[i], st);
                    ++fails;
                }
            }
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "run_two_children_sync: TIMEOUT\n");
            for (int i = 0; i < 2; ++i) {
                if (!done[i]) {
                    kill(pids[i], SIGKILL);
                    waitpid(pids[i], nullptr, 0);
                }
            }
            return fails + 1;
        }
        usleep(5000);
    }
    return fails;
}

}  // namespace shuttle_test
