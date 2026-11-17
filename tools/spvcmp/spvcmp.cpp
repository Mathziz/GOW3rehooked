// Direct SPIR-V emitter checks (the GLSL path it replaced, and the comparison against it, are gone):
//   spvcmp list                               one line per recipe (index, stage, program hash, slots)
//   spvcmp fail                               the recipes the emitter cannot translate, and why
//   spvcmp one <recipe>                       the emitter on one recipe
//   spvcmp words <recipe> <first pc> <count>  raw dwords of a recipe's program
//   spvcmp eboot [dfmt nfmt]                  the EBOOT's embedded compute programs: words, emission with made-up float V# formats, validation
#include <spirv-tools/libspirv.h>
#include <spirv-tools/optimizer.hpp>
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "gpu/shaders/gcn_internal.h"
#include "gpu/shaders/gcn_translator.h"
#include "gpu/shaders/shader_catalog.h"
#include "gpu/shaders/shader_parser.h"
#include "gpu/shaders/shader_recipes.h"
#include <format>

namespace fs = std::filesystem;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

static const fs::path kGame = [] {  // the game folder, from GOW3_GAME_DIR
    char* dir = nullptr;
    std::size_t size = 0;
    fs::path game = _dupenv_s(&dir, &size, "GOW3_GAME_DIR") == 0 && dir ? fs::path(dir) : fs::path();
    std::free(dir);
    return game;
}();
static const fs::path kCache = kGame / "_rehooked_/shader_cache";

namespace Gpu {
// Same layout as shader_recipes.cpp (the tool does not link it: its Key() needs the renderer).
Gcn::Input Recipe::Input() const {
    Gcn::Input in;
    in.stage = Gcn::Stage(h.stage), in.rsrc1 = h.rsrc1, in.rsrc2 = h.rsrc2, in.rect = h.rect;
    in.ps_input_ena = h.ps_input_ena, in.ps_input_addr = h.ps_input_addr, in.col_format = h.col_format;
    std::memcpy(in.ps_input_cntl, h.cntl, sizeof(h.cntl)), std::memcpy(in.threads, h.threads, sizeof(h.threads));
    std::memcpy(in.mrt_class, h.mrt_class, 8), std::memcpy(in.mrt_map, h.mrt_map, 8);
    return in;
}
std::vector<Gcn::SlotSpec> Recipe::Specs() const {
    std::vector<Gcn::SlotSpec> s;
    for (std::uint64_t w : spec) s.push_back(Gcn::Unpack(w));
    return s;
}
} // namespace Gpu

static std::vector<Gpu::Recipe> LoadRecipes() {
    std::vector<Gpu::Recipe> out;
    std::ifstream f(kCache / (Gpu::PieceHash::UsesAes() ? "recipes-hash2-aes.bin" : "recipes-hash2-xxh.bin"), std::ios::binary);
    std::uint32_t head[2];
    f.read((char*)head, 8);
    struct Rec { u32 type, size; } r;
    std::vector<std::uint8_t> body;
    std::unordered_map<u64, bool> seen;
    while (f.read((char*)&r, sizeof r)) {
        body.resize(r.size);
        if (!f.read((char*)body.data(), r.size)) break;
        if ((r.type != 1 && r.type != 3) || r.size < sizeof(Gpu::RecipeHead)) continue;
        Gpu::Recipe rc;
        std::memcpy(&rc.h, body.data(), sizeof(Gpu::RecipeHead));
        const size_t spec_end = sizeof(Gpu::RecipeHead) + rc.h.nspec * 8ull;
        rc.spec.resize(rc.h.nspec), std::memcpy(rc.spec.data(), body.data() + sizeof(Gpu::RecipeHead), rc.h.nspec * 8ull);
        rc.fetch.resize((r.size - spec_end) / 4), std::memcpy(rc.fetch.data(), body.data() + spec_end, r.size - spec_end);
        if (r.type == 1 && rc.h.fetch_hash) continue;
        out.push_back(std::move(rc));
    }
    return out;
}

struct Game {
    Gpu::Parser::Index index;
    std::vector<std::unique_ptr<Gpu::Parser::Mapping>> maps;
    std::unordered_multimap<u64, u32> by_code;
    Game() {
        index = Gpu::Parser::Load(kGame, kCache / (Gpu::PieceHash::UsesAes() ? "wad-hash2-aes.index" : "wad-hash2-xxh.index"), [](u32) {}, [] {});
        maps.resize(index.files.size());
        for (u32 k = 0; k < index.programs.size(); ++k) by_code.emplace(index.programs[k].code_hash, k);
    }
    // The recipe's code and fetch words; false when the game's files do not hold it.
    bool Code(const Gpu::Recipe& r, std::vector<u32>& code, std::vector<u32>& fetch) {
        for (auto [it, end] = by_code.equal_range(r.h.code_hash); it != end; ++it) {
            const Gpu::Parser::Program& p = index.programs[it->second];
            if (r.fetch.empty() && p.fetch_hash != r.h.fetch_hash) continue;
            auto& m = maps[p.file];
            if (!m) {
                m = std::make_unique<Gpu::Parser::Mapping>();
                if (!m->Open(kGame / index.files[p.file])) { m.reset(); continue; }
            }
            code.resize(p.size / 4), std::memcpy(code.data(), m->data + p.start, code.size() * 4);
            if (!r.fetch.empty()) fetch = r.fetch;
            else fetch.resize(p.fetch_size / 4), std::memcpy(fetch.data(), m->data + p.fetch_start, fetch.size() * 4);
            return true;
        }
        return false;
    }
};

static std::string Disassemble(const std::vector<u32>& w) {
    spv_context ctx = spvContextCreate(SPV_ENV_VULKAN_1_3);
    spv_text text = nullptr;
    spv_diagnostic diag = nullptr;
    std::string s;
    if (spvBinaryToText(ctx, w.data(), w.size(), SPV_BINARY_TO_TEXT_OPTION_INDENT | SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES, &text, &diag) == SPV_SUCCESS)
        s.assign(text->str, text->length);
    spvTextDestroy(text), spvDiagnosticDestroy(diag), spvContextDestroy(ctx);
    return s;
}

static bool Validate(const std::vector<u32>& w, std::string& msg) {
    spvtools::SpirvTools t(SPV_ENV_VULKAN_1_3);
    t.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos, const char* m) {
        if (msg.empty()) msg = std::string(m) + " (word " + std::to_string(pos.index) + ")";
    });
    spvtools::ValidatorOptions o;
    o.SetScalarBlockLayout(true);  // the draw tables are scalar-layout uniform blocks
    return t.Validate(w.data(), w.size(), o);
}
int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "spvcmp list | fail | one <recipe> | words <recipe> <pc> <count> | eboot [dfmt nfmt]\n"), 1;
    if (kGame.empty()) return std::fprintf(stderr, "Set GOW3_GAME_DIR to the game folder\n"), 1;
    const std::string mode = argv[1];
    auto recipes = LoadRecipes();
    std::printf("%zu recipes\n", recipes.size());
    if (mode == "eboot") {  // the embedded compute programs' words: out/cs<k>.txt
        const auto data = Gpu::Catalog::Read(kGame, 12);
        for (size_t k = 0, n = 0; k < data.embedded.size(); ++k) {
            const auto& s = data.embedded[k];
            if (s.stage != 4) continue;
            std::ofstream f("cs" + std::to_string(n++) + ".txt");
            f << "table " << std::hex << s.table << " group " << s.group << " identity " << s.identity << "\n";
            std::printf("cs%zu table %x group %u hash %016llx\n", n - 1, s.table, s.group, (unsigned long long)XXH3_64bits(data.eboot->data + s.code, s.footer - s.code));
            for (size_t p = s.code, e = s.footer, w = 0; p + 4 <= e; p += 4, ++w) f << std::format("{:05x}: {:08x}\n", w, data.eboot->Read<u32>(p));
            // Emission with made-up specs: every format V# is a float32 element (dfmt 4, nfmt 7; argv[2]: dfmt, argv[3]: nfmt).
            std::vector<u32> code(s.footer - s.code >> 2);
            std::memcpy(code.data(), data.eboot->data + s.code, code.size() * 4);
            Gcn::Input in;
            in.stage = Gcn::Stage::Compute, in.rsrc2 = 12 << 1, in.threads[0] = 64, in.threads[1] = in.threads[2] = 1, in.code = code;
            const Gcn::Analysis an = Gcn::Analyze(in);
            std::vector<Gcn::SlotSpec> spec(an.slots.size() + an.formats.size());
            for (size_t j = an.slots.size(); j < spec.size(); ++j) spec[j].vfmt = true, spec[j].dfmt = argc > 2 ? std::atoi(argv[2]) : 4, spec[j].nfmt = argc > 3 ? std::atoi(argv[3]) : 7, spec[j].dst_sel = 0xFAC;
            std::vector<u32> words;
            std::uint64_t mask;
            std::string err, msg;
            const bool ok = Gcn::EmitSpirv(an, in, spec, words, mask, err);
            std::printf("  emit %s %s (%zu words, stored %llx)", ok ? "ok" : "failed", err.c_str(), words.size(), (unsigned long long)an.stored);
            std::printf(" validation: %s\n", !ok ? "-" : Validate(words, msg) ? "valid" : msg.c_str());
            if (ok) std::ofstream("cs" + std::to_string(n - 1) + ".spvasm") << Disassemble(words);
        }
        return 0;
    }
    if (mode == "list") {
        for (size_t k = 0; k < recipes.size(); ++k) {
            const auto& h = recipes[k].h;
            std::printf("%zu stage %u code %016llx fetch %016llx specs %u rect %u\n", k, h.stage, (unsigned long long)h.code_hash, (unsigned long long)h.fetch_hash, h.nspec, h.rect);
        }
        return 0;
    }
    Game game;
    if (mode == "msaa") {
        size_t tested = 0, matched = 0, changed = 0, bad = 0, missing = 0;
        fs::create_directories("build-fast/msaa-validation");
        for (size_t k = 0; k < recipes.size(); ++k) {
            auto spec = recipes[k].Specs();
            if (recipes[k].h.stage != 1 || std::none_of(spec.begin(), spec.end(), [](const auto& s) { return s.dim == Gcn::Dim::D2Ms; })) continue;
            std::vector<u32> code, fetch, original, four;
            if (!game.Code(recipes[k], code, fetch)) { ++missing; continue; }
            auto in = recipes[k].Input();
            in.code = code, in.fetch = fetch;
            const auto analysis = Gcn::Analyze(in);
            if (!analysis.program) { ++bad; continue; }
            const auto pattern = Gcn::FindDepthResolve(*analysis.program);
            std::string error, validation;
            u64 out_mask{};
            for (auto& s : spec) s.msaa4 = false;
            if (!Gcn::EmitSpirv(analysis, in, spec, original, out_mask, error) || !Validate(original, validation)) {
                std::printf("recipe %zu original: %s %s\n", k, error.c_str(), validation.c_str()); ++bad; continue;
            }
            for (auto& s : spec) if (s.dim == Gcn::Dim::D2Ms) s.msaa4 = true;
            error.clear(), validation.clear();
            if (!Gcn::EmitSpirv(analysis, in, spec, four, out_mask, error) || !Validate(four, validation)) {
                std::printf("recipe %zu four: %s %s\n", k, error.c_str(), validation.c_str()); ++bad; continue;
            }
            const bool eligible = pattern.slot >= 0 && size_t(pattern.slot) < spec.size() && spec[pattern.slot].dim == Gcn::Dim::D2Ms &&
                                  spec[pattern.slot].cls == Gcn::Class::Float && spec[pattern.slot].msaa4;
            ++tested;
            if (eligible) {
                ++matched;
                const auto prefix = "build-fast/msaa-validation/recipe-" + std::to_string(k);
                std::ofstream(prefix + "-2x.spvasm") << Disassemble(original);
                std::ofstream(prefix + "-4x.spvasm") << Disassemble(four);
                std::ofstream binary(prefix + "-4x.spv", std::ios::binary);
                binary.write(reinterpret_cast<const char*>(four.data()), std::streamsize(four.size() * 4));
            }
            if (original != four) ++changed;
            if ((original != four) != eligible) { std::printf("recipe %zu unexpected difference\n", k); ++bad; }
        }
        std::printf("MSAA recipes: tested=%zu matched=%zu changed=%zu missing=%zu errors=%zu\n", tested, matched, changed, missing, bad);
        return bad ? 1 : 0;
    }
    if (mode == "fail") {  // the direct emitter alone (fast): the recipes it cannot translate
        for (size_t k = 0; k < recipes.size(); ++k) {
            std::vector<u32> code, fetch, words;
            if (!game.Code(recipes[k], code, fetch)) continue;
            Gcn::Input in = recipes[k].Input();
            in.code = code, in.fetch = fetch;
            std::string err;
            std::uint64_t mask;
            if (!Gcn::EmitSpirv(Gcn::Analyze(in), in, recipes[k].Specs(), words, mask, err))
                std::printf("%zu: stage %u code %016llx fetch %016llx: %s\n", k, recipes[k].h.stage, (unsigned long long)recipes[k].h.code_hash, (unsigned long long)recipes[k].h.fetch_hash, err.c_str());
        }
        return 0;
    }
    if (mode == "one") {  // one <recipe>: the direct emitter on that recipe alone
        std::vector<u32> code, fetch, words;
        const size_t k = std::stoull(argv[2]);
        if (!game.Code(recipes[k], code, fetch)) return 1;
        Gcn::Input in = recipes[k].Input();
        in.code = code, in.fetch = fetch;
        std::string err;
        std::uint64_t mask;
        const bool ok = Gcn::EmitSpirv(Gcn::Analyze(in), in, recipes[k].Specs(), words, mask, err);
        std::printf("%zu: %s %s\n", k, ok ? "ok" : "failed", err.c_str());
        return 0;
    }
    if (mode == "words") {  // words <recipe> <first pc> <count>: the program's raw dwords
        std::vector<u32> code, fetch;
        const size_t k = std::stoull(argv[2]);
        if (!game.Code(recipes[k], code, fetch)) return 1;
        for (size_t p = std::stoull(argv[3], nullptr, 16), n = std::stoull(argv[4]); n-- && p < code.size(); ++p) std::printf("%05zx: %08x\n", p, code[p]);
        return 0;
    }
}
