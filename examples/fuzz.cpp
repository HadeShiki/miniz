/* miniz fuzz test — standalone fuzz harness (no libFuzzer required)
 * Build: g++ -std=c++17 -O2 -Iinclude fuzz.cpp -o fuzz
 * Run:   ./fuzz [iterations]
 *
 * Tests round-trip compress/decompress on random data with many patterns.
 * If decompress produces wrong output or crashes, the test fails.
 *
 * NOTE: miniz follows "fail fast, loud, early" — corrupted input causes
 * std::abort(). This is by design. This fuzz harness only feeds VALID
 * compressed data (output of compress()) to decompress(), so no abort
 * is expected. Corruption handling is covered by the abort() calls in
 * decompress() itself.
 *
 * To test with a real fuzzer (libFuzzer/AFL), write a harness that calls
 * miniz::compress() then miniz::decompress() on the output, and let the
 * fuzzer mutate the input to compress().
 */

#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>

// Simple xorshift32 PRNG (no <random> dependency)
static uint32_t rng_state = 0x12345678u;
static inline uint32_t rng_next() noexcept {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static inline uint8_t rng_byte() noexcept { return static_cast<uint8_t>(rng_next() & 0xFF); }

static int failures = 0;
static size_t tests_run = 0;

static void check_roundtrip(const char* name, const uint8_t* data, size_t len) noexcept {
    ++tests_run;

    miniz::ByteBuf cb;
    if (!miniz::compress(data, len, cb)) {
        printf("FAIL [%s] compress returned false (len=%zu)\n", name, len);
        ++failures;
        return;
    }

    // Output buffer sized for original size (decompress needs that much)
    uint8_t* out = static_cast<uint8_t*>(std::malloc(len ? len : 1));
    if (!out) { printf("FAIL [%s] malloc failed\n", name); ++failures; miniz::miniz_buf_free(cb); return; }

    size_t w = 0;
    bool ok = miniz::decompress(cb.begin(), cb.length(), out, len, w);

    if (!ok) {
        printf("FAIL [%s] decompress returned false (len=%zu)\n", name, len);
        ++failures;
    } else if (w != len) {
        printf("FAIL [%s] size mismatch: got %zu want %zu (len=%zu)\n", name, w, len, len);
        ++failures;
    } else if (std::memcmp(out, data, len) != 0) {
        printf("FAIL [%s] data mismatch (len=%zu)\n", name, len);
        ++failures;
    }

    std::free(out);
    miniz::miniz_buf_free(cb);
}

static void gen_pattern(uint8_t* buf, size_t len, int pattern) noexcept {
    switch (pattern) {
    case 0: // all zeros
        std::memset(buf, 0, len);
        break;
    case 1: // all 0xFF
        std::memset(buf, 0xFF, len);
        break;
    case 2: // sequential
        for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(i & 0xFF);
        break;
    case 3: // random
        for (size_t i = 0; i < len; ++i) buf[i] = rng_byte();
        break;
    case 4: // text-like (printable ASCII)
        for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(32 + (rng_next() % 95));
        break;
    case 5: // long run of single byte
        std::memset(buf, static_cast<int>(rng_byte()), len);
        break;
    case 6: // alternating 0x00 0xFF
        for (size_t i = 0; i < len; ++i) buf[i] = (i & 1) ? 0xFF : 0x00;
        break;
    default: // mixed blocks
        for (size_t i = 0; i < len; i += 64) {
            size_t block = len - i < 64 ? len - i : 64;
            std::memset(buf + i, static_cast<int>(rng_byte()), block);
        }
        break;
    }
}

int main(int argc, char** argv) {
    size_t iterations = (argc > 1) ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 10000;
    if (iterations == 0) iterations = 10000;

    printf("miniz fuzz test — %llu iterations\n",
           (unsigned long long)iterations);
    printf("  (valid round-trips only; corrupted input aborts by design)\n");

    // Edge cases first
    check_roundtrip("empty", nullptr, 0);
    {
        uint8_t b = 0x42;
        check_roundtrip("single", &b, 1);
    }
    {
        uint8_t buf[1000] = {0};
        check_roundtrip("zeros_1000", buf, 1000);
    }
    {
        uint8_t buf[1000];
        std::memset(buf, 0xFF, 1000);
        check_roundtrip("ff_1000", buf, 1000);
    }

    // Random fuzz loop
    for (size_t it = 0; it < iterations; ++it) {
        size_t len = 1 + (rng_next() % 50000); // 1..50000
        int pattern = rng_next() % 8;

        std::vector<uint8_t> buf(len);
        gen_pattern(buf.data(), len, pattern);

        check_roundtrip("random", buf.data(), len);

        if (it % 1000 == 0) {
            printf("  progress: %llu/%llu iterations, %zu tests, %d failures\n",
                   (unsigned long long)it, (unsigned long long)iterations, tests_run, failures);
        }
    }

    printf("\nFuzz complete: %llu iterations, %zu tests, %d failures\n",
           (unsigned long long)iterations, tests_run, failures);
    return failures == 0 ? 0 : 1;
}