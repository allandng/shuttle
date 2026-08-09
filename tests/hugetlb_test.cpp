// Explicit hugetlbfs-backed segments (kFlagHugeTLB2M/1G, the C ABI's
// SHUTTLE_CREATE_HUGETLB_2MB/1GB). The contract under test is a GUARANTEE, not
// an optimization hint: either the segment's bytes live in reserved huge pages,
// or create fails with SHUTTLE_ERR_NO_HUGEPAGES and creates nothing. There is
// deliberately no silent fallback to normal pages — that is what the advisory
// SHUTTLE_CREATE_HUGEPAGES flag (tests/hugepage_test.cpp) is for.
//
// SPLIT BY HOST CAPABILITY, honestly:
//
//   Always, everywhere (including CI, which reserves no huge pages):
//     (c) both hugetlb bits set -> SHUTTLE_ERR_INVALID_ARGS (two page sizes
//         cannot both be honored, and masking one off would be a silent lie).
//     (a) the ERROR PATH itself: a create that cannot be honored returns
//         EXACTLY -14 — not -5/SYS, not success — and leaves NOTHING behind in
//         either namespace (/dev/shm and every hugetlbfs mount are checked, as
//         crash_leak_test checks /dev/shm) and nothing openable by name.
//         On such a host the test then prints SKIP and exits 0.
//
//   Only where an operator has reserved pages (the positive path, (b) and (d)):
//     (b) 2 MB-backed create; a spawned child OPENS it with no flag of its own
//         and gets a byte-exact payload; the persisted flags word reads 0x2;
//         the object is a file on a hugetlbfs mount and NOT in /dev/shm;
//         unlink removes that file; stats+hugetlb (0x8|0x2) is a legal combo.
//     (d) THP advice + hugetlb together: hugetlb wins, both bits persist, and
//         it is not an error.
//
// Operator recipe to make the positive path run (Linux, root):
//     sysctl -w vm.nr_hugepages=64                 # 64 x 2 MB reserved
//     mkdir -p /dev/hugepages
//     mount -t hugetlbfs -o pagesize=2M none /dev/hugepages
//     grep -i huge /proc/meminfo && grep hugetlbfs /proc/mounts
// A non-root process additionally needs write permission on the mount point
// (mount -o uid=/gid=/mode=, or chmod), or create fails EACCES — which this
// library reports as SHUTTLE_ERR_NO_HUGEPAGES like any other "cannot deliver".
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"  // shuttle::open / Channel / kFlagHugeTLB2M
#include "shuttle/shuttle_c.h"  // the surface under test

namespace {

constexpr uint64_t kChildTimeoutNs = 10ull * 1000000000ull;
constexpr size_t kCapacity = 1u << 20;  // < one 2 MB page; rounding is tested
constexpr size_t kMaxPayload = 1u << 16;
constexpr size_t kPayloadLen = 4096;

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

unsigned char pattern_byte(size_t i) {
    return static_cast<unsigned char>((i * 37u + 11u) & 0xFFu);
}

// Child role: open by name only — no flag, no hint about the backing — and
// report the persisted flags word. This is the proof that discovery works:
// the opener never learns from the caller that the segment is on hugetlbfs.
int run_opener(const char* name, uint32_t expected_flags) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return fail("opener: open()", err);
    const uint32_t flags = ch->hdr->flags;
    const uint64_t cap = ch->hdr->data_capacity;
    shuttle::close(ch);
    if (flags != expected_flags) {
        std::fprintf(stderr, "FAIL: opener flags=0x%x want 0x%x\n", flags,
                     expected_flags);
        return 1;
    }
    // The huge-page rounding must not leak into the advertised geometry.
    if (cap != kCapacity) {
        std::fprintf(stderr, "FAIL: opener data_capacity=%llu want %zu\n",
                     static_cast<unsigned long long>(cap), kCapacity);
        return 1;
    }
    return 0;
}

int run_consumer(const char* name, size_t expected_len) {
    int err = 0;
    shuttle_channel* ch = shuttle_open(name, &err);
    if (ch == nullptr) return fail("consumer: shuttle_open", err);
    unsigned char out[kPayloadLen];
    const long n = shuttle_read(ch, out, sizeof out, 0);  // blocking
    int rc = 0;
    if (n < 0 || static_cast<size_t>(n) != expected_len) {
        rc = fail("consumer: read length", n);
    } else {
        for (size_t i = 0; i < expected_len; ++i) {
            if (out[i] != pattern_byte(i)) {
                rc = fail("consumer: payload byte mismatch",
                          static_cast<long>(i));
                break;
            }
        }
    }
    shuttle_close(ch);
    return rc;
}

int transfer_roundtrip(const char* self, shuttle_channel* ch,
                       const char* name) {
    unsigned char buf[kPayloadLen];
    for (size_t i = 0; i < kPayloadLen; ++i) buf[i] = pattern_byte(i);
    const int wr = shuttle_write(ch, buf, kPayloadLen, 0);
    if (wr != SHUTTLE_OK) return fail("driver: shuttle_write", wr);
    char len_arg[24];
    std::snprintf(len_arg, sizeof len_arg, "%zu", kPayloadLen);
    return shuttle_test::run_child_sync(self, "consumer", name, len_arg,
                                        kChildTimeoutNs);
}

// Neither namespace may hold anything under `name`. Filesystem checks where
// they exist (-1 = unobservable, e.g. shm on macOS), plus the behavioral check
// that works on every platform: the name must not open.
int assert_no_object(const char* name, const char* stage) {
    int fails = 0;
    if (shuttle::shm_object_exists_fs(name) == 1) {
        std::fprintf(stderr, "FAIL: %s left /dev/shm object %s\n", stage, name);
        ++fails;
    }
    if (shuttle::hugetlb_object_exists_fs(name) == 1) {
        std::fprintf(stderr, "FAIL: %s left a hugetlbfs file for %s\n", stage,
                     name);
        ++fails;
    }
    int err = 0;
    shuttle_channel* probe = shuttle_open(name, &err);
    if (probe != nullptr) {
        std::fprintf(stderr, "FAIL: %s left %s openable\n", stage, name);
        shuttle_close(probe);
        ++fails;
    } else if (err != SHUTTLE_ERR_NOT_FOUND) {
        std::fprintf(stderr, "FAIL: %s: open(%s) err=%d want NOT_FOUND\n",
                     stage, name, err);
        ++fails;
    }
    return fails;
}

// (b) + the stats combo, on a host that really has the pages.
int run_positive(const char* self, const char* name, shuttle_channel* huge,
                 const char* stats_name) {
    int fails = 0;
    int err = 0;

    // The object is a hugetlbfs FILE, not an shm object. This is the check
    // that would catch a silent fallback to normal pages.
    if (shuttle::hugetlb_object_exists_fs(name) != 1) {
        std::fprintf(stderr, "FAIL: no hugetlbfs file for %s\n", name);
        ++fails;
    }
    if (shuttle::shm_object_exists_fs(name) == 1) {
        std::fprintf(stderr, "FAIL: %s exists in /dev/shm too\n", name);
        ++fails;
    }
    // An opener with no knowledge of the backing finds it and sees flags 0x2.
    if (shuttle_test::run_child_sync(self, "opener", name, "2",
                                     kChildTimeoutNs) != 0) {
        std::fprintf(stderr, "FAIL: hugetlb opener flags/geometry\n");
        ++fails;
    }
    if (transfer_roundtrip(self, huge, name) != 0) {
        std::fprintf(stderr, "FAIL: hugetlb byte-exact transfer\n");
        ++fails;
    }
    shuttle_close(huge);
    // unlink must reach into the hugetlbfs namespace and remove the file.
    if (shuttle_unlink(name) != SHUTTLE_OK) {
        std::fprintf(stderr, "FAIL: unlink(hugetlb) failed\n");
        ++fails;
    }
    fails += assert_no_object(name, "unlink(hugetlb)");

    // stats + hugetlb: orthogonal. A v2 header that happens to live on
    // hugetlbfs; both bits persist and the counters work.
    shuttle_channel* both = shuttle_create_ex(
        stats_name, kCapacity, kMaxPayload,
        SHUTTLE_CREATE_STATS | SHUTTLE_CREATE_HUGETLB_2MB, &err);
    if (both == nullptr) {
        ++fails;
        fail("create_ex(STATS|HUGETLB_2MB)", err);
    } else {
        unsigned char buf[64];
        std::memset(buf, 0x5A, sizeof buf);
        if (shuttle_write(both, buf, sizeof buf, 0) != SHUTTLE_OK) {
            std::fprintf(stderr, "FAIL: write on stats+hugetlb channel\n");
            ++fails;
        }
        shuttle_stats st{};
        if (shuttle_get_stats(both, &st) != SHUTTLE_OK ||
            st.msgs_written != 1 || st.bytes_written != sizeof buf) {
            std::fprintf(stderr, "FAIL: stats on hugetlb segment\n");
            ++fails;
        }
        if (shuttle_test::run_child_sync(self, "opener", stats_name, "10",
                                         kChildTimeoutNs) != 0) {
            std::fprintf(stderr, "FAIL: stats+hugetlb flags word\n");
            ++fails;
        }
        shuttle_close(both);
        if (shuttle_unlink(stats_name) != SHUTTLE_OK) {
            std::fprintf(stderr, "FAIL: unlink(stats+hugetlb)\n");
            ++fails;
        }
    }
    return fails;
}

// (d) THP advice + explicit hugetlb: hugetlb wins (the madvise is skipped as
// meaningless), it is NOT an error, and both bits are persisted so an inspector
// can still see what was asked for.
int run_thp_combo(const char* self, const char* name) {
    int err = 0;
    shuttle_channel* ch = shuttle_create_ex(
        name, kCapacity, kMaxPayload,
        SHUTTLE_CREATE_HUGEPAGES | SHUTTLE_CREATE_HUGETLB_2MB, &err);
    if (ch == nullptr) return fail("create_ex(HUGEPAGES|HUGETLB_2MB)", err);
    int fails = 0;
    if (shuttle_test::run_child_sync(self, "opener", name, "3",
                                     kChildTimeoutNs) != 0) {
        std::fprintf(stderr, "FAIL: THP+hugetlb flags word\n");
        ++fails;
    }
    shuttle_close(ch);
    if (shuttle_unlink(name) != SHUTTLE_OK) {
        std::fprintf(stderr, "FAIL: unlink(THP+hugetlb)\n");
        ++fails;
    }
    return fails;
}

int run_driver(const char* self) {
    const int pid = static_cast<int>(getpid()) % 1000000;
    char huge_name[32], both_name[32], stats_name[32], thp_name[32];
    std::snprintf(huge_name, sizeof huge_name, "/shht.h%d", pid);
    std::snprintf(both_name, sizeof both_name, "/shht.b%d", pid);
    std::snprintf(stats_name, sizeof stats_name, "/shht.s%d", pid);
    std::snprintf(thp_name, sizeof thp_name, "/shht.t%d", pid);
    shuttle_unlink(huge_name);  // clear any stale objects
    shuttle_unlink(both_name);
    shuttle_unlink(stats_name);
    shuttle_unlink(thp_name);

    int fails = 0;
    int err = 0;

    // ---- (c) both page sizes at once: rejected, everywhere. --------------
    err = 0;
    shuttle_channel* both = shuttle_create_ex(
        both_name, kCapacity, kMaxPayload,
        SHUTTLE_CREATE_HUGETLB_2MB | SHUTTLE_CREATE_HUGETLB_1GB, &err);
    if (both != nullptr) {
        std::fprintf(stderr, "FAIL: create_ex(2MB|1GB) succeeded\n");
        shuttle_close(both);
        shuttle_unlink(both_name);
        ++fails;
    } else if (err != SHUTTLE_ERR_INVALID_ARGS) {
        ++fails;
        fail("create_ex(2MB|1GB): want INVALID_ARGS", err);
    }
    // Rejected before any object was made: nothing may exist under that name.
    fails += assert_no_object(both_name, "create_ex(2MB|1GB)");

    // ---- (a) the probe. Either the host has pages, or the error path is
    //          what this run tests. -------------------------------------
    err = 0;
    shuttle_channel* huge = shuttle_create_ex(huge_name, kCapacity, kMaxPayload,
                                              SHUTTLE_CREATE_HUGETLB_2MB, &err);
    if (huge == nullptr) {
        // The error must be the SPECIFIC one. A generic SYS (-5) here would
        // mean the library could not tell "no huge pages" from a broken
        // syscall, and a caller could not act on it.
        if (err != SHUTTLE_ERR_NO_HUGEPAGES) {
            ++fails;
            fail("create_ex(HUGETLB_2MB): want NO_HUGEPAGES", err);
        }
        // A failed create leaves no trace in EITHER namespace.
        fails += assert_no_object(huge_name, "failed create_ex(HUGETLB_2MB)");
        if (fails != 0) return 1;
        std::printf(
            "SKIP: no hugetlb pages available — error path verified "
            "(NO_HUGEPAGES=%d, both-bits rejected, no object left behind); "
            "positive path needs reserved pages + a hugetlbfs mount "
            "(platform=%s)\n",
            SHUTTLE_ERR_NO_HUGEPAGES, shuttle::platform_name());
        return 0;
    }

    // ---- (b) + (d): this host really has huge pages. --------------------
    fails += run_positive(self, huge_name, huge, stats_name);
    fails += run_thp_combo(self, thp_name);

    if (fails == 0) {
        std::printf(
            "hugetlb_test ok: 2 MB-backed segment created on hugetlbfs, "
            "opener discovered it, byte-exact transfer, flags persisted, "
            "unlink cleaned both namespaces (platform=%s)\n",
            shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 4 && std::strcmp(argv[1], "opener") == 0)
        return run_opener(
            argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)));
    if (argc == 4 && std::strcmp(argv[1], "consumer") == 0)
        return run_consumer(
            argv[2], static_cast<size_t>(std::strtoul(argv[3], nullptr, 0)));
    std::fprintf(stderr,
                 "usage: %s [opener </shm> <flags> | consumer </shm> <len>]\n",
                 argv[0]);
    return 2;
}
