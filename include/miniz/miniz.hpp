/* ============================================================================
 * miniz/miniz.hpp — LZSS + optional Huffman compression (header-only)
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Compresses byte streams using LZSS (64 KiB sliding window) optionally
 *   followed by static Huffman coding on the LZSS token stream. On-disk
 *   format:
 *     [MAGIC "MZ2\0"][method u8][orig_size u32 LE][payload...]
 *   method 0 = raw LZSS tokens, method 1 = Huffman-coded LZSS tokens.
 *   All I/O uses raw pointers and growable byte buffers; no std::vector,
 *   no iostream, no exceptions in hot paths.
 *
 *   Additional modules:
 *     CompressLevel   — FAST / NORMAL / BEST encode effort
 *     crc32           — ISO/HDLC CRC-32 integrity checksum
 *     StreamEnc/Dec   — incremental (chunked) compress/decompress
 *
 * MEMORY LAYOUT (outer format):
 *      +--------+--------+--------+--------+--------+-----------------------+
 *      | MAGIC  | method | orig_size (LE u32)         | payload...        |
 *      | 4 B    | 1 B    | 4 B                          | variable          |
 *      +--------+--------+--------+--------+--------+-----------------------+
 *
 * THINGS YOU SHOULD KNOW:
 *   - Deterministic but not optimal; greedy match search.
 *   - Window is 64 KiB; longer matches fall back to literals.
 *   - Decompress validates original size; aborts on corrupted stream.
 *   - Not thread-safe; use one context per thread or serialize.
 *   - abort() on magic mismatch, truncated stream, or bad Huffman tree.
 *
 * BUILD:
 *   g++     -std=c++17 -O2 example.cpp -o example
 *   clang++ -std=c++17 -O2 example.cpp -o example
 *   cl      /std:c++17 /O2 example.cpp
 * ============================================================================ */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <atomic>
#include <cassert>
#include <utility>

#include "detail/buffer.hpp"
#include "detail/level.hpp"
#include "detail/crc32.hpp"
#include "detail/lzss.hpp"
#include "detail/huffman.hpp"
#include "detail/stream.hpp"

namespace miniz {

// Rationale: bring detail implementation symbols into miniz so users only
// include one header and type miniz::ByteBuf instead of miniz::detail::ByteBuf.
using detail::ByteBuf;
using detail::CompressLevel;
using detail::DEFAULT_LEVEL;
using detail::lzss_encode;
using detail::lzss_decode;
using detail::huff_encode;
using detail::huff_decode;
using detail::miniz_buf_free;
using detail::crc32;
using detail::crc32_update;
using detail::StreamEnc;
using detail::StreamDec;

static constexpr uint32_t MAGIC_LE = 0x00325A4Du; // "MZ2\0" little-endian

// ---------------------------------------------------------------------------
// One-shot compress / decompress
// ---------------------------------------------------------------------------

// Rationale: compress returns an allocated ByteBuf; caller must call
// miniz_buf_free when done. Ownership is explicit; no hidden allocations
// inside the function. level controls encode effort: FAST accepts first
// valid match, NORMAL walks hash chain for best match.
inline bool compress(const uint8_t* in, size_t in_len, ByteBuf& out,
                     CompressLevel level = DEFAULT_LEVEL) noexcept {
    out.clear();

    // LZSS encode first (level affects match search effort only)
    ByteBuf lz;
    if (!lzss_encode(in, in_len, lz, level)) return false;

    // Try Huffman; keep the smaller of the two
    ByteBuf hz;
    bool use_huff = false;
    if (in_len > 0) {
        if (huff_encode(lz.begin(), lz.length(), hz)) {
            if (hz.length() < lz.length()) use_huff = true;
        }
    }

    // outer header: [MAGIC][method][orig_size]
    uint8_t hdr[9];
    hdr[0] = static_cast<uint8_t>((MAGIC_LE >> 0) & 0xFF);
    hdr[1] = static_cast<uint8_t>((MAGIC_LE >> 8) & 0xFF);
    hdr[2] = static_cast<uint8_t>((MAGIC_LE >> 16) & 0xFF);
    hdr[3] = static_cast<uint8_t>((MAGIC_LE >> 24) & 0xFF);
    hdr[4] = use_huff ? 1u : 0u;
    hdr[5] = static_cast<uint8_t>((in_len >> 0) & 0xFF);
    hdr[6] = static_cast<uint8_t>((in_len >> 8) & 0xFF);
    hdr[7] = static_cast<uint8_t>((in_len >> 16) & 0xFF);
    hdr[8] = static_cast<uint8_t>((in_len >> 24) & 0xFF);

    if (!out.append(hdr, 9)) return false;
    if (use_huff) {
        if (!out.append(hz.begin(), hz.length())) return false;
    } else {
        if (!out.append(lz.begin(), lz.length())) return false;
    }
    return true;
}

inline bool decompress(const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t& out_written) noexcept {
    out_written = 0;
    if (in_len < 9) { fprintf(stderr, "[miniz] stream too short\n"); std::abort(); }

    // verify magic (byte comparison, endian-safe)
    if (std::memcmp(in, "MZ2\0", 4) != 0) {
        fprintf(stderr, "[miniz] bad magic\n");
        std::abort();
    }

    uint8_t  method = in[4];
    uint32_t orig_size = static_cast<uint32_t>(in[5]) |
                         (static_cast<uint32_t>(in[6]) << 8) |
                         (static_cast<uint32_t>(in[7]) << 16) |
                         (static_cast<uint32_t>(in[8]) << 24);

    const uint8_t* payload = in + 9;
    size_t payload_len = in_len - 9;

    if (orig_size > out_cap) { fprintf(stderr, "[miniz] output buffer too small\n"); std::abort(); }

    // Rationale: method 1 payload is Huffman-coded LZSS tokens. Huffman
    // decode recovers the LZSS token stream into a temp buffer, then LZSS
    // decode expands tokens into the final output. Method 0 payload is
    // already raw LZSS tokens.
    if (method == 0) {
        if (!lzss_decode(payload, payload_len, out, out_cap, out_written)) return false;
    } else if (method == 1) {
        // Huffman header's orig_size == LZSS token stream length
        if (payload_len < 6) { fprintf(stderr, "[miniz] huff stream too short\n"); std::abort(); }
        uint32_t lz_len = static_cast<uint32_t>(payload[0]) |
                          (static_cast<uint32_t>(payload[1]) << 8) |
                          (static_cast<uint32_t>(payload[2]) << 16) |
                          (static_cast<uint32_t>(payload[3]) << 24);

        uint8_t* temp = static_cast<uint8_t*>(std::malloc(lz_len ? lz_len : 1));
        if (!temp) { fprintf(stderr, "[miniz] temp alloc failed\n"); std::abort(); }

        size_t temp_written = 0;
        bool ok = huff_decode(payload, payload_len, temp, lz_len, temp_written);
        if (ok) {
            ok = lzss_decode(temp, temp_written, out, out_cap, out_written);
        }
        std::free(temp);
        if (!ok) return false;
    } else {
        fprintf(stderr, "[miniz] unknown method %u\n", method);
        std::abort();
    }

    if (out_written != orig_size) {
        fprintf(stderr, "[miniz] size mismatch: got %zu, want %u\n", out_written, orig_size);
        std::abort();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Dictionary mode — prepend dictionary to input before compression so
// matches can reference dictionary data. Dictionary is stored in the
// output so every compressed frame is self-contained.
// Format: [MAGIC][method=2][dict_len u32][orig_size u32][compressed(dict+input)]
// ---------------------------------------------------------------------------

inline bool compress_dict(const uint8_t* dict, size_t dict_len,
                          const uint8_t* in, size_t in_len,
                          ByteBuf& out, CompressLevel level = DEFAULT_LEVEL) noexcept {
    out.clear();

    // Combined buffer: [dict data][input data]
    size_t combined_len = dict_len + in_len;
    uint8_t* combined = static_cast<uint8_t*>(std::malloc(combined_len ? combined_len : 1));
    if (!combined) return false;
    if (dict_len > 0) std::memcpy(combined, dict, dict_len);
    if (in_len > 0) std::memcpy(combined + dict_len, in, in_len);

    // LZSS encode on combined stream — hash tables built naturally from
    // dictionary data, so matches can reference dictionary positions.
    ByteBuf lz;
    if (!lzss_encode(combined, combined_len, lz, level)) {
        std::free(combined);
        return false;
    }
    std::free(combined);

    // Try Huffman
    ByteBuf hz;
    bool use_huff = false;
    if (combined_len > 0) {
        if (huff_encode(lz.begin(), lz.length(), hz)) {
            if (hz.length() < lz.length()) use_huff = true;
        }
    }

    // Header: [MAGIC][method=2][dict_len][orig_size][huff_flag]
    uint8_t hdr[10];
    hdr[0] = static_cast<uint8_t>((MAGIC_LE >> 0) & 0xFF);
    hdr[1] = static_cast<uint8_t>((MAGIC_LE >> 8) & 0xFF);
    hdr[2] = static_cast<uint8_t>((MAGIC_LE >> 16) & 0xFF);
    hdr[3] = static_cast<uint8_t>((MAGIC_LE >> 24) & 0xFF);
    hdr[4] = 2u; // method 2 = dictionary
    hdr[5] = static_cast<uint8_t>((dict_len >> 0) & 0xFF);
    hdr[6] = static_cast<uint8_t>((dict_len >> 8) & 0xFF);
    hdr[7] = static_cast<uint8_t>((dict_len >> 16) & 0xFF);
    hdr[8] = static_cast<uint8_t>((dict_len >> 24) & 0xFF);
    hdr[9] = use_huff ? 1u : 0u;

    if (!out.append(hdr, 10)) { miniz_buf_free(lz); miniz_buf_free(hz); return false; }

    // orig_size u32 LE (original input size, NOT including dictionary)
    uint8_t sz[4];
    sz[0] = static_cast<uint8_t>((in_len >> 0) & 0xFF);
    sz[1] = static_cast<uint8_t>((in_len >> 8) & 0xFF);
    sz[2] = static_cast<uint8_t>((in_len >> 16) & 0xFF);
    sz[3] = static_cast<uint8_t>((in_len >> 24) & 0xFF);
    if (!out.append(sz, 4)) { miniz_buf_free(lz); miniz_buf_free(hz); return false; }

    if (use_huff) {
        if (!out.append(hz.begin(), hz.length())) { miniz_buf_free(lz); miniz_buf_free(hz); return false; }
    } else {
        if (!out.append(lz.begin(), lz.length())) { miniz_buf_free(lz); miniz_buf_free(hz); return false; }
    }
    miniz_buf_free(lz);
    miniz_buf_free(hz);
    return true;
}

inline bool decompress_dict(const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t out_cap, size_t& out_written) noexcept {
    out_written = 0;
    if (in_len < 14) { fprintf(stderr, "[miniz] dict stream too short\n"); std::abort(); }

    if (std::memcmp(in, "MZ2\0", 4) != 0) {
        fprintf(stderr, "[miniz] bad magic\n");
        std::abort();
    }

    uint8_t method = in[4];
    if (method != 2) {
        fprintf(stderr, "[miniz] not a dictionary stream (method=%u)\n", method);
        std::abort();
    }

    uint32_t dict_len = static_cast<uint32_t>(in[5]) |
                        (static_cast<uint32_t>(in[6]) << 8) |
                        (static_cast<uint32_t>(in[7]) << 16) |
                        (static_cast<uint32_t>(in[8]) << 24);

    uint32_t orig_size = static_cast<uint32_t>(in[10]) |
                         (static_cast<uint32_t>(in[11]) << 8) |
                         (static_cast<uint32_t>(in[12]) << 16) |
                         (static_cast<uint32_t>(in[13]) << 24);

    uint8_t huff_flag = in[13];

    const uint8_t* payload = in + 14;
    size_t payload_len = in_len - 14;

    // Decompressed size = dict_len + orig_size
    size_t total_size = static_cast<size_t>(dict_len) + static_cast<size_t>(orig_size);

    // Decompress into a temporary buffer that holds dict + input
    uint8_t* temp = static_cast<uint8_t*>(std::malloc(total_size ? total_size : 1));
    if (!temp) { fprintf(stderr, "[miniz] temp alloc failed\n"); std::abort(); }

    size_t temp_written = 0;
    bool ok;

    if (huff_flag == 1) {
        ok = huff_decode(payload, payload_len, temp, total_size, temp_written);
    } else {
        ok = lzss_decode(payload, payload_len, temp, total_size, temp_written);
    }

    if (ok && temp_written != total_size) {
        fprintf(stderr, "[miniz] dict size mismatch: got %zu, want %zu\n", temp_written, total_size);
        ok = false;
    }

    if (ok) {
        // Skip dictionary prefix, copy original input to output
        if (orig_size > out_cap) {
            fprintf(stderr, "[miniz] output buffer too small for orig_size\n");
            ok = false;
        } else {
            std::memcpy(out, temp + dict_len, static_cast<size_t>(orig_size));
            out_written = static_cast<size_t>(orig_size);
        }
    }

    std::free(temp);
    if (!ok) return false;
    return true;
}

// ---------------------------------------------------------------------------
// CRC-32 integrity checksum (public wrappers)
// ---------------------------------------------------------------------------

inline uint32_t miniz_crc32(const uint8_t* data, size_t len) noexcept {
    return crc32(data, len);
}

inline uint32_t miniz_crc32_update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    return crc32_update(crc, data, len);
}

// ---------------------------------------------------------------------------
// STL allocator adapter (stateless, empty)
// ---------------------------------------------------------------------------

template <typename T>
struct Allocator {
    using value_type = T;
    Allocator() noexcept = default;
    template <typename U> Allocator(const Allocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
        return static_cast<T*>(std::malloc(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t /*n*/) noexcept {
        if (p) std::free(p);
    }
    template <typename U> bool operator==(const Allocator<U>&) const noexcept { return true; }
    template <typename U> bool operator!=(const Allocator<U>&) const noexcept { return false; }
};

// ---------------------------------------------------------------------------
// RAII helpers
// ---------------------------------------------------------------------------

template <typename T>
inline T* miniz_make(std::size_t n = 1) noexcept {
    return static_cast<T*>(std::malloc(n * sizeof(T)));
}

template <typename T>
inline void miniz_destroy(T* p) noexcept {
    if (p) std::free(p);
}

} // miniz