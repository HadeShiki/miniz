/* C ABI wrapper for miniz (for ctypes / FFI bindings)
 * Author: HadeShiki
 * License: MIT
 *
 * All functions are extern "C" and safe to call from Python ctypes,
 * C, or any language with a C FFI.
 *
 * Memory ownership:
 *   - miniz_compress / miniz_compress_dict allocate *out via malloc;
 *     caller must call miniz_free(*out) when done.
 *   - miniz_decompress / miniz_decompress_dict write into caller-provided
 *     out buffer; caller must ensure out_cap is large enough.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compression level (matches C++ CompressLevel enum)
typedef enum {
    MINIZ_FAST   = 0,
    MINIZ_NORMAL = 1,
    MINIZ_BEST   = 2,
} miniz_level_t;

// One-shot compress. Returns 0 on success, nonzero on failure.
// *out is allocated with malloc; caller must call miniz_free(*out).
// *out_len receives the compressed size.
int miniz_compress(const uint8_t* in, size_t in_len,
                   uint8_t** out, size_t* out_len,
                   int level);

// One-shot decompress. Returns 0 on success, nonzero on failure.
// *out_written receives the number of bytes written to out.
// out must be at least out_cap bytes; out_cap must be >= original size.
int miniz_decompress(const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_cap,
                     size_t* out_written);

// Return the original (uncompressed) size from a compressed header.
// Reads the magic+size fields without decompressing. Returns 0 on failure.
size_t miniz_uncompress_size(const uint8_t* in, size_t in_len);

// Dictionary compress. Dictionary is prepended to input; output is
// self-contained (includes dictionary).
int miniz_compress_dict(const uint8_t* dict, size_t dict_len,
                        const uint8_t* in, size_t in_len,
                        uint8_t** out, size_t* out_len,
                        int level);

// Dictionary decompress.
int miniz_decompress_dict(const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t out_cap,
                          size_t* out_written);

// Return the original (uncompressed) size from a dictionary-compressed header.
size_t miniz_dict_uncompress_size(const uint8_t* in, size_t in_len);

// CRC-32 integrity checksum.
uint32_t miniz_crc32(const uint8_t* data, size_t len);

// Free a buffer allocated by miniz_compress / miniz_compress_dict.
void miniz_free(void* ptr);

#ifdef __cplusplus
}
#endif