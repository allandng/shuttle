// G7.2 (NFR-P2): the zero-copy borrow path spends NEGLIGIBLE CPU on
// copy/serialize — proven by CPU accounting (getrusage, the scriptable
// profiler) plus an in-place pointer proof:
//
//   - The Shuttle consumer drains N x 50 MB via acquire/release, touching
//     only two bytes per payload. Its measured CPU must be (a) a small
//     fraction (<= 1/20) of the UDS baseline consumer's CPU for the same
//     bytes — the baseline MUST copy each payload through the kernel into
//     a private buffer — and (b) tiny in absolute terms per message.
//     A hidden memcpy anywhere in the borrow path would multiply the
//     consumer's CPU up to baseline levels and fail (a).
//   - Every borrowed pointer must lie inside the consumer's own mapping of
//     the segment's data region: the consumer is reading the producer's
//     bytes IN PLACE, not from any intermediate buffer.
//
// Built unsanitized (instrumentation would distort the CPU accounting).
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shuttle/platform.hpp"
#include "shuttle/shuttle.hpp"
#include "shuttle/spsc.hpp"

extern char** environ;

namespace {

constexpr uint64_t kSize = 50ull * 1000 * 1000;
constexpr int kIters = 40;  // 2 GB per transport
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;

uint64_t cpu_self_ns() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    auto tv = [](const timeval& t) {
        return static_cast<uint64_t>(t.tv_sec) * 1000000000ull +
               static_cast<uint64_t>(t.tv_usec) * 1000ull;
    };
    return tv(ru.ru_utime) + tv(ru.ru_stime);
}

bool write_all(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n > 0) {
        const ssize_t w = write(fd, c, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        c += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool read_all(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n > 0) {
        const ssize_t r = read(fd, c, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

volatile unsigned char g_sink;  // defeat optimizing away the touches

int shu_producer(const char* name) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Producer p(ch);
    for (int i = 0; i < kIters; ++i) {
        void* span = nullptr;
        if (p.acquire_write(&span, kSize) != shuttle::kOk) return 1;
        std::memset(span, 0x42 + (i & 7), kSize);
        if (p.commit_write(kSize) != shuttle::kOk) return 1;
    }
    shuttle::close(ch);
    return 0;
}

int shu_consumer(const char* name, const char* outpath) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Consumer c(ch);
    const auto* lo = static_cast<const unsigned char*>(
        shuttle::resolve(ch->base, ch->hdr->data_offset));
    const unsigned char* hi = lo + ch->hdr->data_capacity;

    const uint64_t cpu0 = cpu_self_ns();
    for (int i = 0; i < kIters; ++i) {
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.read(&p, &len) != shuttle::kOk || len != kSize) return 1;
        if (p < lo || p + len > hi) {
            std::fprintf(stderr,
                         "shu-cons: borrowed ptr OUTSIDE the mapped data"
                         " region — not reading in place\n");
            return 1;
        }
        g_sink = static_cast<unsigned char>(g_sink ^ p[0] ^ p[len - 1]);
        c.release();
    }
    const uint64_t cpu = cpu_self_ns() - cpu0;
    FILE* f = std::fopen(outpath, "w");
    if (f == nullptr) return 1;
    std::fprintf(f, "%llu\n", (unsigned long long)cpu);
    std::fclose(f);
    shuttle::close(ch);
    return 0;
}

int uds_producer(const char* path) {
    int fd = -1;
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0)
            break;
        close(fd);
        if (shuttle::monotonic_ns() > deadline) return 1;
        usleep(2000);
    }
    std::vector<unsigned char> buf(kSize);
    for (int i = 0; i < kIters; ++i) {
        std::memset(buf.data(), 0x42 + (i & 7), kSize);
        const uint64_t len = kSize;
        if (!write_all(fd, &len, 8) || !write_all(fd, buf.data(), kSize))
            return 1;
    }
    close(fd);
    return 0;
}

int uds_consumer(const char* path, const char* outpath) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(s, 1) != 0)
        return 1;
    int fd = accept(s, nullptr, nullptr);
    if (fd < 0) return 1;
    std::vector<unsigned char> buf(kSize);

    const uint64_t cpu0 = cpu_self_ns();
    for (int i = 0; i < kIters; ++i) {
        uint64_t len = 0;
        if (!read_all(fd, &len, 8) || len != kSize) return 1;
        if (!read_all(fd, buf.data(), len)) return 1;  // the obligatory copy
        g_sink = static_cast<unsigned char>(g_sink ^ buf[0] ^ buf[len - 1]);
    }
    const uint64_t cpu = cpu_self_ns() - cpu0;
    FILE* f = std::fopen(outpath, "w");
    if (f == nullptr) return 1;
    std::fprintf(f, "%llu\n", (unsigned long long)cpu);
    std::fclose(f);
    close(fd);
    close(s);
    unlink(path);
    return 0;
}

pid_t spawn_role(const char* self, const char* role, const char* a1,
                 const char* a2) {
    char* argv[5];
    int n = 0;
    argv[n++] = const_cast<char*>(self);
    argv[n++] = const_cast<char*>(role);
    argv[n++] = const_cast<char*>(a1);
    if (a2 != nullptr) argv[n++] = const_cast<char*>(a2);
    argv[n] = nullptr;
    pid_t pid = 0;
    if (posix_spawn(&pid, self, nullptr, nullptr, argv, environ) != 0)
        return -1;
    return pid;
}

int wait_deadline(pid_t pid, const char* what) {
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                std::fprintf(stderr, "%s failed (0x%x)\n", what, st);
                return 1;
            }
            return 0;
        }
        if (shuttle::monotonic_ns() > deadline) {
            std::fprintf(stderr, "%s TIMEOUT\n", what);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return 1;
        }
        usleep(5000);
    }
}

bool load_u64(const char* path, uint64_t* out) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    unsigned long long x = 0;
    const bool ok = std::fscanf(f, "%llu", &x) == 1;
    std::fclose(f);
    *out = x;
    return ok;
}

int run_driver(const char* self) {
    char tag[32], shm[32], sock[64], out_s[64], out_u[64];
    std::snprintf(tag, sizeof tag, "%d", static_cast<int>(getpid()) % 100000);
    std::snprintf(shm, sizeof shm, "/shnc.%s", tag);
    std::snprintf(sock, sizeof sock, "/tmp/shnc.%s.sock", tag);
    std::snprintf(out_s, sizeof out_s, "/tmp/shnc.%s.shu", tag);
    std::snprintf(out_u, sizeof out_u, "/tmp/shnc.%s.uds", tag);

    // Shuttle pair.
    shuttle::unlink(shm);
    int err = 0;
    shuttle::Channel* ch =
        shuttle::create(shm, 128ull << 20, 64ull << 20, &err);
    if (ch == nullptr) {
        std::fprintf(stderr, "driver: create err=%d\n", err);
        return 1;
    }
    int fails = 0;
    {
        pid_t c = spawn_role(self, "shu-cons", shm, out_s);
        pid_t p = spawn_role(self, "shu-prod", shm, nullptr);
        fails += wait_deadline(p, "shu-prod");
        fails += wait_deadline(c, "shu-cons");
    }
    shuttle::close(ch);
    shuttle::unlink(shm);

    // UDS baseline pair.
    {
        pid_t c = spawn_role(self, "uds-cons", sock, out_u);
        pid_t p = spawn_role(self, "uds-prod", sock, nullptr);
        fails += wait_deadline(p, "uds-prod");
        fails += wait_deadline(c, "uds-cons");
    }
    if (fails != 0) return 1;

    uint64_t cpu_s = 0, cpu_u = 0;
    if (!load_u64(out_s, &cpu_s) || !load_u64(out_u, &cpu_u)) {
        std::fprintf(stderr, "driver: missing result files\n");
        return 1;
    }
    const double gb = kIters * kSize / 1e9;
    std::printf("consumer CPU to receive %.1f GB (50 MB msgs):\n", gb);
    std::printf("  shuttle borrow path: %8.2f ms (%6.1f us/msg)\n",
                cpu_s / 1e6, cpu_s / 1e3 / kIters);
    std::printf("  uds copy baseline:   %8.2f ms (%6.1f us/msg)\n",
                cpu_u / 1e6, cpu_u / 1e3 / kIters);
    std::printf("  ratio: shuttle uses %.2f%% of the baseline's CPU\n",
                100.0 * cpu_s / cpu_u);

    if (cpu_s * 20 > cpu_u) {
        std::fprintf(stderr,
                     "FAIL: borrow-path CPU is not negligible (> 1/20 of"
                     " the copy baseline) — is something copying?\n");
        return 1;
    }
    if (cpu_s / kIters > 500ull * 1000) {  // 500 us per 50 MB message
        std::fprintf(stderr, "FAIL: absolute borrow-path CPU too high\n");
        return 1;
    }
    std::printf("nocopy_cpu ok: NFR-P2 — in-place borrow verified, copy"
                " CPU negligible\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc >= 3) {
        const std::string role = argv[1];
        if (role == "shu-prod") return shu_producer(argv[2]);
        if (role == "shu-cons" && argc == 4)
            return shu_consumer(argv[2], argv[3]);
        if (role == "uds-prod") return uds_producer(argv[2]);
        if (role == "uds-cons" && argc == 4)
            return uds_consumer(argv[2], argv[3]);
    }
    std::fprintf(stderr, "usage: %s [<role> <rendezvous> [outfile]]\n",
                 argv[0]);
    return 2;
}
