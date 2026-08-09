// WP9: host-only unit test for the CUDA IPC DESCRIPTOR CODEC.
//
// This is the ONLY part of the experimental CUDA module that is proven
// anywhere. It has NO CUDA dependency and runs on every platform: the handles
// are opaque byte blobs the codec copies verbatim and never interprets. It is
// linked directly against src/shuttle_cuda.cpp's host half (the device glue is
// #ifdef'd out because SHUTTLE_WITH_CUDA is not defined for this target).
//
// What each block pins down:
//   1. WIRE SIZE is exactly what the header promises (160), and sizeof the
//      struct matches — a padding regression would trip the static_assert in
//      the .cpp, and this asserts the public constant here too.
//   2. ROUNDTRIP: init -> fill -> pack -> unpack reproduces every field
//      byte-exact, for a plain descriptor and one carrying an event handle,
//      including a maximal device id / offset / len and a full-of-0xFF handle.
//   3. ENDIANNESS is fixed little-endian: a hand-built descriptor packs to
//      exact, known bytes at known offsets (this is the wire contract, and the
//      test that would catch a big-endian host silently diverging).
//   4. REJECTION: pack refuses a short buffer and a malformed descriptor;
//      unpack refuses a short buffer, bad magic, bad version, zero len,
//      negative device, unknown flag bits, and event/flag inconsistency. Each
//      corruption is a single-field mutation of an otherwise-valid wire image,
//      so the verdict can only come from that field.
//   5. FUZZ-LITE: feed pseudo-random bytes to unpack in a bounded loop and
//      assert it NEVER accepts an invalid descriptor and NEVER reads out of
//      bounds. Under ASan (the CI build) an OOB read here is a hard failure;
//      the buffers are sized exactly to the wire size so a one-byte over-read
//      would be caught. Falsifiability: any accepted blob is re-packed and must
//      reproduce identical bytes, so "accept" can never be a false pass.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shuttle/shuttle_cuda.h"

namespace {

int fails = 0;

#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            std::fprintf(stderr, "FAIL: " __VA_ARGS__); \
            std::fprintf(stderr, " [%s]\n", #cond);     \
            ++fails;                                    \
        }                                               \
    } while (0)

// A valid, representative descriptor with distinctive, non-trivial field
// values so a misplaced byte is visible.
shuttle_cuda_desc make_valid(bool with_event) {
    shuttle_cuda_desc d;
    shuttle_cuda_desc_init(&d);
    d.device = 3;
    d.offset = 0x1122334455667788ull;
    d.len = 0x00000000DEADBEEFull;
    for (uint32_t i = 0; i < SHUTTLE_CUDA_HANDLE_BYTES; ++i)
        d.mem_handle[i] = static_cast<uint8_t>(0xA0 + i);
    if (with_event) {
        d.flags |= SHUTTLE_CUDA_FLAG_HAS_EVENT;
        for (uint32_t i = 0; i < SHUTTLE_CUDA_HANDLE_BYTES; ++i)
            d.event_handle[i] = static_cast<uint8_t>(0x50 + i);
    }
    return d;
}

bool desc_eq(const shuttle_cuda_desc& a, const shuttle_cuda_desc& b) {
    return a.magic == b.magic && a.version == b.version &&
           a.device == b.device && a.flags == b.flags && a.offset == b.offset &&
           a.len == b.len &&
           std::memcmp(a.mem_handle, b.mem_handle, SHUTTLE_CUDA_HANDLE_BYTES) ==
               0 &&
           std::memcmp(a.event_handle, b.event_handle,
                       SHUTTLE_CUDA_HANDLE_BYTES) == 0;
}

void test_sizes() {
    CHECK(SHUTTLE_CUDA_DESC_WIRE_SIZE == 160u, "wire size constant is 160");
    CHECK(sizeof(shuttle_cuda_desc) == SHUTTLE_CUDA_DESC_WIRE_SIZE,
          "struct size %zu != wire size %u", sizeof(shuttle_cuda_desc),
          SHUTTLE_CUDA_DESC_WIRE_SIZE);
}

void test_roundtrip(bool with_event) {
    shuttle_cuda_desc d = make_valid(with_event);
    CHECK(shuttle_cuda_desc_validate(&d) == SHUTTLE_CUDA_OK,
          "make_valid is valid (event=%d)", with_event);

    uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
    const int n = shuttle_cuda_desc_pack(&d, buf, sizeof(buf));
    CHECK(n == static_cast<int>(SHUTTLE_CUDA_DESC_WIRE_SIZE),
          "pack returns wire size, got %d", n);

    shuttle_cuda_desc got;
    std::memset(&got, 0x5A, sizeof(got));
    const int rc = shuttle_cuda_desc_unpack(buf, sizeof(buf), &got);
    CHECK(rc == SHUTTLE_CUDA_OK, "unpack ok, got %d", rc);
    CHECK(desc_eq(d, got), "roundtrip preserves every field (event=%d)",
          with_event);
}

// Extreme but valid field values must survive the roundtrip untouched.
void test_roundtrip_extremes() {
    shuttle_cuda_desc d;
    shuttle_cuda_desc_init(&d);
    d.device = 0;                      // minimal valid device
    d.offset = 0xFFFFFFFFFFFFFFFFull;  // max offset
    d.len = 0xFFFFFFFFFFFFFFFFull;     // max len
    std::memset(d.mem_handle, 0xFF,
                SHUTTLE_CUDA_HANDLE_BYTES);  // all-ones handle
    uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
    CHECK(shuttle_cuda_desc_pack(&d, buf, sizeof(buf)) ==
              static_cast<int>(SHUTTLE_CUDA_DESC_WIRE_SIZE),
          "extremes pack");
    shuttle_cuda_desc got;
    CHECK(shuttle_cuda_desc_unpack(buf, sizeof(buf), &got) == SHUTTLE_CUDA_OK,
          "extremes unpack");
    CHECK(desc_eq(d, got), "extremes roundtrip byte-exact");
}

// The wire contract: known descriptor -> known bytes. Fixed little-endian.
void test_endianness_bytes() {
    shuttle_cuda_desc d;
    shuttle_cuda_desc_init(&d);
    d.device = 1;
    d.offset = 0x0102030405060708ull;
    d.len = 0x1122334455667788ull;
    // mem_handle: byte i == i; event_handle stays zero (no event).
    for (uint32_t i = 0; i < SHUTTLE_CUDA_HANDLE_BYTES; ++i)
        d.mem_handle[i] = static_cast<uint8_t>(i);

    uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
    CHECK(shuttle_cuda_desc_pack(&d, buf, sizeof(buf)) > 0, "pack for bytes");

    // magic 0x53435544 little-endian -> 44 55 43 53
    CHECK(buf[0] == 0x44 && buf[1] == 0x55 && buf[2] == 0x43 && buf[3] == 0x53,
          "magic bytes LE");
    // version 1 -> 01 00 00 00
    CHECK(buf[4] == 0x01 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0,
          "version bytes LE");
    // device 1 -> 01 00 00 00
    CHECK(buf[8] == 0x01 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0,
          "device bytes LE");
    // flags 0 -> 00 00 00 00
    CHECK(buf[12] == 0 && buf[13] == 0 && buf[14] == 0 && buf[15] == 0,
          "flags bytes");
    // mem_handle at offset 16, byte i == i
    for (uint32_t i = 0; i < SHUTTLE_CUDA_HANDLE_BYTES; ++i)
        CHECK(buf[16 + i] == static_cast<uint8_t>(i), "mem_handle byte %u", i);
    // offset 0x0102030405060708 LE at byte 80 -> 08 07 06 05 04 03 02 01
    CHECK(buf[80] == 0x08 && buf[81] == 0x07 && buf[82] == 0x06 &&
              buf[83] == 0x05 && buf[84] == 0x04 && buf[85] == 0x03 &&
              buf[86] == 0x02 && buf[87] == 0x01,
          "offset bytes LE");
    // len 0x1122334455667788 LE at byte 88 -> 88 77 66 55 44 33 22 11
    CHECK(buf[88] == 0x88 && buf[95] == 0x11, "len bytes LE endpoints");
    // event_handle at offset 96 all zero
    bool ev_zero = true;
    for (uint32_t i = 0; i < SHUTTLE_CUDA_HANDLE_BYTES; ++i)
        if (buf[96 + i] != 0) ev_zero = false;
    CHECK(ev_zero, "event handle bytes zero when unused");
}

void test_pack_rejects() {
    shuttle_cuda_desc d = make_valid(false);
    uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
    // short buffer
    CHECK(shuttle_cuda_desc_pack(&d, buf, SHUTTLE_CUDA_DESC_WIRE_SIZE - 1) ==
              SHUTTLE_CUDA_ERR_BUF_TOO_SMALL,
          "pack short buffer -> BUF_TOO_SMALL");
    // null args
    CHECK(shuttle_cuda_desc_pack(nullptr, buf, sizeof(buf)) ==
              SHUTTLE_CUDA_ERR_INVALID_ARGS,
          "pack null desc");
    CHECK(shuttle_cuda_desc_pack(&d, nullptr, sizeof(buf)) ==
              SHUTTLE_CUDA_ERR_INVALID_ARGS,
          "pack null buf");
    // malformed descriptor is never emitted
    shuttle_cuda_desc bad = make_valid(false);
    bad.len = 0;
    CHECK(shuttle_cuda_desc_pack(&bad, buf, sizeof(buf)) ==
              SHUTTLE_CUDA_ERR_BAD_LEN,
          "pack zero-len desc -> BAD_LEN");
}

// Build a valid wire image, then mutate exactly one field and assert unpack
// rejects it with the matching code.
void test_unpack_rejects() {
    shuttle_cuda_desc d = make_valid(false);
    uint8_t good[SHUTTLE_CUDA_DESC_WIRE_SIZE];
    CHECK(shuttle_cuda_desc_pack(&d, good, sizeof(good)) > 0, "base pack");
    shuttle_cuda_desc out;

    // sanity: the untouched image unpacks
    CHECK(shuttle_cuda_desc_unpack(good, sizeof(good), &out) == SHUTTLE_CUDA_OK,
          "good image unpacks");

    // short buffer
    CHECK(shuttle_cuda_desc_unpack(good, SHUTTLE_CUDA_DESC_WIRE_SIZE - 1,
                                   &out) == SHUTTLE_CUDA_ERR_BUF_TOO_SMALL,
          "unpack short -> BUF_TOO_SMALL");
    // null args
    CHECK(shuttle_cuda_desc_unpack(nullptr, sizeof(good), &out) ==
              SHUTTLE_CUDA_ERR_INVALID_ARGS,
          "unpack null buf");
    CHECK(shuttle_cuda_desc_unpack(good, sizeof(good), nullptr) ==
              SHUTTLE_CUDA_ERR_INVALID_ARGS,
          "unpack null out");

    // bad magic: flip byte 0
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        b[0] ^= 0xFF;
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_BAD_MAGIC,
              "bad magic -> BAD_MAGIC");
    }
    // bad version: byte 4
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        b[4] = 0x99;
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_BAD_VERSION,
              "bad version -> BAD_VERSION");
    }
    // negative device: set byte 11 high bit (device sign)
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        b[11] = 0x80;  // device becomes negative
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_BAD_DEVICE,
              "negative device -> BAD_DEVICE");
    }
    // unknown flag bit: set bit in flags word (bytes 12..15)
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        b[12] = 0x02;  // an undefined flag bit
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_BAD_FLAGS,
              "unknown flag -> BAD_FLAGS");
    }
    // zero len: bytes 88..95
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        std::memset(b + 88, 0, 8);
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_BAD_LEN,
              "zero len -> BAD_LEN");
    }
    // event/flag inconsistency: HAS_EVENT set but event handle all zero
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        b[12] = SHUTTLE_CUDA_FLAG_HAS_EVENT;  // set bit, leave event zero
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_EVENT_MISMATCH,
              "event flag set but handle zero -> EVENT_MISMATCH");
    }
    // the inverse: event handle nonzero but HAS_EVENT clear
    {
        uint8_t b[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        std::memcpy(b, good, sizeof(b));
        b[96] = 0x01;  // nonzero event byte, flags stay 0
        CHECK(shuttle_cuda_desc_unpack(b, sizeof(b), &out) ==
                  SHUTTLE_CUDA_ERR_EVENT_MISMATCH,
              "event bytes set but flag clear -> EVENT_MISMATCH");
    }
}

// Deterministic PRNG so the run is reproducible.
uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint8_t rand_byte() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint8_t>(g_rng >> 24);
}

// Fuzz-lite: random bytes into unpack, bounded. It must never accept an invalid
// descriptor, and (under ASan) never read past the exact-size buffer. Any
// acceptance is verified by re-packing and requiring identical bytes.
void test_fuzz_lite() {
    constexpr int kIters = 200000;
    long accepted = 0;
    for (int it = 0; it < kIters; ++it) {
        uint8_t buf[SHUTTLE_CUDA_DESC_WIRE_SIZE];
        for (uint32_t i = 0; i < SHUTTLE_CUDA_DESC_WIRE_SIZE; ++i)
            buf[i] = rand_byte();
        shuttle_cuda_desc out;
        const int rc = shuttle_cuda_desc_unpack(buf, sizeof(buf), &out);
        if (rc == SHUTTLE_CUDA_OK) {
            ++accepted;
            // An accepted blob must be internally consistent: re-validate and
            // re-pack, and the re-packed bytes must equal the accepted input
            // (the codec is a bijection on valid images).
            CHECK(shuttle_cuda_desc_validate(&out) == SHUTTLE_CUDA_OK,
                  "fuzz: accepted descriptor must validate");
            uint8_t re[SHUTTLE_CUDA_DESC_WIRE_SIZE];
            CHECK(shuttle_cuda_desc_pack(&out, re, sizeof(re)) > 0,
                  "fuzz: accepted descriptor must re-pack");
            CHECK(std::memcmp(re, buf, sizeof(re)) == 0,
                  "fuzz: re-pack reproduces the accepted bytes");
        } else {
            CHECK(rc < 0, "fuzz: rejection must be a negative code (rc=%d)",
                  rc);
        }
    }
    // Also feed truncated buffers of every length below the wire size: each
    // must be BUF_TOO_SMALL and must not read past the shorter buffer (ASan).
    for (uint32_t len = 0; len < SHUTTLE_CUDA_DESC_WIRE_SIZE; ++len) {
        // exact-size allocation so an over-read past `len` that stays within a
        // 160-byte stack buffer would NOT be caught — so size to `len` exactly.
        uint8_t* b = len ? new uint8_t[len] : new uint8_t[1];
        for (uint32_t i = 0; i < len; ++i) b[i] = rand_byte();
        shuttle_cuda_desc out;
        CHECK(shuttle_cuda_desc_unpack(b, len, &out) ==
                  SHUTTLE_CUDA_ERR_BUF_TOO_SMALL,
              "fuzz: truncated len %u -> BUF_TOO_SMALL", len);
        delete[] b;
    }
    std::printf("  fuzz-lite: %d random images, %ld accepted (all re-packed "
                "identically), all sub-size buffers rejected\n",
                kIters, accepted);
}

}  // namespace

int main() {
    test_sizes();
    test_roundtrip(false);
    test_roundtrip(true);
    test_roundtrip_extremes();
    test_endianness_bytes();
    test_pack_rejects();
    test_unpack_rejects();
    test_fuzz_lite();

    if (fails == 0) {
        std::printf(
            "cuda_desc_test ok: descriptor codec roundtrips byte-exact, fixed "
            "little-endian wire (%u bytes), rejects every malformed image, and "
            "never over-reads (HOST-ONLY; the CUDA glue is unproven)\n",
            SHUTTLE_CUDA_DESC_WIRE_SIZE);
    }
    return fails == 0 ? 0 : 1;
}
