/* miniz_cli — command-line interface for miniz
 * Author: HadeShiki
 * License: MIT
 *
 * Usage:
 *   miniz_cli c <in> <out>    compress
 *   miniz_cli d <in> <out>    decompress
 *
 * Build:
 *   g++     -std=c++17 -O2 -I../include tools/miniz_cli.cpp -o miniz_cli
 *   clang++ -std=c++17 -O2 -I../include tools/miniz_cli.cpp -o miniz_cli
 *   cl      /std:c++17 /O2 /I..\include tools\miniz_cli.cpp
 */

#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>

static bool read_file(const char* path, std::vector<uint8_t>& buf) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    buf.resize(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (sz > 0 && std::fread(buf.data(), 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

static bool write_file(const char* path, const uint8_t* data, size_t len) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    if (len > 0 && std::fwrite(data, 1, len, f) != len) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
            "usage:\n"
            "  miniz_cli c <in> <out>    compress\n"
            "  miniz_cli d <in> <out>    decompress\n");
        return 1;
    }

    std::vector<uint8_t> in;
    if (!read_file(argv[2], in)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }

    bool ok;
    if (std::strcmp(argv[1], "c") == 0) {
        miniz::ByteBuf out;
        ok = miniz::compress(in.data(), in.size(), out, miniz::CompressLevel::NORMAL);
        if (ok) {
            ok = write_file(argv[3], out.begin(), out.length());
            miniz::miniz_buf_free(out);
        }
    } else if (std::strcmp(argv[1], "d") == 0) {
        // Decompress into a large buffer; for CLI we cap at 256 MB.
        const size_t MAX_OUT = 256u * 1024u * 1024u;
        uint8_t* out = static_cast<uint8_t*>(std::malloc(MAX_OUT));
        if (!out) {
            std::fprintf(stderr, "out of memory\n");
            return 3;
        }
        size_t written = 0;
        ok = miniz::decompress(in.data(), in.size(), out, MAX_OUT, written);
        if (ok) {
            ok = write_file(argv[3], out, written);
        }
        std::free(out);
    } else {
        std::fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 1;
    }

    if (!ok) {
        std::fprintf(stderr, "codec failed\n");
        return 3;
    }
    return 0;
}