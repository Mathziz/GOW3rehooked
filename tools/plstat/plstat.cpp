// SPDX-License-Identifier: GPL-2.0-or-later
// plstat: the graphics pipelines a run used (pipelines-*.bin, GOW3_TEMP_PIPELOG) against what the AOT derives
// (pipeline-contract-*.cache), the player's pipeline recipes (in a format 2 pipelines file; earlier builds:
// pipeline-recipes-*.bin) and the shader contract (contract-*.cache). Without a pipelines file the recipes stand for the run.
//   plstat [--dir <shader_cache>] [--caps <hex>] [--list N] [pipelines-*.bin ...]
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cctype>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace fs = std::filesystem;

// Mirrors of the renderer's records (shader_runtime.h, shader_recipes.h, shader_contract.h, renderer_impl.h).
struct Blend { u8 enable, mask, sc, dc, cop, sa, da, aop; bool operator==(const Blend&) const = default; };
struct State {
    u32 fmt[8], depth, stencil;
    u8 count, samples, topo, flags, sqc, sqa, pad[2];
    Blend blend[8];
    bool operator==(const State&) const = default;
};
static_assert(sizeof(State) == 112);
constexpr u8 kNegOne = 1, kClamp = 2, kA2c = 4, kHasPs = 8, kHasGs = 16;
struct UseState { u64 vs, ps, gs, canon; State raw; u32 first_frame, frames, last_frame, flags; u64 draws; };
static_assert(sizeof(UseState) == 168);
struct Made { u32 origin, us, made_frame, waits, wait_frame, pad; };
struct Contracted { u64 vs, ps, gs; u32 file, pad; State st; };
static_assert(sizeof(Contracted) == 144);
struct PlRecipe { u64 vs, ps, gs; State raw; };
static_assert(sizeof(PlRecipe) == 136);
constexpr u64 kClearRectVs = 0x474F5733434C5231ull;
constexpr const char* kOrigins[]{"unknown", "warm", "hit", "ahead-hit", "ahead", "miss", "gpl-lto", "gpl-link", "trace"};
// Caps bits (TempPlWrite): neg_one, clamp, samples, a2c, blend enable, blend equation, write mask, unrestricted topology.
enum : u32 { cNegOne = 1, cClamp = 2, cSamples = 4, cA2c = 8, cBlendEn = 16, cBlendEq = 32, cMask = 64, cTopo = 128 };

struct Key {
    u64 vs, ps, gs;
    State st;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        u64 w[17], h = 0x9E3779B97F4A7C15ull;
        std::memcpy(w, &k, sizeof(w));
        for (u64 x : w) h = (h ^ x) * 0xBF58476D1CE4E5B9ull, h ^= h >> 31;
        return size_t(h);
    }
};
static_assert(sizeof(Key) == 136);

State Canon(State s, u32 c) {
    if (c & cNegOne) s.flags &= ~kNegOne;
    if (c & cClamp) s.flags &= ~kClamp;
    if (c & cA2c) s.flags &= ~kA2c;
    if (c & cSamples) s.samples = 1;
    if ((c & cTopo) && !(s.flags & kHasGs)) s.topo = 2;
    if ((c & cBlendEn) && (c & cBlendEq)) s.sqc = s.sqa = 0;
    for (u32 k = s.count; k < 8; ++k) s.fmt[k] = 0, s.blend[k] = {};
    for (u32 k = 0; k < s.count && k < 8; ++k) {
        Blend& b = s.blend[k];
        if (c & cBlendEn) b.enable = 0;
        if ((c & cBlendEq) || (!(c & cBlendEn) && !b.enable)) b.sc = b.dc = b.cop = b.sa = b.da = b.aop = 0;
        if (c & cMask) b.mask = 0xF;
    }
    s.pad[0] = s.pad[1] = 0;
    return s;
}
Key MakeKey(u64 vs, u64 ps, u64 gs, const State& raw, u32 caps) {
    Key k{vs, (raw.flags & kHasPs) ? ps : 0, (raw.flags & kHasGs) ? gs : 0, Canon(raw, caps)};
    return k;
}
// The state without its programs: the render target configuration a pipeline is made for.
Key Config(const Key& k) { return {0, 0, 0, k.st}; }

std::string Fmt(u32 f) {
    switch (f) {
    case 0: return "-";
    case 9: return "R8";
    case 16: return "RG8";
    case 37: return "RGBA8";
    case 43: return "RGBA8_SRGB";
    case 44: return "BGRA8";
    case 50: return "BGRA8_SRGB";
    case 58: return "A2R10G10B10";
    case 64: return "A2B10G10R10";
    case 70: return "R16";
    case 76: return "R16F";
    case 77: return "RG16";
    case 83: return "RG16F";
    case 91: return "RGBA16";
    case 97: return "RGBA16F";
    case 98: return "R32UI";
    case 100: return "R32F";
    case 103: return "RG32F";
    case 109: return "RGBA32F";
    case 122: return "B10G11R11F";
    case 124: return "D16";
    case 126: return "D32F";
    case 129: return "D24S8";
    case 130: return "D32FS8";
    default: return "f" + std::to_string(f);
    }
}
std::string Blends(const State& s) {
    std::string t;
    for (u32 k = 0; k < s.count; ++k) {
        const Blend& b = s.blend[k];
        char buf[48];
        std::snprintf(buf, sizeof buf, "%s%s%x", k ? "," : "", b.enable ? "B" : "", b.mask);
        t += buf;
        if (b.enable) std::snprintf(buf, sizeof buf, "(%u.%u.%u/%u.%u.%u)", b.sc, b.dc, b.cop, b.sa, b.da, b.aop), t += buf;
    }
    return t;
}
std::string Describe(const State& s, bool blend) {
    std::string t = "c" + std::to_string(s.count) + "[";
    for (u32 k = 0; k < s.count; ++k) t += (k ? " " : "") + Fmt(s.fmt[k]);
    t += "] z " + Fmt(s.depth) + (s.stencil ? "+s" : "") + " x" + std::to_string(s.samples) + " t" + std::to_string(s.topo);
    t += (s.flags & kHasPs) ? " ps" : " noPS";
    if (s.flags & kHasGs) t += " gs";
    if (s.flags & kNegOne) t += " -1..1";
    if (s.flags & kClamp) t += " clamp";
    if (s.flags & kA2c) t += " a2c";
    if (s.sqc | s.sqa) t += " sq";
    if (blend) t += " {" + Blends(s) + "}";
    return t;
}
// What differs between a used state and a derived one, field by field ("" equal).
std::string Diff(const State& u, const State& c) {
    std::string t;
    auto add = [&](const std::string& s) { t += (t.empty() ? "" : "; ") + s; };
    if (u.count != c.count) add("count " + std::to_string(u.count) + "->" + std::to_string(c.count));
    for (u32 k = 0; k < 8; ++k)
        if (u.fmt[k] != c.fmt[k]) add("mrt" + std::to_string(k) + " " + Fmt(u.fmt[k]) + "->" + Fmt(c.fmt[k]));
    if (u.depth != c.depth) add("depth " + Fmt(u.depth) + "->" + Fmt(c.depth));
    if (u.stencil != c.stencil) add(std::string("stencil ") + (u.stencil ? "y" : "n") + "->" + (c.stencil ? "y" : "n"));
    if (u.samples != c.samples) add("samples " + std::to_string(u.samples) + "->" + std::to_string(c.samples));
    if (u.topo != c.topo) add("topo " + std::to_string(u.topo) + "->" + std::to_string(c.topo));
    if (u.flags != c.flags) add("flags " + std::to_string(u.flags) + "->" + std::to_string(c.flags));
    if (u.sqc != c.sqc || u.sqa != c.sqa) add("square");
    for (u32 k = 0; k < 8; ++k)
        if (!(u.blend[k] == c.blend[k])) { add("blend"); break; }
    return t;
}
u32 Fields(const State& u, const State& c) {
    u32 n = (u.count != c.count) + (u.depth != c.depth) + (u.stencil != c.stencil) + (u.samples != c.samples) + (u.topo != c.topo) + (u.flags != c.flags);
    for (u32 k = 0; k < 8; ++k) n += (u.fmt[k] != c.fmt[k]) + !(u.blend[k] == c.blend[k]);
    return n;
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    std::vector<u8> b(size_t(in.tellg()));
    in.seekg(0);
    in.read((char*)b.data(), std::streamsize(b.size()));
    return b;
}
fs::path Pick(const fs::path& dir, const char* stem, const char* ext) {
    for (const char* h : {"aes", "xxh"})
        if (fs::path p = dir / (std::string(stem) + "-hash2-" + h + ext); fs::exists(p)) return p;
    return {};
}

struct Used {  // a pipeline of the run under the given caps
    u64 draws = 0;
    u32 frames = 0, first = ~0u, states = 0, origin = 0, us = 0, waits = 0;
    bool vs_cached = false, ps_cached = false;
};
template <class M> auto Sorted(const M& m) {
    std::vector<std::pair<typename M::key_type, typename M::mapped_type>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    return v;
}
const char* Pct(u64 a, u64 b) {
    static char buf[8][16];
    static int i = 0;
    char* s = buf[i++ & 7];
    std::snprintf(s, 16, "%.1f%%", b ? 100.0 * double(a) / double(b) : 0.0);
    return s;
}

int main(int argc, char** argv) {
    fs::path dir, game_dir;
    u32 caps_arg = ~0u;
    size_t list = 20;
    std::vector<fs::path> uses;
    std::unordered_set<std::string> opened;  // --opened <file>: WAD names the run opened, one per line
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--game" && i + 1 < argc) game_dir = argv[++i];
        else if (a == "--opened" && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            for (std::string n; std::getline(in, n);)
                if (n.erase(n.find_last_not_of(" \r\n\t") + 1), !n.empty()) opened.insert(n);
        }
        else if (a == "--caps" && i + 1 < argc) caps_arg = u32(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--list" && i + 1 < argc) list = size_t(std::atoi(argv[++i]));
        else uses.push_back(a);
    }
    if (dir.empty()) {
        const char* game = std::getenv("GOW3_GAME_DIR");
        dir = fs::path(game ? game : "D:\\GAMES\\EMULATION\\ISOs\\PS4\\CUSA01715") / "_rehooked_" / "shader_cache";
    }

    // The run: its draw states and made pipelines (several files add up), or the recipes standing for it.
    std::vector<UseState> states;
    std::unordered_map<u64, Made> made;  // runtime canonical hash -> how it was made
    std::vector<PlRecipe> recipes;       // format 2 runs: the pipeline recipes as that GPU drew them
    u32 run_caps = 0xFF, path = 1;
    for (const fs::path& p : uses) {
        const auto b = ReadAll(p);
        u32 head[8]{};
        if (b.size() < 24) { std::fprintf(stderr, "%s: unreadable\n", p.string().c_str()); return 1; }
        std::memcpy(head, b.data(), std::min<size_t>(b.size(), sizeof(head)));
        const size_t head_size = head[1] == 2 ? 32 : 24, nrec = head[1] == 2 ? head[6] : 0;
        if (head[0] != 0x4C503347 || (head[1] != 1 && head[1] != 2) ||
            b.size() != head_size + head[4] * sizeof(UseState) + head[5] * 32ull + nrec * sizeof(PlRecipe)) {
            std::fprintf(stderr, "%s: not a pipelines file of format 1 or 2\n", p.string().c_str());
            return 1;
        }
        path = head[2], run_caps = head[3];
        const u8* at = b.data() + head_size;
        for (u32 k = 0; k < head[4]; ++k, at += sizeof(UseState)) std::memcpy(&states.emplace_back(), at, sizeof(UseState));
        for (u32 k = 0; k < head[5]; ++k, at += 32) {
            u64 h;
            Made m;
            std::memcpy(&h, at, 8), std::memcpy(&m, at + 8, sizeof(m));
            if (auto [it, fresh] = made.try_emplace(h, m); !fresh) it->second.waits += m.waits;
        }
        if (nrec) recipes.clear();  // the last run's list holds the earlier ones
        for (size_t k = 0; k < nrec; ++k, at += sizeof(PlRecipe)) std::memcpy(&recipes.emplace_back(), at, sizeof(PlRecipe));
    }
    if (caps_arg != ~0u) run_caps = caps_arg;
    if (recipes.empty()) {  // earlier builds: their own file
        const auto recipes_bytes = ReadAll(Pick(dir, "pipeline-recipes", ".bin"));
        for (size_t at = 8; at + sizeof(PlRecipe) <= recipes_bytes.size(); at += sizeof(PlRecipe)) std::memcpy(&recipes.emplace_back(), recipes_bytes.data() + at, sizeof(PlRecipe));
    }
    const bool from_recipes = uses.empty();
    if (from_recipes)
        for (const PlRecipe& r : recipes) states.push_back({r.vs, r.ps, r.gs, 0, r.raw, 0, 1, 0, 0, 1});

    const auto contract_bytes = ReadAll(Pick(dir, "pipeline-contract", ".cache"));
    std::vector<Contracted> contract;
    if (contract_bytes.size() >= 24) {
        u32 count;
        std::memcpy(&count, contract_bytes.data() + 16, 4);
        if (24 + u64(count) * sizeof(Contracted) == contract_bytes.size()) {
            contract.resize(count);
            std::memcpy(contract.data(), contract_bytes.data() + 24, count * sizeof(Contracted));
        }
    }
    // Shader contract: translation keys of the variants the AOT derives (stage 0 VS, 1 PS).
    std::unordered_map<u64, u32> shader_stage;
    {
        const auto b = ReadAll(Pick(dir, "contract", ".cache"));
        u32 count = 0;
        if (b.size() >= 24) std::memcpy(&count, b.data() + 16, 4);
        size_t at = 24;
        for (u32 k = 0; k < count && at + 248 <= b.size(); ++k) {
            u16 nspec;
            u8 stage;
            u64 key;
            u32 fetch_words;
            std::memcpy(&nspec, b.data() + at + 192, 2), std::memcpy(&stage, b.data() + at + 194, 1), std::memcpy(&key, b.data() + at + 200, 8);
            std::memcpy(&fetch_words, b.data() + at + 240, 4);
            shader_stage.emplace(key, stage);
            at += 248 + nspec * 8ull + fetch_words * 4ull;
        }
    }
    std::printf("Inputs: %s %zu draw states, %zu made pipelines (path %u, caps %02x) | %zu pipeline recipes | %zu derived pipelines | %zu shader variants derived\n",
                from_recipes ? "recipes as the run:" : "run:", states.size(), made.size(), path, run_caps, recipes.size(), contract.size(), shader_stage.size());
    {   // Shader recipes (every variant the player's runs met, any path): records {type, size} of RecipeHead + specs + fetch words.
        const auto b = ReadAll(Pick(dir, "recipes", ".bin"));
        u32 n[3]{};
        std::unordered_set<u64> programs[3];
        std::map<u32, u32> mrts;  // PS: exported targets (SPI_SHADER_COL_FORMAT fields) -> variants
        for (size_t at = 8; at + 8 + 200 <= b.size();) {
            u32 type, size;
            std::memcpy(&type, b.data() + at, 4), std::memcpy(&size, b.data() + at + 4, 4);
            const u8* h = b.data() + at + 8;
            if ((type == 1 || type == 3) && size >= 200 && h[194] < 3) {
                u64 code;
                u32 col;
                std::memcpy(&code, h, 8), std::memcpy(&col, h + 32, 4);
                ++n[h[194]], programs[h[194]].insert(code);
                if (h[194] == 1) {
                    u32 m = 0;
                    for (u32 k = 0; k < 8; ++k) m += (col >> (4 * k) & 0xF) != 0;
                    ++mrts[m];
                }
            }
            at += 8 + size;
        }
        std::printf("Shader recipes (all runs): VS %u variants of %zu programs, PS %u of %zu, CS %u of %zu; PS exporting to", n[0], programs[0].size(), n[1],
                    programs[1].size(), n[2], programs[2].size());
        for (auto& [m, c] : mrts) std::printf(" %u targets: %u,", m, c);
        std::printf("\n");
    }

    // ---- The run under its caps ----
    const u32 caps = run_caps;
    std::unordered_map<Key, Used, KeyHash> used;
    std::unordered_set<u64> runtime_canon;
    u64 draws = 0;
    for (const UseState& s : states) {
        Used& u = used[MakeKey(s.vs, s.ps, s.gs, s.raw, caps)];
        u.draws += s.draws, u.frames = std::max(u.frames, s.frames), u.first = std::min(u.first, s.first_frame), ++u.states;
        u.vs_cached |= s.flags & 1, u.ps_cached |= (s.flags >> 1) & 1;
        if (const auto it = made.find(s.canon); it != made.end()) u.origin = it->second.origin, u.us = it->second.us, u.waits = it->second.waits;
        runtime_canon.insert(s.canon);
        draws += s.draws;
    }
    std::unordered_set<Key, KeyHash> any_caps[2];  // no dynamic state 3, every one
    std::unordered_set<Key, KeyHash> triples;
    for (const UseState& s : states) {
        any_caps[0].insert(MakeKey(s.vs, s.ps, s.gs, s.raw, 0)), any_caps[1].insert(MakeKey(s.vs, s.ps, s.gs, s.raw, 0xFF));
        Key t = MakeKey(s.vs, s.ps, s.gs, s.raw, caps);
        triples.insert({t.vs, t.ps, t.gs, {}});
    }
    std::printf("\nRun: %zu draw states, %zu pipelines under its caps (runtime counted %zu), %zu program pairs, %llu draws\n", states.size(), used.size(),
                from_recipes ? used.size() : runtime_canon.size(), triples.size(), (unsigned long long)draws);
    std::printf("  pipelines without extended dynamic state 3: %zu; with all of it: %zu\n", any_caps[0].size(), any_caps[1].size());
    {
        u32 buckets[6]{};
        for (const auto& [k, u] : used) ++buckets[u.frames <= 1 ? 0 : u.frames < 10 ? 1 : u.frames < 100 ? 2 : u.frames < 1000 ? 3 : u.frames < 10000 ? 4 : 5];
        std::printf("  frames each pipeline was used in: 1: %u, 2-9: %u, 10-99: %u, 100-999: %u, 1k-10k: %u, more: %u\n", buckets[0], buckets[1], buckets[2],
                    buckets[3], buckets[4], buckets[5]);
        u32 vs_c = 0, ps_c = 0, ps_n = 0;
        for (const auto& [k, u] : used) vs_c += u.vs_cached || k.vs == kClearRectVs, ps_n += k.ps != 0, ps_c += k.ps && u.ps_cached;
        if (!from_recipes) std::printf("  programs in the shader cache at first use: VS %u of %zu pipelines, PS %u of %u\n", vs_c, used.size(), ps_c, ps_n);
    }
    if (!from_recipes) {
        u32 n[9]{}, unused[9]{};
        u64 us[9]{}, d[9]{}, waits[9]{}, waited[9]{};
        for (const auto& [k, u] : used) { const u32 o = std::min(u.origin, 8u); ++n[o], us[o] += u.us, d[o] += u.draws, waits[o] += u.waits, waited[o] += u.waits != 0; }
        for (const auto& [h, m] : made) if (!runtime_canon.contains(h)) ++unused[std::min(m.origin, 8u)];
        std::printf("\nHow the run's pipelines came to be (made but never drawn with: unused):\n");
        for (u32 o = 0; o < 9; ++o)
            if (n[o] || unused[o])
                std::printf("  %-10s %6u used (%s of draws, %.0f ms compiling, %llu draws waited on %llu of them), %6u unused\n", kOrigins[o], n[o], Pct(d[o], draws),
                            us[o] / 1e3, (unsigned long long)waits[o], (unsigned long long)waited[o], unused[o]);
        std::printf("  hit = in the VkPipelineCache (AOT or an earlier run); ahead = compiled for Discover; miss = compiled for a draw that needed it\n");
    }

    // ---- Against the derivation ----
    std::unordered_map<Key, std::vector<const Contracted*>, KeyHash> by_triple;  // canonical program triple -> derived states
    std::unordered_set<Key, KeyHash> derived;
    std::unordered_set<u64> derived_vs, derived_ps;
    for (const Contracted& c : contract) {
        const Key k = MakeKey(c.vs, c.ps, c.gs, c.st, caps);
        if (derived.insert(k).second) by_triple[{k.vs, k.ps, k.gs, {}}].push_back(&c);
        derived_vs.insert(k.vs), derived_ps.insert(k.ps);
    }
    std::unordered_set<Key, KeyHash> recipe_keys;
    for (const PlRecipe& r : recipes) recipe_keys.insert(MakeKey(r.vs, r.ps, r.gs, r.raw, caps));
    const auto shader_known = [&](u64 key, u32 stage) {
        if (!key) return true;
        if (key == kClearRectVs) return true;
        const auto it = shader_stage.find(key);
        return it != shader_stage.end() && it->second == stage;
    };
    enum Cat { kExact, kState, kPairing, kNoVs, kNoPs, kNoBoth, kCats };
    static constexpr const char* cat_names[]{"derived exactly", "programs derived together, other state", "programs derived, not paired", "VS not derived",
                                             "PS not derived", "VS and PS not derived"};
    u64 cat_n[kCats]{}, cat_frames[kCats]{}, cat_draws[kCats]{}, cat_recipe[kCats]{}, cat_hit[kCats]{}, cat_us[kCats]{};
    std::map<std::string, std::array<u64, 2>> diffs;  // closest derived state's difference -> pipelines, frames
    std::vector<std::pair<const Key*, const Used*>> missed;
    u64 frames_total = 0;
    for (const auto& [k, u] : used) {
        frames_total += u.frames;
        Cat c;
        if (derived.contains(k)) c = kExact;
        else if (const auto it = by_triple.find({k.vs, k.ps, k.gs, {}}); it != by_triple.end()) {
            c = kState;
            const Contracted* best = nullptr;
            State bs{};
            for (const Contracted* d : it->second) {
                const State s = Canon(d->st, caps);
                if (!best || Fields(k.st, s) < Fields(k.st, bs)) best = d, bs = s;
            }
            auto& e = diffs[Diff(k.st, bs)];
            ++e[0], e[1] += u.frames;
        } else {
            const bool vs = shader_known(k.vs, 0), ps = shader_known(k.ps, 1);
            c = vs && ps ? kPairing : !vs && !ps ? kNoBoth : !vs ? kNoVs : kNoPs;
        }
        if (c != kExact) missed.push_back({&k, &u});
        ++cat_n[c], cat_frames[c] += u.frames, cat_draws[c] += u.draws, cat_recipe[c] += recipe_keys.contains(k), cat_us[c] += u.us;
        cat_hit[c] += u.origin == 1 || u.origin == 2 || u.origin == 3 || u.origin == 6;
    }
    std::printf("\nThe run's %zu pipelines against the %zu derived (%zu under these caps, %zu program pairs):\n", used.size(), contract.size(), derived.size(), by_triple.size());
    for (u32 c = 0; c < kCats; ++c) {
        std::printf("  %-40s %6llu (%s; %s of pipeline-frames, %s of draws), %llu in the recipes", cat_names[c], (unsigned long long)cat_n[c], Pct(cat_n[c], used.size()),
                    Pct(cat_frames[c], frames_total), Pct(cat_draws[c], draws), (unsigned long long)cat_recipe[c]);
        if (!from_recipes) std::printf(", %llu cache hits, %.0f ms compiling", (unsigned long long)cat_hit[c], cat_us[c] / 1e3);
        std::printf("\n");
    }
    if (!from_recipes) {  // the recipes against the run: canonical pipelines and raw draw states (without EDS3) they lack
        u32 by_origin[9]{};
        for (const auto& [k, u] : used) by_origin[std::min(u.origin, 8u)] += !recipe_keys.contains(k);
        std::unordered_set<Key, KeyHash> raw_recipes;
        for (const PlRecipe& r : recipes) raw_recipes.insert(MakeKey(r.vs, r.ps, r.gs, r.raw, 0));
        u32 raw_missing = 0;
        for (const Key& k : any_caps[0]) raw_missing += !raw_recipes.contains(k);
        std::printf("  not in the recipes by origin:");
        for (u32 o = 0; o < 9; ++o) if (by_origin[o]) std::printf(" %s %u", kOrigins[o], by_origin[o]);
        std::printf("; raw states (no EDS3) not in the recipes: %u of %zu\n", raw_missing, any_caps[0].size());
    }
    std::printf("\nClosest derived state of the pipelines whose programs were derived together (pipelines, pipeline-frames):\n");
    {
        auto v = Sorted(diffs);
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second[0] > b.second[0]; });
        for (size_t i = 0; i < v.size() && i < 30; ++i) std::printf("  %5llu %8llu  %s\n", (unsigned long long)v[i].second[0], (unsigned long long)v[i].second[1], v[i].first.c_str());
    }

    // ---- Render target configurations: what the run used, what the derivation assumes ----
    std::unordered_map<Key, std::array<u64, 4>, KeyHash> configs;  // config -> run pipelines, run frames, derived pipelines, run's exact
    for (const auto& [k, u] : used) { auto& e = configs[Config(k)]; ++e[0], e[1] += u.frames, e[3] += derived.contains(k); }
    for (const Key& k : derived) ++configs[Config(k)][2];
    {
        std::vector<std::pair<Key, std::array<u64, 4>>> v(configs.begin(), configs.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second[1] != b.second[1] ? a.second[1] > b.second[1] : a.second[2] > b.second[2]; });
        u32 run_only = 0, derived_only = 0, both = 0;
        for (auto& [k, e] : v) run_only += e[0] && !e[2], derived_only += !e[0] && e[2], both += e[0] && e[2];
        std::printf("\nRender target configurations (state without programs): %u used and derived, %u used only, %u derived only\n", both, run_only, derived_only);
        std::printf("  %7s %6s %9s %8s  configuration\n", "used", "exact", "frames", "derived");
        size_t shown = 0;
        for (auto& [k, e] : v)
            if (e[0] && shown++ < 40)
                std::printf("  %7llu %6llu %9llu %8llu  %s\n", (unsigned long long)e[0], (unsigned long long)e[3], (unsigned long long)e[1], (unsigned long long)e[2],
                            Describe(k.st, caps != 0xFF).c_str());
        std::printf("  derived, never used:\n");
        shown = 0;
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second[2] > b.second[2]; });
        for (auto& [k, e] : v) if (!e[0] && shown++ < 15) std::printf("  %7s %9s %8llu  %s\n", "", "", (unsigned long long)e[2], Describe(k.st, caps != 0xFF).c_str());
    }

    // ---- What the derivation spends: derived pipelines of the programs the run used ----
    {
        std::unordered_set<u64> run_vs, run_ps;
        for (const auto& [k, u] : used) run_vs.insert(k.vs), run_ps.insert(k.ps);
        u64 of_run_pairs = 0, of_run_vs = 0, hit = 0;
        for (const Key& k : derived) of_run_pairs += triples.contains({k.vs, k.ps, k.gs, {}}), of_run_vs += run_vs.contains(k.vs) && run_ps.contains(k.ps), hit += used.contains(k);
        std::printf("\nDerived pipelines: %zu; of the run's program pairs %llu; used %llu (%s of the derived, %s of the run's pipelines)\n", derived.size(),
                    (unsigned long long)of_run_pairs, (unsigned long long)hit, Pct(hit, derived.size()), Pct(hit, used.size()));
        std::map<size_t, u32> per_pair;
        for (const auto& [t, list] : by_triple) ++per_pair[std::min<size_t>(list.size(), 9)];
        std::printf("  derived states per program pair:");
        for (auto& [n, c] : per_pair) std::printf(" %zu%s: %u", n, n == 9 ? "+" : "", c);
        std::printf("\n");
        std::map<u32, u32> run_per_pair;
        std::unordered_map<Key, u32, KeyHash> pairs;
        for (const auto& [k, u] : used) ++pairs[{k.vs, k.ps, k.gs, {}}];
        for (auto& [t, n] : pairs) ++run_per_pair[std::min(n, 9u)];
        std::printf("  used states per program pair:   ");
        for (auto& [n, c] : run_per_pair) std::printf(" %u%s: %u", n, n == 9 ? "+" : "", c);
        std::printf("\n  program pairs: used %zu, of them derived %zu; VS used %zu (derived in a pipeline %zu, as a shader %zu), PS used %zu (%zu, %zu)\n", pairs.size(),
                    size_t(std::count_if(pairs.begin(), pairs.end(), [&](auto& p) { return by_triple.contains(p.first); })), run_vs.size(),
                    size_t(std::count_if(run_vs.begin(), run_vs.end(), [&](u64 k) { return derived_vs.contains(k); })),
                    size_t(std::count_if(run_vs.begin(), run_vs.end(), [&](u64 k) { return shader_known(k, 0); })), run_ps.size(),
                    size_t(std::count_if(run_ps.begin(), run_ps.end(), [&](u64 k) { return derived_ps.contains(k); })),
                    size_t(std::count_if(run_ps.begin(), run_ps.end(), [&](u64 k) { return shader_known(k, 1); })));
    }

    // ---- What-if: derivation rules changed, against the run ----
    {
        const auto what_if = [&](const char* name, auto&& expand) {
            std::unordered_set<Key, KeyHash> set;
            std::vector<State> out;
            for (const Contracted& c : contract) {
                out.clear(), expand(c.st, out);
                for (const State& s : out) set.insert(MakeKey(c.vs, c.ps, c.gs, s, caps));
            }
            u64 hit = 0, frames = 0;
            for (const auto& [k, u] : used) if (set.contains(k)) ++hit, frames += u.frames;
            std::printf("  %-52s %6zu derived, %5llu of the run's pipelines exact (%s, %s of pipeline-frames)\n", name, set.size(), (unsigned long long)hit,
                        Pct(hit, used.size()), Pct(frames, frames_total));
        };
        std::printf("\nWhat-if derivation rules:\n");
        what_if("as derived", [](const State& s, std::vector<State>& o) { o.push_back(s); });
        what_if("1 target: RGBA8 -> RGBA8_SRGB", [](const State& s, std::vector<State>& o) {
            State t = s;
            if (t.count == 1 && t.fmt[0] == 37) t.fmt[0] = 43;
            o.push_back(t);
        });
        what_if("1 target: RGBA8 and RGBA8_SRGB", [](const State& s, std::vector<State>& o) {
            o.push_back(s);
            if (s.count == 1 && s.fmt[0] == 37) { State t = s; t.fmt[0] = 43; o.push_back(t); }
        });
        what_if("1 target with depth: SRGB, -1..1, scene samples x2", [](const State& s, std::vector<State>& o) {
            State t = s;
            t.flags |= kNegOne;
            if (t.count == 1 && t.fmt[0] == 37 && t.depth) t.fmt[0] = 43, t.samples = 2;
            o.push_back(t);
        });
    }

    // ---- Compiles during play, by the frame the run first needed them ----
    if (!from_recipes) {
        std::map<u32, std::array<u64, 3>> at;  // 1000-frame window -> pipelines compiled, compile us, draws waited
        for (const auto& [k, u] : used)
            if (u.origin == 4 || u.origin == 5 || u.origin == 7) { auto& e = at[u.first / 1000]; ++e[0], e[1] += u.us, e[2] += u.waits; }
        std::printf("\nPipelines compiled in play by the frame they were first drawn in (window of 1000 frames: pipelines, compile ms, draws waited):\n ");
        for (const auto& [w, e] : at) std::printf(" %u: %llu/%.0f/%llu", w, (unsigned long long)e[0], e[1] / 1e3, (unsigned long long)e[2]);
        std::printf("\n");
    }

    // ---- Derived pipelines of the WADs the run opened (EBOOT always): what building per opened WAD would cost and cover ----
    if (!opened.empty()) {
        const char* game_env = std::getenv("GOW3_GAME_DIR");
        const fs::path game = game_dir.empty() ? fs::path(game_env ? game_env : "D:\\GAMES\\EMULATION\\ISOs\\PS4\\CUSA01715") : game_dir;
        std::vector<fs::path> paths;  // the catalog's files: EXEC/WAD/**.wad sorted, without bak/
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(game / "EXEC" / "WAD", ec))
            if (e.is_regular_file() && (e.path().extension() == ".WAD" || e.path().extension() == ".wad") && e.path().parent_path().filename() != "bak") paths.push_back(e.path());
        std::sort(paths.begin(), paths.end());
        std::vector<bool> in(paths.size());
        u32 found = 0;
        for (size_t f = 0; f < paths.size(); ++f) {
            std::string n = paths[f].stem().string();
            for (char& ch : n) ch = char(std::toupper((unsigned char)ch));
            if ((in[f] = opened.contains(n))) ++found;
        }
        for (bool srgb : {false, true}) {
            std::unordered_set<Key, KeyHash> all, opened_set;
            for (const Contracted& c : contract) {
                State s = c.st;
                if (srgb && s.count == 1 && s.fmt[0] == 37) s.fmt[0] = 43;
                const Key k = MakeKey(c.vs, c.ps, c.gs, s, caps);
                all.insert(k);
                if (c.file == ~0u || (c.file < in.size() && in[c.file])) opened_set.insert(k);
            }
            u64 hit = 0;
            for (const auto& [k, u] : used) hit += opened_set.contains(k);
            std::printf("\n%s: the %u opened WADs (of %zu files, %zu names given) + EBOOT derive %zu of %zu pipelines; %llu of the run's %zu exact (%s)",
                        srgb ? "With the sRGB rule" : "Derived per opened WAD", found, paths.size(), opened.size(), opened_set.size(), all.size(), (unsigned long long)hit,
                        used.size(), Pct(hit, used.size()));
        }
        std::printf("\n");
    }

    // ---- The pipelines the derivation missed, most used first ----
    std::sort(missed.begin(), missed.end(), [](auto& a, auto& b) { return a.second->frames > b.second->frames; });
    std::printf("\nMost used pipelines not derived (frames, draws, origin, compile ms):\n");
    for (size_t i = 0; i < missed.size() && i < list; ++i) {
        const Key& k = *missed[i].first;
        const Used& u = *missed[i].second;
        std::printf("  %7u %9llu %-9s %6.1f vs %016llx%s ps %016llx%s %s\n", u.frames, (unsigned long long)u.draws, kOrigins[std::min(u.origin, 8u)], u.us / 1e3,
                    (unsigned long long)k.vs, shader_known(k.vs, 0) ? "" : "*", (unsigned long long)k.ps, shader_known(k.ps, 1) ? "" : "*", Describe(k.st, caps != 0xFF).c_str());
    }
    std::printf("  (* the shader contract does not derive that program variant)\n");
    return 0;
}
