/* ============================================================================
 * LZSS encode/decode with 64 KiB sliding window + hash chain match search
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Greedy LZSS over a 64 KiB sliding window. Tokens:
 *     0x00 count bytes...   literal run (count = 1..255)
 *     0x01 dist_hi dist_lo len-3  match (len = 3..258)
 *     0x02                   end of stream
 *   Match search uses a hash chain on the first 3 bytes instead of an
 *   O(window) brute-force scan. HEAD[hash] points to the most recent
 *   position with that 3-byte prefix; PREV[pos] links to the previous
 *   position with the same hash. Chain walk stops at WINDOW_SIZE distance
 *   or MAX_CHAIN depth to bound worst-case cost.
 *
 *   FAST level:  accept first match >= MIN_MATCH (stop chain early).
 *   NORMAL level: walk chain (up to MAX_CHAIN) to find the best match.
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
 *   - Hash tables are heap-allocated per encode call (~320 KB for small
 *     inputs, scales with input size for PREV table).
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

#include "level.hpp"

namespace miniz {
namespace detail {

static constexpr uint32_t WINDOW_SIZE  = 65535u;
static constexpr uint32_t MIN_MATCH    = 3u;
static constexpr uint32_t MAX_MATCH    = 258u;
static constexpr uint32_t MAX_LIT_RUN  = 255u;
static constexpr uint32_t HASH_BITS    = 15u;
static constexpr uint32_t HASH_SIZE    = 1u << HASH_BITS;
static constexpr uint32_t HASH_MASK    = HASH_SIZE - 1u;
static constexpr uint32_t MAX_CHAIN    = 128u;

// Rationale: hash first 3 bytes at position p into a 15-bit bucket.
// 3 bytes is the minimum match length, so any match must share this prefix.
// XOR-fold keeps distribution even across bucket count.
static inline uint32_t hash3(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16)) & HASH_MASK;
}

// Rationale: hash-chain match search replaces O(window) brute-force scan
// with O(chain_length) walk over positions sharing the same 3-byte prefix.
// Chain walk stops at WINDOW_SIZE distance or MAX_CHAIN depth to bound
// worst-case cost. FAST level stops at first valid match; NORMAL walks
// chain to find best match.
static inline void lzss_find_match(const uint8_t* in, size_t pos, size_t in_len,
                                   CompressLevel level,
                                   int32_t* head, int32_t* prev,
                                   size_t& best_dist, size_t& best_len) noexcept {
    best_dist = 0;
    best_len  = 0;
    if (pos < MIN_MATCH) return;

    size_t remaining = in_len - pos;
    size_t maxlen = remaining < MAX_MATCH ? remaining : MAX_MATCH;
    if (maxlen < MIN_MATCH) return;

    uint32_t h = hash3(in + pos);
    int32_t j = head[h];
    uint32_t chain = 0;

    while (j >= 0) {
        size_t dist = pos - static_cast<size_t>(j);
        if (dist > WINDOW_SIZE) break;   // outside sliding window

        if (in[j] == in[pos]) {
            size_t len = 1;
            while (len < maxlen && in[j + len] == in[pos + len]) ++len;
            if (len > best_len) {
                best_len  = len;
                best_dist = dist;
                if (len == maxlen) break;
                if (level == CompressLevel::FAST && len >= MIN_MATCH) break;
            }
        }
        j = prev[j];
        if (++chain >= MAX_CHAIN) break;
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

static inline bool lzss_encode(const uint8_t* in, size_t in_len, ByteBuf& out,
                               CompressLevel level) noexcept {
    out.clear();

    // Rationale: heap-allocate hash tables. HEAD is fixed 128 KB; PREV scales
    // with input size (4 bytes per position). Free both on every exit path.
    int32_t* head = static_cast<int32_t*>(std::malloc(HASH_SIZE * sizeof(int32_t)));
    int32_t* prev = static_cast<int32_t*>(std::malloc(in_len ? in_len * sizeof(int32_t) : 1));
    if (!head || !prev) {
        std::free(head);
        std::free(prev);
        return false;
    }
    for (uint32_t i = 0; i < HASH_SIZE; ++i) head[i] = -1;
    for (size_t i = 0; i < in_len; ++i) prev[i] = -1;

    size_t pos = 0;
    uint8_t lit[MAX_LIT_RUN];
    size_t lit_cnt = 0;

    while (pos < in_len) {
        size_t dist, len;
        lzss_find_match(in, pos, in_len, level, head, prev, dist, len);

        // Insert current position into hash chain (if enough bytes remain
        // to form a MIN_MATCH-length key).
        if (pos + MIN_MATCH <= in_len) {
            uint32_t h = hash3(in + pos);
            prev[pos] = head[h];
            head[h] = static_cast<int32_t>(pos);
        }

        if (len >= MIN_MATCH) {
            if (!lzss_flush_lit(lit, lit_cnt, out)) { std::free(head); std::free(prev); return false; }
            if (!out.push_back(0x01)) { std::free(head); std::free(prev); return false; }
            if (!out.push_back(static_cast<uint8_t>((dist >> 8) & 0xFF))) { std::free(head); std::free(prev); return false; }
            if (!out.push_back(static_cast<uint8_t>(dist & 0xFF))) { std::free(head); std::free(prev); return false; }
            if (!out.push_back(static_cast<uint8_t>(len - MIN_MATCH))) { std::free(head); std::free(prev); return false; }
            pos += len;
        } else {
            lit[lit_cnt++] = in[pos++];
            if (lit_cnt >= MAX_LIT_RUN) {
                if (!lzss_flush_lit(lit, lit_cnt, out)) { std::free(head); std::free(prev); return false; }
            }
        }
    }
    if (!lzss_flush_lit(lit, lit_cnt, out)) { std::free(head); std::free(prev); return false; }
    if (!out.push_back(0x02)) { std::free(head); std::free(prev); return false; }

    std::free(head);
    std::free(prev);
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

} // detail
} // miniz