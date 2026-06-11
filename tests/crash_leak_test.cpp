// G5.3 (NFR-R2): after a CRASHED run, once the survivor tears down, no
// named shm object remains — on either platform.
//
//   crasher (producer): marker write, then keepalive until SIGKILLed.
//   survivor (consumer, 1 s threshold): reads marker, gets kErrPeerDead on
//     the next read (G5.1 machinery), then performs the teardown a real
//     application would: close() its handle and unlink() the channel by
//     name. It then verifies the object is gone from its own vantage.
//   driver: after the survivor exits, independently re-verifies from a
//     second process: filesystem view clean (Linux /dev/shm; macOS has no
//     view — behavioral check only) and the name not openable (both).
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/spsc.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kStaleNs = 1ull * 1000000000ull;
constexpr uint64_t kKillAfterNs = 1ull * 1000000000ull;
constexpr uint64_t kChildTimeoutNs = 60ull * 1000000000ull;
constexpr char kMarker[] = "marker";

int run_crasher(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Producer p(ch);
    if (p.write(kMarker, sizeof(kMarker)) != shuttle::kOk) return 1;
    for (;;) {
        p.keepalive();
        usleep(50000);
    }
}

int run_survivor(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "survivor: open err=%d\n", err);
        return 1;
    }
    shuttle::Consumer c(ch, kStaleNs);
    const unsigned char* p = nullptr;
    uint64_t len = 0;
    if (c.read(&p, &len) != shuttle::kOk || len != sizeof(kMarker)) {
        std::fprintf(stderr, "survivor: marker read failed\n");
        return 1;
    }
    c.release();
    const int rc = c.read(&p, &len);
    if (rc != shuttle::kErrPeerDead) {
        std::fprintf(stderr, "survivor: rc=%d, want kErrPeerDead\n", rc);
        return 1;
    }

    // Survivor teardown: this is the NFR-R2 obligation under test.
    shuttle::close(ch);
    if (shuttle::unlink(name) != shuttle::kOk) {
        std::fprintf(stderr, "survivor: unlink failed\n");
        return 1;
    }

    int fails = 0;
    if (shuttle::shm_object_exists_fs(name) == 1) {
        std::fprintf(stderr, "survivor: object still in /dev/shm\n");
        ++fails;
    }
    ch = shuttle::open(name, &err);
    if (ch != nullptr || err != shuttle::kErrNotFound) {
        std::fprintf(stderr, "survivor: name still openable (err=%d)\n", err);
        if (ch != nullptr) shuttle::close(ch);
        ++fails;
    }
    if (fails == 0)
        std::printf("survivor: peer died -> teardown -> object gone\n");
    return fails == 0 ? 0 : 1;
}

int run_driver(const char* self) {
    char name[32];
    std::snprintf(name, sizeof name, "/shclk.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }

    const char* roles[2] = {"crasher", "survivor"};
    pid_t pids[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        char* argv[] = {const_cast<char*>(self), const_cast<char*>(roles[i]),
                        name, nullptr};
        if (posix_spawn(&pids[i], self, nullptr, nullptr, argv, environ) !=
            0) {
            std::fprintf(stderr, "driver: spawn %s failed\n", roles[i]);
            shuttle::close(ch);
            shuttle::unlink(name);
            return 1;
        }
    }
    usleep(static_cast<useconds_t>(kKillAfterNs / 1000));
    kill(pids[0], SIGKILL);
    waitpid(pids[0], nullptr, 0);

    int fails = 0;
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pids[1], &st, WNOHANG) == pids[1]) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "driver: survivor failed (0x%x)\n", st);
                ++fails;
            }
            break;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "driver: TIMEOUT on survivor\n");
            kill(pids[1], SIGKILL);
            waitpid(pids[1], nullptr, 0);
            ++fails;
            break;
        }
        usleep(10000);
    }

    // Independent re-verification from a second process: nothing remains.
    if (shuttle::shm_object_exists_fs(name) == 1) {
        std::fprintf(stderr, "driver: object STILL in /dev/shm after"
                             " survivor teardown\n");
        ++fails;
    }
    int err2 = 0;
    shuttle::Channel* reopened = shuttle::open(name, &err2);
    if (reopened != nullptr || err2 != shuttle::kErrNotFound) {
        std::fprintf(stderr, "driver: name still openable (err=%d)\n", err2);
        if (reopened != nullptr) shuttle::close(reopened);
        ++fails;
    }
    shuttle::close(ch);  // our own mapping; name already unlinked
    if (fails == 0)
        std::printf("crash_leak ok: crashed run leaves no shm object once"
                    " the survivor tears down\n");
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3 && std::strcmp(argv[1], "crasher") == 0)
        return run_crasher(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "survivor") == 0)
        return run_survivor(argv[2]);
    std::fprintf(stderr, "usage: %s [crasher|survivor </name>]\n", argv[0]);
    return 2;
}
