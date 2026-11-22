// SPDX-License-Identifier: GPL-2.0-or-later
// JIT strategy: asynchronous discovery (Discover) and parallel compilation on draw misses.
#include "gpu/shaders/shader_jit.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/renderer_impl.h"
#include "main/cpu.h"
#include "orbis/common/thread.h"

namespace Gpu {

Compiled Renderer::Impl::Compile(Gcn::Stage stage, u64 prog, u64 key, Gcn::Input in, std::vector<u32> code, std::vector<u32> fetch,
                                 std::vector<Gcn::SlotSpec> spec) {
    static constexpr VkShaderStageFlagBits vk_stage[]{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    const VkShaderStageFlags next = stage == Gcn::Stage::Vertex ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT : 0;
    ShaderCache& cache = ShaderCache::Instance();
    Compiled c;
    auto make_stage = [&](std::span<const u32> words) {
        if (caps.path == ShaderPath::Objects) {
            c.s = Create({words.begin(), words.end()}, vk_stage[(int)stage], next);
        } else if (stage == Gcn::Stage::Compute) {
            c.pl = CreateComputePipeline(words);
        } else {
            c.mod = CreateShaderModule(words);
            if (caps.path == ShaderPath::Gpl && c.mod) {
                if (stage == Gcn::Stage::Vertex) c.pl = CreateGplPreRaster(c.mod, VK_NULL_HANDLE, false, false);
                else if (stage == Gcn::Stage::Pixel) c.pl = CreateGplFragShader(c.mod, 1);
            }
        }
    };
    // Debug flags: the probe variant (traced frames only) neither reads nor writes the cache; the exploded-vertex one is cached
    // under its own key (ProgramKey's boom bit), every debug run has it. One they only observe (TEMP_STATS) translates again.
    static const bool temp_stats = DebugDeep("GOW3_TEMP_STATS");  // driver statistics of each program
    const bool altered = in.probe;
    if (!(altered || temp_stats)) {  // translated by an earlier run (or by the AOT build)
        if (caps.path == ShaderPath::Objects) {
            const u64 content = cache.ContentOf(key);
            if (const auto bin = content ? cache.FindBinary(BinaryKey(content)) : nullptr) {  // the driver's own code for this GPU: no compile at all
                c.s = CreateFrom(VK_SHADER_CODE_TYPE_BINARY_EXT, bin->data.data(), bin->data.size(), vk_stage[(int)stage], next), c.out_mask = bin->out_mask;
                if (c.s) return c;
            }
        }
        if (const auto hit = cache.Find(key)) {
            make_stage(hit->spirv), c.out_mask = hit->out_mask;
            if (c.Ready(caps.path, stage)) return c;
        }
    }
    in.code = code, in.fetch = fetch;
    std::vector<u32> spirv;
    u64 mask = 0;
    if (Gcn::EmitSpirv(Gcn::Analyze(in), in, spec, spirv, mask, c.err)) {
        make_stage(spirv), c.out_mask = mask;  // exported parameters, for the rect list geometry shader
        if (temp_stats && !spirv.empty() && stage != Gcn::Stage::Compute) {
            std::string e2;
            static const std::vector<u32> vs0 = compiler.Spirv("#version 460\nvoid main() { gl_Position = vec4(0.0); }\n", shaderc_vertex_shader, e2);
            VkShaderModule m[2]{};
            const std::vector<u32>* code[2]{stage == Gcn::Stage::Vertex ? &spirv : &vs0, &spirv};
            const u32 nst = stage == Gcn::Stage::Vertex ? 1 : 2;
            VkPipelineShaderStageCreateInfo st[2]{};
            for (u32 k = 0; k < nst; ++k) {
                VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code[k]->size() * 4, code[k]->data()};
                vkCreateShaderModule(dev, &mi, nullptr, &m[k]);
                st[k] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, k ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, m[k], "main"};
            }
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT};
            VkPipelineColorBlendAttachmentState cba[8]{};
            for (auto& a : cba) a.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 8, cba};
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            const VkDynamicState dyns[]{VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
            VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyns};
            VkFormat fmts[8];
            std::fill(fmts, fmts + 8, VK_FORMAT_R32G32B32A32_SFLOAT);
            VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, nst == 2 ? 8u : 0u, fmts, VK_FORMAT_D32_SFLOAT};
            VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR, nst, st, &vi, &ia,
                                            nullptr, &vps, &rs, &ms, &ds, nst == 2 ? &cb : nullptr, &dsi, layout};
            VkPipeline pl = VK_NULL_HANDLE;
            std::string line = std::format("TEMP stats {} {:x}:", stage == Gcn::Stage::Vertex ? "vs" : "ps", prog);
            if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pl) == VK_SUCCESS) {
                auto get_exec = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
                auto get_stat = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");
                VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pl};
                u32 ne = 0;
                get_exec(dev, &pi, &ne, nullptr);
                std::vector<VkPipelineExecutablePropertiesKHR> ep(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
                get_exec(dev, &pi, &ne, ep.data());
                for (u32 x = 0; x < ne; ++x) {
                    if (!(ep[x].stages & vk_stage[(int)stage])) continue;
                    VkPipelineExecutableInfoKHR xi{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, nullptr, pl, x};
                    u32 ns = 0;
                    get_stat(dev, &xi, &ns, nullptr);
                    std::vector<VkPipelineExecutableStatisticKHR> sv(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
                    get_stat(dev, &xi, &ns, sv.data());
                    for (auto& s : sv)
                        line += std::format(" [{}]={}", s.name, s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR ? s.value.f64 : (double)s.value.u64);
                }
                vkDestroyPipeline(dev, pl, nullptr);
            } else {
                line += " pipeline failed";
            }
            for (auto mm : m)
                if (mm) vkDestroyShaderModule(dev, mm, nullptr);
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }
    if (c.Ready(caps.path, stage) && !altered) cache.Record(key, (u32)stage, c.out_mask, spirv);
    return c;
}

u32 Renderer::Impl::Prepare(u64 key, u64 binary, u32 stage) {
    if (caps.path != ShaderPath::Objects) return 0;
    static constexpr VkShaderStageFlagBits vk_stage[]{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    if (stage >= std::size(vk_stage)) return 2;
    {
        std::shared_lock lock(prog_lock);
        if (shaders.contains(key) || compiling.contains(key)) return 0;
    }
    const auto bin = ShaderCache::Instance().FindBinary(binary);
    const bool vertex = stage == u32(Gcn::Stage::Vertex);
    const VkShaderEXT s = bin ? CreateFrom(VK_SHADER_CODE_TYPE_BINARY_EXT, bin->data.data(), bin->data.size(), vk_stage[stage],
                                           vertex ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT : 0)
                              : VK_NULL_HANDLE;
    if (!s) return 2;
    std::unique_lock lock(prog_lock);
    if (shaders.contains(key)) return lock.unlock(), DestroyShader(dev, s, nullptr), 0;  // a draw made it meanwhile
    if (vertex) vs_outputs[key] = bin->out_mask;
    shaders[key] = {key, s, VK_NULL_HANDLE, VK_NULL_HANDLE};
    return 1;
}

void Renderer::Impl::Enqueue(Gcn::Stage stage, u64 prog, const u32* user, const CpuRead& cpu, u32 rsrc1, u32 rsrc2, bool rect, bool urgent) {
    Prepared p;
    if (!prog || !Identify(stage, prog, user, cpu, rsrc1, rsrc2, rect, p)) return;
    if (HasClearRectVs() && p.in.rect && ClearVs(p.in.code)) return;
    Specify(stage, p);
    ShaderCache& cache = ShaderCache::Instance();
    {
        std::shared_lock lock(prog_lock);
        if (shaders.contains(p.key) || compiling.contains(p.key)) return;
    }
    const u64 content = cache.ContentOf(p.key);
    const bool built = caps.path == ShaderPath::Objects ? (content && cache.ContainsBinary(BinaryKey(content))) : (content != 0);  // a short load, which a draw waits for
    if (urgent && !built) return;
    std::unique_lock lock(prog_lock);
    if (!shaders.contains(p.key) && !compiling.contains(p.key)) compiling.emplace(p.key, Async(stage, prog, p, urgent, built));
}

std::shared_future<Compiled> Renderer::Impl::Async(Gcn::Stage stage, u64 prog, const Prepared& p, bool miss, bool waited) {
    stall_compiles.fetch_add(1, std::memory_order_relaxed);
    // What makes it a new variant: the parts of its key (Prepare, Shader) against its program's last variant. The PS state
    // runs from ps_input_ena to mrt_map in Gcn::Input.
    const Gcn::Input& pin = p.in;
    Variant now{::Gpu::PieceHash::ShaderCode(pin.code.data(), pin.code.size_bytes()), ::Gpu::PieceHash::ShaderCode(pin.fetch.data(), pin.fetch.size_bytes()),
                ::Gpu::PieceHash::ShaderVariant(&pin.ps_input_ena, size_t((const char*)pin.threads - (const char*)&pin.ps_input_ena)), ::Gpu::PieceHash::ShaderVariant(pin.threads, sizeof(pin.threads)),
                pin.rsrc1, pin.rsrc2, p.rect, p.spec};
    Stats::Compile rec{prog, p.key, (u32)stage, miss ? 0u : 1u << 31, ~0u};
    auto pack = [](const Gcn::SlotSpec& s, u32* w) {
        w[0] = u32(s.dim) | u32(s.cls) << 4 | s.unnormalized << 8 | s.degamma << 9 | s.srgb << 10 | s.vfmt << 11 | u32(s.dfmt) << 12 | u32(s.nfmt) << 16 | u32(s.dst_sel) << 20;
        w[1] = s.part;
    };
    if (auto [it, first] = variants.try_emplace(prog | (u64)stage); first) {
        rec.changed |= 1, it->second = std::move(now);
    } else {
        Variant& v = it->second;
        rec.changed |= (v.code != now.code) << 1 | (v.fetch != now.fetch) << 2 | (v.rsrc1 != now.rsrc1) << 3 | (v.rsrc2 != now.rsrc2) << 4 |
                       (v.rect != now.rect) << 5 | (v.spec.size() != now.spec.size()) << 6 | (v.ps != now.ps) << 8 | (v.threads != now.threads) << 9;
        for (size_t k = 0; k < std::min(v.spec.size(), now.spec.size()); ++k)
            if (Gcn::Pack(v.spec[k]) != Gcn::Pack(now.spec[k])) {
                rec.changed |= 128, rec.slot = u32(k), pack(v.spec[k], rec.was), pack(now.spec[k], rec.now);
                break;
            }
        v = std::move(now);
    }
    ShaderCache::Instance().Queued();
    auto task = std::make_shared<std::packaged_task<Compiled()>>([this, stage, prog, key = p.key, rec, in = p.in, waited,
                                                                   code = std::vector<u32>(p.in.code.begin(), p.in.code.end()),
                                                                   fetch = std::vector<u32>(p.in.fetch.begin(), p.in.fetch.end()), spec = p.spec]() mutable {
#ifdef _WIN32
        if (waited) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);  // the worker sets its own again for the next task
#else
        if (waited) Common::SetCurrentThreadPriority(Common::ThreadPriority::Normal);
#endif
        const auto t = std::chrono::steady_clock::now();
        Compiled c = Compile(stage, prog, key, in, std::move(code), std::move(fetch), std::move(spec));
        ShaderCache::Instance().Completed();
        rec.us = u32(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
        std::scoped_lock lock(compiled_lock);
        compiled.push_back(rec);
        return c;
    });
    auto future = task->get_future().share();
    Queue([task] { (*task)(); }, miss ? 0 : 1);
    return future;
}

void Renderer::Impl::Queue(std::function<void()> task, u32 priority) {
    {
        std::scoped_lock lock(task_lock);
        if (compilers.empty())
            for (unsigned i = 0; i < Gow3::Cpu::plan.shader_threads; ++i)  // idle workers sleep
                compilers.emplace_back([this] {
                    for (;;) {
                        std::function<void()> next;
                        {
                            std::unique_lock lock(task_lock);
                            task_cv.wait(lock, [&] { return task_stop || !tasks[0].empty() || !tasks[1].empty(); });
                            auto& queue = tasks[0].empty() ? tasks[1] : tasks[0];
                            if (queue.empty()) return;  // stopping, nothing left
                            next = std::move(queue.front()), queue.pop_front();
                        }
                        // Sync: the game waits for the shader. Async: it keeps running, and its threads come first.
#ifdef _WIN32
                        SetThreadPriority(GetCurrentThread(), Jit::mode == Jit::Mode::Async ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#else
                        Common::SetCurrentThreadPriority(Jit::mode == Jit::Mode::Async ? Common::ThreadPriority::Low : Common::ThreadPriority::Normal);
#endif
                        next();
                    }
                });
        tasks[priority].push_back(std::move(task));
    }
    task_cv.notify_one();
}

// Runs what is queued, then ends the workers.
void Renderer::Impl::StopCompilers() {
    {
        std::scoped_lock lock(task_lock);
        task_stop = true;
    }
    task_cv.notify_all();
    for (auto& t : compilers) t.join();
    compilers.clear();
}

// The rest of the frame from the packet being executed, registers only: the programs its draws and dispatches select
// (as Draw and Dispatch do) go to the compile threads. The registers are restored afterwards.
void Renderer::Impl::Discover(const CpuRead& cpu, Pos to, bool urgent) {
    const auto saved = std::make_unique<Regs>();
    SaveRegs(*saved);
    Walk(walk_chain, {walk_seg, walk_pos}, to, [&](u32 op, const u32* b, size_t, size_t) {
        if (op == 0x27 || op == 0x2D || op == 0x35) {
            const bool rect = RectVs(op, b, cpu);
            const u32 mode = ctx[0x202] >> 4 & 7;
            Enqueue(Gcn::Stage::Vertex, (u64)(sh[0x48] | (u64)sh[0x49] << 32) << 8, &sh[0x4C], cpu, sh[0x4A], sh[0x4B], rect, urgent);
            if (mode == 1 || ctx[0x203] & 0x41)
                Enqueue(Gcn::Stage::Pixel, (u64)(sh[0x08] | (u64)sh[0x09] << 32) << 8, &sh[0x0C], cpu, sh[0x0A], sh[0x0B], uc[0x242] == 0x11, urgent);
        } else if (op == 0x15) {
            Enqueue(Gcn::Stage::Compute, (u64)(sh[0x20C] | (u64)sh[0x20D] << 32) << 8, &sh[0x240], cpu, sh[0x212], sh[0x213], false, urgent);
        }
    });
    LoadRegs(*saved);
}
} // namespace Gpu
