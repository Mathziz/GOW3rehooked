// SPDX-License-Identifier: GPL-2.0-or-later
// AOT strategy: before the game runs, translates into aot.cache the variants the player's earlier runs recorded as recipes
// (shader_recipes.h), then every program of the game's data (shader_catalog.h, shader_contract.cpp) in the order of the parse,
// once each, with the driver's binary of every shader for the GPU in use. A draw then finds its shader ready; what is not
// there is translated by the JIT and becomes a recipe for the next run's build. Renderer::Impl::AotBuild is the build.
#pragma once
#include <atomic>
#include <filesystem>
#include "gpu/shaders/shader_cache.h"

namespace Gpu::Aot {
inline std::filesystem::path game_directory;  // set at startup: where the WADs are
inline bool lazy = false;                   // fixed quarter of all WAD variants per eligible launch; EBOOT and recipes in full
inline unsigned sample_log2 = 1;
enum class Phase { Waiting, Index, Recipes, Catalog, Variants, Finalizing, Finished };
inline std::atomic<Phase> phase{Phase::Waiting};
inline ShaderCache::Progress phase_start[static_cast<unsigned>(Phase::Finished)]{};
inline void SetPhase(Phase next) {
    if (next != Phase::Finished) phase_start[static_cast<unsigned>(next)] = ShaderCache::Instance().Snapshot();
    phase.store(next, std::memory_order_release);
}
} // namespace Gpu::Aot
