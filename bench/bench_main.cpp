// Phase 7 headline benchmark (G7.1): identical workload over three
// transports —
//   1. Shuttle zero-copy borrow path,
//   2. raw-binary localhost HTTP (D7: body = payload, keep-alive, sensible
//      socket buffers, no JSON/base64 — HTTP doing the least wasteful
//      thing it can),
//   3. raw-binary Unix domain socket (binding minor amendment).
//
// Metric: end-to-end latency, producer-commit -> consumer-holds-full-
// payload, via CLOCK_MONOTONIC timestamps written into the payload head
// (cross-process, same machine). Producers fill the whole payload BEFORE
// stamping on every transport, so payload generation is identically
// excluded and only transport time is measured. Warm-up iterations are
// discarded; median and p99 reported.
//
// Workloads: 50 MB blob (the gate: both ratios >= 10x), and a 16 KB frame
// stream (throughput, informational — NFR-P4 context). The stream figure is
// timed by the CONSUMER over its post-warm-up frames only, not by the driver's
// wall clock around the process pair — see write_window() for why.
//
// This binary is built WITHOUT sanitizers (they would invalidate the
// comparison). Numbers printed under a container are labeled
// "virtualized - not headline figures" per the amendment.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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

constexpr uint64_t kBlobSize = 50ull * 1000 * 1000;  // 50 MB
constexpr int kBlobWarmup = 3;
constexpr int kBlobIters = 20;
constexpr uint64_t kFrameSize = 16 * 1024;
constexpr int kFrameWarmup = 500;
constexpr int kFrameIters = 5000;
constexpr uint64_t kChildTimeoutNs = 240ull * 1000000000ull;
constexpr int kSockBuf = 4 << 20;  // D7: sensible socket buffers

// ---------- small utilities ----------

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

pid_t spawn_role(const char* self, std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(self));
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, self, nullptr, nullptr, argv.data(), environ) != 0)
        return -1;
    return pid;
}

struct Stats {
    double median_us = 0, p99_us = 0;
};

// Reads ns-per-line latency file, drops `warmup` leading entries.
bool load_stats(const char* path, int warmup, Stats* out) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    std::vector<uint64_t> v;
    unsigned long long x;
    while (std::fscanf(f, "%llu", &x) == 1) v.push_back(x);
    std::fclose(f);
    if (v.size() <= static_cast<size_t>(warmup)) return false;
    v.erase(v.begin(), v.begin() + warmup);
    std::sort(v.begin(), v.end());
    out->median_us = v[v.size() / 2] / 1e3;
    out->p99_us = v[v.size() * 99 / 100] / 1e3;
    return true;
}

// ---------- steady-state throughput window (E7) ----------
//
// The driver's wall clock around a spawned pair is NOT a throughput
// measurement. Besides the frames it also contains the segment/socket setup,
// two posix_spawns, both children's startup and teardown, and wait_deadline's
// poll loop — several milliseconds against a 16 KB stream whose actual
// transfer is under three. Worse, that poll loop sleeps 5 ms per turn, so
// whether a child's exit is noticed on poll N or poll N+1 shifted the
// denominator by ~5 ms and the reported MB/s by a factor of two: the bimodal
// Shuttle stream row in E6 (13734 / 6717 / 13291 MB/s) was that, and nothing
// about the transport — in the reproduction the consumer's own window was
// unchanged in the low run. The baselines looked flat only because the same
// fixed overhead sits next to a much larger true signal.
//
// So the consumer times itself: from just before it takes delivery of the
// first TIMED frame to just after the last one. That window contains only
// transferred frames — no setup, no teardown, no warm-up — and it is what
// `frames_per_s` / `frame_mbps` are computed from. It is written to a sidecar
// file so the latency file stays exactly one ns sample per line.
void write_window(const char* outpath, uint64_t start_ns, uint64_t end_ns) {
    if (outpath == nullptr || start_ns == 0) return;
    char wp[128];
    std::snprintf(wp, sizeof wp, "%s.win", outpath);
    FILE* f = std::fopen(wp, "w");
    if (f == nullptr) return;
    std::fprintf(f, "%llu\n", (unsigned long long)(end_ns - start_ns));
    std::fclose(f);
}

bool load_window(const char* outpath, uint64_t* out) {
    char wp[128];
    std::snprintf(wp, sizeof wp, "%s.win", outpath);
    FILE* f = std::fopen(wp, "r");
    if (f == nullptr) return false;
    unsigned long long v = 0;
    const bool ok = std::fscanf(f, "%llu", &v) == 1 && v > 0;
    std::fclose(f);
    if (ok) *out = v;
    return ok;
}

void fill_payload(unsigned char* p, uint64_t n, uint64_t iter) {
    // Cheap full-payload write so every transport pays identical
    // generation cost BEFORE the timestamp is taken.
    std::memset(p, static_cast<int>(0xA0 + (iter & 0xF)), n);
}

void stamp(unsigned char* p) {
    const uint64_t t = shuttle::monotonic_ns();
    std::memcpy(p, &t, 8);
}

uint64_t read_stamp_delta(const unsigned char* p) {
    uint64_t t = 0;
    std::memcpy(&t, p, 8);
    return shuttle::monotonic_ns() - t;
}

// ---------- Shuttle transport ----------

int shu_producer(const char* name, uint64_t size, int iters) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Producer p(ch);
    for (int i = 0; i < iters; ++i) {
        void* span = nullptr;
        if (p.acquire_write(&span, size) != shuttle::kOk) return 1;
        fill_payload(static_cast<unsigned char*>(span), size,
                     static_cast<uint64_t>(i));
        stamp(static_cast<unsigned char*>(span));
        if (p.commit_write(size) != shuttle::kOk) return 1;
    }
    shuttle::close(ch);
    return 0;
}

int shu_consumer(const char* name, uint64_t size, int iters,
                 const char* outpath, int warmup) {
    int err = 0;
    shuttle::Channel* ch = shuttle::open(name, &err);
    if (ch == nullptr) return 1;
    shuttle::Consumer c(ch);
    FILE* out = std::fopen(outpath, "w");
    if (out == nullptr) return 1;
    uint64_t t_win = 0;
    for (int i = 0; i < iters; ++i) {
        if (i == warmup) t_win = shuttle::monotonic_ns();
        const unsigned char* p = nullptr;
        uint64_t len = 0;
        if (c.read(&p, &len) != shuttle::kOk || len != size) return 1;
        // Zero-copy: the full payload is resident at borrow return.
        std::fprintf(out, "%llu\n",
                     (unsigned long long)read_stamp_delta(p));
        c.release();
    }
    write_window(outpath, t_win, shuttle::monotonic_ns());
    std::fclose(out);
    shuttle::close(ch);
    return 0;
}

// ---------- UDS / TCP common framing: [u64 len | payload] ----------

int stream_producer(int fd, uint64_t size, int iters) {
    std::vector<unsigned char> buf(size);
    for (int i = 0; i < iters; ++i) {
        fill_payload(buf.data(), size, static_cast<uint64_t>(i));
        stamp(buf.data());
        const uint64_t len = size;
        if (!write_all(fd, &len, 8) || !write_all(fd, buf.data(), size))
            return 1;
    }
    return 0;
}

int stream_consumer(int fd, uint64_t size, int iters, const char* outpath,
                    int warmup) {
    std::vector<unsigned char> buf(size);
    FILE* out = std::fopen(outpath, "w");
    if (out == nullptr) return 1;
    uint64_t t_win = 0;
    for (int i = 0; i < iters; ++i) {
        if (i == warmup) t_win = shuttle::monotonic_ns();
        uint64_t len = 0;
        if (!read_all(fd, &len, 8) || len != size) return 1;
        if (!read_all(fd, buf.data(), len)) return 1;
        std::fprintf(out, "%llu\n",
                     (unsigned long long)read_stamp_delta(buf.data()));
    }
    write_window(outpath, t_win, shuttle::monotonic_ns());
    std::fclose(out);
    return 0;
}

void set_bufs(int fd) {
    int v = kSockBuf;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v);
}

int uds_consumer(const char* path, uint64_t size, int iters,
                 const char* outpath, int warmup) {
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
    set_bufs(fd);
    const int rc = stream_consumer(fd, size, iters, outpath, warmup);
    close(fd);
    close(s);
    unlink(path);
    return rc;
}

int uds_producer(const char* path, uint64_t size, int iters) {
    int fd = -1;
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    for (;;) {  // server may not be listening yet
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
    set_bufs(fd);
    const int rc = stream_producer(fd, size, iters);
    close(fd);
    return rc;
}

// ---------- HTTP/1.1 baseline ----------

int http_consumer(const char* portfile, uint64_t size, int iters,
                  const char* outpath, int warmup) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(s, 1) != 0)
        return 1;
    socklen_t alen = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen);
    {
        FILE* pf = std::fopen(portfile, "w");
        if (pf == nullptr) return 1;
        std::fprintf(pf, "%d\n", ntohs(addr.sin_port));
        std::fclose(pf);
    }
    int fd = accept(s, nullptr, nullptr);
    if (fd < 0) return 1;
    set_bufs(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    std::vector<unsigned char> body(size);
    std::string carry;  // bytes read past the header terminator
    FILE* out = std::fopen(outpath, "w");
    if (out == nullptr) return 1;
    const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    uint64_t t_win = 0;
    for (int i = 0; i < iters; ++i) {
        if (i == warmup) t_win = shuttle::monotonic_ns();
        // Read headers until CRLFCRLF (carry may already contain them).
        std::string hdr = carry;
        carry.clear();
        size_t hend;
        while ((hend = hdr.find("\r\n\r\n")) == std::string::npos) {
            char tmp[8192];
            const ssize_t r = read(fd, tmp, sizeof tmp);
            if (r <= 0) return 1;
            hdr.append(tmp, static_cast<size_t>(r));
        }
        const char* cl = std::strstr(hdr.c_str(), "Content-Length:");
        if (cl == nullptr) return 1;
        const uint64_t len = std::strtoull(cl + 15, nullptr, 10);
        if (len != size) return 1;
        // Body: prefix may have arrived with the headers.
        const size_t have = hdr.size() - (hend + 4);
        std::memcpy(body.data(), hdr.data() + hend + 4, have);
        if (!read_all(fd, body.data() + have, len - have)) return 1;
        std::fprintf(out, "%llu\n",
                     (unsigned long long)read_stamp_delta(body.data()));
        if (!write_all(fd, resp, sizeof resp - 1)) return 1;
    }
    write_window(outpath, t_win, shuttle::monotonic_ns());
    std::fclose(out);
    close(fd);
    close(s);
    return 0;
}

int http_producer(const char* portfile, uint64_t size, int iters) {
    int port = 0;
    const uint64_t deadline = shuttle::monotonic_ns() + kChildTimeoutNs;
    while (port == 0) {
        FILE* pf = std::fopen(portfile, "r");
        if (pf != nullptr) {
            if (std::fscanf(pf, "%d", &port) != 1) port = 0;
            std::fclose(pf);
        }
        if (port == 0) {
            if (shuttle::monotonic_ns() > deadline) return 1;
            usleep(2000);
        }
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    for (;;) {
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0)
            break;
        if (shuttle::monotonic_ns() > deadline) return 1;
        usleep(2000);
    }
    set_bufs(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    std::vector<unsigned char> body(size);
    char hdr[160];
    char resp[256];
    for (int i = 0; i < iters; ++i) {
        fill_payload(body.data(), size, static_cast<uint64_t>(i));
        const int hl = std::snprintf(
            hdr, sizeof hdr,
            "POST /msg HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: %llu\r\n\r\n",
            (unsigned long long)size);
        stamp(body.data());
        if (!write_all(fd, hdr, static_cast<size_t>(hl)) ||
            !write_all(fd, body.data(), size))
            return 1;
        // Keep-alive: consume the 200 before the next request.
        if (!read_all(fd, resp, 38)) return 1;  // fixed-size response
    }
    close(fd);
    return 0;
}

// ---------- driver ----------

struct RunResult {
    Stats blob;
    double frame_mbps = 0, frames_per_s = 0;
};

int run_pair(const char* self, const std::string& cons_role,
             const std::string& prod_role, const std::string& rendezvous,
             uint64_t size, int iters, const char* outpath, int warmup) {
    pid_t cons = spawn_role(self, {cons_role, rendezvous,
                                   std::to_string(size),
                                   std::to_string(iters), outpath,
                                   std::to_string(warmup)});
    if (cons < 0) return 1;
    pid_t prod = spawn_role(self, {prod_role, rendezvous,
                                   std::to_string(size),
                                   std::to_string(iters)});
    if (prod < 0) {
        kill(cons, SIGKILL);
        waitpid(cons, nullptr, 0);
        return 1;
    }
    int fails = wait_deadline(prod, prod_role.c_str());
    fails += wait_deadline(cons, cons_role.c_str());
    return fails;
}

int bench_transport(const char* self, const char* tag, RunResult* rr) {
    char tmp[64], out[96], rdv[96];
    std::snprintf(tmp, sizeof tmp, "%d", static_cast<int>(getpid()) % 100000);
    std::snprintf(out, sizeof out, "/tmp/shb.%s.%s.lat", tag, tmp);

    // Rendezvous per transport.
    std::string cons_role = std::string(tag) + "-cons";
    std::string prod_role = std::string(tag) + "-prod";
    if (std::strcmp(tag, "shu") == 0) {
        std::snprintf(rdv, sizeof rdv, "/shb.%s", tmp);
    } else if (std::strcmp(tag, "uds") == 0) {
        std::snprintf(rdv, sizeof rdv, "/tmp/shb.uds.%s.sock", tmp);
    } else {
        std::snprintf(rdv, sizeof rdv, "/tmp/shb.http.%s.port", tmp);
    }

    // 50 MB blob workload.
    if (std::strcmp(tag, "shu") == 0) {
        shuttle::unlink(rdv);
        int err = 0;
        shuttle::Channel* ch =
            shuttle::create(rdv, 128ull << 20, 64ull << 20, &err);
        if (ch == nullptr) return 1;
        if (run_pair(self, cons_role, prod_role, rdv, kBlobSize,
                     kBlobWarmup + kBlobIters, out, kBlobWarmup) != 0)
            return 1;
        shuttle::close(ch);
        shuttle::unlink(rdv);
    } else {
        unlink(rdv);
        if (run_pair(self, cons_role, prod_role, rdv, kBlobSize,
                     kBlobWarmup + kBlobIters, out, kBlobWarmup) != 0)
            return 1;
    }
    if (!load_stats(out, kBlobWarmup, &rr->blob)) return 1;

    // 16 KB frame stream (throughput, informational).
    char out2[96];
    std::snprintf(out2, sizeof out2, "/tmp/shb.%s.%s.frames", tag, tmp);
    if (std::strcmp(tag, "shu") == 0) {
        shuttle::unlink(rdv);
        int err = 0;
        shuttle::Channel* ch =
            shuttle::create(rdv, 8ull << 20, 1ull << 20, &err);
        if (ch == nullptr) return 1;
        if (run_pair(self, cons_role, prod_role, rdv, kFrameSize,
                     kFrameWarmup + kFrameIters, out2, kFrameWarmup) != 0)
            return 1;
        shuttle::close(ch);
        shuttle::unlink(rdv);
    } else {
        unlink(rdv);
        if (run_pair(self, cons_role, prod_role, rdv, kFrameSize,
                     kFrameWarmup + kFrameIters, out2, kFrameWarmup) != 0)
            return 1;
    }
    // Consumer-timed steady-state window: kFrameIters frames, warm-up and
    // process lifecycle excluded (write_window above).
    uint64_t win_ns = 0;
    if (!load_window(out2, &win_ns)) return 1;
    const double secs = win_ns / 1e9;
    const double frames = kFrameIters;
    rr->frames_per_s = frames / secs;
    rr->frame_mbps = frames * kFrameSize / 1e6 / secs;
    return 0;
}

bool in_container() {
    struct stat st;
    return stat("/.dockerenv", &st) == 0;
}

int run_driver(const char* self) {
    RunResult shu, uds, http;
    if (bench_transport(self, "shu", &shu) != 0) {
        std::fprintf(stderr, "FAIL: shuttle transport bench\n");
        return 1;
    }
    if (bench_transport(self, "uds", &uds) != 0) {
        std::fprintf(stderr, "FAIL: uds transport bench\n");
        return 1;
    }
    if (bench_transport(self, "http", &http) != 0) {
        std::fprintf(stderr, "FAIL: http transport bench\n");
        return 1;
    }

    const char* label =
        in_container() ? "VIRTUALIZED (container) - not headline figures"
                       : "native";
    std::printf("=== Shuttle headline benchmark (%s, %s) ===\n",
                shuttle::platform_name(), label);
    std::printf("50 MB blob, %d iters (+%d warmup), commit->payload-held:\n",
                kBlobIters, kBlobWarmup);
    std::printf("  shuttle  median %10.1f us   p99 %10.1f us\n",
                shu.blob.median_us, shu.blob.p99_us);
    std::printf("  uds      median %10.1f us   p99 %10.1f us\n",
                uds.blob.median_us, uds.blob.p99_us);
    std::printf("  http     median %10.1f us   p99 %10.1f us\n",
                http.blob.median_us, http.blob.p99_us);
    const double r_uds = uds.blob.median_us / shu.blob.median_us;
    const double r_http = http.blob.median_us / shu.blob.median_us;
    std::printf("  ratio: uds/shuttle %.1fx, http/shuttle %.1fx"
                " (gate: both >= 10x)\n",
                r_uds, r_http);
    std::printf("16 KB stream throughput (%d timed frames, consumer-timed):"
                " shuttle %.0f MB/s, uds %.0f MB/s, http %.0f MB/s\n",
                kFrameIters,
                shu.frame_mbps, uds.frame_mbps, http.frame_mbps);

    if (r_uds < 10.0 || r_http < 10.0) {
        std::fprintf(stderr,
                     "FAIL: NFR-P1 ratio below 10x (uds %.1fx, http %.1fx)\n",
                     r_uds, r_http);
        return 1;
    }
    std::printf("bench_g71 ok: NFR-P1 satisfied against both baselines\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) return run_driver(argv[0]);
    if (argc >= 5) {
        const std::string role = argv[1];
        const char* rdv = argv[2];
        const uint64_t size = std::strtoull(argv[3], nullptr, 10);
        const int iters = std::atoi(argv[4]);
        const char* out = argc >= 6 ? argv[5] : nullptr;
        const int warmup = argc >= 7 ? std::atoi(argv[6]) : 0;
        if (role == "shu-prod") return shu_producer(rdv, size, iters);
        if (role == "shu-cons")
            return shu_consumer(rdv, size, iters, out, warmup);
        if (role == "uds-prod") return uds_producer(rdv, size, iters);
        if (role == "uds-cons")
            return uds_consumer(rdv, size, iters, out, warmup);
        if (role == "http-prod") return http_producer(rdv, size, iters);
        if (role == "http-cons")
            return http_consumer(rdv, size, iters, out, warmup);
    }
    std::fprintf(stderr, "usage: %s [<role> <rendezvous> <size> <iters>"
                         " [latfile [warmup]]]\n",
                 argv[0]);
    return 2;
}
