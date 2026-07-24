#pragma once

// Internal C++ lifecycle API (Phase 1). The extern "C" ABI that mirrors
// these signatures is frozen in Phase 6; error codes are plain ints from
// day one so that freeze is mechanical.

#include <cstddef>

#include "shuttle/header.hpp"

namespace shuttle {

enum Err : int {
    kOk = 0,
    kErrInvalidArgs = -1,
    kErrNameTooLong = -2,       // platform shm name limit (macOS ~31 chars)
    kErrExists = -3,            // create: name already exists
    kErrNotFound = -4,          // open/unlink: no such segment
    kErrSys = -5,               // unexpected syscall failure (see errno)
    kErrBadMagic = -6,          // FR-3
    kErrBadVersion = -7,        // FR-3 (distinct from magic)
    kErrCapacityTooSmall = -8,  // FR-4: capacity < max_payload + framing
    kErrInitTimeout = -9,       // opener: creator never published init
    kErrCorrupt = -10,          // NFR-S2: header fields fail validation
    kErrMsgTooLarge = -11,      // payload > max_payload: fail fast (§2.2)
    kErrWouldBlock = -12,       // non-blocking op cannot proceed (IF-3)
    kErrPeerDead = -13,         // blocked wait aborted: peer heartbeat stale
};

// Local (per-process) handle; never stored in the segment.
struct Channel {
    void* base;
    size_t map_len;
    ChannelHeader* hdr;
};

// FR-1: shm_open(O_CREAT|O_EXCL) + one-shot ftruncate + mmap + header init,
// publishing init last with a release store. Owner-only permissions (NFR-S1).
// create_flags carries opt-in create-time bits (kFlagHugePages); unknown bits
// are masked off. Defaulted for source-compatibility with pre-flags callers.
Channel* create(const char* name, size_t capacity_bytes,
                size_t max_payload_bytes, int* err, uint32_t create_flags = 0);

// FR-2/FR-3: attach without re-init; waits for init publication, then
// validates magic, version, and header sanity (NFR-S2).
Channel* open(const char* name, int* err);

// FR-5: unmap and free the local handle; the named object survives.
void close(Channel* ch);

// FR-5: remove the named object. Returns kOk / kErrNotFound / kErrSys.
int unlink(const char* name);

}  // namespace shuttle
