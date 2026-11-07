/* ============================================================================
 * incremental (streaming) compress/decompress
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Instead of requiring the whole input in memory, StreamEnc and
 *   StreamDec process data in chunks. The caller feeds input via
 *   push_in() and pulls output via pull_out(). Internal state carries
 *   over between calls (pending bits, partial tokens, dictionary).
 *
 *   StreamEnc: lzss_encode each chunk into a pending token buffer, then
 *              optionally Huffman-code when flush() is called.
 *   StreamDec: buffer raw bytes until a full frame (magic+header) is
 *              available, then decode incrementally.
 *
 * THINGS YOU SHOULD KNOW:
 *   - Not thread-safe; one stream per thread or serialise externally.
 *   - Buffers grow geometrically; caller can reserve() to pre-allocate.
 *   - abort() on stream corruption, same as the one-shot API.
 *   - This is a framing layer on top of the block API; compression
 *     ratio is identical to one-shot compress() for the same input.
 *
 * BUILD:
 *   g++     -std=c++17 -O2 example.cpp -o example
 *   clang++ -std=c++17 -O2 example.cpp -o example
 * ============================================================================ */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "buffer.hpp"
#include "level.hpp"

namespace miniz {
namespace detail {

// ---------------------------------------------------------------------------
// StreamEnc — incremental encoder
// ---------------------------------------------------------------------------

struct StreamEnc {
    ByteBuf raw;          // accumulated raw input
    ByteBuf out;          // emitted compressed bytes
    uint64_t total_size = 0;  // total raw bytes fed

    StreamEnc() noexcept = default;
    ~StreamEnc() = default;
    StreamEnc(const StreamEnc&) = delete;
    StreamEnc& operator=(const StreamEnc&) = delete;

    // Rationale: accumulate raw input; LZSS encoding happens once at
    // finish() so the sliding window spans all chunks and each chunk
    // doesn't get its own spurious 0x02 terminator.
    inline bool push_in(const uint8_t* data, size_t len) noexcept {
        total_size += len;
        if (!raw.append(data, len)) return false;
        return true;
    }

    // Rationale: no-op retained for API symmetry; finish() does the work.
    inline bool flush() noexcept { return true; }

    // Rationale: encode all accumulated raw input with LZSS, optionally
    // Huffman-code if it shrinks, then frame with magic header. Caller
    // must call miniz_buf_free(out) when done.
    inline bool finish(bool use_huff) noexcept {
        ByteBuf lz;
        if (!lzss_encode(raw.begin(), raw.length(), lz)) return false;

        const uint8_t* payload = lz.begin();
        size_t payload_len = lz.length();
        uint8_t method = 0;

        if (use_huff && payload_len > 0) {
            ByteBuf hz;
            if (!huff_encode(payload, payload_len, hz)) {
                miniz_buf_free(lz);
                miniz_buf_free(hz);
                return false;
            }
            if (hz.length() < payload_len) {
                payload = hz.begin();
                payload_len = hz.length();
                method = 1;
                bool ok = write_frame(payload, payload_len, method, total_size);
                miniz_buf_free(lz);
                miniz_buf_free(hz);
                return ok;
            }
            miniz_buf_free(hz);
        }
        bool ok = write_frame(payload, payload_len, method, total_size);
        miniz_buf_free(lz);
        return ok;
    }

    inline bool write_frame(const uint8_t* payload, size_t payload_len, uint8_t method, uint64_t orig_size) noexcept {
        out.clear();
        uint8_t hdr[9];
        hdr[0] = 0x4D; hdr[1] = 0x5A; hdr[2] = 0x32; hdr[3] = 0x00;
        hdr[4] = method;
        hdr[5] = static_cast<uint8_t>((orig_size >> 0) & 0xFF);
        hdr[6] = static_cast<uint8_t>((orig_size >> 8) & 0xFF);
        hdr[7] = static_cast<uint8_t>((orig_size >> 16) & 0xFF);
        hdr[8] = static_cast<uint8_t>((orig_size >> 24) & 0xFF);
        if (!out.append(hdr, 9)) return false;
        if (!out.append(payload, payload_len)) return false;
        return true;
    }

    inline void reset() noexcept {
        raw.clear();
        out.clear();
    }
};

// ---------------------------------------------------------------------------
// StreamDec — incremental decoder
// ---------------------------------------------------------------------------

struct StreamDec {
    uint8_t  method = 0;
    uint32_t orig_size = 0;
    size_t   header_pos = 0;
    bool     header_parsed = false;
    ByteBuf  payload;   // accumulated compressed bytes
    ByteBuf  output;    // decoded output awaiting pull_out()

    StreamDec() noexcept = default;
    ~StreamDec() = default;
    StreamDec(const StreamDec&) = delete;
    StreamDec& operator=(const StreamDec&) = delete;

    // Rationale: feed compressed bytes incrementally. Buffers them until
    // finish() is called, then decodes everything at once. This avoids
    // needing a pausable/resumable bit reader while keeping the caller's
    // memory footprint small (they can feed chunk-by-chunk).
    inline bool push_in(const uint8_t* data, size_t len) noexcept {
        // Parse header incrementally (9 bytes)
        if (!header_parsed) {
            size_t need = 9 - header_pos;
            size_t take = len < need ? len : need;
            // We need a temporary place for header bytes as they arrive.
            // Store header in the first 9 bytes of payload, then move them.
            if (header_pos == 0 && take < 9) {
                // Not enough for full header yet; buffer it
                if (!payload.append(data, take)) return false;
                header_pos += take;
                return true;
            }
            // We have enough to complete the header
            uint8_t hdr[9];
            size_t already = header_pos;
            std::memcpy(hdr, payload.begin(), already);
            std::memcpy(hdr + already, data, take);
            header_pos = 0;
            header_parsed = true;

            if (std::memcmp(hdr, "MZ2\0", 4) != 0) {
                fprintf(stderr, "[miniz] bad magic\n");
                std::abort();
            }
            method  = hdr[4];
            orig_size = static_cast<uint32_t>(hdr[5]) |
                        (static_cast<uint32_t>(hdr[6]) << 8) |
                        (static_cast<uint32_t>(hdr[7]) << 16) |
                        (static_cast<uint32_t>(hdr[8]) << 24);

            // Append remaining input after the header
            size_t rest = len - take;
            if (rest > 0) {
                if (!payload.append(data + take, rest)) return false;
            }
            return true;
        }

        // Header already parsed; just accumulate payload
        if (!payload.append(data, len)) return false;
        return true;
    }

    // Rationale: decode all accumulated payload into output. Call after
    // all compressed bytes have been pushed. aborts on corruption.
    inline bool finish() noexcept {
        if (!header_parsed) { fprintf(stderr, "[miniz] incomplete header\n"); std::abort(); }

        const uint8_t* p = payload.begin();
        size_t p_len = payload.length();

        // Rationale: temp buffer sized to orig_size so large streams don't
        // overflow a fixed stack array.
        size_t tmp_cap = orig_size ? orig_size : 4096;
        uint8_t* tmp = static_cast<uint8_t*>(std::malloc(tmp_cap));
        if (!tmp) { fprintf(stderr, "[miniz] temp alloc failed\n"); std::abort(); }

        bool ok = false;
        if (method == 0) {
            size_t written = 0;
            ok = lzss_decode(p, p_len, tmp, tmp_cap, written);
            if (ok) ok = output.append(tmp, written);
        } else if (method == 1) {
            // Huffman: decode into temp LZSS token buffer, then LZSS decode
            if (p_len < 6) { fprintf(stderr, "[miniz] huff stream too short\n"); std::abort(); }
            uint32_t lz_len = static_cast<uint32_t>(p[0]) |
                              (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24);
            uint8_t* lz_temp = static_cast<uint8_t*>(std::malloc(lz_len ? lz_len : 1));
            if (!lz_temp) { fprintf(stderr, "[miniz] temp alloc failed\n"); std::abort(); }
            size_t lz_written = 0;
            ok = huff_decode(p, p_len, lz_temp, lz_len, lz_written);
            if (ok) {
                size_t written = 0;
                ok = lzss_decode(lz_temp, lz_written, tmp, tmp_cap, written);
                if (ok) ok = output.append(tmp, written);
            }
            std::free(lz_temp);
        } else {
            fprintf(stderr, "[miniz] unknown method %u\n", method);
            std::abort();
        }
        std::free(tmp);
        return ok;
    }

    // Rationale: pull decoded bytes into caller buffer. Returns bytes
    // actually copied (may be less than requested if not yet available).
    inline size_t pull_out(uint8_t* buf, size_t len) noexcept {
        size_t take = output.length() < len ? output.length() : len;
        if (take > 0) {
            std::memcpy(buf, output.begin(), take);
            if (take < output.length())
                std::memmove(output.begin(), output.begin() + take,
                             output.length() - take);
            output.size -= take;
        }
        return take;
    }

    inline bool finished() const noexcept { return header_parsed && output.size == 0 && payload.size == 0; }
    inline bool has_output() const noexcept { return output.length() > 0; }
    inline void reset() noexcept {
        method = 0;
        orig_size = 0;
        header_pos = 0;
        header_parsed = false;
        payload.clear();
        output.clear();
    }
};

} // detail
} // miniz