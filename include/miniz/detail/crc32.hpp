/* ============================================================================
 * CRC-32 (ISO/HDLC) integrity checksum
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Table-driven CRC-32 with the standard reflected polynomial
 *   0xEDB88320. 256-entry table built lazily on first use (thread-safe
 *   under C++11 static init). One pass over data, O(n), one multiply-free
 *   XOR-shift per byte.
 *
 * MEMORY LAYOUT (table):
 *   static uint32_t table[256]  — built once, read-only thereafter
 *
 * THINGS YOU SHOULD KNOW:
 *   - Not cryptographically secure; for corruption detection only.
 *   - Table construction is ~256 iterations, done once per process.
 *   - crc32_update is hot-path friendly: inline, no branches in the loop
 *     body beyond the table lookup, no allocations.
 *   - Initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF (standard CRC-32).
 *
 * BUILD:
 *   g++     -std=c++17 -O2 example.cpp -o example
 *   clang++ -std=c++17 -O2 example.cpp -o example
 * ============================================================================ */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace miniz {
namespace detail {

// Rationale: function-local static avoids static-initialisation-order
// fiasco; C++11 guarantees thread-safe one-time init.
inline const uint32_t* crc32_table() noexcept {
    static uint32_t table[256];
    static bool     built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    return table;
}

// Rationale: reflected processing (LSB first) lets us use a single table
// lookup per byte without bit-reversing the input.
inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    crc = ~crc;
    const uint32_t* tab = crc32_table();
    for (size_t i = 0; i < len; ++i)
        crc = tab[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

inline uint32_t crc32(const uint8_t* data, size_t len) noexcept {
    return crc32_update(0u, data, len);
}

} // detail
} // miniz