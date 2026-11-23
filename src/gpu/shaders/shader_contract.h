// SPDX-License-Identifier: GPL-2.0-or-later
// A variant derived from the game's own data (shader_catalog.h) instead of met in play: the recipe the draw would produce,
// with where its code lies in the mapped game files.
#pragma once
#include <cstdint>
#include <span>
#include <type_traits>
#include "gpu/shaders/shader_recipes.h"
#include "gpu/shaders/shader_runtime.h"

namespace Gpu {
// Where a configuration comes from; the AOT's log counts, per path, the variants made and the recipes they predicted.
enum ContractPath : std::uint8_t { kPsMaterial, kPsPass, kPsSrgb, kPsEmbedded, kVsMesh, kVsGlyph, kVsProcedural, kVsLayout, kVsEmbedded, kCsEmbedded, kContractPaths };
inline constexpr const char* kContractPathNames[kContractPaths]{"ps material", "ps pass", "ps pass srgb subsets", "ps embedded", "vs mesh", "vs glyph", "vs procedural",
                                                                "vs layouts", "vs embedded", "cs embedded"};
struct Contracted {
    Recipe recipe;
    std::uint64_t key;
    std::span<const std::uint8_t> code, fetch;  // in the catalog's mappings (fetch: empty without vertex semantics)
    std::uint32_t file;                         // Catalog::Data::files index, ~0u: the EBOOT
    std::uint32_t paths = 0;                    // bit per ContractPath that produced this variant
};

// Kept apart from the recipes and the sample count: what depends on them is resolved when the AOT builds.
struct ContractedPipeline {
    std::uint64_t vs_key = 0;
    std::uint64_t ps_key = 0;
    std::uint64_t gs_mask = 0;
    std::uint64_t gate = 0;      // code hash of the PS program the recipes must have met (0: always built)
    std::uint32_t file = ~0u;    // Catalog::Data::files index, ~0u: EBOOT
    std::uint32_t pad = 0;
    FixedPipelineState state{};  // samples 0: the scene's sample count
};
static_assert(std::is_trivially_copyable_v<ContractedPipeline>);
static_assert(sizeof(ContractedPipeline) == 152);
} // namespace Gpu
