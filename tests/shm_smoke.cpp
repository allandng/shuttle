// Phase 0 shm smoke (G0.3): proves POSIX shm works end-to-end on both legs.
//
// Two checks:
//   1. 4 KB segment: create, map, touch, unmap, unlink — toolchain proof.
//   2. 128 MB segment, every page touched: tmpfs only backs pages on write,
//      so this fails with ENOSPC/SIGBUS under Docker's default 64 MB
//      /dev/shm — proving the harness's --shm-size=512m is actually applied.
//      128 MB ~ the >100 MB segment implied by 50 MB payloads + the SRS
//      2x capacity rule.
//
// Names stay short: macOS caps shm names at ~31 chars including the '/'.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

#include "shuttle/platform.hpp"

namespace {

int map_touch_unmap(const char* name, size_t size) {
    shm_unlink(name);  // clear any stale object from a crashed prior run
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        std::perror("shm_open");
        return 1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        std::perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return 1;
    }
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap");
        close(fd);
        shm_unlink(name);
        return 1;
    }
    auto* bytes = static_cast<volatile unsigned char*>(p);
    constexpr size_t kPage = 4096;
    for (size_t i = 0; i < size; i += kPage) {
        bytes[i] = 0xA5;
    }
    bytes[size - 1] = 0x5A;
    int rc = 0;
    if (munmap(p, size) != 0) {
        std::perror("munmap");
        rc = 1;
    }
    close(fd);
    if (shm_unlink(name) != 0) {
        std::perror("shm_unlink");
        rc = 1;
    }
    return rc;
}

}  // namespace

int main() {
    char small[32];
    char big[32];
    std::snprintf(small, sizeof small, "/shsmk4k.%d",
                  static_cast<int>(getpid()) % 1000000);
    std::snprintf(big, sizeof big, "/shsmk128m.%d",
                  static_cast<int>(getpid()) % 1000000);

    if (map_touch_unmap(small, 4096) != 0) {
        std::fprintf(stderr, "shm_smoke FAILED: 4 KB segment\n");
        return 1;
    }
    if (map_touch_unmap(big, 128ull << 20) != 0) {
        std::fprintf(stderr,
                     "shm_smoke FAILED: 128 MB segment (is --shm-size set?)\n");
        return 1;
    }
    std::printf("shm_smoke ok: 4 KB + 128 MB map/touch/unmap (platform=%s)\n",
                shuttle::platform_name());
    return 0;
}
