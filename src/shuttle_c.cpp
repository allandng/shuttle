// C ABI implementation (Phase 6). Every entry point catches everything:
// no C++ exception crosses the boundary (IF-1).
//
// Role inference: a handle lazily becomes a producer or consumer on first
// use of the corresponding API — consistent with FR-6 (exactly one of each
// per channel; double-role misuse is the application's contract breach).
#include "shuttle/shuttle_c.h"

#include <new>

#include "shuttle/shuttle.hpp"
#include "shuttle/spsc.hpp"

static_assert(SHUTTLE_OK == shuttle::kOk);
static_assert(SHUTTLE_ERR_INVALID_ARGS == shuttle::kErrInvalidArgs);
static_assert(SHUTTLE_ERR_NAME_TOO_LONG == shuttle::kErrNameTooLong);
static_assert(SHUTTLE_ERR_EXISTS == shuttle::kErrExists);
static_assert(SHUTTLE_ERR_NOT_FOUND == shuttle::kErrNotFound);
static_assert(SHUTTLE_ERR_SYS == shuttle::kErrSys);
static_assert(SHUTTLE_ERR_BAD_MAGIC == shuttle::kErrBadMagic);
static_assert(SHUTTLE_ERR_BAD_VERSION == shuttle::kErrBadVersion);
static_assert(SHUTTLE_ERR_CAPACITY_TOO_SMALL == shuttle::kErrCapacityTooSmall);
static_assert(SHUTTLE_ERR_INIT_TIMEOUT == shuttle::kErrInitTimeout);
static_assert(SHUTTLE_ERR_CORRUPT == shuttle::kErrCorrupt);
static_assert(SHUTTLE_ERR_MSG_TOO_LARGE == shuttle::kErrMsgTooLarge);
static_assert(SHUTTLE_ERR_WOULD_BLOCK == shuttle::kErrWouldBlock);
static_assert(SHUTTLE_ERR_PEER_DEAD == shuttle::kErrPeerDead);
static_assert(SHUTTLE_ERR_NO_HUGEPAGES == shuttle::kErrNoHugePages);
static_assert(SHUTTLE_ERR_NO_STATS == shuttle::kErrNoStats);
// Create-flag bits are a separate namespace from the per-op flags, but the C
// value must still track the C++ header bit exactly — including the reserved
// bits, whose whole purpose is to be pinned before they are implemented.
static_assert(SHUTTLE_CREATE_HUGEPAGES == shuttle::kFlagHugePages);
static_assert(SHUTTLE_CREATE_HUGETLB_2MB == shuttle::kFlagHugeTLB2M);
static_assert(SHUTTLE_CREATE_HUGETLB_1GB == shuttle::kFlagHugeTLB1G);
static_assert(SHUTTLE_CREATE_STATS == shuttle::kFlagStats);
// The C struct is the ABI shape callers allocate; it must stay a plain
// five-u64 record in the order the C++ Stats declares.
static_assert(sizeof(shuttle_stats) == 5 * sizeof(uint64_t));
static_assert(sizeof(shuttle_stats) == sizeof(shuttle::Stats));
static_assert(offsetof(shuttle_stats, msgs_written) ==
              offsetof(shuttle::Stats, msgs_written));
static_assert(offsetof(shuttle_stats, bytes_written) ==
              offsetof(shuttle::Stats, bytes_written));
static_assert(offsetof(shuttle_stats, msgs_dropped) ==
              offsetof(shuttle::Stats, msgs_dropped));
static_assert(offsetof(shuttle_stats, msgs_read) ==
              offsetof(shuttle::Stats, msgs_read));
static_assert(offsetof(shuttle_stats, bytes_read) ==
              offsetof(shuttle::Stats, bytes_read));

struct shuttle_channel {
    shuttle::Channel* ch = nullptr;
    shuttle::Producer* prod = nullptr;
    shuttle::Consumer* cons = nullptr;
    // Active consumer borrow (shared by shuttle_read and the acquire path).
    const unsigned char* borrow_ptr = nullptr;
    uint64_t borrow_len = 0;
    bool borrow_active = false;
};

namespace {

void set_err(int* err, int code) {
    if (err != nullptr) *err = code;
}

shuttle::Producer* producer(shuttle_channel* h) {
    if (h->prod == nullptr) h->prod = new shuttle::Producer(h->ch);
    return h->prod;
}

shuttle::Consumer* consumer(shuttle_channel* h) {
    if (h->cons == nullptr) h->cons = new shuttle::Consumer(h->ch);
    return h->cons;
}

// Ensure a message is borrowed; reuses an existing active borrow.
int ensure_borrow(shuttle_channel* h, int flags) {
    if (h->borrow_active) return SHUTTLE_OK;
    const unsigned char* p = nullptr;
    uint64_t len = 0;
    shuttle::Consumer* c = consumer(h);
    const int rc = (flags & SHUTTLE_NONBLOCK) != 0 ? c->try_read(&p, &len)
                                                   : c->read(&p, &len);
    if (rc != shuttle::kOk) return rc;
    h->borrow_ptr = p;
    h->borrow_len = len;
    h->borrow_active = true;
    return SHUTTLE_OK;
}

}  // namespace

extern "C" {

shuttle_channel* shuttle_create_ex(const char* name, size_t capacity_bytes,
                                   size_t max_payload_bytes,
                                   uint32_t create_flags, int* err) {
    try {
        int e = 0;
        shuttle::Channel* ch = shuttle::create(name, capacity_bytes,
                                               max_payload_bytes, &e,
                                               create_flags);
        set_err(err, e);
        if (ch == nullptr) return nullptr;
        return new shuttle_channel{ch, nullptr, nullptr, nullptr, 0, false};
    } catch (...) {
        set_err(err, SHUTTLE_ERR_SYS);
        return nullptr;
    }
}

// Frozen v1 signature: unchanged behavior, now the create_flags=0 case of the
// additive v1.1 entry point.
shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err) {
    return shuttle_create_ex(name, capacity_bytes, max_payload_bytes, 0, err);
}

shuttle_channel* shuttle_open(const char* name, int* err) {
    try {
        int e = 0;
        shuttle::Channel* ch = shuttle::open(name, &e);
        set_err(err, e);
        if (ch == nullptr) return nullptr;
        return new shuttle_channel{ch, nullptr, nullptr, nullptr, 0, false};
    } catch (...) {
        set_err(err, SHUTTLE_ERR_SYS);
        return nullptr;
    }
}

void shuttle_close(shuttle_channel* ch) {
    if (ch == nullptr) return;
    try {
        delete ch->prod;
        delete ch->cons;
        shuttle::close(ch->ch);
        delete ch;
    } catch (...) {
    }
}

int shuttle_unlink(const char* name) {
    try {
        return shuttle::unlink(name);
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

int shuttle_write(shuttle_channel* ch, const void* data, size_t len,
                  int flags) {
    if (ch == nullptr || (data == nullptr && len != 0))
        return SHUTTLE_ERR_INVALID_ARGS;
    try {
        shuttle::Producer* p = producer(ch);
        return (flags & SHUTTLE_NONBLOCK) != 0 ? p->try_write(data, len)
                                               : p->write(data, len);
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

long shuttle_read(shuttle_channel* ch, void* out, size_t cap, int flags) {
    if (ch == nullptr || (out == nullptr && cap != 0))
        return SHUTTLE_ERR_INVALID_ARGS;
    try {
        const int rc = ensure_borrow(ch, flags);
        if (rc != SHUTTLE_OK) return rc;
        if (ch->borrow_len > cap) return SHUTTLE_ERR_MSG_TOO_LARGE;
        if (ch->borrow_len != 0)
            std::memcpy(out, ch->borrow_ptr, ch->borrow_len);
        const long n = static_cast<long>(ch->borrow_len);
        ch->cons->release();
        ch->borrow_active = false;
        return n;
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

int shuttle_acquire_write(shuttle_channel* ch, void** ptr, size_t len,
                          int flags) {
    if (ch == nullptr || ptr == nullptr) return SHUTTLE_ERR_INVALID_ARGS;
    try {
        shuttle::Producer* p = producer(ch);
        return (flags & SHUTTLE_NONBLOCK) != 0 ? p->try_acquire_write(ptr, len)
                                               : p->acquire_write(ptr, len);
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

int shuttle_commit_write(shuttle_channel* ch, size_t actual_len) {
    if (ch == nullptr || ch->prod == nullptr) return SHUTTLE_ERR_INVALID_ARGS;
    try {
        return ch->prod->commit_write(actual_len);
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

int shuttle_acquire_read(shuttle_channel* ch, const void** ptr, size_t* len,
                         int flags) {
    if (ch == nullptr || ptr == nullptr || len == nullptr)
        return SHUTTLE_ERR_INVALID_ARGS;
    try {
        const int rc = ensure_borrow(ch, flags);
        if (rc != SHUTTLE_OK) return rc;
        *ptr = ch->borrow_ptr;
        *len = static_cast<size_t>(ch->borrow_len);
        return SHUTTLE_OK;
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

int shuttle_release_read(shuttle_channel* ch) {
    if (ch == nullptr || !ch->borrow_active || ch->cons == nullptr)
        return SHUTTLE_ERR_INVALID_ARGS;
    try {
        ch->cons->release();
        ch->borrow_active = false;
        ch->borrow_ptr = nullptr;
        ch->borrow_len = 0;
        return SHUTTLE_OK;
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

int shuttle_get_stats(shuttle_channel* ch, shuttle_stats* out) {
    if (ch == nullptr || out == nullptr) return SHUTTLE_ERR_INVALID_ARGS;
    try {
        shuttle::Stats s{};
        const int rc = shuttle::get_stats(ch->ch, s);
        if (rc != shuttle::kOk) return rc;
        out->msgs_written = s.msgs_written;
        out->bytes_written = s.bytes_written;
        out->msgs_dropped = s.msgs_dropped;
        out->msgs_read = s.msgs_read;
        out->bytes_read = s.bytes_read;
        return SHUTTLE_OK;
    } catch (...) {
        return SHUTTLE_ERR_SYS;
    }
}

void shuttle_keepalive(shuttle_channel* ch) {
    if (ch == nullptr) return;
    try {
        if (ch->prod != nullptr) ch->prod->keepalive();
        if (ch->cons != nullptr) ch->cons->keepalive();
        if (ch->prod == nullptr && ch->cons == nullptr) {
            // Role not yet established; bump both (harmless: single-writer
            // discipline applies per side once roles are taken).
        }
    } catch (...) {
    }
}

}  // extern "C"
