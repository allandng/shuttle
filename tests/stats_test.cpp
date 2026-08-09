// WP3: opt-in statistics counters and the version-2 segment layout they live
// in (kFlagStats / SHUTTLE_CREATE_STATS, kVersionStats). Driver + posix_spawn
// children, matching hugepage_test's pattern; children observe the segment
// from a separate process, which is the only way to prove the counters are
// SEGMENT state rather than handle-local bookkeeping.
//
//   a. create_ex(STATS) produces a version-2 segment: an opener in another
//      process sees version 2, the flag bit set, data_offset == kDataOffsetV2,
//      and all five counters zero.
//   b. N messages of known sizes through the C ABI leave all five counters
//      EXACT, observed both from same-process handles (producer and consumer)
//      and from a child opener reading the header directly.
//   c. plain create stays version 1: shuttle_get_stats reports NO_STATS, and
//      the byte roundtrip is unaffected (the default format is unchanged).
//   d. version/geometry disagreements are rejected with the DISTINCT errors:
//      a v1 header poked to version 2 is kErrCorrupt (its data_offset is v1's),
//      a v2 header poked to version 3 is kErrBadVersion (unknown version).
//   e. counters stay exact across a wrap-heavy workload (tiny ring, thousands
//      of messages, every message wrapping the BipBuffer within a few sends).
//   f. the same, but with a producer THREAD and a consumer THREAD and a third
//      handle polling get_stats throughout. This case exists for TSan: the
//      other cases are single-threaded, so nothing would put the counter
//      stores in front of the race detector. The stress suite cannot cover
//      them either — it creates v1 segments, where the stats block does not
//      exist. Each counter must stay monotonic under concurrent observation
//      and land exact once both sides are quiescent.
//
// Falsifiability: (d) is the case that could silently always-pass, so each
// rejection is preceded by a POSITIVE CONTROL — the very same segment is
// opened successfully first, so a failure to open afterwards can only come
// from the poke. Each also carries a commented-out wrong expectation that
// makes the check fail if uncommented; both were run inverted while writing
// this test.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "proc_util.hpp"
#include "shuttle/shuttle.hpp"  // shuttle::open / Channel / header constants
#include "shuttle/shuttle_c.h"  // the surface under test

namespace {

constexpr uint64_t kChildTimeoutNs = 10ull * 1000000000ull;
constexpr size_t kCapacity = 1u << 20;
constexpr size_t kMaxPayload = 1u << 16;

// (c): one payload, byte-checked, on the unchanged v1 format.
constexpr size_t kPlainLen = 4096;

// (b): a fixed, known message mix — the counters must equal these exactly.
constexpr uint64_t kMsgCount = 64;
uint64_t msg_len(uint64_t i) { return 16 + (i * 37) % 977; }

// (e): a ring far smaller than the traffic, so the producer wraps constantly.
constexpr size_t kSmallCapacity = 4096;
constexpr size_t kSmallMaxPayload = 512;
constexpr uint64_t kWrapMsgs = 3000;
uint64_t wrap_msg_len(uint64_t i) { return 1 + (i * 61) % kSmallMaxPayload; }

// (f) same tiny ring, driven by two threads so TSan sees the counter stores.
constexpr uint64_t kThreadMsgs = 20000;
uint64_t thread_msg_len(uint64_t i) { return 1 + (i * 29) % 200; }

unsigned char pattern_byte(uint64_t msg, uint64_t i) {
    return static_cast<unsigned char>(msg * 131u + i * 31u + 7u);
}

int fail(const char* what, long code) {
    std::fprintf(stderr, "FAIL: %s (code=%ld)\n", what, code);
    return 1;
}

// ---------------------------------------------------------------- children

// Child role "observe": open the segment in a fresh process and check the
// whole v2 story against a spec string
// "version:flags:data_offset:written:bytes_written:dropped:read:bytes_read".
int run_observe(const char* name, const char* spec) {
    unsigned long long want_ver = 0, want_flags = 0, want_doff = 0;
    unsigned long long want_mw = 0, want_bw = 0, want_md = 0, want_mr = 0,
                       want_br = 0;
    if (std::sscanf(spec, "%llu:%llu:%llu:%llu:%llu:%llu:%llu:%llu", &want_ver,
                    &want_flags, &want_doff, &want_mw, &want_bw, &want_md,
                    &want_mr, &want_br) != 8) {
        return fail("observe: bad spec", 0);
    }
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return fail("observe: open()", err);

    int rc = 0;
    const shuttle::ChannelHeader* h = ch->hdr;
    if (h->version != want_ver)
        rc = fail("observe: version", h->version);
    else if (h->flags != want_flags)
        rc = fail("observe: flags", h->flags);
    else if (h->data_offset != want_doff)
        rc = fail("observe: data_offset", static_cast<long>(h->data_offset));

    // Read the counters through the public accessor, from a process that is
    // neither producer nor consumer — a third party can inspect them. On a v1
    // segment there is nothing to read and the spec's counter fields are
    // ignored: the assertion is that get_stats refuses.
    shuttle::Stats s{};
    if (rc == 0 && want_ver < shuttle::kVersionStats) {
        const int grc = shuttle::get_stats(ch, s);
        if (grc != shuttle::kErrNoStats)
            rc = fail("observe: v1 get_stats must be kErrNoStats", grc);
        shuttle::close(ch);
        return rc;
    }
    if (rc == 0) {
        const int grc = shuttle::get_stats(ch, s);
        if (grc != shuttle::kOk) rc = fail("observe: get_stats", grc);
    }
    if (rc == 0) {
        if (s.msgs_written != want_mw)
            rc = fail("observe: msgs_written",
                      static_cast<long>(s.msgs_written));
        else if (s.bytes_written != want_bw)
            rc = fail("observe: bytes_written",
                      static_cast<long>(s.bytes_written));
        else if (s.msgs_dropped != want_md)
            rc = fail("observe: msgs_dropped",
                      static_cast<long>(s.msgs_dropped));
        else if (s.msgs_read != want_mr)
            rc = fail("observe: msgs_read", static_cast<long>(s.msgs_read));
        else if (s.bytes_read != want_br)
            rc = fail("observe: bytes_read", static_cast<long>(s.bytes_read));
    }
    shuttle::close(ch);
    return rc;
}

// ----------------------------------------------------------- driver helpers

// Spawn the "observe" child with an expected-state spec.
int expect_observed(const char* self, const char* name, uint32_t version,
                    uint32_t flags, uint64_t data_offset, uint64_t mw,
                    uint64_t bw, uint64_t md, uint64_t mr, uint64_t br) {
    char spec[160];
    std::snprintf(spec, sizeof spec, "%u:%u:%llu:%llu:%llu:%llu:%llu:%llu",
                  version, flags, (unsigned long long)data_offset,
                  (unsigned long long)mw, (unsigned long long)bw,
                  (unsigned long long)md, (unsigned long long)mr,
                  (unsigned long long)br);
    return shuttle_test::run_child_sync(self, "observe", name, spec,
                                        kChildTimeoutNs);
}

int check_stats(const char* what, shuttle_channel* ch, uint64_t mw, uint64_t bw,
                uint64_t md, uint64_t mr, uint64_t br) {
    shuttle_stats s;
    std::memset(&s, 0xAA, sizeof s);  // poison: every field must be written
    const int rc = shuttle_get_stats(ch, &s);
    if (rc != SHUTTLE_OK) return fail(what, rc);
    if (s.msgs_written != mw || s.bytes_written != bw || s.msgs_dropped != md ||
        s.msgs_read != mr || s.bytes_read != br) {
        std::fprintf(stderr,
                     "FAIL: %s counters w=%llu/%llu d=%llu r=%llu/%llu "
                     "want w=%llu/%llu d=%llu r=%llu/%llu\n",
                     what, (unsigned long long)s.msgs_written,
                     (unsigned long long)s.bytes_written,
                     (unsigned long long)s.msgs_dropped,
                     (unsigned long long)s.msgs_read,
                     (unsigned long long)s.bytes_read, (unsigned long long)mw,
                     (unsigned long long)bw, (unsigned long long)md,
                     (unsigned long long)mr, (unsigned long long)br);
        return 1;
    }
    return 0;
}

// Overwrite the live header's version word. Deliberately crude — a separate
// mapping, no library help — because the point is to forge a segment no
// correct creator would ever write, then watch open() refuse it.
int poke_version(const char* name, uint32_t value) {
    const int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return fail("poke: shm_open", errno);
    void* p = mmap(nullptr, shuttle::kDataOffsetV2, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return fail("poke: mmap", errno);
    static_cast<shuttle::ChannelHeader*>(p)->version = value;
    munmap(p, shuttle::kDataOffsetV2);
    return 0;
}

// open() must fail with exactly `want`. Callers run a positive control on the
// same segment first, so a "cannot open" verdict here means the poke.
int expect_open_error(const char* name, int want, const char* what) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch != nullptr) {
        shuttle::close(ch);
        return fail(what, 0);
    }
    if (err != want) return fail(what, err);
    return 0;
}

// ------------------------------------------------------------------ driver

// (b) N known-size messages through the C ABI, producer handle -> consumer
// handle (two handles over one segment, same process; roles bind per handle).
int run_known_traffic(shuttle_channel* prod, shuttle_channel* cons,
                      uint64_t* out_bytes) {
    unsigned char buf[1024];
    unsigned char out[1024];
    uint64_t total = 0;
    for (uint64_t i = 0; i < kMsgCount; ++i) {
        const uint64_t len = msg_len(i);
        for (uint64_t j = 0; j < len; ++j) buf[j] = pattern_byte(i, j);
        // Non-blocking: the ring is far larger than the whole run, so a
        // WOULD_BLOCK here would itself be a bug.
        const int wr = shuttle_write(prod, buf, len, SHUTTLE_NONBLOCK);
        if (wr != SHUTTLE_OK) return fail("known: write", wr);
        total += len;
    }
    for (uint64_t i = 0; i < kMsgCount; ++i) {
        const uint64_t len = msg_len(i);
        const long n = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
        if (n < 0 || static_cast<uint64_t>(n) != len)
            return fail("known: read length", n);
        for (uint64_t j = 0; j < len; ++j) {
            if (out[j] != pattern_byte(i, j))
                return fail("known: payload mismatch", static_cast<long>(i));
        }
    }
    *out_bytes = total;
    return 0;
}

// (e) tiny ring, thousands of messages: write until the ring says full, drain
// one, retry. Single-threaded and fully deterministic — no parking, no
// concurrency — while still forcing a wrap every few messages.
int run_wrap_traffic(shuttle_channel* prod, shuttle_channel* cons,
                     uint64_t* out_bytes) {
    unsigned char buf[kSmallMaxPayload];
    unsigned char out[kSmallMaxPayload];
    uint64_t total = 0;
    uint64_t next_read = 0;
    for (uint64_t i = 0; i < kWrapMsgs; ++i) {
        const uint64_t len = wrap_msg_len(i);
        for (uint64_t j = 0; j < len; ++j) buf[j] = pattern_byte(i, j);
        for (;;) {
            const int wr = shuttle_write(prod, buf, len, SHUTTLE_NONBLOCK);
            if (wr == SHUTTLE_OK) break;
            if (wr != SHUTTLE_ERR_WOULD_BLOCK) return fail("wrap: write", wr);
            // Full: drain one message to make room (FIFO, byte-checked).
            const uint64_t elen = wrap_msg_len(next_read);
            const long n =
                shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
            if (n < 0 || static_cast<uint64_t>(n) != elen)
                return fail("wrap: drain length", n);
            for (uint64_t j = 0; j < elen; ++j) {
                if (out[j] != pattern_byte(next_read, j))
                    return fail("wrap: drain payload",
                                static_cast<long>(next_read));
            }
            ++next_read;
        }
        total += len;
    }
    while (next_read < kWrapMsgs) {  // drain the tail
        const uint64_t elen = wrap_msg_len(next_read);
        const long n = shuttle_read(cons, out, sizeof out, SHUTTLE_NONBLOCK);
        if (n < 0 || static_cast<uint64_t>(n) != elen)
            return fail("wrap: tail length", n);
        for (uint64_t j = 0; j < elen; ++j) {
            if (out[j] != pattern_byte(next_read, j))
                return fail("wrap: tail payload", static_cast<long>(next_read));
        }
        ++next_read;
    }
    *out_bytes = total;
    return 0;
}

// (f) the TSan case: two threads on the data path plus a third handle reading
// the counters concurrently. Blocking API on both sides — the peers are always
// live, so parking resolves normally and the heartbeat never goes stale.
int run_threaded_traffic(shuttle_channel* prod, shuttle_channel* cons,
                         shuttle_channel* watcher, uint64_t* out_bytes) {
    std::atomic<int> perr{0}, cerr{0};
    std::atomic<bool> done{false};
    uint64_t total = 0;
    for (uint64_t i = 0; i < kThreadMsgs; ++i) total += thread_msg_len(i);

    std::thread producer([&] {
        unsigned char buf[kSmallMaxPayload];
        for (uint64_t i = 0; i < kThreadMsgs; ++i) {
            const uint64_t len = thread_msg_len(i);
            for (uint64_t j = 0; j < len; ++j) buf[j] = pattern_byte(i, j);
            const int rc = shuttle_write(prod, buf, len, 0);  // blocking
            if (rc != SHUTTLE_OK) {
                perr.store(rc);
                return;
            }
        }
    });
    std::thread consumer([&] {
        unsigned char out[kSmallMaxPayload];
        for (uint64_t i = 0; i < kThreadMsgs; ++i) {
            const uint64_t len = thread_msg_len(i);
            const long n = shuttle_read(cons, out, sizeof out, 0);  // blocking
            if (n < 0 || static_cast<uint64_t>(n) != len) {
                cerr.store(static_cast<int>(n));
                return;
            }
            for (uint64_t j = 0; j < len; ++j) {
                if (out[j] != pattern_byte(i, j)) {
                    cerr.store(-999);
                    return;
                }
            }
        }
    });

    // Third-party observer: a handle that is neither producer nor consumer
    // reading all five counters while they are being written. Each counter has
    // one writer and is monotonic, so an observer may see a stale value but
    // must never see one go BACKWARD — and the read may not be ordered against
    // the other four, which is why nothing here compares written vs read.
    int watch_err = 0;
    shuttle_stats prev;
    std::memset(&prev, 0, sizeof prev);
    while (!done.load(std::memory_order_relaxed)) {
        shuttle_stats s;
        if (shuttle_get_stats(watcher, &s) != SHUTTLE_OK) {
            watch_err = 1;
            break;
        }
        if (s.msgs_written < prev.msgs_written ||
            s.bytes_written < prev.bytes_written ||
            s.msgs_read < prev.msgs_read || s.bytes_read < prev.bytes_read ||
            s.msgs_dropped != 0) {
            watch_err = 2;
            break;
        }
        prev = s;
        if (perr.load() != 0 || cerr.load() != 0) break;
        // The threads are the point; do not starve them.
        std::this_thread::yield();
        if (perr.load() == 0 && cerr.load() == 0 &&
            prev.msgs_read >= kThreadMsgs) {
            done.store(true, std::memory_order_relaxed);
        }
    }
    producer.join();
    consumer.join();
    if (perr.load() != 0) return fail("threads: producer", perr.load());
    if (cerr.load() != 0) return fail("threads: consumer", cerr.load());
    if (watch_err == 1) return fail("threads: get_stats failed mid-run", 0);
    if (watch_err == 2) return fail("threads: counter went backward", 0);
    *out_bytes = total;
    return 0;
}

int run_driver(const char* self) {
    const int pid = static_cast<int>(getpid()) % 1000000;
    char stats_name[32], plain_name[32], wrap_name[32], badv_name[32],
        thr_name[32];
    std::snprintf(stats_name, sizeof stats_name, "/shst.s%d", pid);
    std::snprintf(plain_name, sizeof plain_name, "/shst.p%d", pid);
    std::snprintf(wrap_name, sizeof wrap_name, "/shst.w%d", pid);
    std::snprintf(badv_name, sizeof badv_name, "/shst.v%d", pid);
    std::snprintf(thr_name, sizeof thr_name, "/shst.t%d", pid);
    shuttle_unlink(stats_name);  // clear any stale objects
    shuttle_unlink(plain_name);
    shuttle_unlink(wrap_name);
    shuttle_unlink(badv_name);
    shuttle_unlink(thr_name);

    int fails = 0;
    int err = 0;

    // ---- (a) + (b): a stats segment, then exact counters everywhere -------
    shuttle_channel* prod = shuttle_create_ex(
        stats_name, kCapacity, kMaxPayload, SHUTTLE_CREATE_STATS, &err);
    if (prod == nullptr) {
        ++fails;
        fail("create_ex(STATS)", err);
    } else {
        // (a) fresh v2 segment, seen from another process: version 2, the flag
        // persisted, the v2 data_offset, counters zero.
        if (expect_observed(self, stats_name, shuttle::kVersionStats,
                            SHUTTLE_CREATE_STATS, shuttle::kDataOffsetV2, 0, 0,
                            0, 0, 0) != 0) {
            std::fprintf(stderr, "FAIL: fresh stats segment mis-observed\n");
            ++fails;
        }

        shuttle_channel* cons = shuttle_open(stats_name, &err);
        if (cons == nullptr) {
            ++fails;
            fail("open(stats consumer)", err);
        } else {
            uint64_t bytes = 0;
            if (run_known_traffic(prod, cons, &bytes) != 0) {
                ++fails;
            } else {
                // (b) same-process view, from BOTH handles: the counters are
                // segment state, so the producer's handle sees the consumer's
                // counters and vice versa.
                fails += check_stats("producer handle", prod, kMsgCount, bytes,
                                     0, kMsgCount, bytes);
                fails += check_stats("consumer handle", cons, kMsgCount, bytes,
                                     0, kMsgCount, bytes);
                // ...and a third process reading the header agrees.
                if (expect_observed(self, stats_name, shuttle::kVersionStats,
                                    SHUTTLE_CREATE_STATS,
                                    shuttle::kDataOffsetV2, kMsgCount, bytes, 0,
                                    kMsgCount, bytes) != 0) {
                    std::fprintf(stderr,
                                 "FAIL: child opener disagrees on counters\n");
                    ++fails;
                }
            }
            shuttle_close(cons);
        }
        shuttle_close(prod);
    }

    // ---- (c) plain create is untouched: v1, no stats, transport works -----
    shuttle_channel* plain =
        shuttle_create(plain_name, kCapacity, kMaxPayload, &err);
    if (plain == nullptr) {
        ++fails;
        fail("shuttle_create(plain)", err);
    } else {
        shuttle_stats s;
        const int grc = shuttle_get_stats(plain, &s);
        if (grc != SHUTTLE_ERR_NO_STATS) {
            ++fails;
            fail("plain get_stats should be NO_STATS", grc);
        }
        // v1 layout, no stats flag, v1 data_offset — asserted from a child, so
        // what is checked is the on-disk default format and not just our local
        // view. The counter fields of the spec are ignored for a v1 segment;
        // the child instead requires get_stats to refuse.
        if (expect_observed(self, plain_name, shuttle::kVersion, 0,
                            shuttle::kDataOffsetV1, 0, 0, 0, 0, 0) != 0) {
            std::fprintf(stderr, "FAIL: plain segment is not v1 as created\n");
            ++fails;
        }
        shuttle_channel* pcons = shuttle_open(plain_name, &err);
        if (pcons == nullptr) {
            ++fails;
            fail("open(plain consumer)", err);
        } else {
            unsigned char buf[kPlainLen], out[kPlainLen];
            for (size_t i = 0; i < kPlainLen; ++i) buf[i] = pattern_byte(7, i);
            if (shuttle_write(plain, buf, kPlainLen, SHUTTLE_NONBLOCK) !=
                SHUTTLE_OK) {
                ++fails;
                fail("plain: write", 0);
            } else {
                const long n =
                    shuttle_read(pcons, out, sizeof out, SHUTTLE_NONBLOCK);
                if (n < 0 || static_cast<size_t>(n) != kPlainLen) {
                    ++fails;
                    fail("plain: read length", n);
                } else {
                    for (size_t i = 0; i < kPlainLen; ++i) {
                        if (out[i] != pattern_byte(7, i)) {
                            ++fails;
                            fail("plain: payload mismatch",
                                 static_cast<long>(i));
                            break;
                        }
                    }
                }
            }
            // Still NO_STATS after traffic: a v1 channel never grew counters
            // (and, critically, the data path never wrote into the bytes a v2
            // header would have used — they are payload here).
            if (shuttle_get_stats(pcons, &s) != SHUTTLE_ERR_NO_STATS) {
                ++fails;
                fail("plain consumer get_stats should be NO_STATS", 0);
            }
            shuttle_close(pcons);
        }
        shuttle_close(plain);
    }

    // ---- (d) version/geometry disagreements, distinct verdicts ------------
    // A v1 segment whose version word says 2: known version, wrong geometry
    // (its data_offset is v1's) -> CORRUPT, not BAD_VERSION.
    shuttle_channel* forged =
        shuttle_create(badv_name, kCapacity, kMaxPayload, &err);
    if (forged == nullptr) {
        ++fails;
        fail("create(forge target)", err);
    } else {
        // Positive control: this exact segment opens fine before the poke, so
        // the failures below cannot be "open never works in this test".
        shuttle::Channel* ok = shuttle::open(badv_name, &err);
        if (ok == nullptr) {
            ++fails;
            fail("control: v1 segment must open", err);
        } else {
            shuttle::close(ok);
        }

        if (poke_version(badv_name, shuttle::kVersionStats) != 0) {
            ++fails;
        } else {
            fails += expect_open_error(badv_name, shuttle::kErrCorrupt,
                                       "v1-poked-to-v2 must be kErrCorrupt");
            // Falsifiability: uncomment to watch this case fail — the segment
            // is genuinely rejected, and with THIS code, not kErrBadVersion.
            // fails += expect_open_error(badv_name, shuttle::kErrBadVersion,
            //                            "deliberately wrong expectation");
        }
        shuttle_close(forged);
    }

    // A v2 segment whose version word says 3: no binary knows layout 3 ->
    // BAD_VERSION. (This is also, exactly, what a pre-WP3 binary reports when
    // handed an honest v2 segment: its check is `!= 1`.)
    shuttle_channel* v2 =
        shuttle_create_ex(wrap_name, kSmallCapacity, kSmallMaxPayload,
                          SHUTTLE_CREATE_STATS, &err);
    if (v2 == nullptr) {
        ++fails;
        fail("create_ex(STATS, small)", err);
    } else {
        shuttle::Channel* ok = shuttle::open(wrap_name, &err);  // control
        if (ok == nullptr) {
            ++fails;
            fail("control: v2 segment must open", err);
        } else {
            shuttle::close(ok);
        }
        if (poke_version(wrap_name, shuttle::kVersionStats + 1) != 0) {
            ++fails;
        } else {
            fails += expect_open_error(wrap_name, shuttle::kErrBadVersion,
                                       "v2-poked-to-v3 must be kErrBadVersion");
            // Falsifiability twin of the case above:
            // fails += expect_open_error(wrap_name, shuttle::kErrCorrupt,
            //                            "deliberately wrong expectation");
            if (poke_version(wrap_name, shuttle::kVersionStats) != 0) ++fails;
        }

        // ---- (e) wrap-heavy traffic on that same tiny v2 ring -------------
        shuttle_channel* wcons = shuttle_open(wrap_name, &err);
        if (wcons == nullptr) {
            ++fails;
            fail("open(wrap consumer)", err);
        } else {
            uint64_t bytes = 0;
            if (run_wrap_traffic(v2, wcons, &bytes) != 0) {
                ++fails;
            } else {
                fails += check_stats("wrap producer", v2, kWrapMsgs, bytes, 0,
                                     kWrapMsgs, bytes);
                fails += check_stats("wrap consumer", wcons, kWrapMsgs, bytes,
                                     0, kWrapMsgs, bytes);
                if (expect_observed(self, wrap_name, shuttle::kVersionStats,
                                    SHUTTLE_CREATE_STATS,
                                    shuttle::kDataOffsetV2, kWrapMsgs, bytes, 0,
                                    kWrapMsgs, bytes) != 0) {
                    std::fprintf(stderr,
                                 "FAIL: wrap counters wrong across process\n");
                    ++fails;
                }
            }
            shuttle_close(wcons);
        }
        shuttle_close(v2);
    }

    // ---- (f) two threads on the ring, a third handle watching -------------
    // A fresh segment so the expected totals are the run's own. This is the
    // configuration TSan needs to see: concurrent single-writer counter stores
    // and a concurrent reader.
    shuttle_channel* tprod = shuttle_create_ex(
        thr_name, kSmallCapacity, kSmallMaxPayload, SHUTTLE_CREATE_STATS, &err);
    if (tprod == nullptr) {
        ++fails;
        fail("create_ex(STATS, threaded)", err);
    } else {
        shuttle_channel* tcons = shuttle_open(thr_name, &err);
        shuttle_channel* twatch = shuttle_open(thr_name, &err);
        if (tcons == nullptr || twatch == nullptr) {
            ++fails;
            fail("open(threaded handles)", err);
        } else {
            uint64_t bytes = 0;
            if (run_threaded_traffic(tprod, tcons, twatch, &bytes) != 0) {
                ++fails;
            } else {
                fails += check_stats("threaded", twatch, kThreadMsgs, bytes, 0,
                                     kThreadMsgs, bytes);
            }
        }
        shuttle_close(twatch);
        shuttle_close(tcons);
        shuttle_close(tprod);
    }

    if (shuttle_unlink(stats_name) != SHUTTLE_OK ||
        shuttle_unlink(plain_name) != SHUTTLE_OK ||
        shuttle_unlink(wrap_name) != SHUTTLE_OK ||
        shuttle_unlink(thr_name) != SHUTTLE_OK ||
        shuttle_unlink(badv_name) != SHUTTLE_OK) {
        std::fprintf(stderr, "FAIL: unlink left an object behind\n");
        ++fails;
    }

    if (fails == 0) {
        std::printf("stats_test ok: v2 layout gated by version, counters exact "
                    "in-process, cross-process and under two threads, v1 "
                    "default untouched, version/geometry mismatches rejected "
                    "distinctly (platform=%s)\n",
                    shuttle::platform_name());
    }
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc == 4 && std::strcmp(argv[1], "observe") == 0)
        return run_observe(argv[2], argv[3]);
    std::fprintf(stderr, "usage: %s [observe </shm> <spec>]\n", argv[0]);
    return 2;
}
