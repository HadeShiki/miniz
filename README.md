# miniz

LZSS + optional Huffman compression library (header-only, C++17).

*Small, fast, dependency-free, easy to embed.*

| Feature | Description |
|---------|-------------|
| **Header-only** | Drop `include/` into your project, `#include "miniz/miniz.hpp"` |
| **No dependencies** | Only `<cstdint>`, `<cstring>`, `<cstdio>`, `<cstdlib>`, `<new>` |
| **Raw pointers** | No `std::vector`, no `iostream`, no exceptions in hot paths |
| **LZSS + Huffman** | 64 KiB sliding window, optional static Huffman on token stream |
| **Compression levels** | `FAST` / `NORMAL` / `BEST` |
| **CRC-32** | Integrity checksum for stored/transmitted data |
| **Streaming** | `StreamEnc` / `StreamDec` for chunked compress/decompress |
| **STL allocator** | `Allocator<T>` for `std::vector` etc. |

## Build

```bash
make          # build demo + tests
make test     # build and run tests
make run      # run demo
make clean    # remove build/
```

Requires `g++` / `clang++` with C++17 support.

## Quick example

```cpp
#include "miniz/miniz.hpp"
#include <cstdio>
#include <cstring>

int main() {
    const char* text = "hello world hello world hello world hello world";
    size_t len = std::strlen(text);

    miniz::ByteBuf compressed;
    miniz::compress(reinterpret_cast<const uint8_t*>(text), len, compressed);

    uint8_t out[512];
    size_t written = 0;
    miniz::decompress(compressed.begin(), compressed.length(),
                      out, sizeof(out), written);

    miniz::miniz_buf_free(compressed);
    return 0;
}
```


## LICENSE

MIT. See [LICENSE](LICENSE) for details.