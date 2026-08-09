// G1.1: process A (driver) creates a channel; process B (spawned child)
// opens it and reads matching magic/version; after the driver bumps the
// header version, a second open must fail with the DISTINCT version-
// mismatch error (FR-1, FR-2, FR-3).
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"

namespace {

constexpr uint64_t kChildTimeoutNs = 10ull * 1000000000ull;

int fail(const char* what, int code) {
    std::fprintf(stderr, "FAIL: %s (err=%d)\n", what, code);
    return 1;
}

// Child role: open, verify magic/version match the constants, close.
int run_opener(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return fail("opener: open()", err);
    int rc = 0;
    if (ch->hdr->magic != shuttle::kMagic) {
        rc = fail("opener: magic mismatch", 0);
    } else if (ch->hdr->version != shuttle::kVersion) {
        rc = fail("opener: version mismatch", 0);
    }
    shuttle::close(ch);
    return rc;
}

// Child role: open MUST fail with exactly kErrBadVersion.
int run_opener_badver(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch != nullptr) {
        shuttle::close(ch);
        return fail("badver: open() unexpectedly succeeded", 0);
    }
    if (err != shuttle::kErrBadVersion) {
        return fail("badver: wrong error code (want kErrBadVersion)", err);
    }
    return 0;
}

int run_driver(const char* self) {
    char name[32];
    std::snprintf(name, sizeof name, "/shlft.%d",
                  static_cast<int>(getpid()) % 1000000);
    shuttle::unlink(name);  // clear any stale object

    int err = 0;
    shuttle::Channel* ch =
        shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) return fail("driver: create()", err);

    int fails = 0;

    int rc = shuttle_test::run_child_sync(self, "opener", name, nullptr,
                                          kChildTimeoutNs);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL: opener child rc=%d\n", rc);
        ++fails;
    }

    // Bump the version in the live header to one no binary knows; a fresh open
    // must now report the distinct mismatch error. It must be past the LAST
    // known version, not kVersion + 1: version 2 is the opt-in stats layout,
    // and poking it onto a v1 header produces a version/geometry disagreement,
    // which open() reports as kErrCorrupt (see stats_test case (d)) — a
    // different, equally deliberate verdict from the one this case is about.
    ch->hdr->version = shuttle::kVersionStats + 1;
    rc = shuttle_test::run_child_sync(self, "opener-badver", name, nullptr,
                                      kChildTimeoutNs);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL: opener-badver child rc=%d\n", rc);
        ++fails;
    }
    ch->hdr->version = shuttle::kVersion;  // restore before close/unlink

    shuttle::close(ch);
    if (shuttle::unlink(name) != shuttle::kOk) {
        std::fprintf(stderr, "FAIL: unlink\n");
        ++fails;
    }
    if (fails == 0) {
        std::printf("lifecycle_test ok: create/open/version-check/close/unlink"
                    " (platform=%s)\n",
                    shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 3 && std::strcmp(argv[1], "opener") == 0)
        return run_opener(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "opener-badver") == 0)
        return run_opener_badver(argv[2]);
    std::fprintf(stderr, "usage: %s [opener|opener-badver </shm-name>]\n",
                 argv[0]);
    return 2;
}
