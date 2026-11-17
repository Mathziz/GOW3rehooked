// SPDX-License-Identifier: GPL-2.0-or-later
// Interface of the shader runtime the renderer consumes: what a program is made of once identified and translated (slots,
// memoized code, compiled shader, last variant). Deciding between a ready shader (AOT) and a translation now (JIT) is
// shader_runtime.cpp's.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "gpu/shaders/gcn_translator.h"

namespace Gpu {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct ProgSlots {
    std::vector<Gcn::Slot> slots;
    std::vector<Gcn::Path> buffers;  // V# locations (tracking, trace)
    std::vector<Gcn::Path> formats;  // V# of format loads (key)
    std::vector<Gcn::Const> consts;  // scalar load constants of the draw table
    std::uint64_t attrs = 0;         // Analysis::attrs
    std::uint64_t stored = 0;        // Analysis::stored
    int depth_resolve_slot = -1;
};
struct ProgMemo {
    u32 frame = ~0u;
    std::span<const u32> code;
    u64 hash = 0;
    int fetch_sgpr = -1;
    u64 ver = 0;  // Version of the code's pages when it was read (0: read it again every frame)
};
struct Compiled {
    VkShaderEXT s = VK_NULL_HANDLE;
    u64 out_mask = 0;  // VS: exported parameters
    std::string err;
};
// Key of a program: its code, the fetch shader it calls and the stage state its translation depends on besides the draw's.
u64 ProgramKey(Gcn::Stage stage, u64 code_hash, u64 fetch_hash, u32 rsrc2, bool probe, bool boom);
// Key of one translation of a program: the program's, the stage registers in `in` (rsrc1, rect, pixel state, threads) and the slots' specs.
u64 TranslationKey(u64 program_key, const Gcn::Input& in, std::span<const Gcn::SlotSpec> spec);
// User SGPR holding the address of the fetch shader a VS calls through s_swappc_b64 (-1: none).
int FetchSgpr(std::span<const u32> code);
struct Variant {  // a program's last compiled variant (prog_lock), for Stats::compiled
    u64 code, fetch, ps, threads;
    u32 rsrc1, rsrc2;
    bool rect;
    std::vector<Gcn::SlotSpec> spec;
};
} // namespace Gpu
