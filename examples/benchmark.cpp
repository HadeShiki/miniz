/* miniz benchmark — compare FAST / NORMAL / zlib / zstd
 * Build: g++ -std=c++17 -O2 -Iinclude benchmark.cpp -lz -lzstd -o benchmark
 * Run:   ./benchmark
 */

#include "miniz/miniz.hpp"
#include <zlib.h>
#include <zstd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <vector>

using hrclock = std::chrono::high_resolution_clock;

static double now_s() noexcept {
    return std::chrono::duration<double>(hrclock::now().time_since_epoch()).count();
}

struct Result {
    const char* name;
    size_t      in_len;
    size_t      out_len;
    double      comp_ms;
    double      decomp_ms;
    double      ratio;
    bool        ok;
};

// ---- miniz ----
static Result bench_miniz(const char* name, miniz::CompressLevel level,
                          const uint8_t* in, size_t in_len) noexcept {
    Result r{name, in_len, 0, 0, 0, 0, false};

    miniz::ByteBuf cb;
    double t0 = now_s();
    bool ok = miniz::compress(in, in_len, cb, level);
    double t1 = now_s();

    if (!ok) return r;

    uint8_t* out = static_cast<uint8_t*>(std::malloc(in_len));
    if (!out) { miniz::miniz_buf_free(cb); return r; }

    size_t w = 0;
    double t2 = now_s();
    bool ok2 = miniz::decompress(cb.begin(), cb.length(), out, in_len, w);
    double t3 = now_s();

    r.out_len = cb.length();
    r.comp_ms = (t1 - t0) * 1000.0;
    r.decomp_ms = (t3 - t2) * 1000.0;
    r.ratio = 100.0 * cb.length() / in_len;
    r.ok = ok2 && (w == in_len) && (std::memcmp(out, in, in_len) == 0);

    std::free(out);
    miniz::miniz_buf_free(cb);
    return r;
}

// ---- zlib ----
static Result bench_zlib(const char* name, int level,
                         const uint8_t* in, size_t in_len) noexcept {
    Result r{name, in_len, 0, 0, 0, 0, false};

    uLongf out_bound = compressBound(in_len);
    uint8_t* out = static_cast<uint8_t*>(std::malloc(out_bound));
    if (!out) return r;

    double t0 = now_s();
    int zr = compress2(out, &out_bound, in, in_len, level);
    double t1 = now_s();

    if (zr != Z_OK) { std::free(out); return r; }

    uint8_t* out2 = static_cast<uint8_t*>(std::malloc(in_len));
    if (!out2) { std::free(out); return r; }
    uLongf out2_len = in_len;

    double t2 = now_s();
    int zr2 = uncompress(out2, &out2_len, out, out_bound);
    double t3 = now_s();

    r.out_len = out_bound;
    r.comp_ms = (t1 - t0) * 1000.0;
    r.decomp_ms = (t3 - t2) * 1000.0;
    r.ratio = 100.0 * out_bound / in_len;
    r.ok = (zr2 == Z_OK) && (out2_len == in_len) &&
           (std::memcmp(out2, in, in_len) == 0);

    std::free(out);
    std::free(out2);
    return r;
}

// ---- zstd ----
static Result bench_zstd(const char* name, int level,
                         const uint8_t* in, size_t in_len) noexcept {
    Result r{name, in_len, 0, 0, 0, 0, false};

    size_t out_bound = ZSTD_compressBound(in_len);
    uint8_t* out = static_cast<uint8_t*>(std::malloc(out_bound));
    if (!out) return r;

    double t0 = now_s();
    size_t csize = ZSTD_compress(out, out_bound, in, in_len, level);
    double t1 = now_s();

    if (ZSTD_isError(csize)) { std::free(out); return r; }

    uint8_t* out2 = static_cast<uint8_t*>(std::malloc(in_len));
    if (!out2) { std::free(out); return r; }

    double t2 = now_s();
    size_t dsize = ZSTD_decompress(out2, in_len, out, csize);
    double t3 = now_s();

    r.out_len = csize;
    r.comp_ms = (t1 - t0) * 1000.0;
    r.decomp_ms = (t3 - t2) * 1000.0;
    r.ratio = 100.0 * csize / in_len;
    r.ok = (!ZSTD_isError(dsize)) && (dsize == in_len) &&
           (std::memcmp(out2, in, in_len) == 0);

    std::free(out);
    std::free(out2);
    return r;
}

// ---- data generators ----
static void gen_text(uint8_t* buf, size_t len) noexcept {
    const char* t = "The quick brown fox jumps over the lazy dog. ";
    size_t tl = std::strlen(t);
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(t[i % tl]);
}

static void gen_repetitive(uint8_t* buf, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>('A' + (i % 26));
}

static void gen_binary(uint8_t* buf, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(i * 7 + 3);
}

static void gen_random_like(uint8_t* buf, size_t len) noexcept {
    uint32_t state = 12345;
    for (size_t i = 0; i < len; ++i) {
        state = state * 1103515245u + 12345u;
        buf[i] = static_cast<uint8_t>((state >> 16) & 0xFF);
    }
}

static void print_result(const Result& r) noexcept {
    if (!r.ok) {
        printf("%-18s len=%-8zu -> %-8zu ratio=----- comp=----- decomp=-----  FAILED\n",
               r.name, r.in_len, r.out_len);
        return;
    }
    printf("%-18s len=%-8zu -> %-8zu ratio=%5.2f%% comp=%7.3f ms decomp=%7.3f ms\n",
           r.name, r.in_len, r.out_len, r.ratio, r.comp_ms, r.decomp_ms);
}

static void run_suite(const char* suite_name, void (*gen)(uint8_t*, size_t),
                      size_t len) noexcept {
    std::vector<uint8_t> data(len);
    gen(data.data(), len);

    printf("\n=== %s (%zu bytes) ===\n", suite_name, len);

    print_result(bench_miniz("miniz FAST",  miniz::CompressLevel::FAST,   data.data(), len));
    print_result(bench_miniz("miniz NORMAL",miniz::CompressLevel::NORMAL, data.data(), len));
    print_result(bench_zlib("zlib level1",   1, data.data(), len));
    print_result(bench_zlib("zlib level6",   6, data.data(), len));
    print_result(bench_zlib("zlib level9",   9, data.data(), len));
    print_result(bench_zstd("zstd level1",   1, data.data(), len));
    print_result(bench_zstd("zstd level3",   3, data.data(), len));
    print_result(bench_zstd("zstd level19", 19, data.data(), len));
}

int main() {
    printf("miniz benchmark — FAST / NORMAL / zlib / zstd\n");
    printf("============================================================\n");

    // Small
    run_suite("text 1KB",    gen_text,    1024);
    run_suite("text 10KB",   gen_text,    10240);
    run_suite("text 100KB",  gen_text,   102400);
    run_suite("text 1MB",    gen_text,  1048576);

    run_suite("repetitive 1KB",   gen_repetitive,   1024);
    run_suite("repetitive 10KB",  gen_repetitive,  10240);
    run_suite("repetitive 100KB", gen_repetitive, 102400);

    run_suite("binary 1KB",   gen_binary,   1024);
    run_suite("binary 10KB",  gen_binary,  10240);
    run_suite("binary 100KB", gen_binary, 102400);

    run_suite("random-like 1KB",   gen_random_like,   1024);
    run_suite("random-like 10KB",  gen_random_like,  10240);
    run_suite("random-like 100KB", gen_random_like, 102400);

    printf("\nDone.\n");
    return 0;
}