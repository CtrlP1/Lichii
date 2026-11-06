// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/div_ceil.h"
#include "common/thread.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Texture {
namespace {
// Helper threads for large (un)swizzles. Each line of a swizzle writes its own bytes, so lines
// can be split across threads with identical results. Texture uploads run on the GPU thread (the
// split's back), where unswizzling took ~12% of its time in Ocarina of Time.
// EDEN_NX2_SWIZZLE_THREADS=<n> sets the helpers (0 disables; default 3).
class SwizzlePool {
public:
    static SwizzlePool& Get() {
        static SwizzlePool pool;
        return pool;
    }

    /// Runs func(chunk) for chunk in [0, chunks), on the caller and the helpers; returns when done.
    /// Falls back to the caller alone when the pool is busy (another thread's swizzle).
    void Run(u32 chunks, const std::function<void(u32)>& func) {
        if (workers.empty() || chunks < 2 || !busy.try_lock()) {
            for (u32 i = 0; i < chunks; ++i) {
                func(i);
            }
            return;
        }
        auto job = std::make_shared<Job>();
        job->func = &func;
        job->chunks = chunks;
        {
            std::scoped_lock lock{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        while (job->done.load(std::memory_order_acquire) != chunks) {
            const u32 seen = job->done.load(std::memory_order_acquire);
            if (seen != chunks) {
                job->done.wait(seen, std::memory_order_acquire);
            }
        }
        {
            std::scoped_lock lock{mutex};
            current.reset();
        }
        busy.unlock();
    }

private:
    struct Job {
        const std::function<void(u32)>* func{};
        u32 chunks{};
        std::atomic<u32> next{0};
        std::atomic<u32> done{0};
    };

    SwizzlePool() {
        u32 count = 3;
        if (const char* value = std::getenv("EDEN_NX2_SWIZZLE_THREADS")) {
            count = static_cast<u32>(std::strtoul(value, nullptr, 10));
        }
        count = (std::min)(count, 8U);
        for (u32 i = 0; i < count; ++i) {
            workers.emplace_back([this](std::stop_token token) { Loop(token); });
        }
    }

    ~SwizzlePool() {
        for (auto& worker : workers) {
            worker.request_stop();
        }
        cv.notify_all();
    }

    static void Work(Job& job) {
        for (u32 i = job.next.fetch_add(1, std::memory_order_relaxed); i < job.chunks;
             i = job.next.fetch_add(1, std::memory_order_relaxed)) {
            (*job.func)(i);
            if (job.done.fetch_add(1, std::memory_order_acq_rel) + 1 == job.chunks) {
                job.done.notify_all();
            }
        }
    }

    void Loop(std::stop_token token) {
        Common::SetCurrentThreadName("Unswizzle");
        u64 seen = 0;
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, token, [&] { return generation != seen; });
                if (token.stop_requested()) {
                    return;
                }
                seen = generation;
                job = current;
            }
            if (job) {
                Work(*job);
            }
        }
    }

    std::mutex busy;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::shared_ptr<Job> current;
    u64 generation{};
    std::vector<std::jthread> workers;
};

/// Splits [0, lines) into chunks of whole GOB rows for SwizzlePool::Run; small images stay serial.
template <typename Func>
void ForEachLineRange(u32 lines, size_t bytes, u32 rows_per_unit, const Func& func) {
    constexpr size_t MinParallelBytes = 256 * 1024;
    constexpr u32 TargetChunks = 16;
    if (bytes < MinParallelBytes || lines <= rows_per_unit) {
        func(0, lines);
        return;
    }
    const u32 units = Common::DivCeil(lines, rows_per_unit);
    const u32 units_per_chunk = (std::max)(1U, Common::DivCeil(units, TargetChunks));
    const u32 chunk_lines = units_per_chunk * rows_per_unit;
    const u32 chunks = Common::DivCeil(lines, chunk_lines);
    const std::function<void(u32)> run = [&](u32 chunk) {
        const u32 begin = chunk * chunk_lines;
        func(begin, (std::min)(lines, begin + chunk_lines));
    };
    SwizzlePool::Get().Run(chunks, run);
}

constexpr u32 NativeGobOffset(u32 offset, bool native_tiling) {
    if (!native_tiling) return offset;
    const u32 difference = ((offset >> 5) ^ (offset >> 6)) & 1;
    return offset ^ (difference << 5) ^ (difference << 6);
}
template <u32 mask>
constexpr u32 pdep(u32 value) {
    u32 result = 0;
    u32 m = mask;
    for (u32 bit = 1; m; bit += bit) {
        if (value & bit)
            result |= m & (~m + 1);
        m &= m - 1;
    }
    return result;
}

template <u32 mask, u32 incr_amount>
void incrpdep(u32& value) {
    static constexpr u32 swizzled_incr = pdep<mask>(incr_amount);
    value = ((value | ~mask) + swizzled_incr) & mask;
}

// EDEN_NX2_SWIZZLE_LINES=1: the line-by-line path for every format (for comparisons).
bool swizzle_line_order = std::getenv("EDEN_NX2_SWIZZLE_LINES") != nullptr;

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void SwizzleImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height, u32 depth,
                 u32 block_height, u32 block_depth, u32 stride, bool native_tiling) {
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_x = 0;
    static constexpr u32 origin_y = 0;
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u32 pitch = width * BYTES_PER_PIXEL;

    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;

    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    // Lines of all slices, flattened: (slice, line) = (index / height, index % height). Chunks are
    // whole GOB rows of one block height (8 << block_height lines) and never share output bytes.
    const size_t total_bytes = size_t{pitch} * height * depth;
    if constexpr (!TO_LINEAR && BYTES_PER_PIXEL == 16) {
        // Streamed textures land here, and walking them line by line came back to each 512-byte
        // GOB eight times with the address worked out per pixel: 36% of the GPU back thread's
        // time (with its helpers) while moving in Ocarina of Time. One GOB at a time reads it in
        // order, from a table of where its 32 16-byte pieces go.
        if (depth == 1 && !swizzle_line_order) {
            struct Piece {
                u16 source;
                u8 x;
                u8 y;
            };
            std::array<Piece, 32> pieces{};
            for (u32 y = 0; y < GOB_SIZE_Y; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const u32 offset = pdep<SWIZZLE_X_BITS>(x * 16) | pdep<SWIZZLE_Y_BITS>(y);
                    pieces[y * 4 + x] = {static_cast<u16>(NativeGobOffset(offset, native_tiling)),
                                         static_cast<u8>(x), static_cast<u8>(y)};
                }
            }
            ForEachLineRange(height, total_bytes, GOB_SIZE_Y << block_height,
                             [&](u32 first, u32 last) {
                for (u32 y0 = first; y0 < last; y0 += GOB_SIZE_Y) {
                    const u32 block_y = y0 >> GOB_SIZE_Y_SHIFT;
                    const u32 offset_y = (block_y >> block_height) * block_size +
                                         ((block_y & block_height_mask) << GOB_SIZE_SHIFT);
                    const bool full_rows = y0 + GOB_SIZE_Y <= height;
                    for (u32 gob_x = 0; gob_x * 4 < width; ++gob_x) {
                        const u8* const gob = input.data() + offset_y + (gob_x << x_shift);
                        u8* const row = output.data() + size_t{y0} * pitch + size_t{gob_x} * 64;
                        const bool full_columns = gob_x * 4 + 4 <= width;
                        for (const Piece& piece : pieces) {
                            if ((full_rows && full_columns) ||
                                (y0 + piece.y < height && gob_x * 4 + piece.x < width)) {
                                std::memcpy(row + size_t{piece.y} * pitch + size_t{piece.x} * 16,
                                            gob + piece.source, 16);
                            }
                        }
                    }
                }
            });
            return;
        }
    }
    ForEachLineRange(height * depth, total_bytes, GOB_SIZE_Y << block_height,
                     [&](u32 first, u32 last) {
    for (u32 index = first; index < last; ++index) {
        const u32 slice = index / height;
        const u32 line = index % height;
        const u32 z = slice + origin_z;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<SWIZZLE_Y_BITS>(y);

            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 swizzled_x = pdep<SWIZZLE_X_BITS>(origin_x * BYTES_PER_PIXEL);
            for (u32 column = 0; column < width;
                 ++column, incrpdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 swizzled_offset = NativeGobOffset(base_swizzled_offset + (swizzled_x | swizzled_y), native_tiling);

                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
    }
    });
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void SwizzleSubrectImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height,
                        u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 num_lines,
                        u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u32 pitch = pitch_linear;
    const u32 stride = Common::AlignUpLog2(width * BYTES_PER_PIXEL, GOB_SIZE_X_SHIFT);

    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;

    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    u32 unprocessed_lines = num_lines;
    u32 extent_y = (std::min)(num_lines, height - origin_y);

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 z = slice + origin_z;
        const u32 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        const u32 lines_in_y = (std::min)(unprocessed_lines, extent_y);
        for (u32 line = 0; line < lines_in_y; ++line) {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<SWIZZLE_Y_BITS>(y);

            const u32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 swizzled_x = pdep<SWIZZLE_X_BITS>(origin_x * BYTES_PER_PIXEL);
            for (u32 column = 0; column < extent_x;
                 ++column, incrpdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                const u32 x = (column + origin_x) * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 swizzled_offset = NativeGobOffset(base_swizzled_offset + (swizzled_x | swizzled_y), native_tiling);

                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
        unprocessed_lines -= lines_in_y;
        if (unprocessed_lines == 0) {
            return;
        }
    }
}

template <bool TO_LINEAR>
void Swizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
             u32 height, u32 depth, u32 block_height, u32 block_depth, u32 stride_alignment, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleImpl<TO_LINEAR, x>(output, input, width, height, depth, block_height,        \
                                         block_depth, stride_alignment, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

} // Anonymous namespace

void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment, bool native_tiling) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    width = (width * bytes_per_pixel) >> new_bpp;
    bytes_per_pixel = 1U << new_bpp;
    Swizzle<false>(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                   stride, native_tiling);
}

void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment, bool native_tiling) {
    const u32 stride = Common::AlignUpLog2(width, stride_alignment) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(width * bytes_per_pixel)));
    width = (width * bytes_per_pixel) >> new_bpp;
    bytes_per_pixel = 1U << new_bpp;
    Swizzle<true>(output, input, bytes_per_pixel, width, height, depth, block_height, block_depth,
                  stride, native_tiling);
}

void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<true, x>(output, input, width, height, depth, origin_x,          \
                                           origin_y, extent_x, extent_y, block_height,             \
                                           block_depth, pitch_linear, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<false, x>(output, input, width, height, depth, origin_x,         \
                                            origin_y, extent_x, extent_y, block_height,            \
                                            block_depth, pitch_linear, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth) {
    if (tiled) {
        const u32 aligned_width = Common::AlignUpLog2(width * bytes_per_pixel, GOB_SIZE_X_SHIFT);
        const u32 aligned_height = Common::AlignUpLog2(height, GOB_SIZE_Y_SHIFT + block_height);
        const u32 aligned_depth = Common::AlignUpLog2(depth, GOB_SIZE_Z_SHIFT + block_depth);
        return aligned_width * aligned_height * aligned_depth;
    } else {
        return width * height * depth * bytes_per_pixel;
    }
}

u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel) {
    auto div_ceil = [](const u32 x, const u32 y) { return ((x + y - 1) / y); };
    const u32 gobs_in_block = 1 << block_height;
    const u32 y_blocks = GOB_SIZE_Y << block_height;
    const u32 x_per_gob = GOB_SIZE_X / bytes_per_pixel;
    const u32 x_blocks = div_ceil(width, x_per_gob);
    const u32 block_size = GOB_SIZE * gobs_in_block;
    const u32 stride = block_size * x_blocks;
    const u32 base = (dst_y / y_blocks) * stride + (dst_x / x_per_gob) * block_size;
    const u32 relative_y = dst_y % y_blocks;
    return base + (relative_y / GOB_SIZE_Y) * GOB_SIZE;
}

} // namespace Tegra::Texture
