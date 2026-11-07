/* miniz example — streaming (incremental) compress/decompress (C++)
 * Build: g++ -std=c++17 -O2 -I../include stream.cpp -o stream
 * Run:   ./stream
 */

#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>

int main() {
    const char* text = "streaming data chunk one streaming data chunk two streaming data chunk three ";
    size_t tlen = std::strlen(text);
    uint8_t src[3000];
    for (size_t i = 0; i < 3000; ++i) src[i] = static_cast<uint8_t>(text[i % tlen]);

    // ---- Encode in chunks ----
    miniz::StreamEnc enc;
    size_t chunk = 256;
    size_t off = 0;
    while (off < 3000) {
        size_t take = 3000 - off < chunk ? 3000 - off : chunk;
        if (!enc.push_in(src + off, take)) {
            std::fprintf(stderr, "push_in failed\n");
            return 1;
        }
        off += take;
    }
    if (!enc.finish(true)) {   // true = try Huffman
        std::fprintf(stderr, "finish failed\n");
        return 1;
    }
    std::printf("stream encoded: %zu bytes\n", enc.out.length());

    // ---- Decode in chunks ----
    miniz::StreamDec dec;
    uint8_t outbuf[4096];
    size_t total_out = 0;

    // Feed compressed data in small pieces
    size_t coff = 0;
    size_t clen = enc.out.length();
    while (coff < clen) {
        size_t take = clen - coff < 128 ? clen - coff : 128;
        if (!dec.push_in(enc.out.begin() + coff, take)) {
            std::fprintf(stderr, "stream push_in failed\n");
            return 1;
        }
        coff += take;
    }
    // Finish decoding all accumulated payload
    if (!dec.finish()) {
        std::fprintf(stderr, "stream finish failed\n");
        return 1;
    }
    // Drain all decoded output
    size_t got;
    while ((got = dec.pull_out(outbuf, sizeof(outbuf))) > 0) {
        total_out += got;
    }

    if (total_out != 3000) {
        std::fprintf(stderr, "stream round-trip mismatch: got %zu want 3000\n", total_out);
        return 1;
    }
    std::printf("stream decoded: %zu bytes OK\n", total_out);

    // ---- Verify with one-shot API ----
    uint8_t one_out[4096];
    size_t one_written = 0;
    if (!miniz::decompress(enc.out.begin(), enc.out.length(),
                           one_out, sizeof(one_out), one_written)) {
        std::fprintf(stderr, "one-shot decompress failed\n");
        return 1;
    }
    if (one_written != 3000 || std::memcmp(one_out, src, 3000) != 0) {
        std::fprintf(stderr, "one-shot mismatch\n");
        return 1;
    }
    std::printf("one-shot verify: OK\n");

    miniz::miniz_buf_free(enc.out);
    std::printf("OK\n");
    return 0;
}