// SPDX-License-Identifier: GPL-2.0-or-later
// Shader runtime: identifies a draw's programs, builds the translation key and gets the shader object, translating on a miss.
#include "gpu/shaders/shader_runtime.h"
#include <bit>
#include "gpu/shaders/shader_jit.h"
#include "gpu/shaders/shader_recipes.h"
#include "gpu/textures.h"
#include "gpu/renderer_impl.h"

namespace Gpu {
namespace {
constexpr u32 kClearVs[]{0xBEEB03FF, 7, 0x36020081, 0x34020281, 0x360000C2, 0x4A0202C1, 0x4A0000C1, 0x7E020B01,
                         0x7E000B00, 0x7E040280, 0x7E0602F2, 0xF80008CF, 0x03020001, 0xF800020F, 0x03030303, 0xBF810000};
}

bool Renderer::Impl::ClearVs(std::span<const u32> code) {
    return code.size() >= std::size(kClearVs) && std::equal(std::begin(kClearVs), std::end(kClearVs), code.begin());
}

bool Renderer::Impl::SetupClearRect(std::string& error) {
    clear_rect_spirv = compiler.Spirv(R"(#version 460
layout(location = 0) out vec4 param0;
void main() {
    gl_Position = vec4(2.0 * float(gl_VertexIndex & 1) - 1.0,
                       2.0 * float((gl_VertexIndex >> 1) & 1) - 1.0, 0.0, 1.0);
    param0 = vec4(1.0);
}
)", shaderc_vertex_shader, error);
    if (clear_rect_spirv.empty()) return false;
    if (caps.path == ShaderPath::Objects) {
        clear_rect_ref = {kClearRectVsKey, Create(clear_rect_spirv, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT),
                          VK_NULL_HANDLE, VK_NULL_HANDLE};
    } else {
        const VkShaderModule mod = CreateShaderModule(clear_rect_spirv);
        const VkPipeline lib = (caps.path == ShaderPath::Gpl && mod) ? CreateGplPreRaster(mod, VK_NULL_HANDLE, false, false) : VK_NULL_HANDLE;
        clear_rect_ref = {kClearRectVsKey, VK_NULL_HANDLE, mod, lib};
    }
    if (!clear_rect_ref || (caps.path == ShaderPath::Gpl && !clear_rect_ref.pl)) {
        DestroyShaderRef(clear_rect_ref);
        clear_rect_ref = {};
        return error = "SDK clear vertex shader creation failed", false;
    }
    return true;
}

bool Renderer::Impl::RectVs(u32 op, const u32* b, const CpuRead& cpu) const {
    if (uc[0x242] != 0x11 || op != 0x2D || b[0] != 3) return false;
    if (!HasClearRectVs()) return true;
    const u64 addr = (u64)(sh[0x48] | (u64)sh[0x49] << 32) << 8;
    const auto* code = static_cast<const u32*>(cpu(addr, sizeof(kClearVs)));
    return code && ClearVs({code, std::size(kClearVs)});
}

VkShaderEXT Renderer::Impl::Create(const std::vector<u32>& spirv, VkShaderStageFlagBits stage, VkShaderStageFlags next) {
    return CreateFrom(VK_SHADER_CODE_TYPE_SPIRV_EXT, spirv.data(), spirv.size() * 4, stage, next);
}

// A shader object from SPIR-V or from a driver binary (VK_INCOMPATIBLE_SHADER_BINARY_EXT, a binary of another driver: null).
VkShaderEXT Renderer::Impl::CreateFrom(VkShaderCodeTypeEXT type, const void* code, size_t bytes, VkShaderStageFlagBits stage, VkShaderStageFlags next) {
    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, sizeof(Push)};
    const VkDescriptorSetLayout layouts[2]{set_layout, table_layout};
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.stage = stage, ci.nextStage = next, ci.codeType = type, ci.codeSize = bytes, ci.pCode = code;
    ci.pName = "main", ci.setLayoutCount = 2, ci.pSetLayouts = layouts, ci.pushConstantRangeCount = 1, ci.pPushConstantRanges = &pcr;
    VkShaderEXT s = VK_NULL_HANDLE;
    if (const VkResult e = CreateShaders(dev, 1, &ci, nullptr, &s); e != VK_SUCCESS) {
        if (e != VK_INCOMPATIBLE_SHADER_BINARY_EXT) Report(std::format("vkCreateShadersEXT {}: stage {:x} next {:x} code type {} of {} bytes", (int)e, (u32)stage, (u32)next, (int)type, bytes));
        s = VK_NULL_HANDLE;
    }
    return s;
}

std::vector<u8> Renderer::Impl::Binary(VkShaderEXT s) {
    size_t size = 0;
    if (!GetShaderBinary || GetShaderBinary(dev, s, &size, nullptr) != VK_SUCCESS || !size) return {};
    std::vector<u8> data(size);
    if (GetShaderBinary(dev, s, &size, data.data()) != VK_SUCCESS) return {};
    data.resize(size);
    return data;
}

int FetchSgpr(std::span<const u32> code) {
    for (size_t i = 0; i < code.size() && code[i] != 0xBF810000;) {
        const u32 x = code[i];
        if (x >> 23 == 0x17D && (x >> 8 & 0xFF) == 33) return int(x & 0xFF);
        const u32 e = x >> 26;
        i += e == 0x34 || e == 0x36 || e == 0x38 || e == 0x3A || e == 0x3C || e == 0x3E ? 2
             : x >> 27 == 0x18                                                         ? 1 + ((x & 0x1FF) == 0xFF)
             : x >> 30 == 2                                                            ? 1 + ((x & 0xFF) == 0xFF || (x >> 8 & 0xFF) == 0xFF)
                                                                                       : 1 + ((x & 0x1FF) == 0xFF);
    }
    return -1;
}

// Program bytes up to its OrbShdr footer (the code size the engine stored after s_endpgm).
static std::span<const u32> Program(u64 addr, const CpuRead& cpu) {
    const u32* h = (const u32*)cpu(addr, 8);
    if (!h || h[0] != 0xBEEB03FF) return {};
    const u64 bytes = ((u64)h[1] + 1) * 8;
    const u32* code = (const u32*)cpu(addr, bytes);
    return code ? std::span<const u32>(code, bytes / 4) : std::span<const u32>{};
}
// Where a descriptor or pointer of `n` dwords lies: user data, then 64-bit pointers loaded from memory (null: unreadable).
static const u32* Locate(const Gcn::Path& path, u32 n, const u32* user, const CpuRead& cpu) {
    const u32* w = path.reg + (path.depth ? 2 : n) <= 16 ? user + path.reg : nullptr;
    for (u32 d = 0; d < path.depth && w; ++d)
        w = (const u32*)cpu(((u64)(w[1] & 0xFFFF) << 32 | w[0]) + path.offset[d], d + 1 < path.depth ? 8 : n * 4);
    return w;
}
static std::span<const u32> FetchProgram(u64 addr, const CpuRead& cpu) {
    for (u32 i = 0; i < 0x100; ++i) {
        const u32* w = (const u32*)cpu(addr + i * 4ull, 4);
        if (!w) return {};
        if ((w[0] & 0xFF80FF00) == 0xBE802000) return {(const u32*)cpu(addr, (i + 1) * 4ull), i + 1};
    }
    return {};
}

// Translated program for a draw stage; fills `table` with user data and the slots' bindless indices.
ShaderRef Renderer::Impl::Shader(Gcn::Stage stage, u64 prog, const u32* user, const CpuRead& cpu, u32 rsrc1, u32 rsrc2, u64& out_mask,
                                 bool rect, std::vector<u32>& table, u32 blend) {
    RenderProbe::Scope shader_time(RenderProbe::Shader);
    Prepared& p = prep;
    if (!Identify(stage, prog, user, cpu, rsrc1, rsrc2, rect, p)) return {};
    p.in.blend_interlock = blend;
    if (track)  // live: the memory of every V# the program loads through, and of the pointers leading to it
        for (const Gcn::Path& bp : *p.paths) {
            const u32* at = bp.reg + (bp.depth ? 2 : 4) <= 16 ? user + bp.reg : nullptr;
            for (u32 d = 0; d < bp.depth && at; ++d) {
                const u64 p = ((u64)(at[1] & 0xFFFF) << 32 | at[0]) + bp.offset[d];
                const u32 n = d + 1 < bp.depth ? 8 : 16;
                Track(p, p + n);
                at = (const u32*)cpu(p, n);
            }
            if (!at) continue;
            const u64 base = (u64)(at[1] & 0xFFFF) << 32 | at[0];
            const u32 stride = at[1] >> 16 & 0x3FFF;
            const u64 size = std::min<u64>(stride ? (u64)stride * at[2] : at[2], 64ull << 20);
            Track(base, base + size), NoteRead(base, base + size);
        }
    // Trace: every V# the program loads through, with what the GPU will find there.
    for (const Gcn::Path& bp : *p.paths) {
        if (!stats.trace || stats.trace_lines.empty()) break;
        const u32* at = bp.reg + (bp.depth ? 2 : 4) <= 16 ? user + bp.reg : nullptr;
        for (u32 d = 0; d < bp.depth && at; ++d)
            at = (const u32*)cpu(((u64)(at[1] & 0xFFFF) << 32 | at[0]) + bp.offset[d], d + 1 < bp.depth ? 8 : 16);
        if (!at) {
            stats.trace_lines.back() += std::format(" | V{} s{} unreadable", "vpc"[(int)stage], bp.reg);
            continue;
        }
        const u64 base = (u64)(at[1] & 0xFFFF) << 32 | at[0];
        const u32 stride = at[1] >> 16 & 0x3FFF, probe = std::min<u32>(stride ? stride * at[2] : at[2], 4096);
        const u8* m = (const u8*)cpu(base, probe);
        const bool mirrored = (base >> 12 < kPieces && written[base >> 12]) ||
                              std::any_of(pending.begin(), pending.end(), [&](const Pending& p) { return base >= p.addr && base < p.addr + p.size; });
        // TEMP: float elements that cannot be geometry (NaN, Inf, |x| >= 2^34): count, first byte offset and bits.
        std::string bad;
        if (const u32 df = at[3] >> 15 & 0xF, comps = df == 4 ? 1 : df == 11 ? 2 : df == 13 ? 3 : df == 14 ? 4 : 0; base && comps && (at[3] >> 12 & 7) == 7) {
            const u32 es = stride ? stride : comps * 4, n = std::min<u32>(stride ? at[2] : at[2] / es, (16u << 20) / es);
            if (const u8* all = n ? (const u8*)cpu(base, (u64)es * (n - 1) + comps * 4) : nullptr) {
                u32 count = 0, first = 0, bits = 0;
                for (u32 i = 0; i < n; ++i)
                    for (u32 c = 0; c < comps; ++c) {
                        u32 v;
                        std::memcpy(&v, all + (u64)i * es + c * 4, 4);
                        if ((v >> 23 & 0xFF) >= 127 + 34 && !count++) first = i * es + c * 4, bits = v;
                    }
                if (count) bad = std::format(" BAD {} of {} @{:x} {:08x}", count, n * comps, first, bits);
            }
        }
        stats.trace_lines.back() += std::format(" | V{} s{}{} {:x} {}x{} f{}/{} {}{}", "vpc"[(int)stage], bp.reg, bp.depth ? "*" : "", base, stride, at[2],
                                                at[3] >> 15 & 0xF, at[3] >> 12 & 7,
                                                !m ? "unreadable" : !mirrored ? "not-mirrored" : std::all_of(m, m + probe, [](u8 x) { return !x; }) ? "zero" : "ok", bad);
    }
    // Draw table (the user data goes as push constants): the scalar loads' constants as the GPU would read them now (past
    // the V#'s range: 0); the bindless element of each slot (0: dummy image / default sampler).
    table.clear();
    for (const Gcn::Const& c : *p.consts) {
        const size_t at = table.size();
        u64 n = (c.hi - c.lo) / 4;
        table.resize(at + n);
        const u32* w = Locate(c.path, c.buffer ? 4 : 2, user, cpu);
        if (!w) continue;
        const u64 base = ((u64)(w[1] & 0xFFFF) << 32 | w[0]) + c.lo;
        if (const u64 stride = w[1] >> 16 & 0x3FFF, lim = stride ? stride * w[2] : w[2]; c.buffer) n = c.lo >= lim ? 0 : std::min(n, (lim - c.lo + 3) / 4);
        if (const u32* s = n ? (const u32*)cpu(base, n * 4) : nullptr) std::copy_n(s, n, table.data() + at);
        if (stats.trace && n)  // TEMP: constants the GPU wrote (the CPU's copy would be stale)
            for (auto* list : {&gpu_writes, &last_writes})
                for (auto& g : *list)
                    if (g.lo < base + n * 4 && g.hi > base) Note(std::format("constants {:x} over a GPU write", base));
    }
    table_ps = stage == Gcn::Stage::Pixel;
    // Everything the variant and the slot elements come from besides the program key (Specify, Texture, Sampler): a
    // repeat of one of the program's last inputs reuses them and redoes only the slots' per-draw checks.
    const u64 ep = Memos().epoch;
    u32 n = 0;  // past kInputs: not kept
    auto put = [&](const u32* w, u32 k) {
        if (n + k <= kInputs) std::memcpy(inputs + n, w, k * 4);
        n += k;
    };
    const u32 head[7]{u32(p.pkey), u32(p.pkey >> 32), rsrc1, u32(rect), u32(ep), u32(ep >> 32), blend};
    put(head, 7);
    if (stage == Gcn::Stage::Pixel) {  // input enables, COL_FORMAT, the 8 CB_COLOR*_INFO (15 registers apart), input controls
        const u32 ps[11]{ctx[0x1B3], ctx[0x1B4], ctx[0x1C5], ctx[0x31C], ctx[0x32B], ctx[0x33A], ctx[0x349], ctx[0x358], ctx[0x367], ctx[0x376], ctx[0x385]};
        put(ps, 11), put(&ctx[0x191], 32);
    }
    if (stage == Gcn::Stage::Compute) put(&sh[0x207], 3);
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const u32 present = p.desc[k] != nullptr;
        put(&present, 1);
        if (present) put(p.desc[k], (*p.slots)[k].kind == Gcn::SlotKind::Sampler ? 4 : 8);
    }
    for (const u32* v : p.fdesc) {
        const u32 w[2]{v != nullptr, v ? v[3] : 0};  // all BufferSpec reads
        put(w, v ? 2 : 1);
    }
    if (!p.stencil_resolve && !stats.trace && n <= kInputs)
        for (u32 i = 0; i < 4; ++i)
            if (const Recent& r = p.use->recent[p.use->order[i]]; r.in.size() == n && !std::memcmp(r.in.data(), inputs, n * 4)) {
                if (RenderProbe::current) ++RenderProbe::current->recent_hits;
                std::rotate(p.use->order, p.use->order + i, p.use->order + i + 1);
                for (size_t k = 0; k < p.slots->size(); ++k)
                    table.push_back(!p.desc[k] || (*p.slots)[k].kind == Gcn::SlotKind::Sampler
                                        ? r.slots[k].e
                                        : Reuse(r.slots[k], p.desc[k], (*p.slots)[k].kind == Gcn::SlotKind::Storage));
                if (p.motion_blur) table.push_back(std::bit_cast<u32>(motion_blur_rate));
                return out_mask = r.out_mask, r.s;
            }
    Specify(stage, p);
    bool subs = false, keep = !stats.trace && n <= kInputs;
    slot_used.clear();
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const auto& slot = (*p.slots)[k];
        const bool tex = p.desc[k] && slot.kind != Gcn::SlotKind::Sampler;
        tex_sub = 0;
        const u32 e = !p.desc[k] ? 0 : tex ? Texture(p.desc[k], p.spec[k], slot.kind == Gcn::SlotKind::Storage, p.stencil_resolve) : Sampler(p.desc[k]);
        table.push_back(e), slot_used.push_back(tex ? tex_used : MemoTex{nullptr, e, VK_NULL_HANDLE, 0}), keep &= !tex || tex_keep;
        if (tex_sub)  // the slot samples part of its image: one variant of the program (the extents come from the T# and the image)
            p.spec[k].part = true, subs = true;
    }
    if (p.motion_blur) table.push_back(std::bit_cast<u32>(motion_blur_rate));
    if (subs) p.key = TranslationKey(p.pkey, p.in, p.spec);
    const ShaderRef s = Translated(stage, prog, cpu, p, out_mask);
    if (ProgUse* u = keep && s ? Memos().progs.Find(p.pkey) : nullptr) {  // not a shader still compiling  // found again: compiling may have added program keys
        Recent& r = u->recent[u->order[3]];  // the least recent gives way
        std::rotate(u->order, u->order + 3, u->order + 4);
        r.in.assign(inputs, inputs + n), r.out_mask = out_mask, r.s = s, r.slots = slot_used;
    }
    return s;
}

ShaderRef Renderer::Impl::Translated(Gcn::Stage stage, u64 prog, const CpuRead& cpu, Prepared& p, u64& out_mask) {
    const u64 key = p.key;
    const bool clear_rect = HasClearRectVs() && p.in.rect && ClearVs(p.in.code);
    auto found = [&] {
        auto it = shaders.find(key);
        if (it == shaders.end()) return false;
        if (auto o = vs_outputs.find(key); o != vs_outputs.end()) out_mask = o->second;
        return true;
    };
    // Traced frames keep the SPIR-V of every program they run, compiled before or not (written with the dump).
    if (stats.trace) {
        std::string name = std::format("{}-{:x}-{:x}", "vpc"[(int)stage], prog, key & 0xFFFF), err;
        if (std::none_of(stats.trace_spirv.begin(), stats.trace_spirv.end(), [&](auto& e) { return e.first == name; })) {
            std::vector<u32> words;
            u64 mask = 0;
            if (clear_rect) words = clear_rect_spirv;
            else Gcn::EmitSpirv(Gcn::Analyze(p.in), p.in, p.spec, words, mask, err);
            stats.trace_spirv.push_back({name, std::string((const char*)words.data(), words.size() * 4)});
        }
    }
    if (clear_rect) return out_mask = 1, clear_rect_ref;
    auto& memo = Memos().shaders;  // compiled shaders stay
    if (const auto* m = memo.Find(key)) return out_mask = m->second, m->first;
    {
        std::shared_lock lock(prog_lock);
        if (found()) return memo[key] = {shaders.find(key)->second, out_mask}, shaders.find(key)->second;
    }
    // First miss of the frame: the rest of the frame's programs go to the compile threads (traced frames compile here,
    // in order). A parallel block's first miss does the same for the rest of the block, so the variants a camera turn
    // reveals together load or translate at once on those threads, not one after another here; the block waits for each.
    if (!stats.trace && !in_worker && !discovered) discovered = true, Discover(cpu, {walk_chain.size(), 0}, false);
    else if (!stats.trace && in_worker && block_now && !block_discovered) block_discovered = true, Discover(cpu, block_now->to, true);
    // First use of this variant in the run (translated now or found in the cache): kept as a recipe for the AOT.
    if (!stats.trace && !p.in.probe && !p.in.boom) Recipes::Instance().Note(key, p.in, p.spec, ShaderCache::Instance().Contains(key));
    Compiled c;
    if (stats.trace) {
        c = Compile(stage, prog, key, p.in,std::vector<u32>(p.in.code.begin(), p.in.code.end()), std::vector<u32>(p.in.fetch.begin(), p.in.fetch.end()),
                    p.spec);
    } else {
        // Sync waits for the compile. Async skips the draw until the workers finish it. Compute always waits.
        const Jit::Mode mode = Jit::mode;
        const bool compute = stage == Gcn::Stage::Compute;
        // Built by an earlier run or the AOT: the driver's binary only loads (no compile), here, so the draw is never skipped.
        const u64 content = mode == Jit::Mode::Async ? ShaderCache::Instance().ContentOf(key) : 0;
        const bool built = caps.path == ShaderPath::Objects
                               ? (content && ShaderCache::Instance().ContainsBinary(BinaryKey(content)))
                               : (content != 0);
        std::promise<Compiled> here;
        std::shared_future<Compiled> f;
        bool own = false;
        {
            std::unique_lock lock(prog_lock);
            if (found()) return shaders.find(key)->second;
            auto [it, fresh] = compiling.try_emplace(key);
            if (fresh) it->second = built ? here.get_future().share() : Async(stage, prog, p, true), own = built;
            f = it->second;
        }
        if (own) here.set_value(Compile(stage, prog, key, p.in, std::vector<u32>(p.in.code.begin(), p.in.code.end()),
                                        std::vector<u32>(p.in.fetch.begin(), p.in.fetch.end()), p.spec));
        if (mode == Jit::Mode::Async && !compute && !built && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return waiting = true, ShaderRef{};
        Stalled(stall_waits, stall_wait_ns, [&] { c = f.get(); });
    }
    std::unique_lock lock(prog_lock);
    if (found()) {  // another block or the WAD's warm-up (Prepare) finished it first: the first to end this compile drops its copy
        if (const auto it = compiling.find(key); it != compiling.end()) {
            compiling.erase(it);
            const ShaderRef cr = c.Ref(key);
            if (cr && cr != shaders.find(key)->second) DestroyShaderRef(cr);
        }
        return shaders.find(key)->second;
    }
    compiling.erase(key);
    if (stage == Gcn::Stage::Vertex) vs_outputs[key] = out_mask = c.out_mask;
    Count(stats.shaders);
    const ShaderRef ref = c.Ref(key);
    if (!c.Ready(caps.path, stage)) Count(stats.shader_fail), Note(std::format("shader {:x} stage {}: {}", prog, (int)stage, c.err.substr(0, 300)));
    return shaders[key] = ref;
}

bool Renderer::Impl::Identify(Gcn::Stage stage, u64 prog, const u32* user, const CpuRead& cpu, u32 rsrc1, u32 rsrc2, bool rect, Prepared& p) {
    RenderProbe::Scope identify_time(RenderProbe::Identify);
    // Code, content hash and fetch SGPR, checked once per program per frame: read again only when a page under the code
    // changed (Version), every frame when that is unknown.
    auto stale = [&](ProgMemo& m, u64 a) {
        if (m.frame == frame) return false;
        m.frame = frame;
        return !m.ver || m.ver != Version(a, m.code.size_bytes());
    };
    ProgMemo& pm = prog_memo[prog | (u64)stage];
    if (stale(pm, prog)) {
        pm.code = Program(prog, cpu);
        pm.hash = ::Gpu::PieceHash::ShaderCode(pm.code.data(), pm.code.size_bytes());
        pm.fetch_sgpr = stage == Gcn::Stage::Vertex ? FetchSgpr(pm.code) : -1;
        pm.ver = Version(prog, pm.code.size_bytes());
    }
    const auto code = pm.code;
    if (code.empty()) return Note(std::format("program {:x} unreadable", prog)), false;
    std::span<const u32> fetch;
    u64 fetch_hash = 0;
    if (const int s = pm.fetch_sgpr; s >= 0 && s < 15)
        if (const u64 fa = (u64)(user[s + 1] & 0xFFFF) << 32 | user[s]) {
            ProgMemo& fm = fetch_memo[fa];
            if (stale(fm, fa)) fm.code = FetchProgram(fa, cpu), fm.hash = ::Gpu::PieceHash::ShaderCode(fm.code.data(), fm.code.size_bytes()), fm.ver = Version(fa, fm.code.size_bytes());
            fetch = fm.code, fetch_hash = fm.hash;
        }
    p.spec.clear(), p.desc.clear(), p.fdesc.clear(), p.in = {}, p.rect = rect;
    p.motion_blur = stage == Gcn::Stage::Pixel && pm.hash == Gcn::MotionBlurCodeHash();
    p.stencil_resolve = stage == Gcn::Stage::Pixel && pm.hash == Gcn::MotionVectorResolveCodeHash();
    Gcn::Input& in = p.in;
    in.stage = stage, in.code = code, in.fetch = fetch, in.rsrc1 = rsrc1, in.rsrc2 = rsrc2, in.probe = stats.trace && stage == Gcn::Stage::Vertex;
    in.boom = boom_on && !in.probe && stage == Gcn::Stage::Vertex;
    in.rect = rect && stage == Gcn::Stage::Vertex;
    static const char* probe_pc = DebugOption("GOW3_PROBE_PC");  // e.g. 100017: after the fetch
    if (in.probe && probe_pc) in.probe_pc = (u32)std::strtoul(probe_pc, nullptr, 16);
    p.pkey = ProgramKey(stage, pm.hash, fetch_hash, rsrc2, in.probe, in.boom);
    // Elements of an unordered_map stay put: the entry outlives the lock. The thread's own entries start over past 4096
    // (programs come and go with the levels; each keeps up to 4 inputs).
    auto& progs = Memos().progs;
    if (progs.used >= 4096 && !progs.Find(p.pkey)) progs.clear();
    ProgUse& u = progs[p.pkey];
    if (!u.ps) {
        std::shared_lock lock(prog_lock);
        if (auto it = slots.find(p.pkey); it != slots.end()) u.ps = &it->second;
    }
    if (!u.ps) {
        Gcn::Analysis an = Gcn::Analyze(in);
        std::unique_lock lock(prog_lock);
        u.ps = &slots.emplace(p.pkey, ProgSlots{std::move(an.slots), std::move(an.buffers), std::move(an.formats), std::move(an.consts), an.attrs, an.stored, an.depth_resolve_slot}).first->second;
    }
    p.use = &u, p.slots = &u.ps->slots, p.paths = &u.ps->buffers, p.consts = &u.ps->consts, p.stored = u.ps->stored;
    p.depth_resolve_slot = u.ps->depth_resolve_slot;
    for (auto& s : *p.slots) p.desc.push_back(Locate(s.path, s.kind == Gcn::SlotKind::Sampler ? 4 : 8, user, cpu));
    for (auto& f : u.ps->formats) p.fdesc.push_back(Locate(f, 4, user, cpu));
    return true;
}

void Renderer::Impl::Specify(Gcn::Stage stage, Prepared& p) {
    Gcn::Input& in = p.in;
    if (stage == Gcn::Stage::Compute) std::copy_n(&sh[0x207], 3, in.threads);  // COMPUTE_NUM_THREAD_X/Y/Z
    if (stage == Gcn::Stage::Pixel) {
        in.ps_input_ena = ctx[0x1B3], in.ps_input_addr = ctx[0x1B4], in.col_format = ctx[0x1C5];
        std::memcpy(in.ps_input_cntl, &ctx[0x191], sizeof(in.ps_input_cntl));
        for (u32 k = 0; k < 32; ++k) in.ps_input_cntl[k] &= -u32(p.use->ps->attrs >> k & 1);  // only the interpolants the code reads count
        for (u32 k = 0; k < 8; ++k) {
            const u32 info = ctx[0x31C + k * 15], nt = info >> 8 & 7;
            if (!(in.col_format >> (4 * k) & 0xF)) continue;
            in.mrt_class[k] = nt == 4 ? 1 : nt == 5 ? 2 : 0;
            const auto swz = AmdGpu::MrtSwizzle(info >> 11 & 3, AmdGpu::DataFormat(info >> 2 & 0x1F));
            in.mrt_map[k] = u8(swz.Map(0) | swz.Map(1) << 2 | swz.Map(2) << 4 | swz.Map(3) << 6);
        }
    }
    SpecifySlots(p);
    if (!p.stencil_resolve) return;
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const u32* w = p.desc[k];
        auto& spec = p.spec[k];
        if (!w || (*p.slots)[k].kind != Gcn::SlotKind::Texture || spec.dim != Gcn::Dim::D2Ms || spec.cls != Gcn::Class::Float ||
            (w[1] >> 20 & 0x3F) != 1 || (w[1] >> 26 & 0xF)) continue;
        const u64 base = ((u64)w[0] | (u64)(w[1] & 0x3F) << 32) << 8;
        if (auto it = stencil_at.find(base); it != stencil_at.end() && it->second.second == frame && it->second.first &&
            it->second.first->samples == (1u << (w[3] >> 16 & 0xF)) && (w[2] & 0x3FFF) + 1 == it->second.first->w &&
            (w[2] >> 14 & 0x3FFF) + 1 == it->second.first->h) {
            const auto sel = DstSel(w);
            spec.stencil = true;
            spec.dst_sel = u16(u32(sel.r) | u32(sel.g) << 3 | u32(sel.b) << 6 | u32(sel.a) << 9);
            p.key = TranslationKey(p.pkey, p.in, p.spec);
        }
    }
}

// The specs of the slots from their descriptors (p.desc, p.fdesc) and the translation key. The AOT calls it with descriptors it made.
void Renderer::Impl::SpecifySlots(Prepared& p) {
    Gcn::Input& in = p.in;
    p.spec.clear();
    for (size_t k = 0; k < p.slots->size(); ++k) {
        const auto& s = (*p.slots)[k];
        const u32* w = p.desc[k];
        p.spec.push_back(!w ? Gcn::SlotSpec{} : s.kind == Gcn::SlotKind::Sampler ? Gcn::SamplerSpec(w) : Gcn::TextureSpec(w));
        if (k != size_t(p.depth_resolve_slot) || p.spec.back().dim != Gcn::Dim::D2Ms || p.spec.back().cls != Gcn::Class::Float)
            p.spec.back().msaa4 = false;
        // Storage views cannot swizzle: the shader applies DST_SEL to its loads and the inverse to its stores (0: identity).
        if (const AmdGpu::CompMapping sel = w && s.kind == Gcn::SlotKind::Storage ? DstSel(w) : AmdGpu::IdentityMapping; sel != AmdGpu::IdentityMapping)
            p.spec.back().dst_sel = u16(u32(sel.r) | u32(sel.g) << 3 | u32(sel.b) << 6 | u32(sel.a) << 9);
    }
    // The engine pairs the fog composite with the fullscreen VS's normalized UV (SPI_PS_INPUT_CNTL_0 = 1: its param 1) yet binds
    // the scene with its unnormalized WhiteBuffer sampler: every pixel would read the corner texel (dark pit). The depth
    // sample at the same UV is normalized: the scene sample takes it as normalized too.
    if (in.stage == Gcn::Stage::Pixel && (in.ps_input_cntl[0] & 0x1F) == 1 && in.code.size() == 128 &&
        ::Gpu::PieceHash::ShaderCode(in.code.data(), in.code.size_bytes()) == Gcn::FogCompositeCodeHash())
        for (auto& s : p.spec) s.unnormalized = false;
    for (const u32* w : p.fdesc) p.spec.push_back(w ? Gcn::BufferSpec(w) : Gcn::SlotSpec{});
    p.key = TranslationKey(p.pkey, in, p.spec);
}

u64 ProgramKey(Gcn::Stage stage, u64 code_hash, u64 fetch_hash, u32 rsrc2, bool probe, bool boom) {
    if (stage == Gcn::Stage::Pixel && code_hash == Gcn::MotionBlurCodeHash()) code_hash ^= 0x6d6f74696f6e3031ull;
    const u64 hashes[2]{code_hash, fetch_hash};
    // Probe and exploded-vertex variants apart: each writes its own record, which only its frames push.
    return ::Gpu::PieceHash::ShaderKey(hashes, sizeof(hashes), (u64)stage << 32 | rsrc2 | (u64)probe << 62 | (boom ? (u64)(0x100 | Gcn::BoomClip()) << 40 | (u64)Gcn::BoomParamLimit() << 49 | 1ull << 63 | 1ull << 62 : 0));  // bit 62: the record carries the constant bank (never with a probe)
}

u64 TranslationKey(u64 program_key, const Gcn::Input& in, std::span<const Gcn::SlotSpec> spec) {
    u64 packed[64], *w = packed;  // the specs field by field
    std::vector<u64> more;
    if (spec.size() > std::size(packed)) more.resize(spec.size()), w = more.data();
    for (size_t k = 0; k < spec.size(); ++k) w[k] = Gcn::Pack(spec[k]);
    u64 key = ::Gpu::PieceHash::ShaderSpecKey(w, spec.size() * 8, program_key ^ in.rsrc1 ^ (in.rect ? 1ull << 63 : 0));
    if (in.stage == Gcn::Stage::Compute) key = ::Gpu::PieceHash::ShaderKey(in.threads, sizeof(in.threads), key);
    if (in.stage == Gcn::Stage::Pixel)
        key = ::Gpu::PieceHash::ShaderKey(in.ps_input_cntl, sizeof(in.ps_input_cntl),
                                   ::Gpu::PieceHash::ShaderKey(&in.mrt_class, 16, key ^ in.ps_input_ena ^ (u64)in.ps_input_addr << 32 ^ in.col_format));
    if (in.blend_interlock) key = ::Gpu::PieceHash::ShaderKey(&in.blend_interlock, sizeof(in.blend_interlock), key);
    return key;
}

// Rect list: three corners of an axis-aligned rectangle; the corner with the right angle is opposite the missing one.
ShaderRef Renderer::Impl::RectGs(u64 mask) {
    std::unique_lock lock(prog_lock);
    if (auto it = rect_gs.find(mask); it != rect_gs.end()) return it->second;
    std::string s = "#version 460\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
    for (u64 m = mask; m; m &= m - 1) s += std::format("layout(location = {0}) in vec4 i{0}[];\nlayout(location = {0}) out vec4 o{0};\n", std::countr_zero(m));
    s += R"(void Emit(int k, int a, int b, int c, bool far) {
    gl_Position = far ? gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position : gl_in[k].gl_Position;
)";
    for (u64 m = mask; m; m &= m - 1) s += std::format("    o{0} = far ? i{0}[a] + i{0}[b] - i{0}[c] : i{0}[k];\n", std::countr_zero(m));
    s += R"(    EmitVertex();
}
void main() {
    vec2 p[3];
    for (int k = 0; k < 3; ++k) p[k] = gl_in[k].gl_Position.xy / gl_in[k].gl_Position.w;
    int c = 0;
    float best = 3.4e38;
    for (int k = 0; k < 3; ++k) {
        float d = abs(dot(p[(k + 1) % 3] - p[k], p[(k + 2) % 3] - p[k]));
        if (d < best) best = d, c = k;
    }
    int a = (c + 1) % 3, b = (c + 2) % 3;
    Emit(c, a, b, c, false);
    Emit(a, a, b, c, false);
    Emit(b, a, b, c, false);
    Emit(0, a, b, c, true);
}
)";
    std::string err;
    auto spirv = compiler.Spirv(s, shaderc_geometry_shader, err);
    ShaderRef gs{};
    if (!spirv.empty()) {
        if (caps.path == ShaderPath::Objects)
            gs = {mask, Create(spirv, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT), VK_NULL_HANDLE, VK_NULL_HANDLE};
        else
            gs = {mask, VK_NULL_HANDLE, CreateShaderModule(spirv), VK_NULL_HANDLE};
    }
    if (!gs) Note("rect list geometry shader: " + err.substr(0, 300));
    return rect_gs[mask] = gs;
}

void FixedPipelineState::Canonicalize(const DynamicCaps& c) {
    if (c.depth_clip_negative_one_to_one) flags &= ~kNegOne;
    if (c.depth_clamp_enable) flags &= ~kDepthClamp;
    if (c.alpha_to_coverage_enable) flags &= ~kAlphaToCoverage;
    if (c.rasterization_samples) samples = 1;
    if (c.unrestricted_topology && !(flags & kHasGs)) topology_class = 2;
    if (c.color_blend_enable && c.color_blend_equation) square_color_mask = square_alpha_mask = 0;
    for (u32 k = color_count; k < 8; ++k) color_formats[k] = VK_FORMAT_UNDEFINED, blend[k] = {};
    for (u32 k = 0; k < color_count; ++k) {
        if (c.color_blend_enable) blend[k].blend_enable = 0;
        if (c.color_blend_equation || (!c.color_blend_enable && !blend[k].blend_enable))
            blend[k].src_color = blend[k].dst_color = blend[k].color_op =
                blend[k].src_alpha = blend[k].dst_alpha = blend[k].alpha_op = 0;
        if (c.color_write_mask) blend[k].write_mask = 0xF;
    }
    pad[0] = pad[1] = 0;
}

u64 GraphicsPipelineKey::Hash() const {
    const u64 head[3]{vs_key, ps_key, (state.flags & FixedPipelineState::kHasGs) ? gs_mask : 0ull};
    return ::Gpu::PieceHash::Stable64Seeded(&state, sizeof(state), ::Gpu::PieceHash::Stable64(head, sizeof(head)));
}

bool Renderer::Impl::ValidateNativePipelineCacheData(std::span<const u8> data) const {
    struct VulkanPipelineCacheHeader {
        u32 header_size, header_version, vendor_id, device_id;
        u8 uuid[VK_UUID_SIZE];
    };
    static_assert(sizeof(VulkanPipelineCacheHeader) == 32);
    if (data.size() < sizeof(VulkanPipelineCacheHeader)) return false;
    VulkanPipelineCacheHeader h{};
    std::memcpy(&h, data.data(), sizeof(h));
    if (h.header_size != sizeof(VulkanPipelineCacheHeader) || h.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return false;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    return h.vendor_id == props.vendorID && h.device_id == props.deviceID && std::memcmp(h.uuid, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

void Renderer::Impl::CreatePipelineCache(std::span<const u8> initial_data) {
    if (pipeline_cache) vkDestroyPipelineCache(dev, pipeline_cache, nullptr), pipeline_cache = VK_NULL_HANDLE;
    const bool valid = !initial_data.empty() && ValidateNativePipelineCacheData(initial_data);
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, valid ? initial_data.size() : 0,
                                 valid ? initial_data.data() : nullptr};
    VkResult e = vkCreatePipelineCache(dev, &ci, nullptr, &pipeline_cache);
    if (e != VK_SUCCESS && valid) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;
        e = vkCreatePipelineCache(dev, &ci, nullptr, &pipeline_cache);
    }
    if (e != VK_SUCCESS) Report(std::format("vkCreatePipelineCache {}", (int)e)), pipeline_cache = VK_NULL_HANDLE;
}

void Renderer::Impl::DestroyShaderRef(const ShaderRef& r) {
    if (r.obj && DestroyShader) DestroyShader(dev, r.obj, nullptr);
    if (r.mod) vkDestroyShaderModule(dev, r.mod, nullptr);
    if (r.pl) vkDestroyPipeline(dev, r.pl, nullptr);
}

VkShaderModule Renderer::Impl::CreateShaderModule(std::span<const u32> spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spirv.size_bytes(), spirv.data()};
    VkShaderModule mod = VK_NULL_HANDLE;
    if (const VkResult e = vkCreateShaderModule(dev, &ci, nullptr, &mod); e != VK_SUCCESS) {
        Report(std::format("vkCreateShaderModule {}: {} bytes", (int)e, spirv.size_bytes()));
        mod = VK_NULL_HANDLE;
    }
    return mod;
}

VkPipeline Renderer::Impl::CreateComputePipeline(std::span<const u32> spirv) {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mod_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spirv.size_bytes(), spirv.data()};
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, caps.maintenance5 ? &mod_ci : nullptr, 0,
                                          VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, "main"};
    if (!caps.maintenance5) {
        mod = CreateShaderModule(spirv);
        if (!mod) return VK_NULL_HANDLE;
        stage.module = mod;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0, stage, layout};
    VkPipeline pl = VK_NULL_HANDLE;
    const VkResult e = vkCreateComputePipelines(dev, pipeline_cache, 1, &ci, nullptr, &pl);
    if (mod) vkDestroyShaderModule(dev, mod, nullptr);
    if (e != VK_SUCCESS) {
        Report(std::format("vkCreateComputePipelines {}: {} bytes", (int)e, spirv.size_bytes()));
        return VK_NULL_HANDLE;
    }
    pipeline_cache_dirty.fetch_add(1, std::memory_order_relaxed);
    return pl;
}

std::vector<VkDynamicState> Renderer::Impl::BuildDynamicStates() const {
    std::vector<VkDynamicState> list{
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    if (caps.depth_clip_negative_one_to_one) list.push_back(VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT);
    if (caps.depth_clamp_enable) list.push_back(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT);
    if (caps.rasterization_samples) list.push_back(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
    if (caps.alpha_to_coverage_enable) list.push_back(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
    if (caps.color_blend_enable) list.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
    if (caps.color_blend_equation) list.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
    if (caps.color_write_mask) list.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    return list;
}

VkPipeline Renderer::Impl::CreateGraphicsPipelineRaw(const ShaderRef& vs, const ShaderRef& ps, const ShaderRef& gs,
                                                     const FixedPipelineState& state, bool fail_on_compile, bool square_pass) {
    if (fail_on_compile && !caps.cache_control) return VK_NULL_HANDLE;
    if (!vs.mod) return VK_NULL_HANDLE;
    const bool has_gs = (state.flags & FixedPipelineState::kHasGs) != 0;
    const bool has_ps = (state.flags & FixedPipelineState::kHasPs) != 0;
    if (has_gs && !gs.mod) return VK_NULL_HANDLE;
    if (has_ps && !ps.mod) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[3]{};
    u32 stage_count = 0;
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs.mod, "main"};
    if (has_gs) stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs.mod, "main"};
    if (has_ps) stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps.mod, "main"};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    static constexpr VkPrimitiveTopology kTopos[]{VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                              kTopos[std::min<u8>(state.topology_class, 2)], VK_FALSE};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, nullptr,
                                                       (state.flags & FixedPipelineState::kNegOne) ? VK_TRUE : VK_FALSE};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                          caps.depth_clip_negative_one_to_one ? nullptr : &dcc};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = (!caps.depth_clamp_enable && (state.flags & FixedPipelineState::kDepthClamp)) ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max<u32>(1, state.samples));
    ms.alphaToCoverageEnable = (!caps.alpha_to_coverage_enable && (state.flags & FixedPipelineState::kAlphaToCoverage)) ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    VkPipelineColorBlendAttachmentState cba[8]{};
    const u32 count = std::min<u32>(state.color_count, 8);
    for (u32 k = 0; k < count; ++k) {
        auto& a = cba[k];
        if (square_pass) {
            const bool active = state.color_formats[k] != VK_FORMAT_UNDEFINED && state.blend[k].write_mask != 0;
            const bool sq_c = (state.square_color_mask >> k & 1) != 0;
            const bool sq_a = (state.square_alpha_mask >> k & 1) != 0;
            a.blendEnable = (!caps.color_blend_enable && active) ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.dstColorBlendFactor = sq_c ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_ONE;
            a.colorBlendOp = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.dstAlphaBlendFactor = sq_a ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_ONE;
            a.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            const auto& b = state.blend[k];
            a.blendEnable = (!caps.color_blend_enable && b.blend_enable) ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = VkBlendFactor(b.src_color);
            a.dstColorBlendFactor = VkBlendFactor(b.dst_color);
            a.colorBlendOp = VkBlendOp(b.color_op);
            a.srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
            a.dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
            a.alphaBlendOp = VkBlendOp(b.alpha_op);
        }
        a.colorWriteMask = caps.color_write_mask ? VkColorComponentFlags(0xF) : VkColorComponentFlags(state.blend[k].write_mask);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY,
                                           count, count ? cba : nullptr};

    const auto dyns = BuildDynamicStates();
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, u32(dyns.size()), dyns.data()};
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, count,
                                      count ? state.color_formats : nullptr, state.depth_format, state.stencil_format};
    const VkPipelineCreateFlags flags = fail_on_compile ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, flags, stage_count, stages, &vi, &ia,
                                    nullptr, &vps, &rs, &ms, &ds, &cb, &dsi, layout};
    VkPipeline pl = VK_NULL_HANDLE;
    const VkResult e = vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &gi, nullptr, &pl);
    if (e == VK_SUCCESS) {
        if (!fail_on_compile) pipeline_cache_dirty.fetch_add(1, std::memory_order_relaxed);
        return pl;
    }
    if (e < 0) Report(std::format("vkCreateGraphicsPipelines {}: vs {:x} ps {:x}", (int)e, vs.key, ps.key));
    return VK_NULL_HANDLE;
}

CompiledPipeline Renderer::Impl::CreateMonolithicPipeline(const ShaderRef& vs, const ShaderRef& ps, const ShaderRef& gs,
                                                          const FixedPipelineState& state, bool fail_on_compile) {
    CompiledPipeline pl{};
    pl.main = CreateGraphicsPipelineRaw(vs, ps, gs, state, fail_on_compile, false);
    if (!pl.main) return {};
    if (state.NeedsSquarePipeline(caps)) {
        pl.square = CreateGraphicsPipelineRaw(vs, ps, gs, state, fail_on_compile, true);
        if (!pl.square) {
            vkDestroyPipeline(dev, pl.main, nullptr);
            return {};
        }
    }
    pl.is_lto = true;
    return pl;
}

void Renderer::Impl::SetupGplGlobals() {
    static constexpr VkPipelineCreateFlags kLibFlags =
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    static constexpr VkPrimitiveTopology kTopos[3]{
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    static constexpr VkDynamicState kViDyns[2]{
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, kViDyns};
    VkGraphicsPipelineLibraryCreateInfoEXT lib_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, nullptr,
                                                  VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT};
    for (u32 k = 0; k < 3; ++k) {
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, kTopos[k], VK_FALSE};
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib_ci, kLibFlags};
        gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pDynamicState = &dsi;
        if (const VkResult e = vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &gi, nullptr, &gpl_vi[k]); e != VK_SUCCESS)
            Report(std::format("vkCreateGraphicsPipelines GPL VI[{}] {}", k, (int)e)), gpl_vi[k] = VK_NULL_HANDLE;
    }
    // Default samples=1 depth-only FS library; MSAA depth-only variants go into gpl_frag_msaa on demand.
    gpl_empty_fs = CreateGplFragShader(VK_NULL_HANDLE, 1);
}

VkPipeline Renderer::Impl::CreateGplPreRaster(VkShaderModule vs_mod, VkShaderModule gs_mod, bool neg_one, bool depth_clamp) {
    if (!vs_mod) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_mod, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs_mod, "main"},
    };
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, nullptr,
                                                       neg_one ? VK_TRUE : VK_FALSE};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                          caps.depth_clip_negative_one_to_one ? nullptr : &dcc};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = (!caps.depth_clamp_enable && depth_clamp) ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;

    std::vector<VkDynamicState> dyns{
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    if (caps.depth_clip_negative_one_to_one) dyns.push_back(VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT);
    if (caps.depth_clamp_enable) dyns.push_back(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT);
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, u32(dyns.size()), dyns.data()};
    VkGraphicsPipelineLibraryCreateInfoEXT lib_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, nullptr,
                                                  VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT};
    VkGraphicsPipelineCreateInfo gi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib_ci,
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
        gs_mod ? 2u : 1u, stages};
    gi.pViewportState = &vps, gi.pRasterizationState = &rs, gi.pDynamicState = &dsi, gi.layout = layout;
    VkPipeline pl = VK_NULL_HANDLE;
    if (const VkResult e = vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &gi, nullptr, &pl); e != VK_SUCCESS) {
        Report(std::format("vkCreateGraphicsPipelines GPL PreRaster {}", (int)e));
        return VK_NULL_HANDLE;
    }
    return pl;
}

VkPipeline Renderer::Impl::CreateGplFragShader(VkShaderModule ps_mod, u32 samples) {
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps_mod, "main"};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max<u32>(1, samples));

    std::vector<VkDynamicState> dyns{
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    if (caps.rasterization_samples) dyns.push_back(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, u32(dyns.size()), dyns.data()};
    VkGraphicsPipelineLibraryCreateInfoEXT lib_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, nullptr,
                                                  VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT};
    VkGraphicsPipelineCreateInfo gi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib_ci,
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
        ps_mod ? 1u : 0u, ps_mod ? &st : nullptr};
    gi.pMultisampleState = &ms, gi.pDepthStencilState = &ds, gi.pDynamicState = &dsi, gi.layout = layout;
    VkPipeline pl = VK_NULL_HANDLE;
    if (const VkResult e = vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &gi, nullptr, &pl); e != VK_SUCCESS) {
        Report(std::format("vkCreateGraphicsPipelines GPL FragShader {}", (int)e));
        return VK_NULL_HANDLE;
    }
    return pl;
}

VkPipeline Renderer::Impl::CreateGplFragOutput(const FixedPipelineState& state, bool square_pass) {
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max<u32>(1, state.samples));
    ms.alphaToCoverageEnable = (!caps.alpha_to_coverage_enable && (state.flags & FixedPipelineState::kAlphaToCoverage)) ? VK_TRUE : VK_FALSE;

    VkPipelineColorBlendAttachmentState cba[8]{};
    const u32 count = std::min<u32>(state.color_count, 8);
    for (u32 k = 0; k < count; ++k) {
        auto& a = cba[k];
        if (square_pass) {
            const bool active = state.color_formats[k] != VK_FORMAT_UNDEFINED && state.blend[k].write_mask != 0;
            const bool sq_c = (state.square_color_mask >> k & 1) != 0;
            const bool sq_a = (state.square_alpha_mask >> k & 1) != 0;
            a.blendEnable = (!caps.color_blend_enable && active) ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.dstColorBlendFactor = sq_c ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_ONE;
            a.colorBlendOp = VK_BLEND_OP_ADD;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.dstAlphaBlendFactor = sq_a ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_ONE;
            a.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            const auto& b = state.blend[k];
            a.blendEnable = (!caps.color_blend_enable && b.blend_enable) ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = VkBlendFactor(b.src_color);
            a.dstColorBlendFactor = VkBlendFactor(b.dst_color);
            a.colorBlendOp = VkBlendOp(b.color_op);
            a.srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
            a.dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
            a.alphaBlendOp = VkBlendOp(b.alpha_op);
        }
        a.colorWriteMask = caps.color_write_mask ? VkColorComponentFlags(0xF) : VkColorComponentFlags(state.blend[k].write_mask);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY,
                                           count, count ? cba : nullptr};

    std::vector<VkDynamicState> dyns{VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    if (caps.rasterization_samples) dyns.push_back(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT);
    if (caps.alpha_to_coverage_enable) dyns.push_back(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
    if (caps.color_blend_enable) dyns.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
    if (caps.color_blend_equation) dyns.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
    if (caps.color_write_mask) dyns.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, u32(dyns.size()), dyns.data()};
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, count,
                                      count ? state.color_formats : nullptr, state.depth_format, state.stencil_format};
    VkGraphicsPipelineLibraryCreateInfoEXT lib_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &rci,
                                                  VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkGraphicsPipelineCreateInfo gi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib_ci,
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT};
    gi.pMultisampleState = &ms, gi.pColorBlendState = &cb, gi.pDynamicState = &dsi;
    VkPipeline pl = VK_NULL_HANDLE;
    if (const VkResult e = vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &gi, nullptr, &pl); e != VK_SUCCESS) {
        Report(std::format("vkCreateGraphicsPipelines GPL FragOutput {}", (int)e));
        return VK_NULL_HANDLE;
    }
    return pl;
}

VkPipeline Renderer::Impl::GetGplPreRaster(const ShaderRef& vs, const ShaderRef& gs, const FixedPipelineState& state) {
    const bool neg_one = !caps.depth_clip_negative_one_to_one && (state.flags & FixedPipelineState::kNegOne);
    const bool depth_clamp = !caps.depth_clamp_enable && (state.flags & FixedPipelineState::kDepthClamp);
    const bool has_gs = (state.flags & FixedPipelineState::kHasGs) != 0;
    if (!has_gs && !neg_one && !depth_clamp && vs.pl) return vs.pl;
    if (!vs.mod || (has_gs && !gs.mod)) return VK_NULL_HANDLE;
    struct PreKey {
        u64 vs, gs;
        u32 flags, pad;
    } pk{vs.key, has_gs ? gs.key : 0ull, u32(neg_one) | u32(depth_clamp) << 1 | u32(has_gs) << 2, 0};
    const u64 h = ::Gpu::PieceHash::Stable64(&pk, sizeof(pk));
    {
        std::shared_lock lock(prog_lock);
        if (auto it = gpl_prerast.find(h); it != gpl_prerast.end()) return it->second;
    }
    const VkPipeline created = CreateGplPreRaster(vs.mod, has_gs ? gs.mod : VK_NULL_HANDLE, neg_one, depth_clamp);
    if (!created) return VK_NULL_HANDLE;
    std::unique_lock lock(prog_lock);
    auto [it, inserted] = gpl_prerast.try_emplace(h, created);
    if (!inserted) vkDestroyPipeline(dev, created, nullptr);
    return it->second;
}

VkPipeline Renderer::Impl::GetGplFragShader(const ShaderRef& ps, const FixedPipelineState& state) {
    const bool has_ps = (state.flags & FixedPipelineState::kHasPs) != 0;
    const u32 samples = caps.rasterization_samples ? 1u : std::max<u32>(1, state.samples);
    if (!has_ps && samples == 1 && gpl_empty_fs) return gpl_empty_fs;
    if (has_ps && samples == 1 && ps.pl) return ps.pl;
    if (has_ps && !ps.mod) return VK_NULL_HANDLE;
    struct FragKey {
        u64 ps;
        u32 samples, has_ps;
    } fk{has_ps ? ps.key : 0ull, samples, has_ps ? 1u : 0u};
    const u64 h = ::Gpu::PieceHash::Stable64(&fk, sizeof(fk));
    {
        std::shared_lock lock(prog_lock);
        if (auto it = gpl_frag_msaa.find(h); it != gpl_frag_msaa.end()) return it->second;
    }
    const VkPipeline created = CreateGplFragShader(has_ps ? ps.mod : VK_NULL_HANDLE, samples);
    if (!created) return VK_NULL_HANDLE;
    std::unique_lock lock(prog_lock);
    auto [it, inserted] = gpl_frag_msaa.try_emplace(h, created);
    if (!inserted) vkDestroyPipeline(dev, created, nullptr);
    return it->second;
}

VkPipeline Renderer::Impl::GetGplFragOutput(const FixedPipelineState& state, bool square_pass) {
    struct FoiKey {
        VkFormat color_formats[8];
        VkFormat depth_format;
        VkFormat stencil_format;
        u8 color_count;
        u8 samples;
        u8 a2c;
        u8 square_pass;
        u8 square_color_mask;
        u8 square_alpha_mask;
        u8 pad[2];
        FixedAttachmentBlend blend[8];
    } fk{};
    static_assert(sizeof(FoiKey) == 112);
    std::copy_n(state.color_formats, 8, fk.color_formats);
    fk.depth_format = state.depth_format;
    fk.stencil_format = state.stencil_format;
    fk.color_count = state.color_count;
    fk.samples = caps.rasterization_samples ? 1 : std::max<u8>(1, state.samples);
    fk.a2c = (!caps.alpha_to_coverage_enable && (state.flags & FixedPipelineState::kAlphaToCoverage)) ? 1 : 0;
    fk.square_pass = square_pass ? 1 : 0;
    fk.square_color_mask = square_pass ? state.square_color_mask : 0;
    fk.square_alpha_mask = square_pass ? state.square_alpha_mask : 0;
    std::copy_n(state.blend, 8, fk.blend);
    const u64 h = ::Gpu::PieceHash::Stable64(&fk, sizeof(fk));
    {
        std::shared_lock lock(prog_lock);
        if (auto it = gpl_foi.find(h); it != gpl_foi.end()) return it->second;
    }
    const VkPipeline created = CreateGplFragOutput(state, square_pass);
    if (!created) return VK_NULL_HANDLE;
    std::unique_lock lock(prog_lock);
    auto [it, inserted] = gpl_foi.try_emplace(h, created);
    if (!inserted) vkDestroyPipeline(dev, created, nullptr);
    return it->second;
}

VkPipeline Renderer::Impl::LinkGplPipeline(const VkPipeline (&libs)[4], bool lto, bool fail_on_compile) {
    if (fail_on_compile && !caps.cache_control) return VK_NULL_HANDLE;
    VkPipelineLibraryCreateInfoKHR lib_ci{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, nullptr, 4, libs};
    VkPipelineCreateFlags flags = 0;
    if (lto) flags |= VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
    if (fail_on_compile) flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &lib_ci, flags};
    gi.layout = layout;
    VkPipeline pl = VK_NULL_HANDLE;
    const VkResult e = vkCreateGraphicsPipelines(dev, pipeline_cache, 1, &gi, nullptr, &pl);
    if (e == VK_SUCCESS) {
        if (lto && !fail_on_compile) pipeline_cache_dirty.fetch_add(1, std::memory_order_relaxed);
        return pl;
    }
    if (e < 0) Report(std::format("vkCreateGraphicsPipelines GPL Link(lto={}) {}", lto, (int)e));
    return VK_NULL_HANDLE;
}

CompiledPipeline Renderer::Impl::LinkGplPair(const VkPipeline (&main_libs)[4], VkPipeline foi_square, bool lto, bool fail_on_compile) {
    CompiledPipeline pl{};
    pl.main = LinkGplPipeline(main_libs, lto, fail_on_compile);
    if (!pl.main) return {};
    if (foi_square) {
        const VkPipeline sq_libs[4]{main_libs[0], main_libs[1], main_libs[2], foi_square};
        pl.square = LinkGplPipeline(sq_libs, lto, fail_on_compile);
        if (!pl.square) {
            vkDestroyPipeline(dev, pl.main, nullptr);
            return {};
        }
    }
    pl.is_lto = lto;
    return pl;
}

CompiledPipeline Renderer::Impl::GetGraphicsPipeline(const ShaderRef& vs, const ShaderRef& ps, const ShaderRef& gs,
                                                     const FixedPipelineState& raw_state, const CpuRead& cpu) {
    GraphicsPipelineKey gkey{vs.key, ps ? ps.key : 0ull, (raw_state.flags & FixedPipelineState::kHasGs) ? gs.key : 0ull, raw_state};
    gkey.state.Canonicalize(caps);
    const u64 h = gkey.Hash();
    auto& pm = Memos().pipelines;
    if (const auto* m = pm.Find(h)) return *m;
    {
        std::shared_lock lock(prog_lock);
        if (auto it = pipelines.find(h); it != pipelines.end()) return pm[h] = it->second;
    }
    if (caps.path == ShaderPath::Gpl) {
        const VkPipeline vi = gpl_vi[std::min<u8>(gkey.state.topology_class, 2)];
        const VkPipeline prerast = GetGplPreRaster(vs, gs, gkey.state);
        const VkPipeline fs = GetGplFragShader(ps, gkey.state);
        const VkPipeline foi = GetGplFragOutput(gkey.state, false);
        const bool need_sq = gkey.state.NeedsSquarePipeline(caps);
        const VkPipeline foi_sq = need_sq ? GetGplFragOutput(gkey.state, true) : VK_NULL_HANDLE;
        if (vi && prerast && fs && foi && (!need_sq || foi_sq)) {
            const VkPipeline libs[4]{vi, prerast, fs, foi};
            CompiledPipeline pl = LinkGplPair(libs, foi_sq, true, true);
            if (!pl.main) pl = LinkGplPair(libs, foi_sq, false, false);
            if (pl.main) {
                bool queue_lto = false;
                CompiledPipeline ret{};
                {
                    std::unique_lock lock(prog_lock);
                    auto [it, inserted] = pipelines.try_emplace(h, pl);
                    if (!inserted) {
                        vkDestroyPipeline(dev, pl.main, nullptr);
                        if (pl.square) vkDestroyPipeline(dev, pl.square, nullptr);
                    }
                    ret = it->second;
                    if (!ret.is_lto && !stats.trace) queue_lto = gpl_lto_queued.insert(h).second;
                }
                if (queue_lto) {
                    Queue([this, h, vi, prerast, fs, foi, foi_sq] {
                        const VkPipeline lto_libs[4]{vi, prerast, fs, foi};
                        const CompiledPipeline lto = LinkGplPair(lto_libs, foi_sq, true, false);
                        if (!lto.main) return;
                        CompiledPipeline old{};
                        bool used = false;
                        {
                            std::unique_lock lock(prog_lock);
                            if (auto it = pipelines.find(h); it != pipelines.end() && !it->second.is_lto) {
                                old = it->second;
                                it->second = lto;
                                std::atomic_ref(pipeline_epoch).fetch_add(1, std::memory_order_release);
                                used = true;
                            }
                        }
                        if (used) {
                            RetirePipeline(old.main);
                            RetirePipeline(old.square);
                        } else {
                            vkDestroyPipeline(dev, lto.main, nullptr);
                            if (lto.square) vkDestroyPipeline(dev, lto.square, nullptr);
                        }
                    }, 1);
                }
                return pm[h] = ret;
            }
        }
    }
    if (!stats.trace && caps.cache_control) {
        if (const CompiledPipeline hit = CreateMonolithicPipeline(vs, ps, gs, gkey.state, true); hit.main) {
            std::unique_lock lock(prog_lock);
            auto [it, inserted] = pipelines.try_emplace(h, hit);
            if (!inserted) {
                vkDestroyPipeline(dev, hit.main, nullptr);
                if (hit.square) vkDestroyPipeline(dev, hit.square, nullptr);
            }
            return pm[h] = it->second;
        }
    }
    if (stats.trace) {
        const CompiledPipeline created = CreateMonolithicPipeline(vs, ps, gs, gkey.state, false);
        if (!created.main) return {};
        std::unique_lock lock(prog_lock);
        auto [it, inserted] = pipelines.try_emplace(h, created);
        if (!inserted) {
            vkDestroyPipeline(dev, created.main, nullptr);
            if (created.square) vkDestroyPipeline(dev, created.square, nullptr);
        }
        return pm[h] = it->second;
    }
    if (!in_worker && !discovered) discovered = true, Discover(cpu, {walk_chain.size(), 0}, false);
    else if (in_worker && block_now && !block_discovered) block_discovered = true, Discover(cpu, block_now->to, true);
    std::shared_future<CompiledPipeline> f;
    {
        std::unique_lock lock(prog_lock);
        if (auto it = pipelines.find(h); it != pipelines.end()) return pm[h] = it->second;
        auto [it, fresh] = compiling_pipelines.try_emplace(h);
        if (fresh) it->second = AsyncPipeline(h, vs, ps, gs, gkey.state, true);
        f = it->second;
    }
    if (Jit::mode == Jit::Mode::Async && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return waiting = true, CompiledPipeline{};
    CompiledPipeline pl{};
    Stalled(stall_waits, stall_wait_ns, [&] { pl = f.get(); });
    if (!pl.main) return {};
    return pm[h] = pl;
}

VkPipeline Renderer::Impl::IndexProbePipeline(const FixedPipelineState& raw_state) {
    if (!index_probe_mod) return VK_NULL_HANDLE;
    FixedPipelineState s = raw_state;
    s.flags &= ~(FixedPipelineState::kHasPs | FixedPipelineState::kHasGs);
    s.square_color_mask = s.square_alpha_mask = 0;
    s.Canonicalize(caps);
    const u64 h = GraphicsPipelineKey{0, 0, 0, s}.Hash();
    {
        std::shared_lock lock(prog_lock);
        if (auto it = index_probe_pls.find(h); it != index_probe_pls.end()) return it->second;
    }
    const VkPipeline pl = CreateGraphicsPipelineRaw({0, VK_NULL_HANDLE, index_probe_mod, VK_NULL_HANDLE}, {}, {}, s, false, false);
    if (!pl) return VK_NULL_HANDLE;
    std::unique_lock lock(prog_lock);
    auto [it, inserted] = index_probe_pls.try_emplace(h, pl);
    if (!inserted) vkDestroyPipeline(dev, pl, nullptr);
    return it->second;
}
} // namespace Gpu
