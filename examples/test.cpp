/* miniz test suite — round-trip + edge cases
 * Build:  make test   (or: g++ -std=c++17 -O2 -Iinclude examples/test.cpp -o build/miniz_test)
 * Run:    ./build/miniz_test
 */

#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

static int failures = 0;

static void check(const char* name, const uint8_t* data, size_t len) {
    miniz::ByteBuf cb;
    if (!miniz::compress(data, len, cb)) {
        printf("FAIL %s: compress returned false\n", name);
        ++failures;
        return;
    }

    uint8_t out[262144];
    size_t w = 0;
    if (!miniz::decompress(cb.begin(), cb.length(), out, sizeof(out), w)) {
        printf("FAIL %s: decompress returned false\n", name);
        ++failures;
        miniz::miniz_buf_free(cb);
        return;
    }
    if (w != len || std::memcmp(out, data, len) != 0) {
        printf("FAIL %s: round-trip mismatch (got %zu, want %zu)\n", name, w, len);
        ++failures;
    } else {
        printf("OK   %s: %zu -> %zu bytes\n", name, len, cb.length());
    }
    miniz::miniz_buf_free(cb);
}

static void check_crc(const char* name, const uint8_t* data, size_t len) {
    uint32_t c1 = miniz::miniz_crc32(data, len);
    uint32_t c2 = miniz::miniz_crc32_update(0u, data, len);
    if (c1 != c2) {
        printf("FAIL %s: crc32 vs crc32_update mismatch (%08X vs %08X)\n", name, c1, c2);
        ++failures;
    } else {
        printf("OK   %s: crc32=%08X\n", name, c1);
    }
}

static void check_stream(const char* name, const uint8_t* data, size_t len) {
    miniz::StreamEnc enc;
    size_t off = 0;
    while (off < len) {
        size_t take = len - off < 256 ? len - off : 256;
        if (!enc.push_in(data + off, take)) {
            printf("FAIL %s: stream push_in failed\n", name);
            ++failures;
            return;
        }
        off += take;
    }
    if (!enc.finish(true)) {
        printf("FAIL %s: stream finish failed\n", name);
        ++failures;
        return;
    }

    miniz::StreamDec dec;
    size_t coff = 0;
    size_t clen = enc.out.length();
    while (coff < clen) {
        size_t take = clen - coff < 128 ? clen - coff : 128;
        if (!dec.push_in(enc.out.begin() + coff, take)) {
            printf("FAIL %s: stream decode push_in failed\n", name);
            ++failures;
            miniz::miniz_buf_free(enc.out);
            return;
        }
        coff += take;
    }
    if (!dec.finish()) {
        printf("FAIL %s: stream finish failed\n", name);
        ++failures;
        miniz::miniz_buf_free(enc.out);
        return;
    }

    uint8_t out[262144];
    size_t total = 0;
    size_t got;
    while ((got = dec.pull_out(out + total, sizeof(out) - total)) > 0) {
        total += got;
    }

    if (total != len || std::memcmp(out, data, len) != 0) {
        printf("FAIL %s: stream round-trip mismatch (got %zu, want %zu)\n", name, total, len);
        ++failures;
    } else {
        printf("OK   %s: stream %zu -> %zu bytes\n", name, len, enc.out.length());
    }
    miniz::miniz_buf_free(enc.out);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    check("empty", nullptr, 0);

    uint8_t single = 0x42;
    check("single", &single, 1);

    uint8_t zeros[1000] = {0};
    check("zeros", zeros, 1000);

    uint8_t allff[1000];
    for (size_t i = 0; i < 1000; ++i) allff[i] = 0xFF;
    check("all_0xFF", allff, 1000);

    uint8_t rep[2000];
    for (size_t i = 0; i < 2000; ++i) rep[i] = static_cast<uint8_t>('A' + (i % 26));
    check("repetitive", rep, 2000);

    uint8_t seq[1000];
    for (size_t i = 0; i < 1000; ++i) seq[i] = static_cast<uint8_t>(i * 7 + 3);
    check("sequential", seq, 1000);

    uint8_t bin[2000];
    for (size_t i = 0; i < 2000; ++i) bin[i] = static_cast<uint8_t>(i % 251);
    check("binary", bin, 2000);

    const char* t = "The quick brown fox jumps over the lazy dog. ";
    size_t tl = std::strlen(t);
    uint8_t txt[5000];
    for (size_t i = 0; i < 5000; ++i) txt[i] = static_cast<uint8_t>(t[i % tl]);
    check("long_text", txt, 5000);

    uint8_t large[50000];
    for (size_t i = 0; i < 50000; ++i) large[i] = static_cast<uint8_t>(i % 256);
    check("large", large, 50000);

    uint8_t longmatch[300];
    for (size_t i = 0; i < 300; ++i) longmatch[i] = 0xAA;
    check("long_match", longmatch, 300);

    check_crc("zeros_crc", zeros, 1000);
    check_crc("rep_crc", rep, 2000);

    check_stream("stream_rep", rep, 2000);
    check_stream("stream_txt", txt, 5000);
    check_stream("stream_large", large, 50000);

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST(S) FAILED\n", failures);
    return 1;
}