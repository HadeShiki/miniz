/* STL allocator adapter (C++)
 * Build: g++ -std=c++17 -O2 -I../include allocator.cpp -o allocator
 * Run:   ./allocator
 */

#include "miniz/miniz.hpp"
#include <vector>
#include <cstdio>
#include <cstring>

int main() {
    // std::vector using miniz::Allocator instead of std::allocator
    std::vector<uint8_t, miniz::Allocator<uint8_t>> buf;
    buf.reserve(256);
    for (int i = 0; i < 256; ++i) buf.push_back(static_cast<uint8_t>(i));

    std::printf("buf.size()=%zu buf.data()=%p\n", buf.size(), (void*)buf.data());

    miniz::ByteBuf compressed;
    std::printf("before compress: size=%zu data=%p\n",
                compressed.length(), (void*)compressed.begin());

    if (!miniz::compress(buf.data(), buf.size(), compressed)) {
        std::fprintf(stderr, "compress failed\n");
        return 1;
    }

    std::printf("after compress: size=%zu data=%p\n",
                compressed.length(), (void*)compressed.begin());

    uint8_t out[512];
    size_t out_written = 0;
    if (!miniz::decompress(compressed.begin(), compressed.length(),
                           out, sizeof(out), out_written)) {
        std::fprintf(stderr, "decompress failed\n");
        return 1;
    }
    if (out_written != buf.size() || std::memcmp(out, buf.data(), buf.size()) != 0) {
        std::fprintf(stderr, "round-trip mismatch\n");
        return 1;
    }

    std::printf("OK (%zu -> %zu bytes)\n", buf.size(), compressed.length());
    miniz::miniz_buf_free(compressed);
    return 0;
}