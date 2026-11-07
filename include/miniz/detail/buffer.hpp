/* ============================================================================
 * growable byte buffer (raw malloc/realloc)
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   A simple growable byte buffer backed by realloc. Geometric 2x growth
 *   amortizes copy cost. No std::vector, no hidden allocations, no
 *   exceptions. The buffer owns its allocation and frees it in the
 *   destructor.
 *
 * THINGS YOU SHOULD KNOW:
 *   - Not thread-safe; use one buffer per thread or serialize access.
 *   - push_back/append return false on allocation failure (never throw).
 *   - copy constructor and assignment are deleted to avoid accidental
 *     deep copies of large buffers.
 *
 * BUILD:
 *   g++     -std=c++17 -O2 example.cpp -o example
 *   clang++ -std=c++17 -O2 example.cpp -o example
 * ============================================================================ */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace miniz {
namespace detail {

struct ByteBuf {
    uint8_t* data = nullptr;
    size_t   size = 0;
    size_t   cap  = 0;

    ByteBuf() noexcept = default;
    ~ByteBuf() { if (data) std::free(data); }
    ByteBuf(const ByteBuf&) = delete;
    ByteBuf& operator=(const ByteBuf&) = delete;

    // Rationale: realloc may move the allocation; we update data/size/cap
    // atomically. Geometric (2x) growth keeps amortized copy cost O(1).
    inline bool push_back(uint8_t b) noexcept {
        if (size == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            uint8_t* ndata = static_cast<uint8_t*>(std::realloc(data, ncap));
            if (!ndata) return false;
            data = ndata;
            cap  = ncap;
        }
        data[size++] = b;
        return true;
    }

    inline bool append(const uint8_t* src, size_t n) noexcept {
        if (n == 0) return true;
        if (size + n > cap) {
            size_t ncap = cap;
            while (ncap < size + n) ncap = ncap ? ncap * 2 : 64;
            uint8_t* ndata = static_cast<uint8_t*>(std::realloc(data, ncap));
            if (!ndata) return false;
            data = ndata;
            cap  = ncap;
        }
        std::memcpy(data + size, src, n);
        size += n;
        return true;
    }

    inline void clear() noexcept { size = 0; }
    inline uint8_t* begin() noexcept { return data; }
    inline const uint8_t* begin() const noexcept { return data; }
    inline size_t length() const noexcept { return size; }
};

// Rationale: centralise ByteBuf deallocation here so detail modules
// (e.g. stream) can free buffers without depending on miniz.hpp.
inline void miniz_buf_free(ByteBuf& b) noexcept {
    if (b.data) { std::free(b.data); b.data = nullptr; b.size = 0; b.cap = 0; }
}

}
}