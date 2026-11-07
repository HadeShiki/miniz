#!/usr/bin/env python3
"""
miniz Python bindings via ctypes
================================

Example of using miniz from Python through the C ABI.

Build the shared library first:
    g++ -std=c++17 -O2 -fPIC -shared -I../include src/miniz_c.cpp -o libminiz.so
    # Windows:
    g++ -std=c++17 -O2 -fPIC -shared -I../include src/miniz_c.cpp -o libminiz.dll

Usage:
    python3 miniz_ctypes.py
"""

import ctypes
import os
import sys


class Miniz:
    """ctypes wrapper for miniz compression library."""

    def __init__(self, lib_path=None):
        if lib_path is None:
            # Look for the shared library next to this file
            here = os.path.dirname(os.path.abspath(__file__))
            candidates = [
                os.path.join(here, "libminiz.so"),
                os.path.join(here, "libminiz.dll"),
                os.path.join(here, "miniz.dll"),
            ]
            for c in candidates:
                if os.path.exists(c):
                    lib_path = c
                    break
            if lib_path is None:
                raise RuntimeError(
                    "libminiz not found. Build it first:\n"
                    "  g++ -std=c++17 -O2 -fPIC -shared -I../include src/miniz_c.cpp -o libminiz.so"
                )

        self._lib = ctypes.CDLL(lib_path)

        # Compression levels
        self.MINIZ_FAST = 0
        self.MINIZ_NORMAL = 1
        self.MINIZ_BEST = 2

        # --- miniz_compress ---
        # int miniz_compress(const uint8_t* in, size_t in_len,
        #                    uint8_t** out, size_t* out_len, int level)
        self._lib.miniz_compress.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.c_int,
        ]
        self._lib.miniz_compress.restype = ctypes.c_int

        # --- miniz_decompress ---
        # int miniz_decompress(const uint8_t* in, size_t in_len,
        #                      uint8_t* out, size_t out_cap,
        #                      size_t* out_written)
        self._lib.miniz_decompress.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._lib.miniz_decompress.restype = ctypes.c_int

        # --- miniz_compress_dict ---
        self._lib.miniz_compress_dict.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.c_int,
        ]
        self._lib.miniz_compress_dict.restype = ctypes.c_int

        # --- miniz_uncompress_size ---
        # size_t miniz_uncompress_size(const uint8_t* in, size_t in_len)
        self._lib.miniz_uncompress_size.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        self._lib.miniz_uncompress_size.restype = ctypes.c_size_t

        # --- miniz_dict_uncompress_size ---
        # size_t miniz_dict_uncompress_size(const uint8_t* in, size_t in_len)
        self._lib.miniz_dict_uncompress_size.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        self._lib.miniz_dict_uncompress_size.restype = ctypes.c_size_t

        # --- miniz_crc32 ---
        # uint32_t miniz_crc32(const uint8_t* data, size_t len)
        self._lib.miniz_crc32.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
        self._lib.miniz_crc32.restype = ctypes.c_uint32

        # --- miniz_free ---
        # void miniz_free(void* ptr)
        self._lib.miniz_free.argtypes = [ctypes.c_void_p]
        self._lib.miniz_free.restype = None

    def compress(self, data: bytes, level: int = 1) -> bytes:
        """Compress bytes. Returns compressed bytes."""
        if not data:
            return b""

        in_buf = (ctypes.c_uint8 * len(data))(*data)
        out_ptr = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t(0)

        ret = self._lib.miniz_compress(
            in_buf, len(data),
            ctypes.byref(out_ptr), ctypes.byref(out_len),
            level,
        )
        if ret != 0:
            raise RuntimeError(f"miniz_compress failed with code {ret}")

        try:
            result = bytes(out_ptr[:out_len.value])
        finally:
            self._lib.miniz_free(out_ptr)

        return result

    def decompress(self, data: bytes, out_cap: int = None) -> bytes:
        """Decompress bytes. Returns original bytes."""
        if not data:
            return b""

        # Determine output size from header
        in_buf = (ctypes.c_uint8 * len(data))(*data)
        orig_size = self._lib.miniz_uncompress_size(in_buf, len(data))
        if orig_size == 0:
            raise RuntimeError("miniz_uncompress_size failed - invalid header")
        if out_cap is not None and out_cap < orig_size:
            out_cap = orig_size
        else:
            out_cap = orig_size

        out_buf = (ctypes.c_uint8 * out_cap)()
        out_written = ctypes.c_size_t(0)

        ret = self._lib.miniz_decompress(
            in_buf, len(data),
            out_buf, out_cap,
            ctypes.byref(out_written),
        )
        if ret != 0:
            raise RuntimeError(f"miniz_decompress failed with code {ret}")

        return bytes(out_buf[:out_written.value])

    def compress_dict(self, dict_data: bytes, data: bytes, level: int = 1) -> bytes:
        """Compress with a dictionary. Returns compressed bytes (self-contained)."""
        if not data:
            return b""

        dict_buf = (ctypes.c_uint8 * len(dict_data))(*dict_data)
        in_buf = (ctypes.c_uint8 * len(data))(*data)
        out_ptr = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t(0)

        ret = self._lib.miniz_compress_dict(
            dict_buf, len(dict_data),
            in_buf, len(data),
            ctypes.byref(out_ptr), ctypes.byref(out_len),
            level,
        )
        if ret != 0:
            raise RuntimeError(f"miniz_compress_dict failed with code {ret}")

        try:
            result = bytes(out_ptr[:out_len.value])
        finally:
            self._lib.miniz_free(out_ptr)

        return result

    def decompress_dict(self, data: bytes, out_cap: int = None) -> bytes:
        """Decompress dictionary-compressed bytes."""
        if not data:
            return b""

        # Determine output size from header
        in_buf = (ctypes.c_uint8 * len(data))(*data)
        orig_size = self._lib.miniz_dict_uncompress_size(in_buf, len(data))
        if orig_size == 0:
            raise RuntimeError("miniz_dict_uncompress_size failed - invalid header")
        out_cap = orig_size

        out_buf = (ctypes.c_uint8 * out_cap)()
        out_written = ctypes.c_size_t(0)

        ret = self._lib.miniz_decompress_dict(
            in_buf, len(data),
            out_buf, out_cap,
            ctypes.byref(out_written),
        )
        if ret != 0:
            raise RuntimeError(f"miniz_decompress_dict failed with code {ret}")

        return bytes(out_buf[:out_written.value])

    def crc32(self, data: bytes) -> int:
        """Compute CRC-32 checksum."""
        if not data:
            return 0
        buf = (ctypes.c_uint8 * len(data))(*data)
        return self._lib.miniz_crc32(buf, len(data))


def main():
    mz = Miniz()

    # Test basic compress/decompress
    text = b"The quick brown fox jumps over the lazy dog. " * 100
    print(f"Original:  {len(text)} bytes")

    compressed = mz.compress(text, mz.MINIZ_NORMAL)
    print(f"Compressed: {len(compressed)} bytes")

    decompressed = mz.decompress(compressed)
    assert decompressed == text, "Round-trip failed!"
    print("Round-trip: OK")

    # Test FAST vs BEST
    c_fast = mz.compress(text, mz.MINIZ_FAST)
    c_best = mz.compress(text, mz.MINIZ_BEST)
    print(f"FAST:  {len(c_fast)} bytes")
    print(f"BEST:  {len(c_best)} bytes")

    # Test dictionary
    dict_data = b"The quick brown fox jumps over the lazy dog. "
    small_text = b"The quick brown fox jumps over the lazy dog. "
    c_dict = mz.compress_dict(dict_data, small_text, mz.MINIZ_NORMAL)
    d_dict = mz.decompress_dict(c_dict)
    assert d_dict == small_text, "Dictionary round-trip failed!"
    print(f"Dictionary: {len(small_text)} -> {len(c_dict)} bytes, round-trip: OK")

    # Test CRC-32
    crc = mz.crc32(text)
    print(f"CRC-32: {crc:#010x}")

    print("\nAll Python ctypes tests passed!")


if __name__ == "__main__":
    main()