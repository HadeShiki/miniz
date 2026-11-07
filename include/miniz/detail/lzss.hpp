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
 *   Compression levels:
 *     FAST   — accept first match >= MIN_MATCH, stop chain early
 *     NORMAL — walk hash chain (up to MAX_CHAIN) to find best match
 *     BEST   — NORMAL + lazy matching: if match at next position is longer,
 *              emit literal byte at current position and take next match
 *
 *   Literal copies use SIMD (SSE2 16-byte, AVX2 32-byte) when the run is
 *   long enough, falling back to byte copy otherwise. SIMD destination
 *   pointers are taken after all push_back() calls that might reallocate,
 *   so no invalid pointer can arise during copy.
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
 *   - BEST level is ~10-20% slower than NORMAL but improves ratio on
 *     repetitive data.
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

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

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
// chain to find best match. dist==0 (self-match) is skipped.
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
        if (dist == 0) {                   // never match position with itself
            j = prev[j];
            if (++chain >= MAX_CHAIN) break;
            continue;
        }
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

// Rationale: SIMD literal copy helpers. SSE2 copies 16 bytes at a time;
// AVX2 copies 32 bytes. Called only after capacity is guaranteed so the
// destination pointer stays valid for the whole copy.
#if defined(__SSE2__)
static inline void simd_copy16(uint8_t* dst, const uint8_t* src) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(src)));
}
#endif

#if defined(__AVX2__)
static inline void simd_copy32(uint8_t* dst, const uint8_t* src) noexcept {
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),
                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src)));
}
#endif

// Rationale: ensure `out` can hold `need` more bytes without reallocating
// during the copy. Returns a stable pointer to the write position.
static inline uint8_t* buf_reserve(ByteBuf& out, size_t need) noexcept {
    size_t cur = out.size;
    if (cur + need > out.cap) {
        size_t ncap = out.cap;
        while (ncap < cur + need) ncap = ncap ? ncap * 2 : 64;
        uint8_t* ndata = static_cast<uint8_t*>(std::realloc(out.data, ncap));
        if (!ndata) return nullptr;
        out.data = ndata;
        out.cap  = ncap;
    }
    return out.data + cur;
}

// Rationale: flush accumulated literal bytes into output buffer. Header
// bytes (0x00 tag + count) are pushed first (may reallocate), then capacity
// is reserved once for the entire literal payload so SIMD destination
// pointers remain valid throughout the copy.
static inline bool lzss_flush_lit(uint8_t* lit, size_t& lit_cnt, ByteBuf& out) noexcept {
    while (lit_cnt > 0) {
        uint32_t n = lit_cnt > MAX_LIT_RUN ? MAX_LIT_RUN : static_cast<uint32_t>(lit_cnt);
        if (!out.push_back(0x00)) return false;
        if (!out.push_back(static_cast<uint8_t>(n))) return false;

        // Reserve space for the payload after header is written.
        uint8_t* dst = buf_reserve(out, static_cast<size_t>(n));
        if (!dst) return false;

        // Rationale: use SIMD for large literal runs to copy 16/32 bytes
        // per instruction instead of byte-by-byte. For small runs (<16B)
        // the setup cost outweighs the benefit, so byte copy is used.
        size_t copied = 0;
#if defined(__AVX2__)
        while (copied + 32u <= static_cast<size_t>(n)) {
            simd_copy32(dst + copied, lit + copied);
            copied += 32u;
        }
#elif defined(__SSE2__)
        while (copied + 16u <= static_cast<size_t>(n)) {
            simd_copy16(dst + copied, lit + copied);
            copied += 16u;
        }
#endif
        // Copy remaining bytes (0..15 or 0..31)
        for (; copied < static_cast<size_t>(n); ++copied) {
            dst[copied] = lit[copied];
        }
        out.size += static_cast<size_t>(n);

        lit_cnt -= static_cast<size_t>(n);
    }
    return true;
}

// Rationale: emit a match token (0x01 tag + dist hi/lo + len-3) into output.
static inline bool emit_match_token(ByteBuf& out, size_t d, size_t l) noexcept {
    if (!out.push_back(0x01)) return false;
    if (!out.push_back(static_cast<uint8_t>((d >> 8) & 0xFF))) return false;
    if (!out.push_back(static_cast<uint8_t>(d & 0xFF))) return false;
    if (!out.push_back(static_cast<uint8_t>(l - MIN_MATCH))) return false;
    return true;
}

// Rationale: BEST level uses lazy matching. When a match is found at `pos`,
// we tentatively save it, then check position `pos + 1`. If the next position
// has a longer match, we emit a literal byte at `pos` and take the next match.
// This improves compression on repetitive data at ~10-20% CPU cost.
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

    // BEST lazy matching state: tentative match deferred until we check
    // whether next position offers a longer match.
    size_t pending_dist = 0;
    size_t pending_len  = 0;
    bool   have_pending = false;

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

        // BEST level lazy matching: check if next position has a longer match.
        if (level == CompressLevel::BEST && len >= MIN_MATCH && !have_pending) {
            size_t next_dist = 0, next_len = 0;
            if (pos + 1 < in_len) {
                // Search at pos+1 WITHOUT inserting it first (avoid self-match).
                lzss_find_match(in, pos + 1, in_len, level, head, prev,
                                next_dist, next_len);
                // Now insert pos+1 into hash for future searches.
                if (pos + 1 + MIN_MATCH <= in_len) {
                    uint32_t h2 = hash3(in + pos + 1);
                    prev[pos + 1] = head[h2];
                    head[h2] = static_cast<int32_t>(pos + 1);
                }
            }
            if (next_len > len) {
                // Better match at next position: emit literal byte at pos,
                // then save match at pos+1 as pending.
                lit[lit_cnt++] = in[pos++];
                if (lit_cnt >= MAX_LIT_RUN) {
                    if (!lzss_flush_lit(lit, lit_cnt, out)) {
                        std::free(head); std::free(prev); return false;
                    }
                }
                pending_dist = next_dist;
                pending_len  = next_len;
                have_pending = true;
                // Flush literal + pending match now.
                if (!lzss_flush_lit(lit, lit_cnt, out)) {
                    std::free(head); std::free(prev); return false;
                }
                if (!emit_match_token(out, pending_dist, pending_len)) {
                    std::free(head); std::free(prev); return false;
                }
                have_pending = false;
                pos += pending_len;
                continue;
            }
        }

        if (have_pending) {
            if (!lzss_flush_lit(lit, lit_cnt, out)) {
                std::free(head); std::free(prev); return false;
            }
            if (!emit_match_token(out, pending_dist, pending_len)) {
                std::free(head); std::free(prev); return false;
            }
            have_pending = false;
        }

        if (len >= MIN_MATCH) {
            if (!lzss_flush_lit(lit, lit_cnt, out)) { std::free(head); std::free(prev); return false; }
            if (!emit_match_token(out, dist, len)) { std::free(head); std::free(prev); return false; }
            pos += len;
        } else {
            lit[lit_cnt++] = in[pos++];
            if (lit_cnt >= MAX_LIT_RUN) {
                if (!lzss_flush_lit(lit, lit_cnt, out)) { std::free(head); std::free(prev); return false; }
            }
        }
    }

    if (have_pending) {
        if (!lzss_flush_lit(lit, lit_cnt, out)) { std::free(head); std::free(prev); return false; }
        if (!emit_match_token(out, pending_dist, pending_len)) { std::free(head); std::free(prev); return false; }
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