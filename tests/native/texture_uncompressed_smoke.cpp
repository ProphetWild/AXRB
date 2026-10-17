// Checks decode_block_texels (uncompressed ETC2/EAC storage, debug.axrb.astc_rgba8)
// against the decoders the BC transcoder uses: ETC2 texels must match exactly,
// and EAC's 16-bit values must reduce to the same 8-bit values. Edge blocks must
// write only their clipped texels.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "../../runtime/vulkan/texture/etc2_to_bc.h"
#include "../../runtime/vulkan/texture/uncached_read.h"

using namespace axrb::texture;

int main() {
    std::mt19937 rng(1234);
    uint8_t block[16];
    for (int round = 0; round < 20000; ++round) {
        for (auto& b : block) b = static_cast<uint8_t>(rng());
        // ETC2 colour formats: RGBA8 texels identical to the transcoder's decode.
        for (Codec codec : {Codec::Etc2Rgb, Codec::Etc2RgbA1, Codec::Etc2Rgba}) {
            uint8_t expected[16][4];
            if (codec == Codec::Etc2Rgba) {
                uint8_t alpha[16];
                decode_eac_alpha(block, alpha);
                decode_etc2_rgb(block + 8, expected, false);
                for (int i = 0; i < 16; ++i) expected[i][3] = alpha[i];
            } else {
                decode_etc2_rgb(block, expected, codec == Codec::Etc2RgbA1);
            }
            uint8_t texels[4][4][4];
            decode_block_texels(codec, block, &texels[0][0][0], 16, 4, 4);
            assert(std::memcmp(texels, expected, sizeof texels) == 0);
        }
        // EAC: 11-bit values expanded to 16 bits reduce to the 8-bit decode.
        for (bool isSigned : {false, true}) {
            int eight[16];
            uint16_t sixteen[16];
            decode_eac_r11(block, eight, isSigned);
            decode_eac_r11_16(block, sixteen, isSigned);
            for (int i = 0; i < 16; ++i) {
                if (isSigned) {
                    const int value = static_cast<int16_t>(sixteen[i]);
                    const int magnitude = (value < 0 ? -value : value) >> 5;
                    const int v = value < 0 ? -magnitude : magnitude;
                    assert(eight[i] == (v * 127 + (v < 0 ? -511 : 511)) / 1023);
                } else {
                    const int v = sixteen[i] >> 5;
                    assert(eight[i] == (v * 255 + 1023) / 2047);
                    assert((sixteen[i] & 31) == (v >> 6));  // low bits replicate the top ones
                }
            }
            uint16_t r[16], rg[16][2];
            decode_block_texels(isSigned ? Codec::EacRSigned : Codec::EacR, block, reinterpret_cast<uint8_t*>(r), 8, 4, 4);
            assert(std::memcmp(r, sixteen, sizeof r) == 0);
            decode_block_texels(isSigned ? Codec::EacRgSigned : Codec::EacRg, block, reinterpret_cast<uint8_t*>(rg), 16, 4, 4);
            for (int i = 0; i < 16; ++i) assert(rg[i][0] == sixteen[i]);
        }
    }
    // A 3 x 2 edge block writes only those texels, rows rowBytes apart.
    uint8_t canvas[8][32];
    std::memset(canvas, 0xAB, sizeof canvas);
    for (auto& b : block) b = static_cast<uint8_t>(rng());
    decode_block_texels(Codec::Etc2Rgb, block, &canvas[0][0], 32, 3, 2);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 32; ++x) {
            const bool inside = y < 2 && x < 12;
            assert(inside || canvas[y][x] == 0xAB);
        }
    // uncached_read copies exactly, from any alignment and of any length.
    std::vector<uint8_t> source(4096 + 64), copy(4096 + 64);
    for (auto& b : source) b = static_cast<uint8_t>(rng());
    for (size_t offset = 0; offset < 32; ++offset)
        for (size_t length : {size_t(0), size_t(1), size_t(15), size_t(16), size_t(17), size_t(63), size_t(64), size_t(65), size_t(1000), size_t(4096)}) {
            std::fill(copy.begin(), copy.end(), uint8_t(0xEE));
            uncached_read(copy.data() + 3, source.data() + offset, length);
            assert(std::memcmp(copy.data() + 3, source.data() + offset, length) == 0);
            assert(copy[2] == 0xEE && copy[3 + length] == 0xEE);
        }
    std::puts("TEXTURE_UNCOMPRESSED_SMOKE_PASS");
    return 0;
}
