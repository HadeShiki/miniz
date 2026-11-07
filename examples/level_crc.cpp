/* miniz example — compression level + CRC-32 integrity (C++)
 * Build: g++ -std=c++17 -O2 -I../include level_crc.cpp -o level_crc
 * Run:   ./level_crc
 */

#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>

int main() {
    const char* text = "The quick brown fox jumps over the lazy dog. ";
    size_t tlen = std::strlen(text);
    uint8_t buf[4000];
    for (size_t i = 0; i < 4000; ++i) buf[i] = static_cast<uint8_t>(text[i % tlen]);

    // CRC-32 of original
    uint32_t crc_orig = miniz::miniz_crc32(buf, 4000);
    std::printf("CRC-32 original : %08X\n", crc_orig);

    // Compress at default level
    miniz::ByteBuf compressed;
    if (!miniz::compress(buf, 4000, compressed, miniz::CompressLevel::NORMAL)) {
        std::fprintf(stderr, "compress failed\n");
        return 1;
    }
    std::printf("compressed: %zu -> %zu bytes (ratio %.2f%%)\n",
                static_cast<size_t>(4000), compressed.length(),
                100.0 * compressed.length() / 4000.0);

    // CRC-32 of compressed payload (integrity check for storage/transmit)
    uint32_t crc_comp = miniz::miniz_crc32(compressed.begin(), compressed.length());
    std::printf("CRC-32 compressed: %08X\n", crc_comp);

    // Decompress and verify
    uint8_t out[8192];
    size_t out_written = 0;
    if (!miniz::decompress(compressed.begin(), compressed.length(),
                           out, sizeof(out), out_written)) {
        std::fprintf(stderr, "decompress failed\n");
        return 1;
    }
    if (out_written != 4000 || std::memcmp(out, buf, 4000) != 0) {
        std::fprintf(stderr, "round-trip mismatch\n");
        return 1;
    }
    uint32_t crc_dec = miniz::miniz_crc32(out, out_written);
    std::printf("CRC-32 decompressed: %08X %s\n",
                crc_dec, (crc_dec == crc_orig) ? "MATCH" : "MISMATCH");

    miniz::miniz_buf_free(compressed);
    std::printf("OK\n");
    return 0;
}