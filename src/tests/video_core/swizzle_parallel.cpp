// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/textures/decoders.h"

namespace {
// Reference: the per-pixel GOB formula, independent of the optimized implementation.
u32 SwizzledOffset(u32 x_bytes, u32 y, u32 z, u32 stride, u32 height, u32 block_height,
                   u32 block_depth, bool native = false) {
    const u32 gobs_in_x = (stride + 63) / 64;
    const u32 block_size = gobs_in_x << (9 + block_height + block_depth);
    const u32 slice_size = ((height + (8u << block_height) - 1) >> (3 + block_height)) * block_size;
    const u32 block_y = y >> 3;
    const u32 offset_z = (z >> block_depth) * slice_size + ((z & ((1u << block_depth) - 1)) << (9 + block_height));
    const u32 offset_y = (block_y >> block_height) * block_size + ((block_y & ((1u << block_height) - 1)) << 9);
    const u32 offset_x = (x_bytes >> 6) << (9 + block_height + block_depth);
    const u32 gx = x_bytes & 63, gy = y & 7;
    const u32 in_gob = ((gx >> 5) << 8) | ((gy >> 1) << 6) | (((gx >> 4) & 1) << 5) | ((gy & 1) << 4) | (gx & 15);
    const u32 offset = offset_z + offset_y + offset_x + in_gob;
    if (!native) return offset;
    const u32 difference = ((offset >> 5) ^ (offset >> 6)) & 1;
    return offset ^ (difference << 5) ^ (difference << 6);
}
} // namespace

TEST_CASE("Large unswizzles split across helper threads match the GOB formula",
          "[video_core][swizzle]") {
    std::mt19937 rng{1234};
    struct Case { u32 bpp, width, height, depth, block_height, block_depth; };
    for (const bool native : {false, true})
    for (const Case c : {Case{4, 1024, 1024, 1, 4, 0}, Case{16, 256, 300, 1, 3, 0},
                         Case{4, 512, 260, 3, 2, 1}, Case{8, 2048, 520, 1, 5, 0},
                         Case{1, 1000, 999, 1, 4, 0}, Case{16, 130, 37, 1, 2, 0},
                         Case{16, 3, 5, 1, 0, 0}, Case{16, 1025, 1023, 1, 5, 0},
                         Case{16, 64, 64, 2, 1, 1}, Case{8, 514, 77, 1, 3, 0}}) {
        const u32 stride = ((c.width * c.bpp) + 63) / 64 * 64;
        const u32 slice_rows = ((c.height + (8u << c.block_height) - 1) >> (3 + c.block_height)) << (3 + c.block_height);
        const u32 slices = ((c.depth + (1u << c.block_depth) - 1) >> c.block_depth) << c.block_depth;
        std::vector<u8> input(size_t{stride} * slice_rows * slices + 4096);
        for (auto& b : input) b = static_cast<u8>(rng());
        std::vector<u8> output(size_t{c.width} * c.bpp * c.height * c.depth);
        Tegra::Texture::UnswizzleTexture(output, input, c.bpp, c.width, c.height, c.depth,
                                         c.block_height, c.block_depth, 1, native);
        size_t mismatches = 0;
        for (u32 z = 0; z < c.depth; ++z)
            for (u32 y = 0; y < c.height; ++y)
                for (u32 xb = 0; xb < c.width * c.bpp; ++xb) {
                    const size_t linear = (size_t{z} * c.height + y) * c.width * c.bpp + xb;
                    const u32 sw = SwizzledOffset(xb, y, z, stride, c.height, c.block_height, c.block_depth, native);
                    mismatches += output[linear] != input[sw];
                }
        INFO("native " << native << " bpp " << c.bpp << " " << c.width << "x" << c.height << "x" << c.depth);
        REQUIRE(mismatches == 0);
    }
}

TEST_CASE("Unswizzle speed", "[.swizzlebench]") {
    std::vector<u8> input(64 << 20), output(64 << 20);
    for (size_t i = 0; i < input.size(); i += 64) input[i] = static_cast<u8>(i >> 6);
    for (int rep = 0; rep < 3; ++rep) {
        const auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < 40; ++i)
            Tegra::Texture::UnswizzleTexture(output, input, 16, 1024, 1024, 1, 4, 0, 1, true);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        std::printf("16 Mo x 40 : %.3f s (%.2f Go/s)\n", t, 40 * 16.0 / 1024 / t);
    }
}
