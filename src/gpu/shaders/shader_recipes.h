// SPDX-License-Identifier: GPL-2.0-or-later
// Recipes: the exact shader variants the game met while played, kept as the fields their translation key hashes (not the
// key, so they outlive key and cache format changes). They come from the player's runs and from the shaders the JIT had to
// translate during play. The AOT translates them first; the recipes file only grows and holds no game data, the code
// is found by its hash in the user's WADs.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include "gpu/shaders/gcn_translator.h"
#include "gpu/shaders/shader_runtime.h"

namespace Gpu {
struct RecipeHead {
    std::uint64_t code_hash, fetch_hash;  // hashes of the program and of the VS fetch shader (0: none)
    std::uint32_t rsrc1, rsrc2, ps_input_ena, ps_input_addr, col_format, threads[3], cntl[32];
    std::uint8_t mrt_class[8], mrt_map[8];
    std::uint16_t nspec;  // slots' Pack words follow
    std::uint8_t stage, rect;
    std::uint32_t reserved = 0;
};
struct Recipe {
    RecipeHead h;
    std::vector<std::uint64_t> spec;
    std::vector<std::uint32_t> fetch;  // the VS fetch shader the engine generated at run time (not in the program's file); not in the key
    Gcn::Input Input() const;  // everything of Gcn::Input but the code
    std::vector<Gcn::SlotSpec> Specs() const;
    std::uint64_t Key() const;  // the translation key the JIT computes for this variant
    // The recipe of `in` (its code and fetch hashes given) with `spec`.
    static Recipe From(const Gcn::Input& in, std::uint64_t code_hash, std::uint64_t fetch_hash, std::span<const Gcn::SlotSpec> spec);
};

class Recipes {
public:
    static Recipes& Instance();
    // Loads recipes from `dir` (created if missing); until then nothing is recorded.
    void Configure(const std::filesystem::path& dir);
    // A draw needs the translation `key` of `in` (code and fetch set) with `spec`: kept once, written at the next flush. `cached`: the
    // shader cache already holds it (a new recipe that is cached was built by the AOT's contract: its hit rate).
    void Note(std::uint64_t key, const Gcn::Input& in, std::span<const Gcn::SlotSpec> spec, bool cached);
    void Report() const;  // run summary on stderr: new recipes and how many of them the AOT had already built
    void Flush();         // writes the recipes noted since the last flush (the ShaderCache's flush thread and the exit call it)
    std::vector<Recipe> All() const;

private:
    void Append(std::uint32_t type, const void* a, std::size_t na, const void* b = nullptr, std::size_t nb = 0);
    bool enabled = false;
    std::uint32_t fresh = 0, fresh_cached = 0;
    mutable std::mutex lock;
    FILE* file = nullptr;
    std::vector<std::uint8_t> staged;  // records not written yet
    std::vector<Recipe> recipes;
    std::unordered_set<std::uint64_t> keys;
};

struct PipelineRecipe {
    std::uint64_t vs_key = 0;
    std::uint64_t ps_key = 0;
    std::uint64_t gs_mask = 0;
    FixedPipelineState raw_state{};  // un-canonicalized, portable across GPUs with different EDS3 caps
    std::uint64_t RawHash() const { return GraphicsPipelineKey{vs_key, ps_key, gs_mask, raw_state}.Hash(); }
};
static_assert(std::is_trivially_copyable_v<PipelineRecipe>);
static_assert(sizeof(PipelineRecipe) == 136);

class PipelineRecipes {
public:
    static PipelineRecipes& Instance();
    void Configure(const std::filesystem::path& dir);
    void Note(std::uint64_t vs_key, std::uint64_t ps_key, std::uint64_t gs_mask, const FixedPipelineState& raw_state);
    void Report() const;
    void Flush();
    std::vector<PipelineRecipe> All() const;

private:
    bool enabled = false;
    std::uint32_t fresh = 0;
    mutable std::mutex lock;
    FILE* file = nullptr;
    std::vector<std::uint8_t> staged;
    std::vector<PipelineRecipe> recipes;
    std::unordered_set<std::uint64_t> keys;
};
} // namespace Gpu
