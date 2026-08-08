// Opt-in transparent-huge-page create-flag (kFlagHugePages / the C ABI's
// SHUTTLE_CREATE_HUGEPAGES). Driver exercises the additive v1.1 entry point
// shuttle_create_ex end to end; spawned children open the segment in a
// separate process and observe the persisted flags word. What is asserted is
// the FLAG CONTRACT and byte-exact transport, never a THP outcome: whether the
// kernel actually backs the mapping with huge pages is policy-dependent
// (/sys/kernel/mm/transparent_hugepage/shmem_enabled) and would flake in CI,
// so /proc/self/smaps and THP counters are deliberately untouched.
//
//   a. create_ex(HUGEPAGES) succeeds and an opener sees kFlagHugePages set.
//   b. a byte-exact payload survives producer->spawned-consumer on that
//      huge-page-flagged channel.
//   c. plain shuttle_create leaves the flag bit clear.
//   d. unknown create-flag bits are masked off (flags shows only known,
//      implemented bits) and the channel still carries data. The explicit
//      hugetlbfs backings (0x2/0x4) are no longer masked — they are known bits
//      now, and tests/hugetlb_test.cpp owns their contract.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"    // shuttle::open / Channel / kFlagHugePages
#include "shuttle/shuttle_c.h"    // the surface under test

namespace {

constexpr uint64_t kChildTimeoutNs = 10ull * 1000000000ull;
constexpr size_t kCapacity = 1u << 20;
constexpr size_t kMaxPayload = 1u << 16;
constexpr size_t kPayloadLen = 4096;

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

// Deterministic, position-dependent pattern so a byte-exact check is meaningful.
unsigned char pattern_byte(size_t i) {
    return static_cast<unsigned char>((i * 31u + 7u) & 0xFFu);
}

// Child role: open the segment (C++ header path — an acceptable inspection
// route per spec) and assert flags == the expected word. Openers must observe
// exactly the known bits the creator persisted, never the caller's raw input.
int run_opener(const char* name, uint32_t expected_flags) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return fail("opener: open()", err);
    const uint32_t flags = ch->hdr->flags;
    shuttle::close(ch);
    if (flags != expected_flags) {
        std::fprintf(stderr, "FAIL: opener flags=0x%x want 0x%x\n", flags,
                     expected_flags);
        return 1;
    }
    return 0;
}

// Child role: consume exactly one message via the C ABI and verify it byte for
// byte. Lifecycle (unlink) stays with the driver; the consumer only closes.
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

// Driver helper: producer writes the pattern via the C ABI, then a spawned
// consumer reads it back and checks every byte.
int transfer_roundtrip(const char* self, shuttle_channel* ch,
                       const char* name) {
    unsigned char buf[kPayloadLen];
    for (size_t i = 0; i < kPayloadLen; ++i) buf[i] = pattern_byte(i);
    // Buffer is empty, so a blocking write completes without parking; the
    // message stays queued until the consumer child drains it.
    const int wr = shuttle_write(ch, buf, kPayloadLen, 0);
    if (wr != SHUTTLE_OK) return fail("driver: shuttle_write", wr);
    char len_arg[24];
    std::snprintf(len_arg, sizeof len_arg, "%zu", kPayloadLen);
    return shuttle_test::run_child_sync(self, "consumer", name, len_arg,
                                        kChildTimeoutNs);
}

int run_driver(const char* self) {
    const int pid = static_cast<int>(getpid()) % 1000000;
    char huge_name[32], plain_name[32], mask_name[32];
    std::snprintf(huge_name, sizeof huge_name, "/shhp.h%d", pid);
    std::snprintf(plain_name, sizeof plain_name, "/shhp.p%d", pid);
    std::snprintf(mask_name, sizeof mask_name, "/shhp.m%d", pid);
    shuttle_unlink(huge_name);  // clear any stale objects
    shuttle_unlink(plain_name);
    shuttle_unlink(mask_name);

    int fails = 0;
    int err = 0;

    // (a) create_ex opting into huge pages succeeds; opener sees the bit set.
    shuttle_channel* huge = shuttle_create_ex(
        huge_name, kCapacity, kMaxPayload, SHUTTLE_CREATE_HUGEPAGES, &err);
    if (huge == nullptr) {
        ++fails;
        fail("create_ex(HUGEPAGES)", err);
    } else {
        if (shuttle_test::run_child_sync(self, "opener", huge_name, "1",
                                         kChildTimeoutNs) != 0) {
            std::fprintf(stderr, "FAIL: huge-page opener saw wrong flags\n");
            ++fails;
        }
        // (b) byte-exact transfer still works on the flagged channel.
        if (transfer_roundtrip(self, huge, huge_name) != 0) {
            std::fprintf(stderr, "FAIL: huge-page byte-exact transfer\n");
            ++fails;
        }
        shuttle_close(huge);
    }

    // (c) plain shuttle_create leaves the flag bit clear.
    shuttle_channel* plain =
        shuttle_create(plain_name, kCapacity, kMaxPayload, &err);
    if (plain == nullptr) {
        ++fails;
        fail("shuttle_create(plain)", err);
    } else {
        if (shuttle_test::run_child_sync(self, "opener", plain_name, "0",
                                         kChildTimeoutNs) != 0) {
            std::fprintf(stderr, "FAIL: plain channel flags not clear\n");
            ++fails;
        }
        shuttle_close(plain);
    }

    // (d) unknown create-flag bits are masked to the known set; channel works.
    //
    // The probe word was 0xFFFFFFFF back when kFlagHugePages was the only
    // known bit. It cannot stay that: every other bit in the low nibble has
    // since become KNOWN and does something, so including one would stop this
    // case from being about masking at all —
    //   * SHUTTLE_CREATE_STATS (0x8) would create a version-2 stats segment;
    //   * SHUTTLE_CREATE_HUGETLB_2MB|1GB (0x2|0x4) now select an explicit
    //     hugetlbfs backing, and setting both is INVALID_ARGS by design
    //     (tests/hugetlb_test.cpp owns that contract, positive path included).
    // The probe is therefore every genuinely unknown bit, plus the one known
    // bit whose effect this test is about.
    constexpr uint32_t kMaskProbe = SHUTTLE_CREATE_HUGEPAGES | 0xFFFFFFF0u;
    static_assert(
        (kMaskProbe & (SHUTTLE_CREATE_STATS | SHUTTLE_CREATE_HUGETLB_2MB |
                       SHUTTLE_CREATE_HUGETLB_1GB)) == 0,
        "probe must contain no known bit but HUGEPAGES");
    shuttle_channel* masked =
        shuttle_create_ex(mask_name, kCapacity, kMaxPayload, kMaskProbe, &err);
    if (masked == nullptr) {
        ++fails;
        fail("create_ex(unknown bits)", err);
    } else {
        // The probe masks down to exactly kFlagHugePages: every unknown bit is
        // gone.
        if (shuttle_test::run_child_sync(self, "opener", mask_name, "1",
                                         kChildTimeoutNs) != 0) {
            std::fprintf(stderr, "FAIL: unknown bits not masked to known set\n");
            ++fails;
        }
        if (transfer_roundtrip(self, masked, mask_name) != 0) {
            std::fprintf(stderr, "FAIL: masked-flags channel transfer\n");
            ++fails;
        }
        shuttle_close(masked);
    }

    if (shuttle_unlink(huge_name) != SHUTTLE_OK ||
        shuttle_unlink(plain_name) != SHUTTLE_OK ||
        shuttle_unlink(mask_name) != SHUTTLE_OK) {
        std::fprintf(stderr, "FAIL: unlink left an object behind\n");
        ++fails;
    }

    if (fails == 0) {
        std::printf("hugepage_test ok: create-flag persisted+masked, opener "
                    "observes it, byte-exact transfer holds (platform=%s)\n",
                    shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 4 && std::strcmp(argv[1], "opener") == 0)
        return run_opener(argv[2],
                          static_cast<uint32_t>(std::strtoul(argv[3], nullptr,
                                                             0)));
    if (argc == 4 && std::strcmp(argv[1], "consumer") == 0)
        return run_consumer(argv[2],
                            static_cast<size_t>(std::strtoul(argv[3], nullptr,
                                                             0)));
    std::fprintf(stderr,
                 "usage: %s [opener </shm> <flags> | consumer </shm> <len>]\n",
                 argv[0]);
    return 2;
}
