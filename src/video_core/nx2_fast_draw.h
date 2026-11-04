// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>

#include "common/common_types.h"

/// EDEN_NX2_FAST_DRAW=1: a draw that follows a draw with the same bound state reuses that draw's
/// resolved bindings, recording only the draw (and the index buffer for indexed draws). Anything
/// that could change what the resolve produces between two draws bumps a serial: a 3D register
/// that changes (other than draw parameters), any 3D method with side effects (constant buffer
/// data and binds, uploads, reports, clears, barriers, ...), any other engine, deferred calls on
/// the split's back (except the ones marked GpuSplit::FastDrawNeutral), GPU::TickWork work,
/// memory manager invalidations and fence releases. The rasterizer reuses a draw only while the
/// serial, the command buffer, the render pass and the bound pipeline are the ones the previous
/// draw left.
namespace VideoCommon::Nx2FastDraw {

enum class Reason : u8 {
    Register,
    Method,
    ConstantData,
    InlineData,
    OtherEngine,
    Call,
    TickWork,
    Invalidation,
    Fence,
    IndirectReset,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Reason::Count)> reason_names{
    "register",  "method",    "constant buffer data", "inline data", "other engine",
    "deferred call", "tick work", "invalidation", "fence release", "indirect reset",
};

inline bool g_enabled = [] {
    const char* const value = std::getenv("EDEN_NX2_FAST_DRAW");
    return value != nullptr && value[0] == '1';
}();

inline std::atomic<u64> g_serial{1};
/// The last break's reason and, for Register and Method, its 3D method (diagnostics).
inline std::atomic<u32> g_last_break{};

inline void Break(Reason reason, u32 method = 0) {
    if (g_enabled) {
        g_serial.fetch_add(1, std::memory_order_relaxed);
        g_last_break.store(static_cast<u32>(reason) | (method << 8), std::memory_order_relaxed);
    }
}

inline u64 Serial() {
    return g_serial.load(std::memory_order_relaxed);
}

} // namespace VideoCommon::Nx2FastDraw
