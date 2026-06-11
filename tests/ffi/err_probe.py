#!/usr/bin/env python3
"""G6.3 Python probe: induced errors must surface as the documented integer
codes — no exception may escape the ABI (a clean exit IS the no-exception
proof; any raise crashes this script nonzero).

usage: err_probe.py <libshuttle_c path>
"""
import sys

import cffi

CDEF = """
typedef struct shuttle_channel shuttle_channel;
shuttle_channel* shuttle_open(const char* name, int* err);
shuttle_channel* shuttle_create(const char* name, size_t capacity_bytes,
                                size_t max_payload_bytes, int* err);
void shuttle_close(shuttle_channel* ch);
int shuttle_unlink(const char* name);
"""

ERR_NOT_FOUND = -4
ERR_CAPACITY_TOO_SMALL = -8


def main():
    ffi = cffi.FFI()
    ffi.cdef(CDEF)
    lib = ffi.dlopen(sys.argv[1])
    errp = ffi.new("int*")

    ch = lib.shuttle_open(b"/shnx.does-not-exist", errp)
    if ch != ffi.NULL or errp[0] != ERR_NOT_FOUND:
        print(f"py: open-nonexistent gave err={errp[0]}", file=sys.stderr)
        return 1

    ch = lib.shuttle_create(b"/shnx.badcap", 64, 1 << 16, errp)
    if ch != ffi.NULL or errp[0] != ERR_CAPACITY_TOO_SMALL:
        print(f"py: bad-capacity create gave err={errp[0]}", file=sys.stderr)
        return 1

    if lib.shuttle_unlink(b"/shnx.does-not-exist") != ERR_NOT_FOUND:
        print("py: unlink-nonexistent wrong code", file=sys.stderr)
        return 1

    print("py: kErrNotFound/kErrCapacityTooSmall surfaced as correct ints,"
          " nothing raised")
    return 0


if __name__ == "__main__":
    sys.exit(main())
