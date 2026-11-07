/* basic compress/decompress round-trip (C++)
 * Build: g++ -std=c++17 -O2 -I../include simple.cpp -o simple
 * Run:   ./simple
 */

#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>

int main() {
    const char* text = "hello world hello world hello world hello world";
    size_t len = std::strlen(text);

    miniz::ByteBuf compressed;
    if (!miniz::compress(reinterpret_cast<const uint8_t*>(text), len, compressed)) {
        std::fprintf(stderr, "compress failed\n");
        return 1;
    }
    std::printf("compressed: %zu -> %zu bytes\n", len, compressed.length());

    uint8_t out[512];
    size_t out_written = 0;
    if (!miniz::decompress(compressed.begin(), compressed.length(),
                           out, sizeof(out), out_written)) {
        std::fprintf(stderr, "decompress failed\n");
        return 1;
    }
    if (out_written != len || std::memcmp(out, text, len) != 0) {
        std::fprintf(stderr, "round-trip mismatch\n");
        return 1;
    }
    out[out_written] = '\0';
    std::printf("decompressed: %s\n", out);

    miniz::miniz_buf_free(compressed);
    std::printf("OK\n");
    return 0;
}