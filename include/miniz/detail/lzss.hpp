/* ============================================================================
 * LZSS encode/decode with 64 KiB sliding window
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Greedy LZSS over a 64 KiB sliding window. Tokens:
 *     0x00 count bytes...   literal run (count = 1..255)
 *     0x01 dist_hi dist_lo len-3  match (len = 3..258)
 *     0x02                   end of stream
 *   Match search is brute-force over the window; branchy on purpose since
 *   the common match sizes predict perfectly and avoid a table load.
 *
 * MEMORY LAYOUT (LZSS token stream):
 *      +--------+-----------------------+  repeat N times  +--------+
 *      | tag 0x00 | count u8 | bytes... |                  | 0x02   |
 *      | tag 0x01 | dist hi  | dist lo  | len-3 u8         | end    |
 *      +--------+-----------------------+                  +--------+
 *
 * THINGS YOU SHOULD KNOW:
 *   - Not thread-safe; callers serialize or use one context per thread.
 *   - Decode aborts on truncated stream, invalid tag, or bad distance.
 *   - Output buffer must be large enough for the original size.
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

namespace miniz {
namespace detail {

static constexpr uint32_t WINDOW_SIZE  = 65535u;
static constexpr uint32_t MIN_MATCH    = 3u;
static constexpr uint32_t MAX_MATCH    = 258u;
static constexpr uint32_t MAX_LIT_RUN  = 255u;

// Rationale: brute-force match search. For the sizes that actually appear
// the inner loop predicts well; a hash chain adds indirection and cache
// misses for tiny inputs.
static inline void lzss_find_match(const uint8_t* in, size_t pos, size_t in_len,
                                   size_t& best_dist, size_t& best_len) noexcept {
    best_dist = 0;
    best_len  = 0;
    if (pos < MIN_MATCH) return;

    size_t start = pos > WINDOW_SIZE ? pos - WINDOW_SIZE : 0;
    size_t remaining = in_len - pos;
    size_t maxlen = remaining < MAX_MATCH ? remaining : MAX_MATCH;
    if (maxlen < MIN_MATCH) return;

    for (size_t j = pos; j-- > start; ) {
        if (in[j] != in[pos]) continue;
        size_t len = 1;
        while (len < maxlen && in[j + len] == in[pos + len]) ++len;
        if (len > best_len) {
            best_len  = len;
            best_dist = pos - j;
            if (len == maxlen) break;
        }
    }
}

// Rationale: free function (not a lambda) to avoid any hidden closure
// overhead. Partial-flush branch is unreachable because the caller always
// flushes at the threshold, so no memmove is needed.
static inline bool lzss_flush_lit(uint8_t* lit, size_t& lit_cnt, ByteBuf& out) noexcept {
    while (lit_cnt > 0) {
        uint32_t n = lit_cnt > MAX_LIT_RUN ? MAX_LIT_RUN : static_cast<uint32_t>(lit_cnt);
        if (!out.push_back(0x00)) return false;
        if (!out.push_back(static_cast<uint8_t>(n))) return false;
        if (!out.append(lit, static_cast<size_t>(n))) return false;
        lit_cnt -= static_cast<size_t>(n);
    }
    return true;
}

static inline bool lzss_encode(const uint8_t* in, size_t in_len, ByteBuf& out) noexcept {
    out.clear();

    size_t pos = 0;
    uint8_t lit[MAX_LIT_RUN];
    size_t lit_cnt = 0;

    while (pos < in_len) {
        size_t dist, len;
        lzss_find_match(in, pos, in_len, dist, len);
        if (len >= MIN_MATCH) {
            if (!lzss_flush_lit(lit, lit_cnt, out)) return false;
            if (!out.push_back(0x01)) return false;
            if (!out.push_back(static_cast<uint8_t>((dist >> 8) & 0xFF))) return false;
            if (!out.push_back(static_cast<uint8_t>(dist & 0xFF))) return false;
            if (!out.push_back(static_cast<uint8_t>(len - MIN_MATCH))) return false;
            pos += len;
        } else {
            lit[lit_cnt++] = in[pos++];
            if (lit_cnt >= MAX_LIT_RUN) {
                if (!lzss_flush_lit(lit, lit_cnt, out)) return false;
            }
        }
    }
    if (!lzss_flush_lit(lit, lit_cnt, out)) return false;
    if (!out.push_back(0x02)) return false;
    return true;
}

static inline bool lzss_decode(const uint8_t* in, size_t in_len,
                               uint8_t* out, size_t out_cap, size_t& out_written) noexcept {
    out_written = 0;
    size_t p = 0;
    while (p < in_len) {
        uint8_t tag = in[p++];
        if (tag == 0x00) {
            if (p >= in_len) { fprintf(stderr, "[miniz] truncated literal run\n"); std::abort(); }
            uint32_t n = in[p++];
            if (p + static_cast<size_t>(n) > in_len) { fprintf(stderr, "[miniz] literal run overruns input\n"); std::abort(); }
            if (out_written + static_cast<size_t>(n) > out_cap) { fprintf(stderr, "[miniz] output buffer overflow\n"); std::abort(); }
            std::memcpy(out + out_written, in + p, static_cast<size_t>(n));
            out_written += static_cast<size_t>(n);
            p += static_cast<size_t>(n);
        } else if (tag == 0x01) {
            if (p + 3 > in_len) { fprintf(stderr, "[miniz] truncated match token\n"); std::abort(); }
            uint32_t dist = (static_cast<uint32_t>(in[p]) << 8) | static_cast<uint32_t>(in[p + 1]);
            uint32_t len  = static_cast<uint32_t>(in[p + 2]) + MIN_MATCH;
            p += 3;
            if (dist == 0 || dist > out_written) { fprintf(stderr, "[miniz] invalid match distance\n"); std::abort(); }
            if (out_written + static_cast<size_t>(len) > out_cap) { fprintf(stderr, "[miniz] output buffer overflow\n"); std::abort(); }
            size_t src = out_written - static_cast<size_t>(dist);
            for (uint32_t i = 0; i < len; ++i)
                out[out_written + static_cast<size_t>(i)] = out[src + static_cast<size_t>(i)];
            out_written += static_cast<size_t>(len);
        } else if (tag == 0x02) {
            break;
        } else {
            fprintf(stderr, "[miniz] unknown LZSS tag 0x%02x at offset %zu\n", tag, p - 1);
            std::abort();
        }
    }
    return true;
}

}
}