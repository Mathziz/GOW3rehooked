// SPDX-License-Identifier: GPL-2.0-or-later
// JIT strategy: shaders translated on demand, in-game (Discover, Async, Compile; members of Gpu::Renderer::Impl).
#pragma once
#include <atomic>

namespace Gpu::Jit {
// What a draw does when its shader is not compiled yet. Sync: waits for the compile; Async: skips the draw until the
// workers finish it.
enum class Mode { Sync, Async };
inline std::atomic<Mode> mode{Mode::Sync};  // set at startup from the settings
} // namespace Gpu::Jit
