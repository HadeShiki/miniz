/* ============================================================================
 * canonical Huffman encode/decode with bit I/O
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Builds a Huffman tree from symbol frequencies, derives canonical codes
 *   from code lengths, and serializes only the lengths (1 byte/symbol) plus
 *   the packed bitstream. Decoding walks a flat decode tree built from the
 *   canonical codes — no tree-pointer chasing during decode.
 *
 * MEMORY LAYOUT (Huffman stream):
 *      +--------+--------+--------+-----------------------+---------------+
 *      | orig_size u32 | tree_len u16 | tree bytes...   | bitstream...  |
 *      | 4 B           | 2 B          | variable        | variable      |
 *      +--------+--------+--------+-----------------------+---------------+
 *
 * THINGS YOU SHOULD KNOW:
 *   - Fixed 512-node stack arrays cover 256 leaves + 255 internal nodes;
 *     no dynamic allocation during tree build.
 *   - O(n^2) tree build is fine because n <= 256 (~65k comparisons worst
 *     case, trivial).
 *   - Decode aborts on truncated stream, bad length, or code collision.
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

namespace miniz {
namespace detail {

struct BitWriter {
    uint8_t* out;
    size_t   out_pos;
    size_t   out_cap;
    uint8_t  cur = 0;
    int      bits = 0;

    BitWriter(uint8_t* o, size_t cap) noexcept : out(o), out_pos(0), out_cap(cap) {}

    // Rationale: write MSB-first within `len` bits. Bit-by-bit is simple
    // and predictable; a barrel-shifter batch version would add complexity
    // for marginal gain on short inputs.
    inline bool write(uint32_t code, int len) noexcept {
        for (int i = len - 1; i >= 0; --i) {
            cur = static_cast<uint8_t>((cur << 1) | ((code >> static_cast<uint32_t>(i)) & 1u));
            if (++bits == 8) {
                if (out_pos >= out_cap) return false;
                out[out_pos++] = cur;
                cur = 0; bits = 0;
            }
        }
        return true;
    }

    inline bool flush() noexcept {
        if (bits > 0) {
            cur = static_cast<uint8_t>(cur << (8 - bits));
            if (out_pos >= out_cap) return false;
            out[out_pos++] = cur;
            cur = 0; bits = 0;
        }
        return true;
    }
};

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t byte_pos = 0;
    int bit_pos = 0;

    BitReader(const uint8_t* d, size_t n) noexcept : data(d), size(n) {}

    inline int read_bit() noexcept {
        if (byte_pos >= size) return -1;
        int b = (data[byte_pos] >> (7 - bit_pos)) & 1;
        if (++bit_pos == 8) { bit_pos = 0; ++byte_pos; }
        return b;
    }
};

struct HNode {
    uint32_t freq;
    int32_t  left, right;
    uint8_t  sym;
    inline bool leaf() const noexcept { return left < 0 && right < 0; }
};

// Rationale: fixed-size arrays on stack; 512 entries covers 256 leaves +
// 255 internal nodes. No dynamic allocation during tree build.
static inline int huff_build_tree(const uint32_t freq[256], HNode nodes[512]) noexcept {
    int node_cnt = 0;
    bool used[512] = {false};
    int active_cnt = 0;

    for (int s = 0; s < 256; ++s) {
        if (freq[s] == 0) continue;
        HNode n{};
        n.freq = freq[s];
        n.left = n.right = -1;
        n.sym  = static_cast<uint8_t>(s);
        nodes[node_cnt] = n;
        used[node_cnt] = true;
        ++active_cnt;
        ++node_cnt;
    }

    if (active_cnt == 1) {
        for (int i = 0; i < node_cnt; ++i) {
            if (used[i]) {
                HNode n{};
                n.freq = nodes[i].freq;
                n.left = i;
                n.right = -1;
                n.sym  = 0;
                nodes[node_cnt] = n;
                return node_cnt;
            }
        }
    }
    if (active_cnt == 0) return -1;

    while (active_cnt > 1) {
        int a = -1, b = -1;
        uint32_t fa = UINT32_MAX, fb = UINT32_MAX;
        for (int i = 0; i < node_cnt; ++i) {
            if (!used[i]) continue;
            uint32_t f = nodes[i].freq;
            if (f < fa) { fb = fa; b = a; fa = f; a = i; }
            else if (f < fb) { fb = f; b = i; }
        }
        used[a] = false;
        used[b] = false;
        active_cnt -= 2;

        HNode n{};
        n.freq = fa + fb;
        n.left = a;
        n.right = b;
        n.sym  = 0;
        nodes[node_cnt] = n;
        used[node_cnt] = true;
        ++active_cnt;
        ++node_cnt;
    }

    for (int i = 0; i < node_cnt; ++i) {
        if (used[i]) return i;
    }
    return -1;
}

static inline void huff_build_codes(const HNode* nodes, int root,
                                    uint32_t code[256], uint8_t len[256]) noexcept {
    for (int s = 0; s < 256; ++s) { code[s] = 0; len[s] = 0; }
    if (root < 0) return;

    // Rationale: iterative DFS with explicit stack — no recursion, no heap.
    struct Item { int idx; uint32_t c; uint8_t l; };
    Item stack[512];
    int sp = 0;
    stack[sp++] = {root, 0, 0};
    while (sp > 0) {
        Item it = stack[--sp];
        const HNode& n = nodes[it.idx];
        if (n.leaf()) {
            uint8_t l = it.l == 0 ? 1 : it.l;
            code[n.sym] = it.l == 0 ? 0u : it.c;
            len[n.sym]  = l;
        } else {
            if (n.right >= 0) stack[sp++] = {n.right, (it.c << 1) | 1u, static_cast<uint8_t>(it.l + 1)};
            if (n.left  >= 0) stack[sp++] = {n.left,  (it.c << 1) | 0u, static_cast<uint8_t>(it.l + 1)};
        }
    }
}

// Rationale: canonical codes let us decode with a flat table instead of
// tree walks, and we only store lengths (1 byte/symbol) not the full tree.
static inline bool huff_canonical_codes(const uint8_t len[256],
                                        uint32_t code[256]) noexcept {
    uint32_t count[33] = {0};
    for (int s = 0; s < 256; ++s) {
        if (len[s] > 32) return false;
        if (len[s] > 0) ++count[len[s]];
    }
    uint32_t first_code[33] = {0};
    uint32_t c = 0;
    for (int L = 1; L <= 32; ++L) {
        first_code[L] = c;
        c = (c + count[L]) << 1;
    }
    for (int s = 0; s < 256; ++s) code[s] = 0;
    uint32_t next[33];
    for (int L = 0; L <= 32; ++L) next[L] = first_code[L];
    for (int s = 0; s < 256; ++s) {
        uint8_t L = len[s];
        if (L == 0) continue;
        code[s] = next[L]++;
    }
    return true;
}

static inline bool huff_serialize_tree(const uint8_t len[256], ByteBuf& out) noexcept {
    uint16_t count = 0;
    for (int s = 0; s < 256; ++s) if (len[s] > 0) ++count;

    // Empty tree: tree_len = 0, nothing else follows. Decoder handles
    // tree_len == 0 by checking orig_size == 0.
    if (count == 0) {
        if (!out.push_back(0x00)) return false;
        if (!out.push_back(0x00)) return false;
        return true;
    }

    // tree_len = 2 (count field) + count * 2 (sym+len pairs)
    uint16_t tree_len = static_cast<uint16_t>(2u + static_cast<uint32_t>(count) * 2u);

    if (!out.push_back(static_cast<uint8_t>(tree_len & 0xFF))) return false;
    if (!out.push_back(static_cast<uint8_t>((tree_len >> 8) & 0xFF))) return false;
    if (!out.push_back(static_cast<uint8_t>(count & 0xFF))) return false;
    if (!out.push_back(static_cast<uint8_t>((count >> 8) & 0xFF))) return false;

    for (int s = 0; s < 256; ++s) {
        if (len[s] == 0) continue;
        if (!out.push_back(static_cast<uint8_t>(s))) return false;
        if (!out.push_back(len[s])) return false;
    }
    return true;
}

static inline bool huff_encode(const uint8_t* in, size_t in_len, ByteBuf& out) noexcept {
    out.clear();

    // orig_size u32 LE
    uint8_t hdr[4];
    hdr[0] = static_cast<uint8_t>(in_len & 0xFF);
    hdr[1] = static_cast<uint8_t>((in_len >> 8) & 0xFF);
    hdr[2] = static_cast<uint8_t>((in_len >> 16) & 0xFF);
    hdr[3] = static_cast<uint8_t>((in_len >> 24) & 0xFF);
    if (!out.append(hdr, 4)) return false;

    // freq
    uint32_t freq[256] = {0};
    for (size_t i = 0; i < in_len; ++i) ++freq[in[i]];

    HNode nodes[512];
    int root = huff_build_tree(freq, nodes);

    uint32_t tree_code[256];
    uint8_t  tree_len[256];
    huff_build_codes(nodes, root, tree_code, tree_len);

    uint32_t code[256];
    if (!huff_canonical_codes(tree_len, code)) return false;

    // serialize tree
    if (!huff_serialize_tree(tree_len, out)) return false;

    if (in_len == 0) return true;

    // bitstream: worst case 32 bits per symbol
    size_t bit_cap = in_len * 4u + 8u;
    uint8_t* bits = static_cast<uint8_t*>(std::malloc(bit_cap));
    if (!bits) return false;

    BitWriter bw(bits, bit_cap);
    bool ok = true;
    for (size_t i = 0; i < in_len; ++i) {
        if (tree_len[in[i]] == 0) { ok = false; break; }
        if (!bw.write(code[in[i]], static_cast<int>(tree_len[in[i]]))) { ok = false; break; }
    }
    if (ok && !bw.flush()) ok = false;

    if (ok) {
        // Rationale: bw.out_pos is already a byte count (incremented per
        // byte written, not per bit). The old (out_pos + 7) / 8 formula
        // treated it as bits, truncating the bitstream to ~1/8th of its
        // real size and causing "bitstream truncated" on decode.
        size_t nbytes = bw.out_pos;
        if (!out.append(bits, nbytes)) ok = false;
    }
    std::free(bits);
    return ok;
}

static inline bool huff_decode(const uint8_t* in, size_t in_len,
                               uint8_t* out, size_t out_cap, size_t& out_written) noexcept {
    out_written = 0;
    if (in_len < 6) { fprintf(stderr, "[miniz] huff stream too short\n"); std::abort(); }

    uint32_t orig_size = static_cast<uint32_t>(in[0]) |
                         (static_cast<uint32_t>(in[1]) << 8) |
                         (static_cast<uint32_t>(in[2]) << 16) |
                         (static_cast<uint32_t>(in[3]) << 24);
    uint16_t tree_len  = static_cast<uint16_t>(in[4]) |
                         (static_cast<uint16_t>(in[5]) << 8);

    if (6 + static_cast<size_t>(tree_len) > in_len) {
        fprintf(stderr, "[miniz] huff tree overruns input\n");
        std::abort();
    }

    uint8_t lens[256] = {0};

    if (tree_len == 0) {
        if (orig_size != 0) { fprintf(stderr, "[miniz] empty tree with nonzero size\n"); std::abort(); }
        return true;
    }

    const uint8_t* tp = in + 6;
    if (tree_len < 2) { fprintf(stderr, "[miniz] huff tree len too small\n"); std::abort(); }
    uint16_t count = static_cast<uint16_t>(tp[0]) | (static_cast<uint16_t>(tp[1]) << 8);
    if (static_cast<size_t>(tree_len) != 2u + static_cast<size_t>(count) * 2u) {
        fprintf(stderr, "[miniz] huff tree length mismatch\n");
        std::abort();
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t sym = tp[2 + static_cast<size_t>(i) * 2];
        uint8_t L   = tp[3 + static_cast<size_t>(i) * 2];
        if (L == 0 || L > 32) { fprintf(stderr, "[miniz] invalid huff length\n"); std::abort(); }
        lens[sym] = L;
    }

    uint32_t code[256];
    if (!huff_canonical_codes(lens, code)) { fprintf(stderr, "[miniz] canonical code failure\n"); std::abort(); }

    // build decode tree
    struct DNode { int32_t child[2]; int32_t sym; };
    DNode tree[512];
    int tree_cnt = 1;
    tree[0].child[0] = tree[0].child[1] = -1;
    tree[0].sym = -1;

    for (int s = 0; s < 256; ++s) {
        uint8_t L = lens[s];
        if (L == 0) continue;
        uint32_t c = code[s];
        int cur = 0;
        for (int i = L - 1; i >= 0; --i) {
            int bit = (c >> i) & 1;
            if (tree[cur].child[bit] < 0) {
                if (tree_cnt >= 512) { fprintf(stderr, "[miniz] huff decode tree overflow\n"); std::abort(); }
                tree[tree_cnt].child[0] = tree[tree_cnt].child[1] = -1;
                tree[tree_cnt].sym = -1;
                tree[cur].child[bit] = tree_cnt++;
            }
            cur = tree[cur].child[bit];
            if (i == 0) {
                if (tree[cur].sym != -1) { fprintf(stderr, "[miniz] huff code collision\n"); std::abort(); }
                tree[cur].sym = s;
            }
        }
    }

    const uint8_t* bp = in + 6 + static_cast<size_t>(tree_len);
    size_t bp_size = in_len - 6 - static_cast<size_t>(tree_len);
    BitReader br(bp, bp_size);

    if (orig_size > out_cap) { fprintf(stderr, "[miniz] output buffer too small for huff\n"); std::abort(); }

    int cur = 0;
    while (out_written < orig_size) {
        int bit = br.read_bit();
        if (bit < 0) { fprintf(stderr, "[miniz] huff bitstream truncated\n"); std::abort(); }
        cur = tree[cur].child[bit];
        if (cur < 0) { fprintf(stderr, "[miniz] huff invalid code path\n"); std::abort(); }
        if (tree[cur].sym >= 0) {
            out[out_written++] = static_cast<uint8_t>(tree[cur].sym);
            cur = 0;
        }
    }
    return true;
}

}
}