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

}  // namespace shuttle_test
