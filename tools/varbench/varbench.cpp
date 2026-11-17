// Offline simulation of the renderer's per-draw variant and hash work on real inputs, from a GOW3_TEMP_VARDUMP dump
// (vk_gpu.cpp, serial recording): hash algorithms per input class, ways to skip the variant work, and where the
// renderer's barriers sit against the engine's own sync packets.
//   varbench <dump.bin> [hash|variants|barriers|all]
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "gpu/shaders/gcn_translator.h"

#include <intrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- Candidate hashes (wyhash final4 and rapidhash v1 structure, reimplemented for the benchmark) ----
static inline void Mum(u64& a, u64& b) { u64 hi; a = _umul128(a, b, &hi), b = hi; }
static inline u64 Mix(u64 a, u64 b) { return Mum(a, b), a ^ b; }
static inline u64 R8(const u8* p) { u64 v; std::memcpy(&v, p, 8); return v; }
static inline u64 R4(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
static inline u64 R3(const u8* p, size_t k) { return (u64)p[0] << 16 | (u64)p[k >> 1] << 8 | p[k - 1]; }
static constexpr u64 kS[4]{0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull};

static inline u64 RapidHash(const void* key, size_t len, u64 seed = 0xbdd89aa982704029ull) {
    const u8* p = (const u8*)key;
    seed ^= Mix(seed ^ kS[0], kS[1]) ^ len;
    u64 a, b;
    if (len <= 16) {
        if (len >= 4) {
            const u8* last = p + len - 4;
            const u64 delta = (len & 24) >> (len >> 3);
            a = R4(p) << 32 | R4(last), b = R4(p + delta) << 32 | R4(last - delta);
        } else if (len) a = R3(p, len), b = 0;
        else a = b = 0;
    } else {
        size_t i = len;
        if (i > 48) {
            u64 s1 = seed, s2 = seed;
            for (; i >= 96; p += 96, i -= 96) {
                seed = Mix(R8(p) ^ kS[0], R8(p + 8) ^ seed), s1 = Mix(R8(p + 16) ^ kS[1], R8(p + 24) ^ s1), s2 = Mix(R8(p + 32) ^ kS[2], R8(p + 40) ^ s2);
                seed = Mix(R8(p + 48) ^ kS[0], R8(p + 56) ^ seed), s1 = Mix(R8(p + 64) ^ kS[1], R8(p + 72) ^ s1), s2 = Mix(R8(p + 80) ^ kS[2], R8(p + 88) ^ s2);
            }
            if (i >= 48)
                seed = Mix(R8(p) ^ kS[0], R8(p + 8) ^ seed), s1 = Mix(R8(p + 16) ^ kS[1], R8(p + 24) ^ s1), s2 = Mix(R8(p + 32) ^ kS[2], R8(p + 40) ^ s2), p += 48,
                i -= 48;
            seed ^= s1 ^ s2;
        }
        if (i > 16) {
            seed = Mix(R8(p) ^ kS[2], R8(p + 8) ^ seed ^ kS[1]);
            if (i > 32) seed = Mix(R8(p + 16) ^ kS[2], R8(p + 24) ^ seed);
        }
        a = R8(p + i - 16), b = R8(p + i - 8);
    }
    a ^= kS[1], b ^= seed, Mum(a, b);
    return Mix(a ^ kS[0] ^ len, b ^ kS[1]);
}

static inline u64 WyHash(const void* key, size_t len, u64 seed = 0) {
    const u8* p = (const u8*)key;
    seed ^= Mix(seed ^ kS[0], kS[1]);
    u64 a, b;
    if (len <= 16) {
        if (len >= 4) a = R4(p) << 32 | R4(p + ((len >> 3) << 2)), b = R4(p + len - 4) << 32 | R4(p + len - 4 - ((len >> 3) << 2));
        else if (len) a = R3(p, len), b = 0;
        else a = b = 0;
    } else {
        size_t i = len;
        if (i >= 48) {
            u64 s1 = seed, s2 = seed;
            do seed = Mix(R8(p) ^ kS[1], R8(p + 8) ^ seed), s1 = Mix(R8(p + 16) ^ kS[2], R8(p + 24) ^ s1), s2 = Mix(R8(p + 32) ^ kS[3], R8(p + 40) ^ s2), p += 48, i -= 48;
            while (i >= 48);
            seed ^= s1 ^ s2;
        }
        while (i > 16) seed = Mix(R8(p) ^ kS[1], R8(p + 8) ^ seed), i -= 16, p += 16;
        a = R8(p + i - 16), b = R8(p + i - 8);
    }
    a ^= kS[1], b ^= seed, Mum(a, b);
    return Mix(a ^ kS[0] ^ len, b ^ kS[1]);
}

// ---- Dump ----
struct Draw {
    u32 frame, seq, stage, rect, nslots, nfmts, rsrc1, rsrc2, table, epoch;
    u64 prog, pkey, key;
    std::vector<u32> user, ps, slots, fmts, elems;  // ps: 46 words; slots: 9 per slot; fmts: 5 per format
    std::vector<Gcn::SlotSpec> spec;
};
struct Mark {
    u32 type, seq, a, b;
};
struct Dump {
    std::vector<u8> bytes;
    std::vector<Draw> draws;
    std::vector<std::span<const u8>> tex, smp, pieces, code;
    std::vector<Mark> marks;  // barriers, engine packets, pass begins, frames: in recording order
};

static bool Load(const char* path, Dump& d) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    d.bytes.resize((size_t)_ftelli64(f));
    std::fseek(f, 0, SEEK_SET);
    const size_t got = std::fread(d.bytes.data(), 1, d.bytes.size(), f);
    std::fclose(f);
    if (got != d.bytes.size()) return false;
    for (size_t o = 0; o + 8 <= d.bytes.size();) {
        u32 h[2];
        std::memcpy(h, &d.bytes[o], 8);
        const u8* p = &d.bytes[o + 8];
        o += 8 + h[1];
        auto words = [&](size_t& at, size_t n) {
            std::vector<u32> v(n);
            std::memcpy(v.data(), p + at, n * 4), at += n * 4;
            return v;
        };
        switch (h[0]) {
        case 1: {
            Draw w;
            size_t at = 0;
            const auto head = words(at, 8);
            std::vector<u64> ids(4);
            std::memcpy(ids.data(), p + at, 32), at += 32;
            w.frame = head[0], w.seq = head[1], w.stage = head[2] & 0xFF, w.rect = head[2] >> 8 & 0xFF, w.nslots = head[2] >> 16 & 0xFF, w.nfmts = head[2] >> 24;
            w.rsrc1 = head[3], w.rsrc2 = head[4], w.table = head[5], w.epoch = head[6], w.prog = ids[0], w.pkey = ids[1], w.key = ids[2];
            w.user = words(at, 16), w.ps = words(at, 46), w.slots = words(at, 9 * w.nslots), w.fmts = words(at, 5 * w.nfmts);
            w.spec.resize(w.nslots + w.nfmts);
            std::memcpy(w.spec.data(), p + at, w.spec.size() * sizeof(Gcn::SlotSpec)), at += w.spec.size() * sizeof(Gcn::SlotSpec);
            w.elems = words(at, w.nslots);
            d.draws.push_back(std::move(w));
            break;
        }
        case 2: d.tex.push_back({p, 32}); break;
        case 3: d.smp.push_back({p, 16}); break;
        case 4: d.pieces.push_back({p, h[1]}); break;
        case 5: d.code.push_back({p, h[1]}); break;
        default: {
            u32 v[3];
            std::memcpy(v, p, 12);
            d.marks.push_back({h[0], v[0], v[1], v[2]});
        }
        }
    }
    return true;
}

static u64 sink = 0;
using Clock = std::chrono::steady_clock;
static double Ns(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::nano>(b - a).count(); }

// ---- Hashes over each input class: ns per call, best of 15 passes over the inputs packed in recording order (hot, as in
// the renderer, which hashes bytes it just read); independent calls (throughput) and each call seeded with the previous
// result (latency, as when the next hash or lookup waits for it). Unseeded classes are called as the renderer calls them.
template <class F> static std::pair<double, double> Bench(const std::vector<std::span<const u8>>& in, F f) {
    double tp = 1e30, lat = 1e30;
    for (int rep = 0; rep < 15; ++rep) {
        u64 x = 0;
        auto t0 = Clock::now();
        for (auto& s : in) x ^= f(s.data(), s.size(), 0);
        auto t1 = Clock::now();
        u64 c = 0;
        for (auto& s : in) c = f(s.data(), s.size(), c);
        auto t2 = Clock::now();
        tp = std::min(tp, Ns(t0, t1) / in.size()), lat = std::min(lat, Ns(t1, t2) / in.size()), sink ^= x ^ c;
    }
    return {tp, lat};
}
// Copies the inputs one after the other, so a pass reads them sequentially instead of across the dump.
static std::vector<std::span<const u8>> Pack(const std::vector<std::span<const u8>>& in, std::vector<u8>& store) {
    size_t n = 0;
    for (auto& s : in) n += (s.size() + 15) & ~size_t(15);
    store.assign(n, 0);
    std::vector<std::span<const u8>> out;
    size_t at = 0;
    for (auto& s : in) std::memcpy(&store[at], s.data(), s.size()), out.push_back({&store[at], s.size()}), at += (s.size() + 15) & ~size_t(15);
    return out;
}

static void Hashes(const Dump& d0) {
    std::vector<std::span<const u8>> spec, ps, pkey;
    std::vector<u8> st[7];
    std::vector<std::array<u64, 2>> keys;
    keys.reserve(d0.draws.size());
    for (auto& w : d0.draws) {
        spec.push_back({(const u8*)w.spec.data(), w.spec.size() * sizeof(Gcn::SlotSpec)});
        if (w.stage == 1) ps.push_back({(const u8*)(w.ps.data() + 3), 128});
        keys.push_back({w.pkey, w.key}), pkey.push_back({(const u8*)keys.back().data(), 16});
    }
    spec = Pack(spec, st[0]), ps = Pack(ps, st[1]), pkey = Pack(pkey, st[2]);
    struct {
        std::vector<std::span<const u8>> tex, smp, pieces, code;
    } d{Pack(d0.tex, st[3]), Pack(d0.smp, st[4]), Pack(d0.pieces, st[5]), Pack(d0.code, st[6])};
    std::map<size_t, size_t> spec_sizes;
    size_t long_specs = 0;
    for (auto& s : spec) ++spec_sizes[s.size()], long_specs += s.size() > 240;
    std::printf("== Hashes (ns per call: independent / chained). Inputs from the dump.\n");
    std::printf("spec arrays: %zu calls, sizes", spec.size());
    for (auto [n, c] : spec_sizes) std::printf(" %zu:%zu", n, c);
    std::printf(" (%zu over 240 B: XXH3's seeded long path builds a custom secret per call)\n", long_specs);
    auto row = [](const char* name, size_t n, auto x, auto r, auto w) {
        std::printf("%-30s %8zu  XXH3 %6.2f / %6.2f  rapid %6.2f / %6.2f (%+4.0f%%)  wy %6.2f / %6.2f (%+4.0f%%)\n", name, n, x.first, x.second, r.first, r.second,
                    100 * (r.first / x.first - 1), w.first, w.second, 100 * (w.first / x.first - 1));
    };
    // Fixed sizes as the renderer calls them (constant length: the compiler specializes each hash).
    // Unseeded in the renderer: a constant seed (folded by the compiler); the chained column then only adds the xor.
    row("T# 32 B (Texture, unseeded)", d.tex.size(), Bench(d.tex, [](const u8* p, size_t, u64 s) { return XXH3_64bits(p, 32) ^ s; }),
        Bench(d.tex, [](const u8* p, size_t, u64 s) { return RapidHash(p, 32) ^ s; }), Bench(d.tex, [](const u8* p, size_t, u64 s) { return WyHash(p, 32) ^ s; }));
    row("S# 16 B (Sampler, unseeded)", d.smp.size(), Bench(d.smp, [](const u8* p, size_t, u64 s) { return XXH3_64bits(p, 16) ^ s; }),
        Bench(d.smp, [](const u8* p, size_t, u64 s) { return RapidHash(p, 16) ^ s; }), Bench(d.smp, [](const u8* p, size_t, u64 s) { return WyHash(p, 16) ^ s; }));
    row("program key 16 B (pkey)", pkey.size(), Bench(pkey, [](const u8* p, size_t, u64 s) { return XXH3_64bits_withSeed(p, 16, s | 1); }),
        Bench(pkey, [](const u8* p, size_t, u64 s) { return RapidHash(p, 16, s | 1); }), Bench(pkey, [](const u8* p, size_t, u64 s) { return WyHash(p, 16, s | 1); }));
    row("PS state 128 B", ps.size(), Bench(ps, [](const u8* p, size_t, u64 s) { return XXH3_64bits_withSeed(p, 128, s | 1); }),
        Bench(ps, [](const u8* p, size_t, u64 s) { return RapidHash(p, 128, s | 1); }), Bench(ps, [](const u8* p, size_t, u64 s) { return WyHash(p, 128, s | 1); }));
    auto var = [](const u8* p, size_t n, u64 s) { return XXH3_64bits_withSeed(p, n, s | 1); };
    row("spec array n x 14 B (seeded)", spec.size(), Bench(spec, var), Bench(spec, [](const u8* p, size_t n, u64 s) { return RapidHash(p, n, s | 1); }),
        Bench(spec, [](const u8* p, size_t n, u64 s) { return WyHash(p, n, s | 1); }));
    auto x_unseeded = Bench(spec, [](const u8* p, size_t n, u64 s) { return XXH3_64bits(p, n) ^ s; });
    std::printf("%-30s %8zu  XXH3 unseeded %6.2f / %6.2f (seed folded in after)\n", "spec array, XXH3 no seed", spec.size(), x_unseeded.first, x_unseeded.second);
    row("4 KB piece (BeginFrame)", d.pieces.size(), Bench(d.pieces, [](const u8* p, size_t, u64 s) { return XXH3_64bits(p, 4096) ^ s; }),
        Bench(d.pieces, [](const u8* p, size_t, u64 s) { return RapidHash(p, 4096) ^ s; }), Bench(d.pieces, [](const u8* p, size_t, u64 s) { return WyHash(p, 4096) ^ s; }));
    size_t code_bytes = 0;
    for (auto& c : d.code) code_bytes += c.size();
    row("program code", d.code.size(), Bench(d.code, [](const u8* p, size_t n, u64 s) { return XXH3_64bits(p, n) ^ s; }),
        Bench(d.code, [](const u8* p, size_t n, u64 s) { return RapidHash(p, n) ^ s; }), Bench(d.code, [](const u8* p, size_t n, u64 s) { return WyHash(p, n) ^ s; }));
    std::printf("program code: %zu blobs, mean %zu B\n", d.code.size(), d.code.empty() ? 0 : code_bytes / d.code.size());
}

// ---- Variant work: the renderer's lookups per draw stage, and ways to skip them ----
// A flat open-addressing table of u64 keys: no node per entry; the slot is the key times a constant (one multiply, so
// aligned addresses spread too).
template <class V> struct Flat {
    std::vector<u64> keys = std::vector<u64>(1 << 16, 0);
    std::vector<V> vals = std::vector<V>(1 << 16);
    static size_t At(u64 k) { return (k * 0x9E3779B97F4A7C15ull) >> 48; }
    V* Find(u64 k) {
        for (size_t i = At(k);; i = (i + 1) & 0xFFFF)
            if (keys[i] == k) return &vals[i];
            else if (!keys[i]) return nullptr;
    }
    V& Put(u64 k) {
        size_t i = At(k);
        while (keys[i] && keys[i] != k) i = (i + 1) & 0xFFFF;
        return keys[i] = k, vals[i];
    }
};
struct Ident {
    size_t operator()(u64 k) const { return (size_t)k; }
};

// What one draw stage's variant key and slot elements are a function of: program identity (address; content and fetch
// shader through the program key), RSRC1/2, rect, the PS/CS registers the key reads, every word of each slot's T#/S#
// (its bindless element depends on all of them), the format word of each V# format load (BufferSpec reads only that
// one), and the texture epoch (an eviction or a new render target changes elements and sub-image extents).
static u32 Blob(const Draw& w, u32* b) {
    u32 n = 0;
    for (u32 v : {(u32)w.prog, (u32)(w.prog >> 32), (u32)w.pkey, (u32)(w.pkey >> 32), w.rsrc1, w.rsrc2, w.rect, w.epoch}) b[n++] = v;
    if (w.stage != 0) std::memcpy(b + n, w.ps.data(), 46 * 4), n += 46;
    for (u32 k = 0; k < w.nslots; ++k) {
        const u32* s = &w.slots[9 * k];
        const u32 m = (s[0] & 0xFF) == 2 ? 5 : 9;
        std::memcpy(b + n, s, m * 4), n += m;
    }
    for (u32 k = 0; k < w.nfmts; ++k) b[n++] = w.fmts[5 * k], b[n++] = w.fmts[5 * k + 4];
    return n;
}

struct Out {
    u64 key;
    std::vector<u32> elems;
};
enum class Hash { Xxh3, Rapid };
enum class Map { Std, Ident, Flat };

// The renderer's current work per draw stage (Prepare + Shader's lookups), with a choice of hash and map.
template <Hash H, Map M> struct Current {
    template <class V> using Table = std::conditional_t<M == Map::Std, std::unordered_map<u64, V>, std::conditional_t<M == Map::Ident, std::unordered_map<u64, V, Ident>, Flat<V>>>;
    Table<u32> prog_memo, slots, tex, smp;
    Table<u64> shaders;
    std::vector<Gcn::SlotSpec> spec;
    std::vector<const u32*> desc;
    static u64 Hs(const void* p, size_t n, u64 s) {
        if constexpr (H == Hash::Xxh3) return XXH3_64bits_withSeed(p, n, s);
        else return RapidHash(p, n, s);
    }
    template <class T> static auto Find(T& t, u64 k) {
        if constexpr (M == Map::Flat) return t.Find(k);
        else {
            auto it = t.find(k);
            return it == t.end() ? nullptr : &it->second;
        }
    }
    template <class T> static auto& Put(T& t, u64 k) {
        if constexpr (M == Map::Flat) return t.Put(k);
        else return t[k];
    }
    u32 epoch = ~0u;
    // Returns the key; fills `elems` as Shader's table tail.
    u64 Run(const Draw& w, std::vector<u32>& elems) {
        if (w.epoch != epoch) epoch = w.epoch, tex = {}, smp = {};  // as Memos(): elements last one texture epoch
        if (!Find(prog_memo, w.prog | w.stage)) Put(prog_memo, w.prog | w.stage) = 1;
        const u64 hashes[2]{w.pkey, w.rsrc2};
        const u64 pkey = Hs(hashes, 16, (u64)w.stage << 32 | w.rsrc2);
        if (!Find(slots, pkey)) Put(slots, pkey) = 1;
        spec.clear(), desc.clear();
        for (u32 k = 0; k < w.nslots; ++k) {
            const u32* s = &w.slots[9 * k];
            const u32 kind = s[0] & 0xFF;
            spec.push_back(!(s[0] >> 16 & 1) ? Gcn::SlotSpec{} : kind == 2 ? Gcn::SamplerSpec(s + 1) : Gcn::TextureSpec(s + 1));
            desc.push_back(s + 1);
        }
        for (u32 k = 0; k < w.nfmts; ++k) spec.push_back(w.fmts[5 * k] >> 16 & 1 ? Gcn::BufferSpec(&w.fmts[5 * k + 1]) : Gcn::SlotSpec{});
        u64 key = Hs(spec.data(), spec.size() * sizeof(Gcn::SlotSpec), pkey ^ w.rsrc1 ^ (w.rect ? 1ull << 63 : 0));
        if (w.stage == 1) key = Hs(&w.ps[3], 128, Hs(&w.ps[35], 16, key ^ w.ps[0] ^ (u64)w.ps[1] << 32 ^ w.ps[2]));
        elems.clear();
        for (u32 k = 0; k < w.nslots; ++k) {
            const u32 kind = w.slots[9 * k] & 0xFF, vkey = kind | (u32)w.spec[k].dim << 4 | (u32)w.spec[k].cls << 8 | (u32)w.spec[k].srgb << 12;  // binding
            const u64 h = kind == 2 ? Hs(desc[k], 16, 0) : Hs(desc[k], 32, 0) + vkey * 0x9E3779B97F4A7C15ull;
            auto& t = kind == 2 ? smp : tex;
            u32* e = Find(t, h);
            if (!e) e = &(Put(t, h) = w.elems[k]);
            elems.push_back(*e);
        }
        if (!Find(shaders, w.key)) Put(shaders, w.key) = w.key;
        return w.key;  // the renderer's own key (the simulated hashes above only cost what they cost)
    }
};

// A cached draw stage: its inputs (Blob) and what they gave. Fixed capacity: no allocation per draw.
struct Entry {
    static constexpr u32 kCap = 256, kElems = 64;
    u32 n = 0, ne = 0;
    u64 key = 0;
    u32 blob[kCap], elems[kElems];
    bool Same(const u32* b, u32 m) const { return n == m && !std::memcmp(blob, b, m * 4); }
    void Set(const u32* b, u32 m, u64 k, const u32* e, u32 c) { n = m, key = k, ne = c, std::memcpy(blob, b, m * 4), std::memcpy(elems, e, c * 4); }
};

static void Variants(const Dump& d) {
    if (d.draws.empty()) return;
    // Facts first: how often each way of skipping finds the same inputs, and whether same inputs ever gave another result.
    std::unordered_map<u64, std::vector<u32>> by_prog;
    std::unordered_map<u64, std::vector<std::vector<u32>>> last_k;  // the program's last 8 distinct inputs, newest first
    size_t hit_k[4]{};                                               // in its last 1, 2, 4, 8
    std::vector<std::vector<u32>> by_pos[3];
    std::unordered_map<std::string, Out> seen;
    size_t n = 0, hit_pos = 0, hit_prog = 0, hit_either = 0, same_key_diff_elems = 0, same_blob_diff_key = 0, user_same_pos = 0, too_big = 0, max_blob = 0;
    std::vector<u32> b(4096);
    u32 first = d.draws.front().frame;
    std::map<u32, size_t> per_frame;
    std::set<u32> epochs;
    std::vector<const Draw*> prev_user(3 * 8192, nullptr);
    for (auto& w : d.draws) {
        ++per_frame[w.frame], epochs.insert(w.epoch);
        const u32 m = Blob(w, b.data());
        max_blob = std::max<size_t>(max_blob, m), too_big += m > Entry::kCap || w.nslots > Entry::kElems;
        const std::vector<u32> blob(b.begin(), b.begin() + m);
        const std::string s((const char*)blob.data(), m * 4);
        if (auto it = seen.find(s); it != seen.end()) same_blob_diff_key += it->second.key != w.key, same_key_diff_elems += it->second.elems != w.elems;
        else seen[s] = {w.key, w.elems};
        auto& v = by_pos[w.stage];
        if (v.size() <= w.seq) v.resize(w.seq + 1);
        if (w.frame != first) {
            ++n;
            const bool p1 = v[w.seq] == blob, p2 = by_prog[w.prog | w.stage] == blob;
            hit_pos += p1, hit_prog += p2, hit_either += p1 || p2;
            if (const Draw* u = w.seq < 8192 ? prev_user[w.stage * 8192 + w.seq] : nullptr; u && u->user == w.user && u->prog == w.prog) ++user_same_pos;
            auto& l = last_k[w.prog | w.stage];
            const size_t at = std::find(l.begin(), l.end(), blob) - l.begin();
            for (int j = 0; j < 4; ++j) hit_k[j] += at < (1u << j);
        }
        if (auto& l = last_k[w.prog | w.stage]; true) {
            std::erase(l, blob), l.insert(l.begin(), blob);
            if (l.size() > 8) l.pop_back();
        }
        if (w.seq < 8192) prev_user[w.stage * 8192 + w.seq] = &w;
        v[w.seq] = blob, by_prog[w.prog | w.stage] = blob;
    }
    size_t slots = 0, tex = 0, depth0 = 0;
    for (auto& w : d.draws)
        for (u32 k = 0; k < w.nslots; ++k) ++slots, tex += (w.slots[9 * k] & 0xFF) != 2, depth0 += (w.slots[9 * k] >> 8 & 0xFF) == 0;
    std::printf("\n== Variants: %zu draw stages in %zu frames (%zu after the first frame), %zu slots (%zu textures/storage), %zu slot descriptors inline in "
                "user data; %zu texture epochs; largest input %zu words (%zu over the entry capacity)\n",
                d.draws.size(), per_frame.size(), n, slots, tex, depth0, epochs.size(), max_blob, too_big);
    std::printf("same inputs as the same draw position last frame: %.2f%%\n", 100.0 * hit_pos / n);
    std::printf("same inputs as the program's last draw:            %.2f%%\n", 100.0 * hit_prog / n);
    std::printf("either:                                            %.2f%%\n", 100.0 * hit_either / n);
    std::printf("among the program's last 1 / 2 / 4 / 8 distinct inputs: %.2f%% / %.2f%% / %.2f%% / %.2f%%\n", 100.0 * hit_k[0] / n, 100.0 * hit_k[1] / n,
                100.0 * hit_k[2] / n, 100.0 * hit_k[3] / n);
    std::printf("same user data and program as last frame's position: %.2f%%\n", 100.0 * user_same_pos / n);
    // Live, each of the 6 recording threads keeps its own caches and takes the next block when it is free: each frame cut
    // in 24 runs of draws (4 per thread), every run on a random thread; caches per thread, the program's last 4 inputs.
    {
        std::vector<std::unordered_map<u64, std::vector<std::vector<u32>>>> per(6);
        std::map<u32, u32> draws_in;
        for (auto& w : d.draws) draws_in[w.frame] = std::max(draws_in[w.frame], w.seq + 1);
        u64 rng = 12345, hits = 0, calls = 0;
        u32 cur_frame = ~0u;
        std::vector<u32> owner(24);
        for (auto& w : d.draws) {
            if (w.frame != cur_frame) {
                cur_frame = w.frame;
                for (auto& o : owner) rng = rng * 6364136223846793005ull + 1442695040888963407ull, o = u32(rng >> 33) % 6;
            }
            const u32 m = Blob(w, b.data());
            const std::vector<u32> blob(b.begin(), b.begin() + m);
            auto& l = per[owner[std::min<u32>(23, w.seq * 24 / draws_in[w.frame])]][w.prog | w.stage];
            if (w.frame != first) ++calls, hits += std::find(l.begin(), l.end(), blob) != l.end();
            std::erase(l, blob), l.insert(l.begin(), blob);
            if (l.size() > 4) l.pop_back();
        }
        std::printf("6 recording threads, own caches, blocks handed out at random: program's last 4 inputs %.2f%%\n", 100.0 * hits / calls);
    }
    std::printf("correctness: same inputs, other key %zu; same inputs, other slot elements %zu\n", same_blob_diff_key, same_key_diff_elems);

    // Timings: steady state (frames after the first), ns per draw stage, best of 5 passes.
    std::vector<const Draw*> steady;
    for (auto& w : d.draws)
        if (w.frame != first) steady.push_back(&w);
    auto time = [&](const char* name, auto make) {
        double best = 1e30;
        u64 wrong = 0;
        for (int rep = 0; rep < 5; ++rep) {
            auto state = make();
            for (auto& w : d.draws)  // warm: the first frame fills the memos
                if (w.frame == first) state->Step(w);
            auto t0 = Clock::now();
            for (const Draw* w : steady) sink ^= state->Step(*w);
            best = std::min(best, Ns(t0, Clock::now()) / steady.size()), wrong = state->wrong;
        }
        std::printf("%-58s %7.1f ns/draw stage  (%.2f ms per 2650-draw frame, both stages)%s\n", name, best, best * 2 * 2650 * 1e-6,
                    wrong ? " WRONG RESULTS" : "");
    };
    std::printf("\n-- simulated cost (Locate reads, texture checks and constant copies are the same in all and left out)\n");
    auto current = [&]<Hash H, Map M>(const char* name) {
        struct S {
            Current<H, M> c;
            std::vector<u32> e;
            u64 wrong = 0;
            u64 Step(const Draw& w) { return c.Run(w, e) ^ e.size(); }
        };
        time(name, [] { return std::make_unique<S>(); });
    };
    current.template operator()<Hash::Xxh3, Map::Std>("current: XXH3, std::unordered_map (std::hash)");
    current.template operator()<Hash::Rapid, Map::Std>("current: rapidhash, std::unordered_map");
    current.template operator()<Hash::Xxh3, Map::Ident>("current: XXH3, unordered_map with identity hash");
    current.template operator()<Hash::Xxh3, Map::Flat>("current: XXH3, flat open-addressing table");
    current.template operator()<Hash::Rapid, Map::Flat>("current: rapidhash, flat table");
    // Skipping: the inputs compared word for word against a cached entry; a hit reuses key and elements, a miss runs the
    // current work (rapidhash + flat table) and refills the entry.
    auto skipping = [&](const char* name, int mode) {  // 0 position, 1 program, 2 position then program
        struct S {
            int mode;
            Current<Hash::Xxh3, Map::Flat> c;
            std::vector<Entry> pos[3] = {std::vector<Entry>(4096), std::vector<Entry>(4096), std::vector<Entry>(4096)};
            std::vector<Entry> progs = std::vector<Entry>(4096);
            std::vector<std::array<Entry, 4>> sets = std::vector<std::array<Entry, 4>>(4096);
            std::vector<u8> rr = std::vector<u8>(4096);
            Flat<u32> prog;
            u32 next = 0, b[4096];
            std::vector<u32> e;
            u64 wrong = 0;
            u64 Step(const Draw& w) {
                const u32 m = Blob(w, b);  // in the renderer: the words Locate reads, gathered as they are read
                Entry* pe = nullptr;
                u32 pi = 0;
                if (mode != 0) {
                    u32* i = prog.Find(w.prog | w.stage);
                    pe = &progs[pi = i ? *i : (prog.Put(w.prog | w.stage) = next++)];
                }
                if (mode == 3) {  // the program's last 4 inputs, replaced round robin
                    for (Entry& x : sets[pi])
                        if (x.Same(b, m)) return wrong += x.key != w.key || x.ne != w.elems.size() || std::memcmp(x.elems, w.elems.data(), x.ne * 4), x.key;
                    const u64 k = c.Run(w, e);
                    if (m <= Entry::kCap && e.size() <= Entry::kElems) sets[pi][rr[pi]++ & 3].Set(b, m, k, e.data(), (u32)e.size());
                    return k;
                }
                Entry& v = pos[w.stage][w.seq & 4095];
                const Entry* hit = mode != 1 && v.Same(b, m) ? &v : pe && pe->Same(b, m) ? pe : nullptr;
                if (hit) {
                    wrong += hit->key != w.key || hit->ne != w.elems.size() || std::memcmp(hit->elems, w.elems.data(), hit->ne * 4);
                    if (mode == 2 && hit != &v) v.Set(b, m, hit->key, hit->elems, hit->ne);
                    return hit->key;
                }
                const u64 k = c.Run(w, e);
                if (m <= Entry::kCap && e.size() <= Entry::kElems) {
                    if (mode != 1) v.Set(b, m, k, e.data(), (u32)e.size());
                    if (pe) pe->Set(b, m, k, e.data(), (u32)e.size());
                }
                return k;
            }
        };
        time(name, [mode] { auto s = std::make_unique<S>(); s->mode = mode; return s; });
    };
    skipping("skip: same draw position last frame (memcmp)", 0);
    skipping("skip: program's last inputs (memcmp)", 1);
    skipping("skip: position, then program's last", 2);
    skipping("skip: program's last 4 inputs", 3);
}

// ---- Barriers: the renderer's (hazard tracking) against the engine's sync packets, per frame ----
static void Barriers(const Dump& d) {
    struct F {
        u32 frame = 0;
        std::vector<Mark> m;
    };
    std::vector<F> frames;
    for (auto& m : d.marks)
        if (m.type == 9) frames.push_back({m.a});
        else if (!frames.empty()) frames.back().m.push_back(m);
    if (frames.size() < 2) return;
    // Engine: EVENT_WRITE 0x16 (+ ACQUIRE_MEM 86c47fc0) = RT -> texture; 0x2A = depth flush at RT change; 0x2C = HTILE;
    // EOS 0x2F + ACQUIRE 80c40000 = compute done. Ours: full (wide or fragment) and attachment barriers.
    size_t nf = 0, eng16 = 0, eng2a = 0, eng2c = 0, eos = 0, full = 0, frag = 0, pass = 0, passes = 0, matched16 = 0, ours_near_engine = 0, ours_near_any = 0,
           any_points = 0;
    std::map<u32, size_t> lines, lines_at;  // our full barriers per source line, and those at an engine sync point
    for (auto& f : frames) {
        if (f.m.empty()) continue;
        ++nf;
        std::vector<u32> e16, ours, any;
        for (auto& m : f.m) {
            if (m.type == 7 && (m.a == 0x48 || m.a == 0x58 || (m.a == 0x46 && (m.b == 0x16 || m.b == 0x2A || m.b == 0x2C)))) any.push_back(m.seq);
            if (m.type == 7 && m.a == 0x46 && m.b == 0x16) ++eng16, e16.push_back(m.seq);
            if (m.type == 7 && m.a == 0x46 && m.b == 0x2A) ++eng2a;
            if (m.type == 7 && m.a == 0x46 && m.b == 0x2C) ++eng2c;
            if (m.type == 7 && m.a == 0x48) ++eos;
            if (m.type == 6 && m.a < 2) ++(m.a ? frag : full), ++lines[m.b], ours.push_back(m.seq);
            if (m.type == 6 && m.a == 2) ++pass;
            if (m.type == 8) ++passes;
        }
        // An engine dependency before draw k sits at seq k; our barrier for draw k is recorded inside it, at k + 1.
        for (u32 s : e16) matched16 += std::any_of(ours.begin(), ours.end(), [&](u32 o) { return o == s || o == s + 1; });
        for (u32 o : ours) ours_near_engine += std::any_of(e16.begin(), e16.end(), [&](u32 s) { return o == s || o == s + 1; });
        for (u32 o : ours) ours_near_any += std::any_of(any.begin(), any.end(), [&](u32 s) { return o == s || o == s + 1; });
        for (auto& m : f.m)
            if (m.type == 6 && m.a < 2) lines_at[m.b] += std::any_of(any.begin(), any.end(), [&](u32 s) { return m.seq == s || m.seq == s + 1; });
        std::sort(any.begin(), any.end()), any_points += std::unique(any.begin(), any.end()) - any.begin();
    }
    std::printf("\n== Barriers over %zu frames (per frame)\n", nf);
    std::printf("engine: RT->texture (0x16 + acquire) %.1f, depth flush at RT change (0x2A) %.1f, HTILE (0x2C) %.1f, compute done (EOS) %.1f\n",
                double(eng16) / nf, double(eng2a) / nf, double(eng2c) / nf, double(eos) / nf);
    std::printf("ours: full %.1f (fragment-only %.1f), attachment %.1f, rendering passes %.1f\n", double(full) / nf, double(frag) / nf, double(pass) / nf,
                double(passes) / nf);
    std::printf("engine RT->texture points with one of our full barriers at the same draw: %.1f%%\n", eng16 ? 100.0 * matched16 / eng16 : 0.0);
    std::printf("our full barriers at an engine RT->texture point: %.1f%%; at any engine sync point: %.1f%%\n",
                full + frag ? 100.0 * ours_near_engine / (full + frag) : 0.0, full + frag ? 100.0 * ours_near_any / (full + frag) : 0.0);
    std::printf("distinct draw positions with an engine sync packet: %.1f per frame\n", double(any_points) / nf);
    std::printf("our full barriers by source line (vk_gpu.cpp):");
    for (auto [l, c] : lines) std::printf(" %u:%.1f(%.0f%% at engine sync)", l, double(c) / nf, 100.0 * lines_at[l] / c);
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) return std::printf("varbench <dump.bin> [hash|variants|barriers|all] | varbench tsc\n"), 1;
    if (!std::strcmp(argv[1], "tsc")) {  // TSC ticks per ns, to read GOW3_TEMP_VARTIME's cycle counts
        const auto t0 = Clock::now();
        const u64 c0 = __rdtsc();
        while (Ns(t0, Clock::now()) < 3e8) {}
        return std::printf("tsc %.4f ticks/ns\n", double(__rdtsc() - c0) / Ns(t0, Clock::now())), 0;
    }
    Dump d;
    if (!Load(argv[1], d)) return std::printf("cannot read %s\n", argv[1]), 1;
    const std::string what = argc > 2 ? argv[2] : "all";
    std::printf("dump: %zu draw stages, %zu T#, %zu S#, %zu pieces, %zu programs, %zu marks\n", d.draws.size(), d.tex.size(), d.smp.size(), d.pieces.size(),
                d.code.size(), d.marks.size());
    if (what == "hash" || what == "all") Hashes(d);
    if (what == "variants" || what == "all") Variants(d);
    if (what == "barriers" || what == "all") Barriers(d);
    return (int)(sink & 0);
}
