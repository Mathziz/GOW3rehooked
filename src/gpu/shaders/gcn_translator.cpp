// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shaders/gcn_translator.h"
#include "gpu/shaders/gcn_internal.h"
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace Gcn {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

SlotSpec TextureSpec(const u32* t) {
    SlotSpec s;
    switch (t[3] >> 28) {
    case 8: s.dim = Dim::D1; break;
    case 10: s.dim = Dim::D3; break;
    case 11: s.dim = Dim::Cube; break;
    case 12: s.dim = Dim::D1Array; break;
    case 13: s.dim = Dim::D2Array; break;
    case 14: s.dim = t[3] >> 16 & 0xF ? Dim::D2Ms : Dim::D2; break;  // LAST_LEVEL = log2(samples): one sample is plain 2D
    case 15: s.dim = t[3] >> 16 & 0xF ? Dim::D2MsArray : Dim::D2Array; break;
    default: s.dim = Dim::D2;
    }
    s.msaa4 = (s.dim == Dim::D2Ms || s.dim == Dim::D2MsArray) && (t[3] >> 16 & 0xF) == 2;
    const u32 nfmt = t[1] >> 26 & 0xF;
    s.cls = nfmt == 4 ? Class::Uint : nfmt == 5 ? Class::Sint : Class::Float;
    s.srgb = nfmt == 9;
    return s;
}
SlotSpec SamplerSpec(const u32* s) {
    SlotSpec r;
    r.unnormalized = s[0] >> 15 & 1;
    r.degamma = s[0] >> 20 & 1;
    return r;
}
SlotSpec BufferSpec(const u32* v) {
    SlotSpec r;
    r.vfmt = true, r.dfmt = u8(v[3] >> 15 & 0xF), r.nfmt = u8(v[3] >> 12 & 7), r.dst_sel = u16(v[3] & 0xFFF);
    return r;
}

namespace {
constexpr u32 kFetchBase = 0x100000;  // pc of inlined fetch shader instructions

// Decodes from the start up to the s_endpgm that no earlier branch jumps past (fetch: up to s_setpc_b64).
std::string Decode(std::span<const u32> code, u32 base, std::span<const u32> fetch, std::vector<Inst>& out) {
    const bool is_fetch = base == kFetchBase;
    u32 reach = 0;
    for (u32 i = 0; i < code.size();) {
        const u32 x = code[i];
        auto word = [&](u32 k) { return i + k < code.size() ? code[i + k] : 0u; };
        Inst in;
        in.pc = base + i, in.w0 = x, in.w1 = word(1);
        u32 size = 1;
        auto literal = [&](u32 s) {
            if (s == 255) in.lit = word(1), size = 2;
        };
        if (x >> 23 == 0x17D) {
            in.enc = SOP1, in.op = x >> 8 & 0xFF, in.dst = x >> 16 & 0x7F, in.src[0] = x & 0xFF;
            literal(in.src[0]);
        } else if (x >> 23 == 0x17E) {
            in.enc = SOPC, in.op = x >> 16 & 0x7F, in.src[0] = x & 0xFF, in.src[1] = x >> 8 & 0xFF;
            literal(in.src[0]), literal(in.src[1]);
        } else if (x >> 23 == 0x17F) {
            in.enc = SOPP, in.op = x >> 16 & 0x7F, in.lit = x & 0xFFFF;
        } else if (x >> 28 == 0xB) {
            in.enc = SOPK, in.op = x >> 23 & 0x1F, in.dst = x >> 16 & 0x7F, in.lit = x & 0xFFFF;
            if (in.op == 0x15) in.lit = word(1), size = 2;
        } else if (x >> 30 == 2) {
            in.enc = SOP2, in.op = x >> 23 & 0x7F, in.dst = x >> 16 & 0x7F, in.src[0] = x & 0xFF, in.src[1] = x >> 8 & 0xFF;
            literal(in.src[0]), literal(in.src[1]);
        } else if (x >> 27 == 0x18) {
            in.enc = SMRD, in.op = x >> 22 & 0x1F, in.dst = x >> 15 & 0x7F, in.src[0] = (x >> 9 & 0x3F) * 2, in.src[1] = x & 0xFF;
            in.imm = x >> 8 & 1;
            if (!in.imm && in.src[1] == 255) in.lit = word(1), size = 2;  // CI: 32-bit literal dword offset
        } else if (x >> 25 == 0x3F) {
            in.enc = VALU, in.op = 384 + (x >> 9 & 0xFF), in.dst = x >> 17 & 0xFF, in.src[0] = x & 0x1FF;
            literal(in.src[0]);
        } else if (x >> 25 == 0x3E) {
            in.enc = VALU, in.op = x >> 17 & 0xFF, in.src[0] = x & 0x1FF, in.src[1] = 256 + (x >> 9 & 0xFF), in.sdst = 106;
            literal(in.src[0]);
        } else if (x >> 31 == 0) {
            const u32 op = x >> 25 & 0x3F;
            in.enc = VALU, in.op = 256 + op, in.dst = x >> 17 & 0xFF, in.src[0] = x & 0x1FF, in.src[1] = 256 + (x >> 9 & 0xFF);
            if (op == 1 || op == 2) in.src[1] = x >> 9 & 0xFF;  // v_readlane/v_writelane: the lane is a scalar operand
            if (op == 0) in.src[2] = 106;                       // v_cndmask_b32 by VCC
            if (op >= 37 && op <= 42) in.sdst = 106, in.src[2] = 106;  // carry out (and in) through VCC
            literal(in.src[0]);
            if (op == 32 || op == 33) in.lit = word(1), size = 2;  // v_madmk/v_madak constant
        } else {
            switch (x >> 26) {
            case 0x34: {
                const u32 y = word(1);
                in.enc = VALU, in.vop3 = true, in.op = x >> 17 & 0x1FF, in.dst = x & 0xFF, in.abs = x >> 8 & 7, in.clamp = x >> 11 & 1;
                in.src[0] = y & 0x1FF, in.src[1] = y >> 9 & 0x1FF, in.src[2] = y >> 18 & 0x1FF, in.omod = y >> 27 & 3, in.neg = y >> 29 & 7;
                if (in.op < 256) in.sdst = x & 0xFF;
                if ((in.op >= 293 && in.op <= 298) || in.op == 365 || in.op == 366) in.sdst = x >> 8 & 0x7F, in.abs = 0, in.clamp = 0;
                size = 2;
                break;
            }
            case 0x32: in.enc = VINTRP, in.op = x >> 16 & 3; break;
            case 0x36: in.enc = DS, in.op = x >> 18 & 0xFF, size = 2; break;
            case 0x38: in.enc = MUBUF, in.op = x >> 18 & 0x7F, size = 2; break;
            case 0x3A: in.enc = MTBUF, in.op = x >> 16 & 7, size = 2; break;
            case 0x3C: in.enc = MIMG, in.op = x >> 18 & 0x7F, size = 2; break;
            case 0x3E: in.enc = EXP, size = 2; break;
            default: return std::format("unknown encoding {:08x} at {:#x}", x, in.pc);
            }
        }
        i += size;
        if (in.enc == SOP1 && in.op == 33 && !is_fetch) {  // s_swappc_b64: inline the fetch shader
            if (fetch.empty()) return "fetch shader missing";
            if (auto e = Decode(fetch, kFetchBase, {}, out); !e.empty()) return "fetch: " + e;
            continue;
        }
        if (is_fetch && in.enc == SOP1 && in.op == 32) return {};  // s_setpc_b64: back to the vertex shader
        if (IsBranch(in)) reach = std::max(reach, Target(in));
        out.push_back(in);
        if (in.enc == SOPP && in.op == 1 && in.pc >= reach) return {};
    }
    return is_fetch ? "fetch shader without s_setpc_b64" : "program without s_endpgm";
}

// Advances a descriptor location by `n` dwords.
Path Next(Path p, u32 n) {
    if (!p.depth) p.reg = u8(p.reg + n);
    else p.offset[p.depth - 1] += 4 * n;
    return p;
}
bool Is64(const Inst& i) {
    if (i.enc == SOP1) return i.op == 4 || i.op == 6 || i.op == 8 || i.op == 10 || i.op == 12 || i.op == 28 || i.op == 30 || i.op == 31 ||
                              (i.op >= 33 && i.op <= 43) || i.op == 45 || i.op == 47 || i.op == 49;
    if (i.enc == SOP2) return i.op == 11 || (i.op >= 15 && i.op <= 37 && (i.op & 1)) || i.op == 41 || i.op == 42;
    return false;
}
// SGPRs an instruction writes: first, count.
std::pair<u32, u32> Sdst(const Inst& i) {
    switch (i.enc) {
    case SOP1: case SOP2: return {i.dst, Is64(i) ? 2u : 1u};
    case SOPK: return {i.dst, i.op < 3 || i.op > 14 ? 1u : 0u};
    case SMRD: return {i.dst, i.op <= 4 ? 1u << i.op : i.op >= 8 && i.op <= 12 ? 1u << (i.op - 8) : 2u};
    case VALU: return i.op == 257 || i.op == 386 ? std::pair{u32(i.dst), 1u} : i.op < 256 || (i.op >= 293 && i.op <= 298) ? std::pair{u32(i.sdst), 2u} : std::pair{0u, 0u};
    default: return {0u, 0u};
    }
}
// VGPRs an instruction may write: first, count (loads count their widest result).
std::pair<u32, u32> VWrites(const Inst& i) {
    switch (i.enc) {
    case VALU: return i.op < 256 || i.op == 257 || i.op == 386 ? std::pair{0u, 0u} : std::pair{u32(i.dst), 1u};  // 32-bit results (the emitter has no others)
    case VINTRP: return {i.w0 >> 18 & 0xFF, 1u};
    case DS: return {i.w1 >> 24, 4u};
    case MUBUF: case MTBUF: {
        static constexpr u32 raw[]{1, 2, 4, 3};
        if (i.op >= 4 && i.op < 8) return {0u, 0u};  // format stores
        const u32 n = i.op < 4 ? i.op + 1 : i.enc == MUBUF && i.op >= 12 && i.op < 16 ? raw[i.op - 12] : 4u;
        return {i.w1 >> 8 & 0xFF, n + (i.w1 >> 23 & 1)};
    }
    case MIMG: return {i.w1 >> 8 & 0xFF, (i.op >= 64 ? 4u : std::max<u32>(1, std::popcount(i.w0 >> 8 & 0xFu))) + (i.w0 >> 16 & 1)};
    default: return {0u, 0u};
    }
}
constexpr u32 kConstBytes = 4096;  // a program's constants in its draw table, at most (the prelude's Table0/Table1 hold 4096 dwords)
} // namespace

// A straight-line depth resolve: the same texel's samples 0 and 1 feed a float maximum exported as depth.
// Track copied VGPR values so register allocation and intervening independent arithmetic do not matter.
DepthResolvePattern FindDepthResolve(const Program& p) {
    if (p.exec_dynamic) return {};
    std::array<u32, 256> v{};
    for (u32 i = 0; i < v.size(); ++i) v[i] = i + 1;
    constexpr u32 zero = 257, one = 258;
    u32 next = 259, loads = 0, x = 0, y = 0, a = 0, b = 0, maximum = 0;
    DepthResolvePattern found;
    bool exported = false;
    const auto source = [&](u32 s) { return s >= 256 && s < 512 ? v[s - 256] : s == 128 ? zero : s == 129 ? one : next++; };
    for (const Inst& i : p.insts) {
        if (IsBranch(i) || WritesExec(i)) return {};
        if (i.enc == MIMG) {
            const u32 address = i.w1 & 0xff, dst = i.w1 >> 8 & 0xff;
            if (loads >= 2 || i.op != 0 || (i.w0 & 0x1ffff) != 0x1100 || i.tex < 0 || address > 253 ||
                v[address + 2] != (loads ? one : zero)) return {};
            if (!loads) x = v[address], y = v[address + 1], found.load_pc = i.pc, found.slot = i.tex;
            else if (i.tex != found.slot || v[address] != x || v[address + 1] != y) return {};
            v[dst] = next++;
            (loads++ ? b : a) = v[dst];
            continue;
        }
        if (i.enc == VALU && i.op == 272 && loads == 2 && !maximum && !i.abs && !i.neg && !i.clamp && !i.omod) {
            const u32 l = source(i.src[0]), r = source(i.src[1]);
            if ((l == a && r == b) || (l == b && r == a)) {
                maximum = next++, v[i.dst] = maximum, found.max_pc = i.pc;
                continue;
            }
        }
        if (i.enc == EXP && (i.w0 >> 4 & 0x3f) == 8 && (i.w0 & 1)) {
            if (!maximum || v[i.w1 & 0xff] != maximum || exported) return {};
            exported = true;
        } else if (maximum && i.enc == EXP) {
            for (u32 k = 0; k < 4; ++k)
                if ((i.w0 >> k & 1) && v[i.w1 >> (8 * k) & 0xff] == maximum) return {};
        } else if (maximum && i.enc == VALU) {
            for (u32 s : i.src)
                if (s >= 256 && s < 512 && v[s - 256] == maximum) return {};
        } else if (maximum && i.enc != SOPP) {
            return {};
        }
        if (i.enc == VALU && i.op == 385 && !i.abs && !i.neg && !i.clamp && !i.omod)
            v[i.dst] = source(i.src[0]);
        else for (auto [r, n] = VWrites(i); n && r < v.size(); --n, ++r) v[r] = next++;
    }
    return loads == 2 && exported ? found : DepthResolvePattern{};
}

Analysis Analyze(const Input& in) {
    Analysis a;
    auto p = std::make_shared<Program>();
    a.error = Decode(in.code, 0, in.fetch, p->insts);
    if (!a.error.empty()) return a;
    p->user_sgprs = in.rsrc2 >> 1 & 0x1F;
    std::set<u32> pcs;
    for (auto& i : p->insts) pcs.insert(i.pc);
    for (auto& i : p->insts)
        if (IsBranch(i)) {
            if (!pcs.count(Target(i))) return a.error = std::format("branch at {:#x} to {:#x} outside the program", i.pc, Target(i)), a;
            p->labels.insert(Target(i));
        }

    // EXEC changes that matter per lane. The pixel shader prologue (save EXEC, s_wqm_b64) and the restore from that copy
    // keep every invocation active, as an all-ones EXEC: no write needs predication then.
    std::map<u32, u32> writes;
    for (auto& i : p->insts)
        if (i.enc == SOP1 || i.enc == SOP2 || i.enc == SOPK) writes[i.dst] += 1, writes[i.dst + 1] += Is64(i);
    std::set<u32> copies;
    for (auto& i : p->insts) {
        if (i.enc == SOP1 && i.op == 4 && i.src[0] == 126 && i.dst < 104 && !p->exec_dynamic && writes[i.dst] == 1 && writes[i.dst + 1] == 1)
            copies.insert(i.dst);
        else if (i.enc == SOP1 && i.op == 10 && i.dst == 126 && i.src[0] == 126) continue;
        else if (i.enc == SOP1 && i.op == 4 && i.dst == 126 && copies.count(i.src[0])) continue;
        else if (i.enc == SOP2 && i.op == 15 && i.dst == 126 && ((i.src[0] == 126 && copies.count(i.src[1])) || (i.src[1] == 126 && copies.count(i.src[0]))))
            continue;
        else if (WritesExec(i)) p->exec_dynamic = true;
    }

    // Within one block and one EXEC mask, non-fragment consumers can use VCC's compare predicate directly.
    // Fragment helpers need the ballot's participation semantics, even for a subsequent per-lane read.
    if (in.stage != Stage::Pixel) {
        bool scalar_vcc = true;
        for (auto it = p->insts.rbegin(); it != p->insts.rend(); ++it) {
            auto& i = *it;
            if (IsBranch(i) || WritesExec(i) || (i.enc != VALU && i.enc != SOPP && i.enc != EXP)) scalar_vcc = true;
            else if (i.enc == SOPP && i.op == 1) scalar_vcc = false;
            else if (i.enc == VALU) {
                const auto [r, n] = Sdst(i);
                if (r < 108 && r + n > 106) {
                    if (i.op < 256 && r == 106) i.lane_only = !scalar_vcc;
                    scalar_vcc = r != 106 || n != 2;
                }
                for (u32 k = 0; k < 3; ++k) {
                    const u32 s = i.src[k];
                    const bool mask = k == 2 && (i.op == 256 || (i.op >= 296 && i.op <= 298));
                    if (mask && s == 106) continue;
                    scalar_vcc |= s == 106 || s == 107 || s == 251 || (mask && s == 105);
                }
            }
            if (p->labels.count(i.pc)) scalar_vcc = true;
        }
    }

    // Descriptor provenance per SGPR, in program order: user data registers and loads through pointers made of them.
    // `same`: no branch joins on the way (a label another path may reach holding something else): the value is constant.
    struct Val {
        bool known = false;
        Path p;
        bool same = false;
    };
    std::array<Val, 128> sv{};
    for (u32 r = 0; r < p->user_sgprs && r < 16; ++r) sv[r] = {true, {u8(r), 0, {}}, true};
    // Per label, the SGPRs written between a branch to it and it (forward) or in the loop it starts (backward).
    std::map<u32, std::bitset<104>> joins;
    std::map<u32, std::bitset<256>> vjoins;  // the same for VGPRs (the lanes SGPRs were spilled to)
    for (auto& b : p->insts)
        if (IsBranch(b))
            for (std::bitset<104>& j = joins[Target(b)]; auto& i : p->insts)
                if (Target(b) > b.pc ? i.pc > b.pc && i.pc < Target(b) : i.pc >= Target(b) && i.pc <= b.pc) {
                    for (auto [r, n] = Sdst(i); n; --n, ++r)
                        if (r < 104) j.set(r);
                    for (auto [r, n] = VWrites(i); n; --n, ++r)
                        if (r < 256) vjoins[Target(b)].set(r);
                }
    auto same = [&](u32 r, u32 n) { return std::all_of(sv.begin() + r, sv.begin() + r + n, [](const Val& v) { return v.same; }); };
    // Constants: one group per V# or pointer, spanning at most 1 KB.
    auto group = [&](const Path& path, bool buf, u32 off, u32 n) {
        u32 total = 0;
        for (auto& c : a.consts) total += c.hi - c.lo;
        for (size_t g = 0; g < a.consts.size(); ++g)
            if (Const& c = a.consts[g]; c.path == path && c.buffer == buf) {
                const u32 lo = std::min(c.lo, off), hi = std::max(c.hi, off + 4 * n);
                if (hi - lo <= 1024 && total + (hi - lo) - (c.hi - c.lo) <= kConstBytes) return c.lo = lo, c.hi = hi, s16(g);
            }
        if (total + 4 * n > kConstBytes) return s16(-1);
        a.consts.push_back({path, off, off + 4 * n, buf});
        return s16(a.consts.size() - 1);
    };
    auto run = [&](u32 r, u32 n, Path& out) {
        if (r + n > 104 || !sv[r].known) return false;
        for (u32 k = 1; k < n; ++k)
            if (!sv[r + k].known || !(sv[r + k].p == Next(sv[r].p, k))) return false;
        return out = sv[r].p, true;
    };
    auto kill = [&](u32 r, u32 n) {
        for (u32 k = 0; k < n && r + k < sv.size(); ++k) sv[r + k].known = false;
    };
    // SGPRs spilled to VGPR lanes (v_writelane with a constant lane), by vgpr << 6 | lane; the compiler restores them with v_readlane.
    std::unordered_map<u32, Val> lanes;
    const auto forget = [&](u32 v) { std::erase_if(lanes, [&](const auto& l) { return l.first >> 6 == v; }); };
    auto slot = [&](SlotKind kind, const Path& path) {
        for (size_t k = 0; k < a.slots.size(); ++k) {
            auto& s = a.slots[k];
            if (!(s.path == path) || (s.kind == SlotKind::Sampler) != (kind == SlotKind::Sampler)) continue;
            if (kind == SlotKind::Storage) s.kind = kind;
            return s16(k);
        }
        a.slots.push_back({kind, path});
        return s16(a.slots.size() - 1);
    };
    auto buffer = [&](u32 r) {
        if (Path path; run(r, 4, path) && std::find(a.buffers.begin(), a.buffers.end(), path) == a.buffers.end()) a.buffers.push_back(path);
    };
    for (auto& i : p->insts) {
        if (auto j = joins.find(i.pc); j != joins.end())
            for (u32 r = 0; r < 104; ++r) sv[r].same &= !j->second[r];
        if (auto j = vjoins.find(i.pc); j != vjoins.end())
            std::erase_if(lanes, [&](const auto& l) { return j->second[l.first >> 6]; });
        if (!(i.enc == VALU && i.op == 258))
            for (auto [r, n] = VWrites(i); n; --n, ++r) forget(r);
        switch (i.enc) {
        case SOP1:
            if (i.op == 3 && i.src[0] < 104) sv[i.dst] = sv[i.src[0]];
            else if (i.op == 4 && i.src[0] < 103 && i.dst < 103) sv[i.dst] = sv[i.src[0]], sv[i.dst + 1] = sv[i.src[0] + 1];
            else kill(i.dst, Is64(i) ? 2 : 1);
            break;
        case SOP2: kill(i.dst, Is64(i) ? 2 : 1); break;
        case SOPK:
            if (i.op < 3 || i.op > 14) kill(i.dst, 1);
            break;
        case SMRD: {
            const u32 n = Sdst(i).second, off = (i.imm ? i.src[1] : i.lit) * 4;
            const bool fixed = i.imm || i.src[1] == 255, buf = i.op >= 8 && i.op <= 12;
            if (buf) buffer(i.src[0]);
            Path base;
            if (fixed && (buf || i.op <= 4) && run(i.src[0], buf ? 4 : 2, base) && same(i.src[0], buf ? 4 : 2)) i.tex = group(base, buf, off, n);
            if (i.op <= 4 && fixed && run(i.src[0], 2, base) && base.depth < 3) {
                const bool s = same(i.src[0], 2);
                for (u32 k = 0; k < n && i.dst + k < 104; ++k) {
                    Path q = base;
                    q.offset[q.depth++] = off + 4 * k;
                    sv[i.dst + k] = {true, q, s};
                }
            } else {
                kill(i.dst, n);
            }
            break;
        }
        case VALU:
            if (i.op < 256 || (i.op >= 293 && i.op <= 298)) kill(i.sdst, 2);
            if (const bool lane = i.src[1] >= 128 && i.src[1] < 192; i.op == 258) {
                if (lane && i.src[0] < 104 && sv[i.src[0]].known) lanes[i.dst << 6 | (i.src[1] - 128)] = sv[i.src[0]];
                else if (lane) lanes.erase(i.dst << 6 | (i.src[1] - 128));
                else forget(i.dst);
            } else if (i.op == 257) {
                const auto it = lane && i.src[0] >= 256 ? lanes.find((i.src[0] - 256) << 6 | (i.src[1] - 128)) : lanes.end();
                if (it != lanes.end() && i.dst < 104) sv[i.dst] = it->second;
                else kill(i.dst, 1);
            } else if (i.op == 386) {
                kill(i.dst, 1);
            }
            break;
        case MIMG: {
            const u32 r = (i.w1 >> 16 & 0x1F) * 4, sr = (i.w1 >> 21 & 0x1F) * 4;
            Path path;
            if (!run(r, i.w0 >> 15 & 1 ? 4 : 8, path)) return a.error = std::format("image descriptor s[{}] at {:#x} not from user data", r, i.pc), a;
            i.tex = slot(i.op >= 8 && i.op < 32 && i.op != 14 ? SlotKind::Storage : SlotKind::Texture, path);
            if (i.op >= 32) {
                if (!run(sr, 4, path)) return a.error = std::format("sampler descriptor s[{}] at {:#x} not from user data", sr, i.pc), a;
                i.smp = slot(SlotKind::Sampler, path);
                if (i.smp >= 0 && i.smp < 64) a.slots[i.tex].samplers |= 1ull << i.smp;
            }
            break;
        }
        case MUBUF: case MTBUF: {
            const bool store = i.op >= 4 && i.op < 8;  // format stores: the V# must be known (its range is mapped for the GPU)
            if (Path path; (store || (i.enc == MUBUF && i.op < 4)) && run((i.w1 >> 16 & 0x1F) * 4, 4, path)) {
                auto f = std::find(a.formats.begin(), a.formats.end(), path);
                i.tex = s16(f - a.formats.begin());
                if (f == a.formats.end()) a.formats.push_back(path);
                if (store) a.stored |= 1ull << i.tex;
            } else if (store) {
                return a.error = std::format("buffer store through a descriptor not from user data at {:#x}", i.pc), a;
            }
            buffer((i.w1 >> 16 & 0x1F) * 4);
            break;
        }
        case VINTRP: a.attrs |= 1ull << (i.w0 >> 10 & 0x3F); break;
        default: break;
        }
    }
    // Table places: the groups in order; each load's first dword.
    std::vector<u32> first;
    for (auto& c : a.consts) first.push_back(a.const_dwords), a.const_dwords += (c.hi - c.lo) / 4;
    for (auto& i : p->insts)
        if (i.enc == SMRD && i.tex >= 0) i.tex = s16(first[i.tex] + ((i.imm ? i.src[1] : i.lit) * 4 - a.consts[i.tex].lo) / 4);
    if (in.stage == Stage::Pixel) a.depth_resolve_slot = FindDepthResolve(*p).slot;
    a.program = std::move(p);
    return a;
}

Compiler::Compiler() {
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_target_spirv(opt, shaderc_spirv_version_1_6);
}
Compiler::~Compiler() {
    shaderc_compile_options_release(opt);
    shaderc_compiler_release(cc);
}
std::vector<u32> Compiler::Spirv(const std::string& glsl, shaderc_shader_kind kind, std::string& error,
                                 const std::vector<std::pair<std::string, std::string>>& macros) {
    shaderc_compile_options_t o = opt;
    if (!macros.empty()) {
        o = shaderc_compile_options_clone(opt);
        for (auto& [k, v] : macros) shaderc_compile_options_add_macro_definition(o, k.data(), k.size(), v.data(), v.size());
    }
    shaderc_compilation_result_t r = shaderc_compile_into_spv(cc, glsl.data(), glsl.size(), kind, "gcn", "main", o);
    if (o != opt) shaderc_compile_options_release(o);
    std::vector<u32> w;
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success)
        w.assign((const u32*)shaderc_result_get_bytes(r), (const u32*)shaderc_result_get_bytes(r) + shaderc_result_get_length(r) / 4);
    else
        error = shaderc_result_get_error_message(r);
    shaderc_result_release(r);
    return w;
}
} // namespace Gcn
