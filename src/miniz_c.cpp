/* miniz_c.cpp — C ABI implementation for miniz
 * Author: HadeShiki
 * License: MIT
 *
 * Build (shared library):
 *   g++     -std=c++17 -O2 -fPIC -shared -I../include src/miniz_c.cpp -o libminiz.so
 *   clang++ -std=c++17 -O2 -fPIC -shared -I../include src/miniz_c.cpp -o libminiz.so
 *   cl      /std:c++17 /O2 /LD /I..\include src\miniz_c.cpp /link /out:miniz.dll
 *
 * Build (static):
 *   g++     -std=c++17 -O2 -c -I../include src/miniz_c.cpp -o miniz_c.o
 *   ar rcs libminiz.a miniz_c.o
 */

#include "miniz/miniz.hpp"
#include "miniz/miniz_c.h"

#include <cstdlib>
#include <cstring>

int miniz_compress(const uint8_t* in, size_t in_len,
                   uint8_t** out, size_t* out_len,
                   int level) {
    if (!in || !out || !out_len) return -1;
    if (in_len == 0) { *out = nullptr; *out_len = 0; return 0; }

    miniz::ByteBuf buf;
    miniz::CompressLevel lvl =
        (level == 0) ? miniz::CompressLevel::FAST :
        (level == 2) ? miniz::CompressLevel::BEST :
                       miniz::CompressLevel::NORMAL;

    if (!miniz::compress(in, in_len, buf, lvl)) return -1;

    *out = static_cast<uint8_t*>(std::malloc(buf.length()));
    if (!*out) { miniz::miniz_buf_free(buf); return -1; }
    std::memcpy(*out, buf.begin(), buf.length());
    *out_len = buf.length();
    miniz::miniz_buf_free(buf);
    return 0;
}

int miniz_decompress(const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_cap,
                     size_t* out_written) {
    if (!in || !out || !out_written) return -1;
    size_t w = 0;
    if (!miniz::decompress(in, in_len, out, out_cap, w)) return -1;
    *out_written = w;
    return 0;
}

size_t miniz_uncompress_size(const uint8_t* in, size_t in_len) {
    if (!in || in_len < 9) return 0;
    if (std::memcmp(in, "MZ2\0", 4) != 0) return 0;
    uint8_t method = in[4];
    if (method > 2) return 0;
    return static_cast<uint32_t>(in[5]) |
           (static_cast<uint32_t>(in[6]) << 8) |
           (static_cast<uint32_t>(in[7]) << 16) |
           (static_cast<uint32_t>(in[8]) << 24);
}

int miniz_compress_dict(const uint8_t* dict, size_t dict_len,
                        const uint8_t* in, size_t in_len,
                        uint8_t** out, size_t* out_len,
                        int level) {
    if (!out || !out_len) return -1;
    if (in_len == 0) { *out = nullptr; *out_len = 0; return 0; }

    miniz::ByteBuf buf;
    miniz::CompressLevel lvl =
        (level == 0) ? miniz::CompressLevel::FAST :
        (level == 2) ? miniz::CompressLevel::BEST :
                       miniz::CompressLevel::NORMAL;

    if (!miniz::compress_dict(dict, dict_len, in, in_len, buf, lvl)) return -1;

    *out = static_cast<uint8_t*>(std::malloc(buf.length()));
    if (!*out) { miniz::miniz_buf_free(buf); return -1; }
    std::memcpy(*out, buf.begin(), buf.length());
    *out_len = buf.length();
    miniz::miniz_buf_free(buf);
    return 0;
}

int miniz_decompress_dict(const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap,
                          size_t* out_written) {
    if (!in || !out || !out_written) return -1;
    size_t w = 0;
    if (!miniz::decompress_dict(in, in_len, out, out_cap, w)) return -1;
    *out_written = w;
    return 0;
}

size_t miniz_dict_uncompress_size(const uint8_t* in, size_t in_len) {
    if (!in || in_len < 14) return 0;
    if (std::memcmp(in, "MZ2\0", 4) != 0) return 0;
    uint8_t method = in[4];
    if (method != 2) return 0;
    // Dictionary format: [MAGIC][method=2][dict_len u32][orig_size u32][huff_flag][payload]
    return static_cast<uint32_t>(in[10]) |
           (static_cast<uint32_t>(in[11]) << 8) |
           (static_cast<uint32_t>(in[12]) << 16) |
           (static_cast<uint32_t>(in[13]) << 24);
}

uint32_t miniz_crc32(const uint8_t* data, size_t len) {
    return miniz::miniz_crc32(data, len);
}

void miniz_free(void* ptr) {
    if (ptr) std::free(ptr);
}