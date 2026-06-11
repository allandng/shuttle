#!/usr/bin/env python3
"""G6.1 Python consumer: opens an existing Shuttle channel via the frozen C
ABI (cffi, ABI mode) and drains N messages over the ZERO-COPY borrow path.

Zero-copy proof: the payload is exposed as a memoryview over ffi.buffer
wrapping the borrowed pointer — no bytes() copy is ever taken; verification
indexes the view in place. Per the binding minor amendment, the view is
wrapped in a guard object that invalidates on release_read: touching it
afterward raises (Python cannot enforce the borrow; it CAN refuse to serve
a stale one).

usage: py_consumer.py <libshuttle_c path> </shm-name> <nmsgs> <seed>
"""
import sys

import cffi

M64 = (1 << 64) - 1

CDEF = """
typedef struct shuttle_channel shuttle_channel;
shuttle_channel* shuttle_open(const char* name, int* err);
void shuttle_close(shuttle_channel* ch);
int shuttle_acquire_read(shuttle_channel* ch, const void** ptr, size_t* len,
                         int flags);
int shuttle_release_read(shuttle_channel* ch);
"""


def splitmix(x):
    x = (x + 0x9E3779B97F4A7C15) & M64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


def msg_len(seed, i, max_payload):
    return splitmix(seed ^ i) % (max_payload + 1)


def fill_byte(msg, j):
    return ((msg * 1315423911) + j * 151 + (j >> 8)) & 0xFF


class BorrowedMessage:
    """Zero-copy view of a borrowed payload; unusable after release."""

    def __init__(self, ffi, ptr, length):
        self._mv = memoryview(ffi.buffer(ptr, length))  # no copy
        self._released = False

    @property
    def view(self):
        if self._released:
            raise RuntimeError("borrowed payload used after release_read")
        return self._mv

    def invalidate(self):
        self._released = True
        self._mv.release()


def main():
    lib_path, name, nmsgs, seed, max_payload = (
        sys.argv[1],
        sys.argv[2].encode(),
        int(sys.argv[3]),
        int(sys.argv[4]),
        int(sys.argv[5]),
    )
    ffi = cffi.FFI()
    ffi.cdef(CDEF)
    lib = ffi.dlopen(lib_path)

    errp = ffi.new("int*")
    ch = lib.shuttle_open(name, errp)
    if ch == ffi.NULL:
        print(f"py_consumer: open failed err={errp[0]}", file=sys.stderr)
        return 1

    ptrp = ffi.new("const void**")
    lenp = ffi.new("size_t*")
    last_msg = None
    for i in range(nmsgs):
        rc = lib.shuttle_acquire_read(ch, ptrp, lenp, 0)  # blocking borrow
        if rc != 0:
            print(f"py_consumer: acquire_read msg {i} rc={rc}",
                  file=sys.stderr)
            return 1
        length = lenp[0]
        want = msg_len(seed, i, max_payload)
        if length != want:
            print(f"py_consumer: msg {i} len {length} != {want} (FIFO?)",
                  file=sys.stderr)
            return 1
        msg = BorrowedMessage(ffi, ptrp[0], length)
        view = msg.view
        bad = 0
        for j in range(length):
            if view[j] != fill_byte(i, j):
                bad += 1
        if bad:
            print(f"py_consumer: msg {i} has {bad} corrupt bytes",
                  file=sys.stderr)
            return 1
        rc = lib.shuttle_release_read(ch)
        msg.invalidate()
        if rc != 0:
            print(f"py_consumer: release rc={rc}", file=sys.stderr)
            return 1
        last_msg = msg

    # Amendment check: the borrow must be unusable after release.
    try:
        _ = last_msg.view[0]
        print("py_consumer: stale borrow did NOT raise", file=sys.stderr)
        return 1
    except RuntimeError:
        pass

    lib.shuttle_close(ch)
    print(f"py_consumer: {nmsgs} msgs byte-exact over zero-copy borrow path;"
          " stale borrow raises")
    return 0


if __name__ == "__main__":
    sys.exit(main())
