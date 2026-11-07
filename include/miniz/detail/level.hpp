/* ============================================================================
 * compression level tunables
 * Author: HadeShiki
 * License: MIT
 *
 * HOW IT WORKS:
 *   Three preset levels control how hard the LZSS match search works.
 *   Higher levels search more of the window for longer matches at the
 *   cost of CPU time. Compression ratio improves, encode speed drops.
 *   Decode speed is unaffected by level (format is identical).
 *
 * LEVELS:
 *   LEVEL_FAST   — stop at first match >= MIN_MATCH, no length maximisation
 *   LEVEL_NORMAL — greedy best match within window (current default)
 *   LEVEL_BEST   — normal + try slightly longer lazy evaluation (reserved)
 *
 * THINGS YOU SHOULD KNOW:
 *   - LEVEL_FAST can produce larger output than LEVEL_NORMAL on repetitive
 *     data, but is 2-4x faster on typical inputs.
 *   - Changing level changes only encode, never decode.
 *   - All values are constexpr; edit in-source if you need custom levels.
 *
 * BUILD:
 *   g++     -std=c++17 -O2 example.cpp -o example
 *   clang++ -std=c++17 -O2 example.cpp -o example
 * ============================================================================ */

#pragma once

#include <cstddef>
#include <cstdint>

namespace miniz {
namespace detail {

enum class CompressLevel : uint8_t {
    FAST   = 0,  // first-fit match, minimal CPU
    NORMAL = 1,  // best match in window (default)
    BEST   = 2,  // reserved: normal + lazy evaluation
};

// Rationale: default NORMAL gives the best speed/ratio trade-off for
// general use. Callers who prioritise throughput pick FAST.
static constexpr CompressLevel DEFAULT_LEVEL = CompressLevel::NORMAL;

// Rationale: in FAST mode we still cap the scan window to keep worst-case
// O(window) bounded; without this a pathological input (all same byte)
// degrades to O(n * window).
static constexpr size_t FAST_MAX_SCAN = 64;

} // detail
} // miniz