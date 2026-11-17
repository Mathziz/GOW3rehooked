// xferbench: CPU -> GPU transfer strategies for one real GOW3 frame (framecap payload), headless Vulkan.
//   xferbench <frame.pay> [frames]
// Each simulated frame uploads the CPU-written blocks the GPU reads (ring slice, arena) and runs a compute pass that
// reads exactly the recorded spans, with 3 frames in flight. Also measures GPU->GPU copies of the aliasing sizes and
// the latency of a small GPU -> host write (readback) as seen by a CPU poll.
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <execution>
#include <stdexcept>
#include <string>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Clock = std::chrono::steady_clock;

static const u32 kShader[] = {
#include "xfer.comp.inc"
};

static void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(r));
}

struct Span { u64 off, size; };
struct BlockIn { u32 cls; std::vector<u8> bytes; std::vector<Span> spans; };

static std::vector<BlockIn> LoadPayload(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) throw std::runtime_error("cannot open payload");
    char magic[8];
    u32 n = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "GOW3PAY1", 8) || std::fread(&n, 4, 1, f) != 1)
        throw std::runtime_error("bad payload");
    std::vector<BlockIn> out;
    for (u32 i = 0; i < n; ++i) {
        BlockIn b;
        u32 ns;
        u64 bytes;
        std::fread(&b.cls, 4, 1, f), std::fread(&ns, 4, 1, f), std::fread(&bytes, 8, 1, f);
        b.bytes.resize(bytes);
        std::fread(b.bytes.data(), 1, bytes, f);
        b.spans.resize(ns);
        std::fread(b.spans.data(), sizeof(Span), ns, f);
        std::erase_if(b.spans, [](const Span& s) { return !s.size; });
        if (!b.spans.empty()) out.push_back(std::move(b));  // blocks the GPU never reads need no upload
    }
    std::fclose(f);
    return out;
}

struct Buffer { VkBuffer buf{}; VkDeviceMemory mem{}; u8* map{}; u64 size{}; };

struct Vk {
    VkInstance inst{};
    VkPhysicalDevice phys{};
    VkDevice dev{};
    u32 gfx_family = ~0u, xfer_family = ~0u;
    VkQueue gfx{}, xfer{};
    VkPhysicalDeviceMemoryProperties mp{};
    float ts_period = 1;

    u32 Type(u32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
        for (u32 i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & avoid)) return i;
        throw std::runtime_error("no memory type");
    }
    Buffer Make(u64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const {
        Buffer b;
        b.size = size;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_CONCURRENT};
        u32 fams[2] = {gfx_family, xfer_family};
        bi.queueFamilyIndexCount = gfx_family == xfer_family ? 1 : 2;
        bi.pQueueFamilyIndices = fams;
        if (bi.queueFamilyIndexCount == 1) bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(dev, &bi, nullptr, &b.buf), "buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b.buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, Type(req.memoryTypeBits, want, avoid)};
        Check(vkAllocateMemory(dev, &ai, nullptr, &b.mem), "alloc");
        Check(vkBindBufferMemory(dev, b.buf, b.mem, 0), "bind");
        if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) Check(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.map), "map");
        return b;
    }
};

static Vk Init() {
    Vk v;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "xferbench", 1, nullptr, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    Check(vkCreateInstance(&ii, nullptr, &v.inst), "instance");
    u32 n = 1;
    vkEnumeratePhysicalDevices(v.inst, &n, &v.phys);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(v.phys, &props);
    v.ts_period = props.limits.timestampPeriod;
    std::printf("device %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(v.phys, &v.mp);
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, qf.data());
    for (u32 i = 0; i < qn; ++i) {
        if (v.gfx_family == ~0u && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) v.gfx_family = i;
        if (v.xfer_family == ~0u && (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            v.xfer_family = i;
    }
    if (v.xfer_family == ~0u) v.xfer_family = v.gfx_family;
    float prio = 1;
    VkDeviceQueueCreateInfo qi[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, v.gfx_family, 1, &prio},
                                     {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, v.xfer_family, 1, &prio}};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    f13.synchronization2 = VK_TRUE;
    const char* ext[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, v.gfx_family == v.xfer_family ? 1u : 2u, qi, 0, nullptr, 1, ext};
    Check(vkCreateDevice(v.phys, &di, nullptr, &v.dev), "device");
    vkGetDeviceQueue(v.dev, v.gfx_family, 0, &v.gfx);
    vkGetDeviceQueue(v.dev, v.xfer_family, 0, &v.xfer);
    return v;
}

static void Barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, src, sa, dst, da};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

struct Stat {
    std::vector<double> v;
    double Avg() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
    double Pct(double p) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(p * v.size()))]; }
};

static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// CPU dirty tracking of heap pages written every frame (heapwatch: ~15 pages of 4 KB-64 KB per frame).
// 1) Windows write watch over a 4 GB reserved view with 1 GB committed and touched; 2) write-protect + fault + unprotect.
static u8* g_fault_lo;
static u64 g_fault_n;
static LONG CALLBACK OnFault(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    u8* a = (u8*)r->ExceptionInformation[1];
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || a < g_fault_lo || a >= g_fault_lo + g_fault_n) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect((void*)((uintptr_t)a & ~0xFFFull), 0x1000, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void DirtyTracking() {
    const u64 kView = 4ull << 30, kCommit = 1ull << 30, kPages = 16;
    std::printf("\nCPU dirty tracking, %llu scattered pages written per frame:\n", (unsigned long long)kPages);
    u8* view = (u8*)VirtualAlloc(nullptr, kView, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (view && VirtualAlloc(view, kCommit, MEM_COMMIT, PAGE_READWRITE)) {
        for (u64 o = 0; o < kCommit; o += 0x1000) view[o] = 1;
        ResetWriteWatch(view, kView);
        std::vector<void*> addrs(4096);
        Stat s;
        for (u32 k = 0; k < 300; ++k) {
            for (u64 i = 0; i < kPages; ++i) view[(i * 7919 + k) % (kCommit / 0x1000) * 0x1000] = (u8)k;
            ULONG_PTR count = addrs.size();
            DWORD gran;
            auto t0 = Clock::now();
            GetWriteWatch(WRITE_WATCH_FLAG_RESET, view, kView, addrs.data(), &count, &gran);
            if (k >= 20) s.v.push_back(Ms(t0, Clock::now()));
        }
        std::printf("  write watch (GetWriteWatch+reset over 4 GB view, 1 GB committed): avg %.3f ms, p99 %.3f ms per frame\n", s.Avg(), s.Pct(0.99));
    } else {
        std::printf("  write watch: allocation failed (%lu)\n", GetLastError());
    }
    if (view) VirtualFree(view, 0, MEM_RELEASE);

    g_fault_n = kPages * 0x1000 * 64;
    g_fault_lo = (u8*)VirtualAlloc(nullptr, g_fault_n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* veh = AddVectoredExceptionHandler(1, OnFault);
    Stat s;
    for (u32 k = 0; k < 2000; ++k) {
        auto t0 = Clock::now();
        DWORD old;
        for (u64 i = 0; i < kPages; ++i) VirtualProtect(g_fault_lo + i * 64 * 0x1000, 0x1000, PAGE_READONLY, &old);
        for (u64 i = 0; i < kPages; ++i) g_fault_lo[i * 64 * 0x1000 + (k & 0xFFF)] = (u8)k;  // faults, handler unprotects
        if (k >= 100) s.v.push_back(Ms(t0, Clock::now()));
    }
    RemoveVectoredExceptionHandler(veh);
    VirtualFree(g_fault_lo, 0, MEM_RELEASE);
    std::printf("  protect + fault + unprotect: avg %.3f ms, p99 %.3f ms per frame (%.2f us per page)\n", s.Avg(), s.Pct(0.99),
                s.Avg() * 1000 / kPages);
}

int main(int argc, char** argv) {
    if (argc < 2) return std::printf("usage: xferbench <frame.pay> [frames]\n"), 1;
    const u32 kFrames = argc > 2 ? std::atoi(argv[2]) : 3000, kWarm = 300, kSlots = 3;
    try {
        auto blocks = LoadPayload(argv[1]);
        // Slot layout: blocks back to back (256-byte aligned); chunks of <= 256 dwords over the spans.
        std::vector<u64> place;
        u64 slot = 0, read_bytes = 0, block_bytes = 0, nspans = 0;
        std::vector<std::array<u32, 2>> chunks;
        for (auto& b : blocks) {
            place.push_back(slot);
            for (auto& s : b.spans) {
                u64 lo = (slot + s.off) / 4, hi = (slot + s.off + s.size + 3) / 4;
                for (u64 d = lo; d < hi; d += 256) chunks.push_back({(u32)d, (u32)std::min<u64>(256, hi - d)});
                read_bytes += s.size, ++nspans;
            }
            block_bytes += b.bytes.size();
            slot += (b.bytes.size() + 255) & ~255ull;
        }
        std::printf("payload: %zu blocks, %.0f KB contiguous, %llu spans, %.0f KB read by the GPU, %zu chunks\n", blocks.size(),
                    block_bytes / 1024.0, (unsigned long long)nspans, read_bytes / 1024.0, chunks.size());

        Vk v = Init();
        const auto HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const auto DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const auto SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, SRC = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, DST = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Buffer rebar = v.Make(slot * kSlots, SSBO, DL | HV);                 // A: CPU writes straight into VRAM
        Buffer host = v.Make(slot * kSlots, SSBO | SRC, HV, DL);             // B source / C: system memory read over PCIe
        Buffer vram = v.Make(slot * kSlots, SSBO | DST | SRC, DL, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);  // B destination
        Buffer chunk_buf = v.Make(chunks.size() * 8, SSBO, DL | HV);
        std::memcpy(chunk_buf.map, chunks.data(), chunks.size() * 8);
        Buffer outb = v.Make(256, SSBO | DST, DL, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        // D: memory the game itself writes (VirtualAlloc, like the guest ring/arenas), imported once into Vulkan; the
        // transfer queue DMA-copies it to VRAM, the layer does no memcpy. The game's own writes are not timed.
        Buffer guest;
        {
            guest.size = slot * kSlots;
            guest.map = (u8*)VirtualAlloc(nullptr, guest.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            auto get_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(v.dev, "vkGetMemoryHostPointerPropertiesEXT");
            VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
            Check(get_props(v.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, guest.map, &hp), "host pointer props");
            VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, guest.size, SRC, VK_SHARING_MODE_CONCURRENT};
            u32 fams[2] = {v.gfx_family, v.xfer_family};
            bi.queueFamilyIndexCount = 2, bi.pQueueFamilyIndices = fams;
            Check(vkCreateBuffer(v.dev, &bi, nullptr, &guest.buf), "guest buffer");
            VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
                                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, guest.map};
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(v.dev, guest.buf, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, guest.size,
                                    v.Type(hp.memoryTypeBits & req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0)};
            auto t0 = Clock::now();
            Check(vkAllocateMemory(v.dev, &ai, nullptr, &guest.mem), "import");
            Check(vkBindBufferMemory(v.dev, guest.buf, guest.mem, 0), "bind guest");
            std::printf("imported %.1f MB of game-written memory once: %.2f ms\n", guest.size / 1048576.0, Ms(t0, Clock::now()));
        }

        // Compute pipeline.
        VkDescriptorSetLayoutBinding binds[3];
        for (u32 i = 0; i < 3; ++i) binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 3, binds};
        VkDescriptorSetLayout dsl;
        Check(vkCreateDescriptorSetLayout(v.dev, &dli, nullptr, &dsl), "dsl");
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pcr};
        VkPipelineLayout pl;
        Check(vkCreatePipelineLayout(v.dev, &pli, nullptr, &pl), "pl");
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kShader), kShader};
        VkShaderModule sm;
        Check(vkCreateShaderModule(v.dev, &smi, nullptr, &sm), "shader");
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"};
        cpi.layout = pl;
        VkPipeline pipe;
        Check(vkCreateComputePipelines(v.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe), "pipeline");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 3, 1, &ps};
        VkDescriptorPool dp;
        Check(vkCreateDescriptorPool(v.dev, &dpi, nullptr, &dp), "dpool");
        auto make_set = [&](const Buffer& data) {
            VkDescriptorSet s;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, dp, 1, &dsl};
            Check(vkAllocateDescriptorSets(v.dev, &ai, &s), "set");
            VkDescriptorBufferInfo bi[3] = {{data.buf, 0, VK_WHOLE_SIZE}, {chunk_buf.buf, 0, VK_WHOLE_SIZE}, {outb.buf, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[3];
            for (u32 i = 0; i < 3; ++i)
                w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[i]};
            vkUpdateDescriptorSets(v.dev, 3, w, 0, nullptr);
            return s;
        };
        VkDescriptorSet set_rebar = make_set(rebar), set_host = make_set(host), set_vram = make_set(vram);

        // Per-slot command buffers, fences, timestamps; a timeline semaphore for the transfer queue variant.
        auto pool_for = [&](u32 fam) {
            VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam};
            VkCommandPool p;
            Check(vkCreateCommandPool(v.dev, &ci, nullptr, &p), "cmdpool");
            return p;
        };
        VkCommandPool gpool = pool_for(v.gfx_family), xpool = pool_for(v.xfer_family);
        std::array<VkCommandBuffer, kSlots> gcb, xcb;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, gpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, kSlots};
        Check(vkAllocateCommandBuffers(v.dev, &cai, gcb.data()), "cb");
        cai.commandPool = xpool;
        Check(vkAllocateCommandBuffers(v.dev, &cai, xcb.data()), "xcb");
        std::array<VkFence, kSlots> fence;
        for (auto& f : fence) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
            Check(vkCreateFence(v.dev, &fi, nullptr, &f), "fence");
        }
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, kSlots * 2};
        VkQueryPool qp;
        Check(vkCreateQueryPool(v.dev, &qpi, nullptr, &qp), "query pool");
        VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
        VkSemaphore timeline;
        Check(vkCreateSemaphore(v.dev, &sci, nullptr, &timeline), "timeline");
        u64 tl_value = 0;
        const u32 groups = (u32)chunks.size();

        auto dispatch = [&](VkCommandBuffer cb, VkDescriptorSet set, u32 s) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
            u32 pc[2] = {(u32)(s * slot / 4), groups};
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
            vkCmdDispatch(cb, std::min(groups, 4096u), (groups + 4095) / 4096, 1);
        };
        // CPU copy of the frame's data: whole blocks (contiguous) or only the spans the GPU reads (gather), on N threads.
        auto upload = [&](u8* dst, bool gather, u32 threads) {
            struct Job { u8* d; const u8* s; u64 n; };
            std::vector<Job> jobs;
            for (size_t i = 0; i < blocks.size(); ++i) {
                if (!gather) {
                    u64 n = blocks[i].bytes.size(), part = (n + threads - 1) / threads;
                    for (u64 o = 0; o < n; o += part) jobs.push_back({dst + place[i] + o, blocks[i].bytes.data() + o, std::min(part, n - o)});
                } else {
                    for (auto& s : blocks[i].spans) jobs.push_back({dst + place[i] + s.off, blocks[i].bytes.data() + s.off, s.size});
                }
            }
            if (threads == 1) for (auto& j : jobs) std::memcpy(j.d, j.s, j.n);
            else std::for_each(std::execution::par, jobs.begin(), jobs.end(), [](const Job& j) { std::memcpy(j.d, j.s, j.n); });
        };

        enum Mode { RebarContig, RebarContigMt, RebarGather, StageSameQueue, StageXferQueue, HostDirect, VramResident, ImportXferQueue };
        const char* names[] = {"A  ReBAR, contiguous, 1 thread", "A  ReBAR, contiguous, par", "A  ReBAR, spans only (gather)",
                               "B  staging + copy, same queue", "B  staging + copy, transfer queue", "C  GPU reads system RAM",
                               "-- data already in VRAM (floor)", "D  imported game memory, DMA queue"};
        std::printf("%-36s %10s %10s %10s %10s %10s\n", "strategy (per frame)", "cpu avg", "cpu p99", "gpu avg", "gpu p99", "frame avg");
        for (int mode = RebarContig; mode <= ImportXferQueue; ++mode) {
            Stat cpu, gpu;
            auto wall0 = Clock::now();
            for (u32 k = 0; k < kWarm + kFrames; ++k) {
                if (k == kWarm) wall0 = Clock::now();
                u32 s = k % kSlots;
                Check(vkWaitForFences(v.dev, 1, &fence[s], VK_TRUE, UINT64_MAX), "wait");
                if (k >= kSlots + kWarm) {
                    u64 ts[2];
                    if (vkGetQueryPoolResults(v.dev, qp, s * 2, 2, 16, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                        gpu.v.push_back((ts[1] - ts[0]) * v.ts_period / 1e6);
                }
                vkResetFences(v.dev, 1, &fence[s]);
                if (mode == ImportXferQueue) upload(guest.map + s * slot, false, 1);  // the game writing its own memory
                auto t0 = Clock::now();
                if (mode <= RebarGather) upload(rebar.map + s * slot, mode == RebarGather, mode == RebarContigMt ? 4 : 1);
                else if (mode != VramResident && mode != ImportXferQueue) upload(host.map + s * slot, false, 1);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                std::vector<VkBufferCopy> regions;
                for (size_t i = 0; i < blocks.size(); ++i) regions.push_back({s * slot + place[i], s * slot + place[i], blocks[i].bytes.size()});
                bool xq = mode == StageXferQueue || mode == ImportXferQueue;
                if (xq) {
                    vkResetCommandBuffer(xcb[s], 0);
                    vkBeginCommandBuffer(xcb[s], &bi);
                    vkCmdCopyBuffer(xcb[s], mode == ImportXferQueue ? guest.buf : host.buf, vram.buf, (u32)regions.size(), regions.data());
                    vkEndCommandBuffer(xcb[s]);
                    ++tl_value;
                    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr, 0, nullptr, 1, &tl_value};
                    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi, 0, nullptr, nullptr, 1, &xcb[s], 1, &timeline};
                    Check(vkQueueSubmit(v.xfer, 1, &si, VK_NULL_HANDLE), "xfer submit");
                }
                VkCommandBuffer cb = gcb[s];
                vkResetCommandBuffer(cb, 0);
                vkBeginCommandBuffer(cb, &bi);
                vkCmdResetQueryPool(cb, qp, s * 2, 2);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, s * 2);
                if (mode == StageSameQueue) {
                    vkCmdCopyBuffer(cb, host.buf, vram.buf, (u32)regions.size(), regions.data());
                    Barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                }
                dispatch(cb, mode <= RebarGather ? set_rebar : mode == HostDirect ? set_host : set_vram, s);  // D reads VRAM too
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, s * 2 + 1);
                vkEndCommandBuffer(cb);
                VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr, 1, &tl_value};
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, xq ? &tsi : nullptr, xq ? 1u : 0u, &timeline, &wait_stage, 1, &cb};
                Check(vkQueueSubmit(v.gfx, 1, &si, fence[s]), "submit");
                if (k >= kWarm) cpu.v.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
            }
            vkDeviceWaitIdle(v.dev);
            double frame = std::chrono::duration<double, std::milli>(Clock::now() - wall0).count() / kFrames;
            std::printf("%-36s %8.3f ms %8.3f ms %8.3f ms %8.3f ms %8.3f ms\n", names[mode], cpu.Avg(), cpu.Pct(0.99), gpu.Avg(), gpu.Pct(0.99), frame);
        }

        // GPU -> GPU copies at the aliasing sizes (streaming compute target 4096x480 RGBA32, RT 896x480 RGBA8).
        std::printf("\nGPU->GPU copy (VRAM to VRAM), timestamps:\n");
        for (u64 bytes : {4096ull * 480 * 16, 896ull * 480 * 4, 2048ull * 2048 * 4}) {
            Buffer a = v.Make(bytes, SRC, DL, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT), b = v.Make(bytes, DST, DL, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            Stat g;
            for (u32 k = 0; k < 200; ++k) {
                VkCommandBuffer cb = gcb[0];
                Check(vkWaitForFences(v.dev, 1, &fence[0], VK_TRUE, UINT64_MAX), "wait");
                vkResetFences(v.dev, 1, &fence[0]);
                vkResetCommandBuffer(cb, 0);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                vkBeginCommandBuffer(cb, &bi);
                vkCmdResetQueryPool(cb, qp, 0, 2);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qp, 0);
                VkBufferCopy r{0, 0, bytes};
                vkCmdCopyBuffer(cb, a.buf, b.buf, 1, &r);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qp, 1);
                vkEndCommandBuffer(cb);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};
                Check(vkQueueSubmit(v.gfx, 1, &si, fence[0]), "submit");
                Check(vkWaitForFences(v.dev, 1, &fence[0], VK_TRUE, UINT64_MAX), "wait");
                u64 ts[2];
                vkGetQueryPoolResults(v.dev, qp, 0, 2, 16, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (k >= 20) g.v.push_back((ts[1] - ts[0]) * v.ts_period / 1e6);
            }
            std::printf("  %8.2f MB: avg %.3f ms, p99 %.3f ms\n", bytes / 1048576.0, g.Avg(), g.Pct(0.99));
            vkDestroyBuffer(v.dev, a.buf, nullptr), vkFreeMemory(v.dev, a.mem, nullptr);
            vkDestroyBuffer(v.dev, b.buf, nullptr), vkFreeMemory(v.dev, b.mem, nullptr);
        }

        // Readback: after the frame's compute, the GPU writes 1 KB (luminance/query/label sized) into host memory.
        // Latency = CPU sees the new value by polling - submit time; fence = when vkWaitForFences returns instead.
        std::printf("\nreadback of 1 KB GPU -> host after the frame's work:\n");
        {
            Buffer rb = v.Make(1024, DST, HV | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            Stat poll, fen;
            for (u32 k = 0; k < 500; ++k) {
                VkCommandBuffer cb = gcb[0];
                Check(vkWaitForFences(v.dev, 1, &fence[0], VK_TRUE, UINT64_MAX), "wait");
                vkResetFences(v.dev, 1, &fence[0]);
                upload(rebar.map, false, 1);
                vkResetCommandBuffer(cb, 0);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                vkBeginCommandBuffer(cb, &bi);
                dispatch(cb, set_rebar, 0);
                Barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdFillBuffer(cb, rb.buf, 0, 1024, k + 1);
                vkEndCommandBuffer(cb);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};
                auto t0 = Clock::now();
                Check(vkQueueSubmit(v.gfx, 1, &si, fence[0]), "submit");
                volatile u32* p = (volatile u32*)(rb.map + 1020);
                while (*p != k + 1) YieldProcessor();
                auto t1 = Clock::now();
                Check(vkWaitForFences(v.dev, 1, &fence[0], VK_TRUE, UINT64_MAX), "wait");
                auto t2 = Clock::now();
                if (k >= 20) {
                    poll.v.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                    fen.v.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
                }
            }
            std::printf("  value visible to CPU poll: avg %.3f ms, p99 %.3f ms (submit -> seen, includes the frame's compute)\n",
                        poll.Avg(), poll.Pct(0.99));
            std::printf("  fence signaled:            avg %.3f ms, p99 %.3f ms\n", fen.Avg(), fen.Pct(0.99));
        }
        vkDeviceWaitIdle(v.dev);
        DirtyTracking();
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    return 0;
}
