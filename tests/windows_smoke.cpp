// EXPERIMENTAL Windows backend smoke (WP8). Windows-only; built and run ONLY by
// the windows-latest CI job (see .github/workflows/ci.yml and CMakeLists.txt).
//
// It drives the REAL Win32 platform seam — a named CreateFileMappingW section
// plus WaitOnAddress/WakeByAddressAll park/wake — two ways:
//   1. threads in one process (copy path + zero-copy borrow path), and
//   2. two processes via CreateProcess, proving the named section is visible
//      cross-process and the park/wake wakes a peer in another address space.
//
// This is a SMOKE test, not a parity gate. There is deliberately no crash
// recovery, no robust-mutex, and no posix_spawn multi-process suite here —
// those are POSIX-only (see docs/ROADMAP.md and the POSIX gate tests).
// Heartbeat liveness remains the Windows crash story, exactly as on macOS.
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !defined(_WIN32)
int main() {
    std::printf("windows_smoke: skipped (not Windows)\n");
    return 0;
}
#else

#include <windows.h>

#include <thread>

#include "shuttle/shuttle.hpp"
#include "shuttle/spsc.hpp"

namespace {

constexpr int kThreadMsgs = 2000;
constexpr int kBorrowMsgs = 500;
constexpr int kProcMsgs = 500;

// Deterministic payload for message i: length varies with i, and every byte is
// a checkable function of (i, position), so a reorder or a torn byte is caught.
uint64_t fill_payload(int i, unsigned char* buf, uint64_t cap) {
    const uint64_t len = 8 + static_cast<uint64_t>((i * 37) % 200);
    for (uint64_t j = 0; j < len && j < cap; ++j)
        buf[j] = static_cast<unsigned char>((i * 31 + static_cast<int>(j) * 7) &
                                            0xFF);
    return len;
}
bool check_payload(int i, const unsigned char* p, uint64_t len) {
    unsigned char expect[256];
    const uint64_t want = fill_payload(i, expect, sizeof expect);
    if (len != want) return false;
    return std::memcmp(p, expect, static_cast<size_t>(len)) == 0;
}

// One producer thread + one consumer thread over the copy path. prod_fails and
// cons_fails are each written by exactly one thread (single-writer), so there
// is no race on them.
int threads_roundtrip() {
    const char* name = "/winsmoke_threads";
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "threads: create err=%d\n", err);
        return 1;
    }
    int prod_fails = 0;
    int cons_fails = 0;
    std::thread prod([&] {
        shuttle::Producer p(ch);
        unsigned char buf[256];
        for (int i = 0; i < kThreadMsgs; ++i) {
            const uint64_t len = fill_payload(i, buf, sizeof buf);
            if (p.write(buf, len) != shuttle::kOk) {
                ++prod_fails;
                return;
            }
        }
    });
    std::thread cons([&] {
        shuttle::Consumer c(ch);
        for (int i = 0; i < kThreadMsgs; ++i) {
            const unsigned char* p = nullptr;
            uint64_t len = 0;
            if (c.read(&p, &len) != shuttle::kOk) {
                ++cons_fails;
                return;
            }
            if (!check_payload(i, p, len)) ++cons_fails;
            c.release();
        }
    });
    prod.join();
    cons.join();
    shuttle::close(ch);
    shuttle::unlink(name);
    const int fails = prod_fails + cons_fails;
    if (fails == 0)
        std::printf("threads: %d messages byte-exact over the Win32 seam\n",
                    kThreadMsgs);
    else
        std::fprintf(stderr, "threads: %d failures\n", fails);
    return fails == 0 ? 0 : 1;
}

// The zero-copy borrow path (acquire_write/commit_write, read/release).
int borrow_roundtrip() {
    const char* name = "/winsmoke_borrow";
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "borrow: create err=%d\n", err);
        return 1;
    }
    int prod_fails = 0;
    int cons_fails = 0;
    std::thread prod([&] {
        shuttle::Producer p(ch);
        unsigned char tmp[256];
        for (int i = 0; i < kBorrowMsgs; ++i) {
            void* span = nullptr;
            const uint64_t len = fill_payload(i, tmp, sizeof tmp);
            if (p.acquire_write(&span, len) != shuttle::kOk) {
                ++prod_fails;
                return;
            }
            std::memcpy(span, tmp, static_cast<size_t>(len));
            if (p.commit_write(len) != shuttle::kOk) {
                ++prod_fails;
                return;
            }
        }
    });
    std::thread cons([&] {
        shuttle::Consumer c(ch);
        for (int i = 0; i < kBorrowMsgs; ++i) {
            const unsigned char* p = nullptr;
            uint64_t len = 0;
            if (c.read(&p, &len) != shuttle::kOk) {
                ++cons_fails;
                return;
            }
            if (!check_payload(i, p, len)) ++cons_fails;
            c.release();
        }
    });
    prod.join();
    cons.join();
    shuttle::close(ch);
    shuttle::unlink(name);
    const int fails = prod_fails + cons_fails;
    if (fails == 0)
        std::printf("borrow: %d zero-copy messages byte-exact\n", kBorrowMsgs);
    else
        std::fprintf(stderr, "borrow: %d failures\n", fails);
    return fails == 0 ? 0 : 1;
}

// Child process: open the named section the parent created, drain kProcMsgs
// messages, verify each, exit 0 on success.
int run_child(const char* name) {
    int err = 0;
    shuttle::Channel* ch = nullptr;
    // The parent creates before spawning us; retry-open briefly against skew.
    for (int t = 0; t < 400 && ch == nullptr; ++t) {
        ch = shuttle::open(name, &err);
        if (ch == nullptr) Sleep(5);
    }
    if (ch == nullptr) {
        std::fprintf(stderr, "child: open err=%d\n", err);
        return 1;
    }
    shuttle::Consumer c(ch);
    int fails = 0;
    for (int i = 0; i < kProcMsgs; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.read(&p, &len) != shuttle::kOk) {
            ++fails;
            break;
        }
        if (!check_payload(i, p, len)) ++fails;
        c.release();
    }
    shuttle::close(ch);
    return fails == 0 ? 0 : 2;
}

// Parent: create the section, CreateProcess a child consumer, produce kProcMsgs
// messages, then verify the child exited cleanly.
int process_echo(const char* self) {
    const char* name = "/winsmoke_proc";
    shuttle::unlink(name);
    int err = 0;
    shuttle::Channel* ch = shuttle::create(name, 1u << 20, 1u << 16, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "proc: create err=%d\n", err);
        return 1;
    }

    char cmdline[1024];
    std::snprintf(cmdline, sizeof cmdline, "\"%s\" child %s", self, name);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        std::fprintf(stderr, "proc: CreateProcess failed (%lu)\n",
                     GetLastError());
        shuttle::close(ch);
        shuttle::unlink(name);
        return 1;
    }

    int fails = 0;
    shuttle::Producer p(ch);
    unsigned char buf[256];
    for (int i = 0; i < kProcMsgs; ++i) {
        const uint64_t len = fill_payload(i, buf, sizeof buf);
        if (p.write(buf, len) != shuttle::kOk) {
            ++fails;
            break;
        }
    }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) {
        std::fprintf(stderr, "proc: child exit=%lu\n", code);
        ++fails;
    }

    shuttle::close(ch);
    shuttle::unlink(name);
    if (fails == 0)
        std::printf("processes: %d messages echoed across a named section\n",
                    kProcMsgs);
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "child") == 0)
        return run_child(argv[2]);

    char self[MAX_PATH];
    if (GetModuleFileNameA(nullptr, self, MAX_PATH) == 0) {
        std::fprintf(stderr, "GetModuleFileName failed (%lu)\n",
                     GetLastError());
        return 1;
    }

    int fails = 0;
    fails += threads_roundtrip();
    fails += borrow_roundtrip();
    fails += process_echo(self);
    if (fails == 0)
        std::printf("windows_smoke OK: Win32 named-section + WaitOnAddress seam"
                    " verified (threads + processes)\n");
    return fails == 0 ? 0 : 1;
}
#endif  // _WIN32
