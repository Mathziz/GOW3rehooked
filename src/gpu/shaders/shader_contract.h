// SPDX-License-Identifier: GPL-2.0-or-later
// A variant derived from the game's own data (shader_catalog.h) instead of met in play: the recipe the draw would produce,
// with where its code lies in the mapped game files.
#pragma once
#include <cstdint>
#include <span>
#include "gpu/shaders/shader_recipes.h"

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
} // namespace Gpu
