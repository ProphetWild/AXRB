#pragma once
// Reads memory a GPU driver maps uncached (write-combined), such as a game's
// texture upload buffers. Plain loads fetch it one uncached access at a time;
// SSE4.1 streaming loads (MOVNTDQA) fetch whole 64-byte lines. Measured on an
// RTX 5090 (axrb-fixes tools/memory_read_probe.cpp): system RAM 0.31 GB/s with
// memcpy, 0.10 GB/s with memcmp, 9.1 GB/s streaming; ReBAR VRAM 0.05, 0.01 and
// 1.26 GB/s. On cached memory streaming loads cost about the same as memcpy.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define AXRB_STREAMING_LOADS 1
#endif

namespace axrb::texture {

#if AXRB_STREAMING_LOADS
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.1")))
#endif
inline void uncached_read(void* destination, const void* source, size_t size) {
    auto* in = static_cast<const uint8_t*>(source);
    auto* out = static_cast<uint8_t*>(destination);
    // Streaming loads need 16-byte aligned sources: the unaligned start is copied plainly.
    const size_t head = std::min(size, size_t((16 - (reinterpret_cast<uintptr_t>(in) & 15)) & 15));
    std::memcpy(out, in, head);
    in += head; out += head; size -= head;
    for (; size >= 64; size -= 64, in += 64, out += 64) {
        auto* line = reinterpret_cast<__m128i*>(const_cast<uint8_t*>(in));
        const __m128i a = _mm_stream_load_si128(line), b = _mm_stream_load_si128(line + 1);
        const __m128i c = _mm_stream_load_si128(line + 2), d = _mm_stream_load_si128(line + 3);
        auto* to = reinterpret_cast<__m128i*>(out);
        _mm_storeu_si128(to, a); _mm_storeu_si128(to + 1, b); _mm_storeu_si128(to + 2, c); _mm_storeu_si128(to + 3, d);
    }
    for (; size >= 16; size -= 16, in += 16, out += 16)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(in))));
    std::memcpy(out, in, size);
}
#else
inline void uncached_read(void* destination, const void* source, size_t size) { std::memcpy(destination, source, size); }
#endif

}  // namespace axrb::texture
