// G1.2: create() with capacity < max_payload + framing fails with the
// DISTINCT FR-4 error code (kErrCapacityTooSmall) — no crash, no blocking,
// and no shm object left behind. Also pins the exact boundary: capacity ==
// max_payload + 8 is the smallest legal segment.
#include <unistd.h>

#include <cstdio>

#include "shuttle/shuttle.hpp"

namespace {

int fails = 0;

void expect_create_fails(const char* name, size_t cap, size_t maxp,
                         int want_err, const char* what) {
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, cap, maxp, &err);
    if (ch != nullptr) {
        std::fprintf(stderr, "FAIL: %s — create unexpectedly succeeded\n",
                     what);
        shuttle::close(ch);
        shuttle::unlink(name);
        ++fails;
        return;
    }
    if (err != want_err) {
        std::fprintf(stderr, "FAIL: %s — err=%d, want %d\n", what, err,
                     want_err);
        ++fails;
    }
}

}  // namespace

int main() {
    char name[32];
    std::snprintf(name, sizeof name, "/shcap.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);  // clear any stale object

    constexpr size_t kMaxP = 1u << 16;

    // FR-4 violations, several shapes of "too small".
    expect_create_fails(name, kMaxP + shuttle::kFrameHeader - 1, kMaxP,
                        shuttle::kErrCapacityTooSmall, "cap = maxp+7");
    expect_create_fails(name, kMaxP, kMaxP, shuttle::kErrCapacityTooSmall,
                        "cap = maxp");
    expect_create_fails(name, 1, kMaxP, shuttle::kErrCapacityTooSmall,
                        "cap = 1");

    // A failed create must leave no object behind: the name must still be
    // unknown to open().
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch != nullptr || err != shuttle::kErrNotFound) {
        std::fprintf(stderr,
                     "FAIL: failed create left debris (open err=%d)\n", err);
        if (ch != nullptr) shuttle::close(ch);
        ++fails;
    }

    // Boundary: capacity == max_payload + framing is legal.
    ch = shuttle::create(name, kMaxP + shuttle::kFrameHeader, kMaxP, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "FAIL: boundary create failed (err=%d)\n", err);
        ++fails;
    } else {
        shuttle::close(ch);
        if (shuttle::unlink(name) != shuttle::kOk) {
            std::fprintf(stderr, "FAIL: boundary unlink\n");
            ++fails;
        }
    }

    if (fails == 0) {
        std::printf("capacity_test ok: FR-4 rejected with distinct error,"
                    " boundary accepted (platform=%s)\n",
                    shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}
