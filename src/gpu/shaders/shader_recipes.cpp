// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/piece_hash.h"
#include "gpu/shaders/shader_recipes.h"
#include <algorithm>
#include <cstring>
#include <xxhash.h>
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_runtime.h"

namespace Gpu {
namespace {
constexpr std::uint32_t kMagic = 0x50523347, kFormat = 1;  // "G3RP"; kFormat: the layout of the records below
// kRecipeFetch: a recipe followed by its fetch shader words. (2 was the WAD open order: skipped if a file still has such records.)
enum Type : std::uint32_t { kRecipe = 1, kRecipeFetch = 3 };
struct Record {
    std::uint32_t type, size;
};
static_assert(sizeof(RecipeHead) == 200);
} // namespace

Gcn::Input Recipe::Input() const {
    Gcn::Input in;
    in.stage = Gcn::Stage(h.stage), in.rsrc1 = h.rsrc1, in.rsrc2 = h.rsrc2, in.rect = h.rect;
    in.blend_interlock = h.reserved;
    in.ps_input_ena = h.ps_input_ena, in.ps_input_addr = h.ps_input_addr, in.col_format = h.col_format;
    std::memcpy(in.ps_input_cntl, h.cntl, sizeof(h.cntl)), std::memcpy(in.threads, h.threads, sizeof(h.threads));
    std::memcpy(in.mrt_class, h.mrt_class, 8), std::memcpy(in.mrt_map, h.mrt_map, 8);
    return in;
}

std::vector<Gcn::SlotSpec> Recipe::Specs() const {
    std::vector<Gcn::SlotSpec> s;
    s.reserve(spec.size());
    for (std::uint64_t w : spec) s.push_back(Gcn::Unpack(w));
    return s;
}

std::uint64_t Recipe::Key() const {
    const auto specs = Specs();
    return TranslationKey(ProgramKey(Gcn::Stage(h.stage), h.code_hash, h.fetch_hash, h.rsrc2, false, false), Input(), specs);
}

Recipe Recipe::From(const Gcn::Input& in, std::uint64_t code_hash, std::uint64_t fetch_hash, std::span<const Gcn::SlotSpec> spec) {
    Recipe r;
    RecipeHead& h = r.h;
    h = {};
    h.code_hash = code_hash, h.fetch_hash = fetch_hash;
    h.rsrc1 = in.rsrc1, h.rsrc2 = in.rsrc2, h.ps_input_ena = in.ps_input_ena, h.ps_input_addr = in.ps_input_addr, h.col_format = in.col_format;
    std::memcpy(h.cntl, in.ps_input_cntl, sizeof(h.cntl)), std::memcpy(h.threads, in.threads, sizeof(h.threads));
    std::memcpy(h.mrt_class, in.mrt_class, 8), std::memcpy(h.mrt_map, in.mrt_map, 8);
    h.nspec = std::uint16_t(spec.size()), h.stage = std::uint8_t(in.stage), h.rect = in.rect;
    h.reserved = in.blend_interlock;
    for (const Gcn::SlotSpec& s : spec) r.spec.push_back(Gcn::Pack(s));
    return r;
}

Recipes& Recipes::Instance() {
    static Recipes r;
    return r;
}

void Recipes::Configure(const std::filesystem::path& dir) {
    std::scoped_lock l(lock);
    std::filesystem::create_directories(dir);
    const auto path = dir / (PieceHash::UsesAes() ? "recipes-hash2-aes.bin" : "recipes-hash2-xxh.bin");
    std::uintmax_t good = 8;  // end of the last whole record
    bool valid = false;
#ifdef _WIN32
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
#else
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
#endif
        std::uint32_t head[2];
        valid = std::fread(head, 4, 2, f) == 2 && head[0] == kMagic && head[1] == kFormat;
        std::vector<std::uint8_t> body;
        for (Record r; valid && std::fread(&r, sizeof(r), 1, f) == 1 && r.size < (1u << 20); good += sizeof(r) + r.size) {
            body.resize(r.size);
            if (std::fread(body.data(), 1, r.size, f) != r.size) break;
            if ((r.type == kRecipe || r.type == kRecipeFetch) && r.size >= sizeof(RecipeHead)) {
                Recipe rc;
                std::memcpy(&rc.h, body.data(), sizeof(RecipeHead));
                const std::size_t spec_end = sizeof(RecipeHead) + rc.h.nspec * 8ull;
                if (r.size < spec_end || (r.type == kRecipe ? r.size != spec_end : (r.size - spec_end) % 4)) break;
                rc.spec.resize(rc.h.nspec), std::memcpy(rc.spec.data(), body.data() + sizeof(RecipeHead), rc.h.nspec * 8ull);
                rc.fetch.resize((r.size - spec_end) / 4), std::memcpy(rc.fetch.data(), body.data() + spec_end, r.size - spec_end);
                // Without the fetch shader's words a recipe of the engine's own fetch shaders cannot be built: noted again.
                if (r.type == kRecipe && rc.h.fetch_hash) continue;
                if (keys.insert(rc.Key()).second) recipes.push_back(std::move(rc));
            }
        }
        std::fclose(f);
    }
    if (valid) {
        std::error_code ec;  // a killed run left a torn tail: cut it
        std::filesystem::resize_file(path, good, ec);
    }
#ifdef _WIN32
    file = _wfopen(path.c_str(), valid ? L"ab" : L"wb");
#else
    file = std::fopen(path.c_str(), valid ? "ab" : "wb");
#endif
    if (!file) return;
    if (!valid) {
        const std::uint32_t head[2]{kMagic, kFormat};
        std::fwrite(head, 4, 2, file);
        std::fflush(file);
    }
    enabled = true;
}

// Staged in memory: the draw that met the variant does no I/O. Flush writes them (loading screens, exit).
void Recipes::Append(std::uint32_t type, const void* a, std::size_t na, const void* b, std::size_t nb) {
    const Record r{type, std::uint32_t(na + nb)};
    const auto bytes = [&](const void* p, std::size_t n) { staged.insert(staged.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n); };
    bytes(&r, sizeof(r)), bytes(a, na);
    if (nb) bytes(b, nb);
}

void Recipes::Flush() {
    std::scoped_lock l(lock);
    if (staged.empty() || !file) return;
    std::fwrite(staged.data(), 1, staged.size(), file), std::fflush(file);
    staged.clear();
}

void Recipes::Report() const {
    std::scoped_lock l(lock);
    if (enabled) std::fprintf(stderr, "Recipes: %zu kept, %u new this run, %u of those already built by the AOT\n", recipes.size(), fresh, fresh_cached);
}

void Recipes::Note(std::uint64_t key, const Gcn::Input& in, std::span<const Gcn::SlotSpec> spec, bool cached) {
    if (!enabled) return;
    std::scoped_lock l(lock);
    if (!keys.insert(key).second) return;
    ++fresh, fresh_cached += cached;
    Recipe r = Recipe::From(in, ::Gpu::PieceHash::ShaderCode(in.code.data(), in.code.size_bytes()), in.fetch.empty() ? 0 : ::Gpu::PieceHash::ShaderCode(in.fetch.data(), in.fetch.size_bytes()), spec);
    RecipeHead& h = r.h;
    if (r.Key() != key) std::fprintf(stderr, "Recipe %016llx does not rebuild its key (%016llx)\n", (unsigned long long)key, (unsigned long long)r.Key());
    r.fetch.assign(in.fetch.begin(), in.fetch.end());
    std::vector<std::uint8_t> tail(r.spec.size() * 8 + r.fetch.size() * 4);
    std::memcpy(tail.data(), r.spec.data(), r.spec.size() * 8), std::memcpy(tail.data() + r.spec.size() * 8, r.fetch.data(), r.fetch.size() * 4);
    Append(r.fetch.empty() ? kRecipe : kRecipeFetch, &h, sizeof(h), tail.data(), tail.size());
    recipes.push_back(std::move(r));
    ShaderCache::Instance().Dirty();
}

std::vector<Recipe> Recipes::All() const {
    std::scoped_lock l(lock);
    return recipes;
}

namespace {
constexpr std::uint32_t kPlMagic = 0x52503347, kPlFormat = 1;  // "G3PR"
} // namespace

PipelineRecipes& PipelineRecipes::Instance() {
    static PipelineRecipes r;
    return r;
}

void PipelineRecipes::Configure(const std::filesystem::path& dir) {
    std::scoped_lock l(lock);
    if (enabled) return;
    std::filesystem::create_directories(dir);
    const auto path = dir / (PieceHash::UsesAes() ? "pipeline-recipes-hash2-aes.bin" : "pipeline-recipes-hash2-xxh.bin");
    std::uintmax_t good = 8;
    bool valid = false;
#ifdef _WIN32
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
#else
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
#endif
        std::uint32_t head[2];
        valid = std::fread(head, 4, 2, f) == 2 && head[0] == kPlMagic && head[1] == kPlFormat;
        for (PipelineRecipe r{}; valid && std::fread(&r, sizeof(r), 1, f) == 1; good += sizeof(r)) {
            if (keys.insert(r.RawHash()).second) recipes.push_back(r);
        }
        std::fclose(f);
    }
    if (valid) {
        std::error_code ec;
        std::filesystem::resize_file(path, good, ec);
    }
#ifdef _WIN32
    file = _wfopen(path.c_str(), valid ? L"ab" : L"wb");
#else
    file = std::fopen(path.c_str(), valid ? "ab" : "wb");
#endif
    if (!file) return;
    if (!valid) {
        const std::uint32_t head[2]{kPlMagic, kPlFormat};
        std::fwrite(head, 4, 2, file);
        std::fflush(file);
    }
    enabled = true;
}

void PipelineRecipes::Note(std::uint64_t vs_key, std::uint64_t ps_key, std::uint64_t gs_mask, const FixedPipelineState& raw_state) {
    if (!vs_key) return;
    PipelineRecipe r{vs_key, ps_key, (raw_state.flags & FixedPipelineState::kHasGs) ? gs_mask : 0ull, raw_state};
    r.raw_state.pad[0] = r.raw_state.pad[1] = 0;
    for (u32 k = r.raw_state.color_count; k < 8; ++k)
        r.raw_state.color_formats[k] = VK_FORMAT_UNDEFINED, r.raw_state.blend[k] = {};
    const u64 h = r.RawHash();
    {
        std::scoped_lock l(lock);
        if (!enabled || !keys.insert(h).second) return;
        ++fresh;
        const auto* p = reinterpret_cast<const std::uint8_t*>(&r);
        staged.insert(staged.end(), p, p + sizeof(r));
        recipes.push_back(r);
    }
    ShaderCache::Instance().Dirty();
}

void PipelineRecipes::Flush() {
    std::scoped_lock l(lock);
    if (staged.empty() || !file) return;
    std::fwrite(staged.data(), 1, staged.size(), file);
    std::fflush(file);
    staged.clear();
}

void PipelineRecipes::Report() const {
    std::scoped_lock l(lock);
    if (enabled) std::fprintf(stderr, "PipelineRecipes: %zu kept, %u new this run\n", recipes.size(), fresh);
}

std::vector<PipelineRecipe> PipelineRecipes::All() const {
    std::scoped_lock l(lock);
    return recipes;
}
} // namespace Gpu
