// shuttle_inspect <shm-name>: dump a segment's header. Debugging aid for
// Phases 1-5 (plan: "you will use it constantly"). Read-only; deliberately
// does NOT validate — its job is to show whatever is there, valid or not.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>

#include "shuttle/header.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s </shm-name>\n", argv[0]);
        return 2;
    }
    int fd = shm_open(argv[1], O_RDONLY, 0);
    if (fd < 0) {
        std::perror("shm_open");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        std::perror("fstat");
        close(fd);
        return 1;
    }
    // The v1 header is the smallest thing that can still be a channel, so that
    // is the bar; a v1 segment must keep dumping cleanly now that
    // sizeof(ChannelHeader) is the LARGER v2 size. Map no more than the object
    // actually has — mapping past the end would fault on first touch.
    if (st.st_size < static_cast<off_t>(shuttle::kDataOffsetV1)) {
        std::fprintf(stderr, "segment too small for a header (%lld bytes)\n",
                     static_cast<long long>(st.st_size));
        close(fd);
        return 1;
    }
    const size_t map_len =
        static_cast<uint64_t>(st.st_size) < shuttle::kDataOffsetV2
            ? static_cast<size_t>(st.st_size)
            : static_cast<size_t>(shuttle::kDataOffsetV2);
    void* p = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    const auto* h = static_cast<const shuttle::ChannelHeader*>(p);
    std::printf("segment            %s (%lld bytes total)\n", argv[1],
                static_cast<long long>(st.st_size));
    std::printf("magic              0x%016" PRIx64 " (%s)\n", h->magic,
                h->magic == shuttle::kMagic ? "ok" : "MISMATCH");
    std::printf("version            %" PRIu32 "   flags 0x%" PRIx32 "\n",
                h->version, h->flags);
    std::printf("init_state         0x%" PRIx32 " (%s)\n",
                h->init_state.load(std::memory_order_acquire),
                h->init_state.load(std::memory_order_acquire) ==
                        shuttle::kInitReady
                    ? "ready"
                    : "NOT READY");
    std::printf("data_offset        %" PRIu64 "\n", h->data_offset);
    std::printf("data_capacity      %" PRIu64 "\n", h->data_capacity);
    std::printf("max_payload        %" PRIu64 "\n", h->max_payload);
    std::printf("write              %" PRIu64 "\n",
                h->write.load(std::memory_order_relaxed));
    std::printf("watermark          %" PRIu64 "\n",
                h->watermark.load(std::memory_order_relaxed));
    std::printf("read               %" PRIu64 "\n",
                h->read.load(std::memory_order_relaxed));
    std::printf("producer_waiting   %" PRIu32 "   consumer_waiting %" PRIu32
                "\n",
                h->producer_waiting.load(std::memory_order_relaxed),
                h->consumer_waiting.load(std::memory_order_relaxed));
    std::printf("producer_heartbeat %" PRIu64 "   consumer_heartbeat %" PRIu64
                "\n",
                h->producer_heartbeat.load(std::memory_order_relaxed),
                h->consumer_heartbeat.load(std::memory_order_relaxed));
    // Stats block: present only in a kVersionStats segment. Guarded by the
    // mapped length as well as the version, because this tool does not
    // validate — a garbage version word must not make it read past the map.
    if (shuttle::has_stats(h) && map_len >= shuttle::kDataOffsetV2) {
        std::printf("stats (v%" PRIu32 " block)\n", shuttle::kVersionStats);
        std::printf("  msgs_written     %" PRIu64 "   bytes_written %" PRIu64
                    "\n",
                    h->stat_msgs_written.load(std::memory_order_relaxed),
                    h->stat_bytes_written.load(std::memory_order_relaxed));
        std::printf("  msgs_dropped     %" PRIu64 " (reserved)\n",
                    h->stat_msgs_dropped.load(std::memory_order_relaxed));
        std::printf("  msgs_read        %" PRIu64 "   bytes_read    %" PRIu64
                    "\n",
                    h->stat_msgs_read.load(std::memory_order_relaxed),
                    h->stat_bytes_read.load(std::memory_order_relaxed));
    }
    munmap(p, map_len);
    return 0;
}
