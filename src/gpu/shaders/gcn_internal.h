// SPDX-License-Identifier: GPL-2.0-or-later
// What the translator's stages share: the decoded program (Analyze) that the emitter turns into SPIR-V.
#pragma once
#include <cstdint>
#include <set>
#include <vector>
#include "gpu/shaders/gcn_translator.h"

namespace Gcn {
enum Enc : std::uint8_t { SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VALU, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP };
inline constexpr std::uint32_t kFloatBits[]{0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000, 0x40000000, 0xc0000000, 0x40800000, 0xc0800000};
inline constexpr std::uint32_t kDoubleHi[]{0x3fe00000, 0xbfe00000, 0x3ff00000, 0xbff00000, 0x40000000, 0xc0000000, 0x40100000, 0xc0100000};

struct Inst {
    std::uint32_t pc = 0;
    Enc enc = SOPP;
    bool vop3 = false, lane_only = false;
    std::uint16_t op = 0, dst = 0, sdst = 0, src[3]{};
    std::uint32_t lit = 0, w0 = 0, w1 = 0;
    std::uint8_t abs = 0, neg = 0, clamp = 0, omod = 0, imm = 0;
    std::int16_t tex = -1, smp = -1;  // MIMG slots; MUBUF format load: tex indexes Analysis::formats
};

struct Program {
    std::vector<Inst> insts;
    std::set<std::uint32_t> labels;
    bool exec_dynamic = false;
    std::uint32_t user_sgprs = 0;
};

struct DepthResolvePattern {
    std::uint32_t load_pc = ~0u, max_pc = ~0u;
    std::int16_t slot = -1;
};
DepthResolvePattern FindDepthResolve(const Program& program);

inline bool IsBranch(const Inst& i) { return i.enc == SOPP && (i.op == 2 || (i.op >= 4 && i.op <= 9)); }
inline std::uint32_t Target(const Inst& i) { return i.pc + 1 + std::uint32_t(std::int32_t(std::int16_t(i.lit))); }
// Scalar writes to EXEC, s_*_saveexec, V_CMPX and VOP3 compares into EXEC.
inline bool WritesExec(const Inst& i) {
    return ((i.enc == SOP1 || i.enc == SOP2 || i.enc == SOPK) && (i.dst == 126 || i.dst == 127)) || (i.enc == SOP1 && i.op >= 36 && i.op <= 43) ||
           (i.enc == VALU && i.op < 256 &&
            ((i.op & 0x70) == 0x10 || (i.op & 0x70) == 0x50 || (i.op & 0xF8) == 0x90 || i.op == 0x98 || (i.op & 0xF8) == 0xD0 || i.sdst == 126));
}
// Sampled image bindings by (dimension kind, class); storage images by (kind, class).
inline int Kind(Dim d) {
    switch (d) {
    case Dim::D1: case Dim::D2: return 0;
    case Dim::D1Array: case Dim::D2Array: return 1;
    case Dim::D3: return 2;
    case Dim::Cube: return 3;
    case Dim::D2Ms: return 4;
    default: return 5;
    }
}
inline constexpr int kSamplerBinding = 18, kStorageBinding = 19;
} // namespace Gcn
