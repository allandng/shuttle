// G1.3 (NFR-R2): after create -> close -> unlink, no named object survives.
// On Linux the proof is /dev/shm itself: the object must be VISIBLE there
// while alive (ground truth that we are checking the right place, i.e. this
// test can fail) and GONE after unlink. On macOS there is no filesystem view
// of POSIX shm, so the proof is behavioral: open() of the unlinked name must
// fail with kErrNotFound.
#include <unistd.h>

#include <cstdio>

#include "shuttle/platform.hpp"
#include "shuttle/shuttle.hpp"

int main() {
    char name[32];
    std::snprintf(name, sizeof name, "/shleak.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);  // clear any stale object
    int fails = 0;

    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "FAIL: create (err=%d)\n", err);
        return 1;
    }

    // While alive: where the platform has a filesystem view, the object must
    // be visible in it.
    int vis = shuttle::shm_object_exists_fs(name);
    if (vis == 0) {
        std::fprintf(stderr,
                     "FAIL: live object not visible in /dev/shm — leak check"
                     " is looking in the wrong place\n");
        ++fails;
    }

    shuttle::close(ch);

    // close() must NOT destroy the named object (FR-5): still openable.
    ch = shuttle::open(name, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "FAIL: object gone after close (err=%d)\n", err);
        ++fails;
    } else {
        shuttle::close(ch);
    }

    if (shuttle::unlink(name) != shuttle::kOk) {
        std::fprintf(stderr, "FAIL: unlink\n");
        ++fails;
    }

    // After unlink: gone from the filesystem view (Linux)...
    vis = shuttle::shm_object_exists_fs(name);
    if (vis == 1) {
        std::fprintf(stderr, "FAIL: object still in /dev/shm after unlink\n");
        ++fails;
    }
    // ...and not re-openable (both platforms).
    ch = shuttle::open(name, &err);
    if (ch != nullptr || err != shuttle::kErrNotFound) {
        std::fprintf(stderr,
                     "FAIL: unlinked name still openable (err=%d)\n", err);
        if (ch != nullptr) shuttle::close(ch);
        ++fails;
    }
    // Second unlink reports the absence distinctly.
    if (shuttle::unlink(name) != shuttle::kErrNotFound) {
        std::fprintf(stderr, "FAIL: double unlink not kErrNotFound\n");
        ++fails;
    }

    if (fails == 0) {
        std::printf("leak_test ok: create/close/unlink leaves nothing behind"
                    " (platform=%s, fs-view=%s)\n",
                    shuttle::platform_name(),
                    shuttle::shm_object_exists_fs(name) >= 0 ? "/dev/shm"
                                                             : "none");
    }
    return fails == 0 ? 0 : 1;
}
