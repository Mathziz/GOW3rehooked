// framecap: coherent snapshot of one GOW3 frame read straight from gow3.exe memory, plus an offline census.
//   capture <out.cap>   waits for the submit of a frame (main ctx chain head changes), suspends the game, saves the
//                       frame's command chain, data ring slice, shader binaries, EBOOT .data, arenas and texture views
//   analyze <in.cap> [draws.csv]   per draw state/tables census (multithreaded), raw file is never modified
//   record <out.rec> [--running] [--frames N]   every frame from launch (waits for a new gow3.exe; --running attaches
//                       to the current one, --frames stops after N submits), lossless: the game stays
//                       suspended while the store falls behind; 4 KB pages stored once by content (xxh3), zstd;
//                       --heap also stores the asset heap each frame's draws read (offline Vulkan replay)
//   timeline <in.rec> <out.csv>    one census row per recorded frame
//   extract <in.rec> <seq> <out.cap>   one recorded frame as a .cap for analyze/find
//   census <in.rec> <prefix>       whole-recording lifetimes (programs, textures), totals and non-draw packets
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <execution>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define XXH_INLINE_ALL
#include "gpu/shaders/gcn_translator.h"
#include "gpu/vk_gpu.h"
#include "gpu/piece_hash.h"
#include "shaderc/shaderc.h"
#include "spirv-tools/libspirv.h"
#include "xxhash.h"
#include "zstd.h"

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// The renderer's one use of the game's common/thread.cpp (which needs the whole kernel): the texture reaper's pacing.
namespace Common {
bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds*, bool, bool) {
    std::this_thread::sleep_for(duration);
    return true;
}
} // namespace Common
namespace Gow3 { void Log(std::string_view) {} }

// Ghidra VAs (live = VA + delta).
constexpr u64 kDataVa = 0x013C0000, kDataSize = 0x1788000;
constexpr u64 kMainCtx = 0x02A78B58, kRingGlobals = 0x024B92E8, kArenaPtrs = 0x02A78060, kFrameCounter = 0x02A780A4;
constexpr u64 kRingCallback = 0x012122C0;

enum Tag : u32 {
    TagMeta = 1, TagCmd, TagRing, TagShader, TagData, TagArena, TagUnused, TagRegions, TagChain, TagLabels,
    TagCtx, TagProg, TagCtxTable,  // mid-frame render ctx sample, its program objects and stage tables at sample time
    TagHeap  // record --heap: asset heap memory the frame's draws read (textures, vertex/index/constant buffers)
};

struct Block {
    u32 tag;
    u64 addr;
    std::vector<u8> bytes;
};

struct Meta {
    u64 delta, old_head, new_head, slice_begin, slice_end, cmd_ring_base, cmd_ring_size;
    u32 frame_counter, slice_index, suspended_us, pad;
};

// ---------------------------------------------------------------- live process access

using NtProc = LONG(NTAPI*)(HANDLE);

struct Process {
    HANDLE h = nullptr;
    u64 delta = 0;
    bool Read(u64 addr, void* out, size_t size) const {
        SIZE_T got = 0;
        return ReadProcessMemory(h, (void*)addr, out, size, &got) && got == size;
    }
    template <class T> T Get(u64 addr) const {
        T v{};
        Read(addr, &v, sizeof(v));
        return v;
    }
    // Reads page by page past unreadable pages (they stay zero).
    void ReadInto(u64 addr, u8* out, size_t size) const {
        if (Read(addr, out, size)) return;
        std::memset(out, 0, size);
        for (size_t o = 0; o < size;) {
            size_t n = std::min<size_t>(0x1000 - ((addr + o) & 0xFFF), size - o);
            Read(addr + o, out + o, n);
            o += n;
        }
    }
    std::vector<u8> ReadRange(u64 addr, size_t size) const {
        std::vector<u8> out(size);
        ReadInto(addr, out.data(), size);
        return out;
    }
};

// Fills blocks[first..] (already sized, addr set) in 1 MB pieces on all cores: the copy is what keeps the game suspended.
static void ReadBlocks(const Process& p, std::vector<Block>& blocks, size_t first) {
    struct Piece {
        u8* out;
        u64 addr;
        size_t size;
    };
    std::vector<Piece> pieces;
    for (size_t i = first; i < blocks.size(); ++i)
        for (size_t o = 0; o < blocks[i].bytes.size(); o += 1 << 20)
            pieces.push_back({blocks[i].bytes.data() + o, blocks[i].addr + o, std::min<size_t>(1 << 20, blocks[i].bytes.size() - o)});
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (size_t t = 0; t < std::min<size_t>(std::thread::hardware_concurrency(), pieces.size()); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < pieces.size();) p.ReadInto(pieces[i].addr, pieces[i].out, pieces[i].size);
        });
    for (auto& t : pool) t.join();
}

static DWORD FindPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof(e)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (!_wcsicmp(e.szExeFile, name)) pid = e.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

struct Region {
    u64 base, size;
    u32 type, protect;
};

static std::vector<Region> Regions(HANDLE h) {
    std::vector<Region> out;
    MEMORY_BASIC_INFORMATION m;
    for (u64 a = 0; VirtualQueryEx(h, (void*)a, &m, sizeof(m)); a = (u64)m.BaseAddress + m.RegionSize)
        if (m.State == MEM_COMMIT) out.push_back({(u64)m.BaseAddress, m.RegionSize, m.Type, m.Protect});
    return out;
}

// Allocation-level map (NtQueryVirtualMemory MemoryRegionInformation): no page-table walk, unlike VirtualQueryEx on
// the multi-GB guest views (~75 ms per full map). Used to tell whether the full map can have changed.
struct Alloc {
    u64 base, size, commit;
    u32 protect, type;
};

static std::vector<Alloc> Allocations(HANDLE h) {
    using NtQvm = LONG(NTAPI*)(HANDLE, void*, int, void*, SIZE_T, SIZE_T*);
    static auto query = (NtQvm)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryVirtualMemory");
    struct {
        u64 base;
        u32 protect, type;
        u64 size, commit, partition, node;
    } r;
    std::vector<Alloc> out;
    MEMORY_BASIC_INFORMATION m;
    for (u64 a = 0; a < (1ull << 47);) {
        if (query(h, (void*)a, 3, &r, sizeof(r), nullptr) >= 0 && r.size) {
            out.push_back({r.base, r.size, r.commit, r.protect, r.type});
            a = r.base + r.size;
        } else if (VirtualQueryEx(h, (void*)a, &m, sizeof(m))) {  // free range: no allocation to describe
            a = (u64)m.BaseAddress + m.RegionSize;
        } else {
            break;
        }
    }
    return out;
}

// Full map only when the allocations changed or a second passed (protection-only changes inside an allocation).
static const std::vector<Region>& RegionsCached(HANDLE h) {
    static std::vector<Alloc> allocs;
    static std::vector<Region> regions;
    static auto last = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    auto a = Allocations(h);
    if (regions.empty() || now - last > std::chrono::seconds(1) || a.size() != allocs.size() ||
        std::memcmp(a.data(), allocs.data(), a.size() * sizeof(Alloc))) {
        regions = Regions(h);
        allocs = std::move(a);
        last = now;
    }
    return regions;
}

// ---------------------------------------------------------------- PM4 chain

constexpr u32 OpSetContext = 0x69, OpSetSh = 0x76, OpSetUconfig = 0x79;

struct Run {
    u64 begin, end;  // contiguous packet bytes in chain order
};

// Follows the chain from old_head to new_head. Inside a segment every packet is type 3; a non type-3 dword at a
// packet boundary is the low half of the 64-bit link to the next segment ([u32 size][body][u64 next]).
// Links that 011A2AA0 merged were already replaced by a NOP and are parsed through.
template <class ReadFn>
static bool WalkChain(u64 old_head, u64 new_head, u64 ring_lo, u64 ring_hi, ReadFn&& read32, std::vector<Run>& runs) {
    u64 pos = old_head + 4, start = pos;
    for (u64 guard = 0; guard < (64ull << 20); ++guard) {
        if (pos == new_head) {
            if (pos > start) runs.push_back({start, pos});
            return true;
        }
        u32 hdr = read32(pos);
        if (hdr >> 30 == 3) {
            pos += ((u64)((hdr >> 16) & 0x3FFF) + 2) * 4;
            continue;
        }
        if (hdr == 0x80000000) {  // type-2 filler
            pos += 4;
            continue;
        }
        u64 next = hdr | (u64)read32(pos + 4) << 32;
        if (next < ring_lo || next >= ring_hi || (next & 3)) {
            std::printf("chain: bad link %llx at %llx\n", (unsigned long long)next, (unsigned long long)pos);
            return false;
        }
        if (pos > start) runs.push_back({start, pos});
        pos = start = next + 4;
    }
    return false;
}

// ---------------------------------------------------------------- capture

static size_t DiscoverFetchShaders(std::vector<Block>& blocks, const Process& p);
static bool ReadFrame(const Process& p, u64 h0, u64 h1, bool trim, const u64* ring_hint, std::vector<Block>& blocks, Meta& meta);
static bool ReadChain(const Process& p, const Meta& meta, const u64* ring_globals, std::vector<Block>& blocks,
                      std::set<u64>& shaders, int& slice, u64* ref_max);

static int Capture(const char* out_path, bool sample) {
    DWORD pid = FindPid(L"gow3.exe");
    if (!pid) return std::printf("gow3.exe not running\n"), 1;
    Process p;
    p.h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!p.h) return std::printf("OpenProcess failed %lu\n", GetLastError()), 1;
    auto regions = Regions(p.h);
    for (auto& r : regions)
        if (r.size == kDataSize) p.delta = r.base - kDataVa;
    if (!p.delta) return std::printf("EBOOT .data region not found\n"), 1;
    auto E = [&](u64 va) { return va + p.delta; };
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto suspend = (NtProc)GetProcAddress(nt, "NtSuspendProcess");
    auto resume = (NtProc)GetProcAddress(nt, "NtResumeProcess");
    std::printf("pid %lu delta +0x%llx\n", pid, (unsigned long long)p.delta);

    const u64 ctx = E(kMainCtx);
    u64 h0 = p.Get<u64>(ctx + 0x40), h1 = h0;
    std::vector<Block> samples;
    if (sample) {
        // Job ctxs exist only while 011CA030 runs, on worker stacks (RBP-0x588): reserve callback 012122C0 at +0x18,
        // self at +0x20. Every freeze rescans the small private RW regions (stacks) for them.
        const u64 cb = E(kRingCallback);
        std::vector<Region> stacks;
        for (auto& r : regions)
            if (r.type == MEM_PRIVATE && r.protect == PAGE_READWRITE && r.size <= (1u << 20)) stacks.push_back(r);
        u64 ring_lo = p.Get<u64>(E(kRingGlobals) + 0x18), ring_hi = p.Get<u64>(E(kRingGlobals) + 0x50);
        // Samples of one frame: when the chain head moves, that frame was just submitted. Capture it in the same freeze
        // if a job ctx was caught, otherwise drop the samples and try the next frame.
        u32 ctx_samples = 0, job_samples = 0, freezes = 0;
        for (u32 tries = 0; tries < 20000; ++tries) {
            suspend(p.h);
            ++freezes;
            u64 h = p.Get<u64>(ctx + 0x40);
            if (ctx_samples && h != h0) {
                if (job_samples) {
                    h1 = h;
                    break;  // still suspended
                }
                samples.clear();
                ctx_samples = 0;
            }
            std::vector<u64> ctxs{ctx};
            for (auto& r : stacks) {
                auto m = p.ReadRange(r.base, r.size);
                for (size_t o = 0x18; o + 16 <= m.size(); o += 8)
                    if (*(u64*)&m[o] == cb && *(u64*)&m[o + 8] == r.base + o - 0x18) ctxs.push_back(r.base + o - 0x18);
            }
            u32 before = ctx_samples;
            for (u64 c : ctxs) {
                auto d = p.ReadRange(c, 0x5A0);
                u64 ps = *(u64*)&d[0x98], vs = *(u64*)&d[0x260], eud_ps = *(u64*)&d[0x240], eud_vs = *(u64*)&d[0x408];
                if (*(u64*)&d[0x18] != cb || *(u64*)&d[0x20] != c || !ps || !vs || eud_ps < ring_lo || eud_ps >= ring_hi) continue;
                bool seen = false;  // same ctx still on the same draw as an earlier sample
                for (auto& s : samples)
                    seen |= s.tag == TagCtx && s.addr == c && *(u64*)&s.bytes[0x240] == eud_ps && *(u64*)&s.bytes[0x10] == *(u64*)&d[0x10];
                if (seen) continue;
                ++ctx_samples;
                job_samples += c != ctx;
                samples.push_back({TagCtx, c, std::move(d)});
                for (u64 obj : {ps, vs}) samples.push_back({TagProg, obj, p.ReadRange(obj, 0x100)});
                for (u64 t : {eud_ps, eud_vs})
                    if (t >= ring_lo && t < ring_hi) samples.push_back({TagCtxTable, t, p.ReadRange(t, 0x200)});
            }
            if (!before && ctx_samples) h0 = h;
            resume(p.h);
            // Busy wait: sleep_for has timer-tick granularity here and would skip whole frames.
            for (auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100 + tries % 7 * 60);
                 std::chrono::steady_clock::now() < until;)
                ;
        }
        std::printf("sampled %u ctxs (%u job) in %u freezes\n", ctx_samples, job_samples, freezes);
        if (!job_samples) return std::printf("no job ctx caught\n"), 1;
    } else {
        // Frame boundary: 011A2AA0 rewrites ctx+0x40 with the next chain head right after submitting.
        auto t0 = std::chrono::steady_clock::now();
        while ((h1 = p.Get<u64>(ctx + 0x40)) == h0)
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) return std::printf("no submit in 3 s\n"), 1;
        suspend(p.h);
    }
    auto ts = std::chrono::steady_clock::now();
    std::vector<Block> blocks = std::move(samples);
    Meta meta{};
    bool chain_ok = ReadFrame(p, h0, h1, false, nullptr, blocks, meta);
    meta.suspended_us = (u32)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - ts).count();
    resume(p.h);
    blocks.insert(blocks.begin(), {TagMeta, 0, std::vector<u8>((u8*)&meta, (u8*)(&meta + 1))});

    FILE* f = std::fopen(out_path, "wb");
    if (!f) return std::printf("cannot write %s\n", out_path), 1;
    u64 total = 0, cmd_bytes = 0;
    size_t runs = 0, shaders = 0;
    std::fwrite("GOW3CAP1", 1, 8, f);
    for (auto& b : blocks) {
        u64 hdr[3] = {b.tag, b.addr, b.bytes.size()};
        std::fwrite(hdr, 8, 3, f);
        std::fwrite(b.bytes.data(), 1, b.bytes.size(), f);
        total += b.bytes.size();
        if (b.tag == TagCmd) ++runs, cmd_bytes += b.bytes.size();
        shaders += b.tag == TagShader;
    }
    std::fclose(f);
    std::printf("frame %u chain %s: %zu runs %llu KB, slice %u, %zu shaders (incl. fetch), suspended %u us, %llu MB -> %s\n",
                meta.frame_counter, chain_ok ? "ok" : "BROKEN", runs, (unsigned long long)cmd_bytes >> 10, meta.slice_index,
                shaders, meta.suspended_us, (unsigned long long)total >> 20, out_path);
    return chain_ok ? 0 : 2;
}

// Everything the frame submitted as chain h0 -> h1 left in memory (the game must be suspended): command runs, data ring
// slice, shaders and fetch shaders, EBOOT .data, CPU arenas, labels, region map. h0 == 0 (no frame yet, e.g. boot) keeps
// .data, arenas, labels and regions. trim (record): double arenas up to used, ring slice up to the frame's extent
// (highest cursor seen in that slice while polling, ring_hint, or the furthest ring pointer in the PM4, + 64 KB).
// Recycled big block buffers: a fresh 28 MB per snapshot costs page faults plus zero fill while the game is suspended.
static std::mutex g_pool_m;
static std::vector<std::vector<u8>> g_pool;
static std::vector<u8> PoolGet(size_t n) {
    std::vector<u8> v;
    {
        std::lock_guard l(g_pool_m);
        size_t best = g_pool.size();
        for (size_t i = 0; i < g_pool.size(); ++i)
            if (g_pool[i].size() >= n && (best == g_pool.size() || g_pool[i].size() < g_pool[best].size())) best = i;
        if (best < g_pool.size()) std::swap(g_pool[best], g_pool.back()), v = std::move(g_pool.back()), g_pool.pop_back();
    }
    v.resize(n);  // shrinking keeps the old bytes untouched; ReadInto overwrites all n
    return v;
}
static void PoolPut(std::vector<u8>&& v) {
    std::lock_guard l(g_pool_m);
    if (v.size() >= (1u << 20) && g_pool.size() < 32) g_pool.push_back(std::move(v));
}

static bool ReadFrame(const Process& p, u64 h0, u64 h1, bool trim, const u64* ring_hint, std::vector<Block>& blocks, Meta& meta) {
    auto E = [&](u64 va) { return va + p.delta; };
    const u64 ctx = E(kMainCtx);
    meta.delta = p.delta;
    meta.old_head = h0;
    meta.new_head = h1;
    meta.frame_counter = p.Get<u32>(E(kFrameCounter));
    meta.cmd_ring_base = p.Get<u64>(ctx + 0x528);
    meta.cmd_ring_size = p.Get<u64>(ctx + 0x530);
    u64 ring_globals[15] = {};
    p.Read(E(kRingGlobals), ring_globals, sizeof(ring_globals));
    bool chain_ok = true;
    std::set<u64> shaders;
    int slice = -1;
    u64 ref_max[4] = {};
    if (h0) chain_ok = ReadChain(p, meta, ring_globals, blocks, shaders, slice, ref_max);

    size_t big = blocks.size();
    if (slice >= 0) {
        meta.slice_index = slice;
        meta.slice_begin = ring_globals[3 + slice * 2];
        meta.slice_end = ring_globals[4 + slice * 2];
        u64 end = meta.slice_end;
        if (trim) end = std::min(end, std::max({ring_hint[slice], ref_max[slice], meta.slice_begin}) + 0x10000);
        blocks.push_back({TagRing, meta.slice_begin, PoolGet(end - meta.slice_begin)});
    }
    blocks.push_back({TagData, E(kDataVa), PoolGet(kDataSize)});
    for (int i = 0; i < 3; ++i) {
        u64 desc = p.Get<u64>(E(kArenaPtrs) + i * 8);
        u32 used = p.Get<u32>(desc), cap = p.Get<u32>(desc + 4);
        u64 base = p.Get<u64>(desc + 8);
        // The 1 MB arena is reset during the frame, so it is always read whole.
        if (base && cap && cap <= (64u << 20)) blocks.push_back({TagArena, base, PoolGet(trim && i < 2 ? std::min(used, cap) : cap)});
    }
    if (ring_globals[1]) blocks.push_back({TagLabels, ring_globals[1], std::vector<u8>(0x1000)});
    ReadBlocks(p, blocks, big);
    if (h0) {
        for (u64 s : shaders) {
            u32 hdr[2] = {};
            p.Read(s, hdr, 8);
            u64 len = hdr[0] == 0xBEEB03FF && hdr[1] < 0x40000 ? ((u64)hdr[1] + 1) * 8 + 0x20 : 0x100;
            blocks.push_back({TagShader, s, p.ReadRange(s, len)});
        }
        DiscoverFetchShaders(blocks, p);
    }
    auto& regions = RegionsCached(p.h);
    blocks.push_back({TagRegions, 0, std::vector<u8>((u8*)regions.data(), (u8*)(regions.data() + regions.size()))});
    return chain_ok;
}

// Command runs of the chain h0 -> h1 plus what they point to: shader addresses and per-slice ring references.
static bool ReadChain(const Process& p, const Meta& meta, const u64* ring_globals, std::vector<Block>& blocks,
                      std::set<u64>& shaders, int& slice, u64* ref_max) {
    std::vector<Run> runs;
    std::unordered_map<u64, std::vector<u8>> pages;  // command pages cached during the walk
    auto read32 = [&](u64 a) {
        auto& pg = pages[a & ~0xFFFull];
        if (pg.empty()) pg = p.ReadRange(a & ~0xFFFull, 0x1000);
        return *(const u32*)&pg[a & 0xFFF];
    };
    bool chain_ok = WalkChain(meta.old_head, meta.new_head, meta.cmd_ring_base, meta.cmd_ring_base + meta.cmd_ring_size, read32, runs);
    for (auto& r : runs) blocks.push_back({TagCmd, r.begin, p.ReadRange(r.begin, r.end - r.begin)});
    blocks.push_back({TagChain, 0, std::vector<u8>((u8*)runs.data(), (u8*)(runs.data() + runs.size()))});

    // Shader binaries and user-data pointers referenced by the frame.
    std::map<int, u32> slice_votes;
    auto slice_of = [&](u64 a) {
        for (int i = 0; i < 4; ++i)
            if (a >= ring_globals[3 + i * 2] && a < ring_globals[4 + i * 2]) return i;
        return -1;
    };
    for (auto& b : blocks) {
        if (b.tag != TagCmd) continue;
        const u32* d = (const u32*)b.bytes.data();
        size_t n = b.bytes.size() / 4;
        for (size_t i = 0; i < n;) {
            u32 hdr = d[i];
            u32 cnt = ((hdr >> 16) & 0x3FFF) + 1, op = (hdr >> 8) & 0xFF;
            if (op == OpSetSh && i + 1 + cnt <= n) {
                u32 reg = d[i + 1];
                for (u32 k = 0; k + 1 < cnt; ++k) {
                    u32 r = reg + k;
                    if ((r == 0x08 || r == 0x48 || r == 0x88 || r == 0xC8 || r == 0x108 || r == 0x148 || r == 0x20C) && k + 2 < cnt)
                        shaders.insert(((u64)d[i + 2 + k] | (u64)d[i + 3 + k] << 32) << 8);
                    if (k + 2 < cnt) {
                        u64 v = (u64)d[i + 2 + k] | (u64)d[i + 3 + k] << 32;
                        if (int s = slice_of(v); s >= 0) ++slice_votes[s], ref_max[s] = std::max(ref_max[s], v);
                    }
                }
            }
            i += cnt + 1;
        }
    }
    u32 best = 0;
    for (auto [s, v] : slice_votes)
        if (v > best) best = v, slice = s;
    return chain_ok;
}

// ---------------------------------------------------------------- offline image

struct Image {
    Meta meta{};
    std::vector<Block> blocks;
    std::vector<const Block*> sorted;  // by address, addressable blocks only
    const Block* Find(u32 tag) const {
        for (auto& b : blocks)
            if (b.tag == tag) return &b;
        return nullptr;
    }
    const u8* Ptr(u64 a, u64 size) const {
        auto it = std::upper_bound(sorted.begin(), sorted.end(), a, [](u64 v, const Block* b) { return v < b->addr; });
        if (it == sorted.begin()) return nullptr;
        const Block* b = *--it;
        if (a + size <= b->addr + b->bytes.size()) return b->bytes.data() + (a - b->addr);
        for (auto* c : sorted)  // overlapping blocks (heap spans around shader copies)
            if (a >= c->addr && a + size <= c->addr + c->bytes.size()) return c->bytes.data() + (a - c->addr);
        return nullptr;
    }
    // Shader blocks are copies of memory that may also lie in a bigger block; search those last.
    void Index() {
        sorted.clear();
        for (auto& b : blocks)
            if (b.addr && b.tag != TagShader && b.tag != TagProg && b.tag != TagCtx && b.tag != TagCtxTable) sorted.push_back(&b);
        for (auto& b : blocks)
            if (b.tag == TagShader || b.tag == TagProg) sorted.push_back(&b);
        std::stable_sort(sorted.begin(), sorted.end(), [](const Block* a, const Block* b) { return a->addr < b->addr; });
    }
};

static bool Load(const char* path, Image& img) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "GOW3CAP1", 8)) return std::fclose(f), false;
    u64 hdr[3];
    while (std::fread(hdr, 8, 3, f) == 3) {
        Block b{(u32)hdr[0], hdr[1], std::vector<u8>(hdr[2])};
        if (hdr[2] && std::fread(b.bytes.data(), 1, hdr[2], f) != hdr[2]) break;
        img.blocks.push_back(std::move(b));
    }
    std::fclose(f);
    if (auto m = img.Find(TagMeta)) std::memcpy(&img.meta, m->bytes.data(), sizeof(Meta));
    img.Index();
    return true;
}

// ---------------------------------------------------------------- shader resource layout (same parse as 0119FED0)

struct Slot {
    u8 type, api, reg, flags;
};

struct ShaderInfo {
    bool ok = false;
    u64 hash = 0;
    u32 code_bytes = 0;
    std::vector<Slot> slots;
    std::vector<u32> extras;  // chunk usage dwords consumed by the pointer-table slots in order
};

static ShaderInfo ParseShader(const Image& img, u64 addr) {
    ShaderInfo s;
    const u8* p = img.Ptr(addr, 8);
    if (!p || *(const u32*)p != 0xBEEB03FF) return s;
    u64 info_off = ((u64)((const u32*)p)[1] + 1) * 8;
    const u8* info = img.Ptr(addr + info_off, 0x1C);
    if (!info || std::memcmp(info, "OrbShdr", 7)) return s;
    u32 chunk = info[0xC], n = info[0xD];
    const u8* ent = img.Ptr(addr + info_off - (chunk + n) * 4, (chunk + n) * 4);
    if (!ent) return s;
    for (u32 i = 0; i < n; ++i) s.slots.push_back({ent[i * 4], ent[i * 4 + 1], ent[i * 4 + 2], ent[i * 4 + 3]});
    for (u32 i = 0; i < chunk; ++i) s.extras.push_back(((const u32*)(ent + n * 4))[i]);
    s.hash = *(const u64*)(info + 0x10);
    s.code_bytes = (u32)info_off;
    s.ok = true;
    return s;
}

// ---------------------------------------------------------------- per-draw record

struct Draw {
    u32 index, op, count, instances, seg;
    u64 vs, ps;
    u32 vs_user[16], ps_user[16];
    u64 state_hash;  // blend/depth/raster/RT formats subset of context registers
    u64 rt0, zbase;
    u32 prim, index_type;
    bool compute;
    u64 ib, ib_bytes;  // index buffer span of indexed draws
    u32 cb[8][6], cb_mask, db[8], depth_control;  // CB_COLOR*_BASE..ATTRIB, CB_TARGET_MASK, DB_Z_INFO..DB_DEPTH_SLICE
    u64 pkt;      // guest address of the draw packet
    u32 reg[12];  // kShadowRegs
    u32 rsrc[2][2];  // VS (CS) and PS PGM_RSRC1/2
    u32 ps_cntl[32], ps_ena, ps_addr, col_format, threads[3];
};
// Context registers the draw shadow (GOW3_DRAWLOG) also applies from each ctx's own stream; same order as the game.
constexpr u32 kShadowRegs[12]{0x08e, 0x08f, 0x200, 0x202, 0x203, 0x205, 0x1e0, 0x10b, 0x10c, 0x10d, 0x318, 0x012};

static u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

// Registers start at zero for every frame unless `carry` holds the previous frame's end state (context then SH, 0x800
// dwords); it receives this frame's end state.
static void Replay(const Image& img, std::vector<Draw>& draws, std::map<u32, u32>& ops, std::vector<u32>* carry = nullptr) {
    std::vector<u32> ctxr(0x400), sh(0x400), ucfg(0x400);
    if (carry && carry->size() == 0x800) std::copy_n(carry->begin(), 0x400, ctxr.begin()), std::copy_n(carry->begin() + 0x400, 0x400, sh.begin());
    struct Save {
        std::vector<u32>*c, &x, &s;
        ~Save() {
            if (c) c->assign(x.begin(), x.end()), c->insert(c->end(), s.begin(), s.end());
        }
    } save{carry, ctxr, sh};
    u32 instances = 1, seg = 0, index_size = 2;
    u64 index_base = 0;
    const Block* chain = img.Find(TagChain);
    if (!chain) return;  // recorded frame without a submit (boot / loading)
    const Run* runs = (const Run*)chain->bytes.data();
    size_t nruns = chain->bytes.size() / sizeof(Run);
    for (size_t ri = 0; ri < nruns; ++ri, ++seg) {
        const u32* d = (const u32*)img.Ptr(runs[ri].begin, runs[ri].end - runs[ri].begin);
        size_t n = (runs[ri].end - runs[ri].begin) / 4;
        for (size_t i = 0; i < n;) {
            u32 hdr = d[i];
            if (hdr >> 30 != 3) {
                ++i;
                continue;
            }
            u32 cnt = ((hdr >> 16) & 0x3FFF) + 1, op = (hdr >> 8) & 0xFF;
            const u32* b = d + i + 1;
            ++ops[op];
            auto set = [&](u32* file, u32 base) {
                for (u32 k = 1; k < cnt && b[0] - base + k - 1 < 0x400; ++k) file[b[0] - base + k - 1] = b[k];
            };
            if (op == OpSetContext) set(ctxr.data(), 0);
            else if (op == OpSetSh) set(sh.data(), 0);
            else if (op == OpSetUconfig) set(ucfg.data(), 0);
            else if (op == 0x2F) instances = b[0];
            else if (op == 0x26) index_base = (u64)b[0] | (u64)(b[1] & 0xFFFF) << 32;  // INDEX_BASE
            else if (op == 0x2A) index_size = b[0] & 1 ? 4 : 2;                       // INDEX_TYPE
            else if (op == 0x27 || op == 0x2D || op == 0x35 || op == 0x15) {
                Draw dr{};
                dr.index = (u32)draws.size();
                dr.op = op;
                dr.compute = op == 0x15;
                dr.count = op == 0x27 ? b[3] : op == 0x2D ? b[0] : op == 0x35 ? b[2] : b[0] * b[1] * b[2];
                dr.instances = instances;
                dr.seg = seg;
                dr.vs = ((u64)sh[0x48] | (u64)sh[0x49] << 32) << 8;
                dr.ps = ((u64)sh[0x08] | (u64)sh[0x09] << 32) << 8;
                if (dr.compute) dr.ps = ((u64)sh[0x20C] | (u64)sh[0x20D] << 32) << 8, dr.vs = 0;
                std::memcpy(dr.vs_user, dr.compute ? &sh[0x240] : &sh[0x4C], 64);
                std::memcpy(dr.ps_user, &sh[0x0C], 64);
                u64 h = 0;
                for (u32 r : {0x08Eu, 0x08Fu, 0x200u, 0x202u, 0x203u, 0x204u, 0x205u, 0x10Bu, 0x10Cu, 0x10Du})
                    h = Mix(h, ctxr[r]);
                for (u32 k = 0; k < 8; ++k) h = Mix(h, ctxr[0x1E0 + k]), h = Mix(h, ctxr[0x31C + k * 0xF]);
                h = Mix(h, ctxr[0x010]), h = Mix(h, ctxr[0x011]);
                dr.state_hash = h;
                dr.rt0 = (u64)ctxr[0x318] << 8;
                dr.zbase = (u64)ctxr[0x012] << 8;
                dr.prim = ucfg[0x242];
                dr.index_type = ctxr[0x29F];
                if (op == 0x27) dr.ib = (u64)b[1] | (u64)(b[2] & 0xFFFF) << 32, dr.ib_bytes = (u64)b[3] * index_size;
                if (op == 0x35) dr.ib = index_base + (u64)b[1] * index_size, dr.ib_bytes = (u64)b[2] * index_size;
                for (u32 k = 0; k < 8; ++k) std::memcpy(dr.cb[k], &ctxr[0x318 + k * 0xF], 24);
                std::memcpy(dr.db, &ctxr[0x010], 32);
                dr.cb_mask = ctxr[0x08E], dr.depth_control = ctxr[0x200];
                dr.pkt = runs[ri].begin + i * 4;
                for (u32 k = 0; k < 12; ++k) dr.reg[k] = ctxr[kShadowRegs[k]];
                std::memcpy(dr.rsrc[0], dr.compute ? &sh[0x212] : &sh[0x4A], 8);
                std::memcpy(dr.rsrc[1], &sh[0x0A], 8);
                std::memcpy(dr.ps_cntl, &ctxr[0x191], 128);
                dr.ps_ena = ctxr[0x1B3], dr.ps_addr = ctxr[0x1B4], dr.col_format = ctxr[0x1C5];
                std::memcpy(dr.threads, &sh[0x207], 12);
                draws.push_back(dr);
            }
            i += cnt + 1;
        }
    }
}

// ---------------------------------------------------------------- table decode

struct Tex {
    u64 base;
    u32 dfmt, nfmt, w, h, type, tile;
    u32 raw[8];
};

static Tex DecodeT(const u32* t) {
    return {((u64)t[0] | (u64)(t[1] & 0x3F) << 32) << 8, (t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF, (t[2] & 0x3FFF) + 1,
            ((t[2] >> 14) & 0x3FFF) + 1, (t[3] >> 28) & 0xF, (t[3] >> 20) & 0x1F, {}};
}

// GCN (CI) instruction size in dwords, enough to walk fetch shaders.
static u32 GcnLength(const u32* w) {
    u32 x = w[0];
    if ((x >> 26) == 0x38 || (x >> 26) == 0x3A || (x >> 26) == 0x3C || (x >> 26) == 0x34 || (x >> 26) == 0x3E || (x >> 26) == 0x36)
        return 2;
    if ((x >> 27) == 0x18) return (x & 0x1FF) == 0xFF ? 2 : 1;                           // SMRD, literal offset
    if ((x >> 30) == 2) return ((x & 0xFF) == 0xFF || ((x >> 8) & 0xFF) == 0xFF) ? 2 : 1; // SOP*
    return (x & 0x1FF) == 0xFF ? 2 : 1;                                                  // VOP1/2/C src0 literal
}

// Dword offsets of the V# loads in a fetch shader, up to s_setpc_b64.
static std::vector<u32> FetchOffsets(const Image& img, u64 addr) {
    std::vector<u32> out;
    const u32* w = addr ? (const u32*)img.Ptr(addr, 4) : nullptr;
    for (u32 i = 0; w && i < 0x100; i += GcnLength(w + i)) {
        if (!img.Ptr(addr + i * 4ull, 8)) break;
        if ((w[i] & 0xFF80FF00) == 0xBE802000) break;
        if ((w[i] >> 27) == 0x18 && ((w[i] >> 22) & 0x1F) == 2 && (w[i] & 0x100))
            if (std::find(out.begin(), out.end(), w[i] & 0xFF) == out.end()) out.push_back(w[i] & 0xFF);
    }
    return out;
}

// Fetch shader size in bytes, through its s_setpc_b64 (0 when unreadable).
static u32 FetchBytes(const Image& img, u64 addr) {
    for (u32 i = 0; i < 0x100;) {
        const u32* w = (const u32*)img.Ptr(addr + i * 4ull, 8);
        if (!w) return 0;
        if ((w[0] & 0xFF80FF00) == 0xBE802000) return (i + 1) * 4;
        i += GcnLength(w);
    }
    return 0;
}

struct StageStats {
    u32 tex = 0, samp = 0, cb = 0, vb = 0, buf = 0, rw = 0, unresolved = 0, eud_dwords = 0;
    std::vector<u64> bufs;  // read-only buffer V# base
    u64 eud = 0, eud_hash = 0, desc_hash = 0, fetch = 0;
    std::vector<Tex> textures;
    std::vector<u64> samplers;  // hash of the 4 dwords
    std::vector<u64> cbs;       // V# base
    std::vector<u64> vbs;       // V# base
    std::vector<std::array<u64, 2>> spans;  // every buffer V# as {base, bytes}
    std::vector<Tex> rw_textures;            // read-write image T#s
    std::vector<std::array<u64, 2>> rw_bufs; // read-write buffer V#s as {base, bytes}
};

static StageStats DecodeStage(const Image& img, const ShaderInfo& si, const u32* user) {
    StageStats st;
    if (!si.ok) return st;
    // Extended user data root (slot 0x1B) holds the engine's per-stage table (ctx+0x240 / ctx+0x408).
    for (auto& s : si.slots)
        if (s.type == 0x1B && s.reg < 15) st.eud = (u64)user[s.reg] | (u64)user[s.reg + 1] << 32;
    // Memory the GPU reads to find descriptors (stage tables, pointer tables) counts as a span too.
    auto at = [&](u64 a, u32 bytes) -> const u32* {
        auto p = (const u32*)img.Ptr(a, bytes);
        if (p) st.spans.push_back({a, bytes});
        return p;
    };
    auto reg_ptr = [&](u8 reg, u32 dwords) -> const u32* {
        if (reg < 16) return reg + dwords <= 16 ? user + reg : nullptr;
        return st.eud ? at(st.eud + (reg - 16) * 4ull, dwords * 4) : nullptr;
    };
    auto ptr64 = [&](u8 reg) -> u64 {
        const u32* p = reg_ptr(reg, 2);
        return p ? (u64)p[0] | (u64)p[1] << 32 : 0;
    };
    auto add_t = [&](const u32* t) {
        if (!t) return void(++st.unresolved);
        ++st.tex;
        st.textures.push_back(DecodeT(t));
        std::memcpy(st.textures.back().raw, t, 32);
        for (u32 k = 0; k < 8; ++k) st.desc_hash = Mix(st.desc_hash, t[k]);
    };
    auto add_s = [&](const u32* s) {
        if (!s) return void(++st.unresolved);
        ++st.samp;
        u64 h = 0;
        for (int k = 0; k < 4; ++k) h = Mix(h, s[k]);
        st.samplers.push_back(h);
        st.desc_hash = Mix(st.desc_hash, h);
    };
    auto add_v = [&](const u32* v, std::vector<u64>& out, u32& cnt) {
        if (!v) return void(++st.unresolved);
        ++cnt;
        out.push_back((u64)v[0] | (u64)(v[1] & 0xFFF) << 32);
        u64 stride = (v[1] >> 16) & 0x3FFF;
        st.spans.push_back({out.back(), stride ? stride * v[2] : v[2]});
        for (int k = 0; k < 4; ++k) st.desc_hash = Mix(st.desc_hash, v[k]);
    };
    // Read-write resource: an image T# when its dword 3 type is an image type (8..15), else a buffer V#.
    auto add_rw = [&](const u32* r, bool image) {
        ++st.rw;
        if (!r) return void(++st.unresolved);
        if (image) {
            st.rw_textures.push_back(DecodeT(r));
            std::memcpy(st.rw_textures.back().raw, r, 32);
        } else {
            u64 stride = (r[1] >> 16) & 0x3FFF;
            st.rw_bufs.push_back({(u64)r[0] | (u64)(r[1] & 0xFFF) << 32, stride ? stride * r[2] : r[2]});
        }
    };
    size_t x = 0;
    auto extra = [&]() { return x < si.extras.size() ? si.extras[x++] : 0u; };
    for (auto& s : si.slots) {
        switch (s.type) {
        case 0:  // 8-dword image T# or 4-dword buffer V#
            if (s.flags & 1) add_t(reg_ptr(s.reg, 8));
            else add_v(reg_ptr(s.reg, 4), st.bufs, st.buf);
            break;
        case 1: add_s(reg_ptr(s.reg, 4)); break;
        case 2: add_v(reg_ptr(s.reg, 4), st.cbs, st.cb); break;
        case 3: add_v(reg_ptr(s.reg, 4), st.vbs, st.vb); break;
        case 4: add_rw(reg_ptr(s.reg, s.flags & 1 ? 8 : 4), s.flags & 1); break;
        case 0x13: {
            u64 t = ptr64(s.reg);
            u8 f = s.flags >> 4;
            u64 lo = f & 1 ? extra() : 0, hi = f & 2 ? (u64)extra() << 32 : 0;
            u64 smp = f & 4 ? extra() : 0;
            if (f & 8) smp |= (u64)extra() << 32;
            for (u64 m = lo | hi; m; m &= m - 1) add_t(t ? at(t + std::countr_zero(m) * 32, 32) : nullptr);
            for (u64 m = smp; m; m &= m - 1) add_s(t ? at(t + 0x800 + std::countr_zero(m) * 32, 16) : nullptr);
            break;
        }
        case 0x15: {
            u64 t = ptr64(s.reg);
            for (u32 m = extra(); m; m &= m - 1) add_s(t ? at(t + std::countr_zero(m) * 16, 16) : nullptr);
            break;
        }
        case 0x16: {
            u64 t = ptr64(s.reg);
            for (u32 m = extra(); m; m &= m - 1) add_v(t ? at(t + std::countr_zero(m) * 16, 16) : nullptr, st.cbs, st.cb);
            break;
        }
        case 0x19: {
            u64 t = ptr64(s.reg);
            for (u32 m = extra(); m; m &= m - 1) {
                const u32* r = t ? at(t + std::countr_zero(m) * 32, 32) : nullptr;
                add_rw(r, r && (r[3] >> 28) >= 8);
            }
            break;
        }
        case 0x17: {  // vertex table: the fetch shader (slot 0x12) loads each V# with s_load_dwordx4
            u64 t = ptr64(s.reg), fetch = 0;
            for (auto& f : si.slots)
                if (f.type == 0x12) fetch = ptr64(f.reg);
            st.fetch = fetch;
            for (u32 off : FetchOffsets(img, fetch)) add_v(t ? at(t + off * 4ull, 16) : nullptr, st.vbs, st.vb);
            break;
        }
        }
    }
    return st;
}

// Runs the replay on what was captured so far (process still suspended) and saves each fetch shader the VSs use.
static size_t DiscoverFetchShaders(std::vector<Block>& blocks, const Process& p) {
    Image img;
    img.blocks = std::move(blocks);
    img.Index();
    std::vector<Draw> draws;
    std::map<u32, u32> ops;
    Replay(img, draws, ops);
    std::unordered_map<u64, ShaderInfo> shaders;
    std::set<u64> fetch;
    for (auto& d : draws) {
        if (!d.vs) continue;
        auto it = shaders.find(d.vs);
        if (it == shaders.end()) it = shaders.emplace(d.vs, ParseShader(img, d.vs)).first;
        if (u64 f = DecodeStage(img, it->second, d.vs_user).fetch) fetch.insert(f);
    }
    blocks = std::move(img.blocks);
    for (u64 f : fetch) blocks.push_back({TagShader, f, p.ReadRange(f, 0x400)});
    return fetch.size();
}

// ---------------------------------------------------------------- analyze

static const char* RegionName(const std::vector<Region>& regs, u64 a) {
    for (auto& r : regs)
        if (a >= r.base && a < r.base + r.size) {
            static thread_local char buf[64];
            std::snprintf(buf, sizeof(buf), "%llx+%llx", (unsigned long long)r.base, (unsigned long long)r.size);
            return buf;
        }
    return "unmapped";
}

static int Analyze(const char* path, const char* csv) {
    Image img;
    if (!Load(path, img)) return std::printf("cannot load %s\n", path), 1;
    const Meta& m = img.meta;
    std::printf("frame %u delta +0x%llx head %llx -> %llx slice %u [%llx,%llx) suspended %u us\n", m.frame_counter,
                (unsigned long long)m.delta, (unsigned long long)m.old_head, (unsigned long long)m.new_head, m.slice_index,
                (unsigned long long)m.slice_begin, (unsigned long long)m.slice_end, m.suspended_us);
    const Block* rb = img.Find(TagRegions);
    std::vector<Region> regs((const Region*)rb->bytes.data(), (const Region*)(rb->bytes.data() + rb->bytes.size()));

    std::vector<Draw> draws;
    std::map<u32, u32> ops;
    Replay(img, draws, ops);
    std::printf("\npackets by opcode:");
    for (auto [op, n] : ops) std::printf(" %02x:%u", op, n);
    std::printf("\n");

    std::unordered_map<u64, ShaderInfo> shaders;
    for (auto& d : draws)
        for (u64 a : {d.vs, d.ps})
            if (a && !shaders.count(a)) shaders[a] = ParseShader(img, a);
    u32 bad_shaders = 0;
    for (auto& [a, s] : shaders) bad_shaders += !s.ok;

    // Per draw decode on all cores.
    std::vector<StageStats> vs(draws.size()), ps(draws.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < draws.size();) {
                auto& d = draws[i];
                if (d.vs) vs[i] = DecodeStage(img, shaders[d.vs], d.vs_user);
                if (d.ps) ps[i] = DecodeStage(img, shaders[d.ps], d.compute ? d.vs_user : d.ps_user);
                for (auto* st : {&vs[i], &ps[i]})
                    if (st->eud) {
                        u32 n = 0;
                        for (auto& s : shaders[st == &vs[i] ? d.vs : d.ps].slots)
                            if (s.reg >= 16) n = std::max<u32>(n, s.reg - 16 + 8);
                        st->eud_dwords = n;
                        if (const u32* p = (const u32*)img.Ptr(st->eud, n * 4ull))
                            for (u32 k = 0; k < n; ++k) st->eud_hash = Mix(st->eud_hash, p[k]);
                    }
            }
        });
    for (auto& t : pool) t.join();

    // Render targets and depth buffers written this frame: their T# reads are GPU-produced textures.
    std::unordered_set<u64> targets;
    for (auto& d : draws) targets.insert(d.rt0), targets.insert(d.zbase);

    u32 ndraw = 0, ncomp = 0;
    std::set<u64> progs_vs, progs_ps, pipes, states, tex_all, samp_all, rts;
    std::unordered_map<u64, u32> eud_dupes, desc_dupes;
    u64 tex_refs = 0, tex_null = 0, tex_targets = 0, samp_refs = 0, cb_refs = 0, vb_refs = 0, unresolved = 0, eud_dw = 0;
    u32 unmapped_samples = 0;
    std::map<std::string, u32> tex_regions, cb_regions, vb_regions, buf_regions;
    for (size_t i = 0; i < draws.size(); ++i) {
        auto& d = draws[i];
        d.compute ? ++ncomp : ++ndraw;
        if (d.vs) progs_vs.insert(d.vs);
        if (d.ps) progs_ps.insert(d.ps);
        states.insert(d.state_hash);
        pipes.insert(Mix(Mix(d.vs, d.ps), d.state_hash));
        rts.insert(d.rt0);
        for (auto* st : {&vs[i], &ps[i]}) {
            for (auto& t : st->textures) {
                ++tex_refs;
                tex_all.insert(t.base);
                if (!t.base) {
                    ++tex_null;
                    continue;
                }
                tex_targets += targets.count(t.base);
                const char* rn = RegionName(regs, t.base);
                ++tex_regions[rn];
                if (!std::strcmp(rn, "unmapped") && unmapped_samples++ < 6)
                    std::printf("unmapped T# draw %zu %s: %08x %08x %08x %08x %08x %08x %08x %08x\n", i, st == &vs[i] ? "VS" : "PS",
                                t.raw[0], t.raw[1], t.raw[2], t.raw[3], t.raw[4], t.raw[5], t.raw[6], t.raw[7]);
            }
            for (u64 s : st->samplers) ++samp_refs, samp_all.insert(s);
            for (u64 c : st->cbs) ++cb_refs, ++cb_regions[RegionName(regs, c)];
            for (u64 v : st->vbs) ++vb_refs, ++vb_regions[RegionName(regs, v)];
            for (u64 v : st->bufs) ++buf_regions[RegionName(regs, v)];
            unresolved += st->unresolved;
            eud_dw += st->eud_dwords;
            if (st->eud) ++eud_dupes[st->eud_hash];
            if (st->desc_hash) ++desc_dupes[st->desc_hash];
        }
    }
    auto distinct_ratio = [](const std::unordered_map<u64, u32>& m) {
        u64 total = 0;
        for (auto& [k, v] : m) total += v;
        return std::make_pair(total, m.size());
    };
    auto [eud_total, eud_distinct] = distinct_ratio(eud_dupes);
    auto [desc_total, desc_distinct] = distinct_ratio(desc_dupes);
    const Block* chain = img.Find(TagChain);
    std::printf("\ndraws %u, dispatches %u, segments %zu\n", ndraw, ncomp, chain ? chain->bytes.size() / sizeof(Run) : 0);
    std::printf("programs: VS %zu, PS/CS %zu (%u without OrbShdr info); state combos %zu; pipeline keys %zu; RT0 bases %zu\n",
                progs_vs.size(), progs_ps.size(), bad_shaders, states.size(), pipes.size(), rts.size());
    std::printf("descriptors per frame: T# %llu (%zu distinct, %llu null, %llu of a target drawn this frame), S# %llu (%zu distinct), "
                "CB %llu, VB %llu, unresolved %llu\n",
                (unsigned long long)tex_refs, tex_all.size(), (unsigned long long)tex_null, (unsigned long long)tex_targets,
                (unsigned long long)samp_refs, samp_all.size(), (unsigned long long)cb_refs, (unsigned long long)vb_refs,
                (unsigned long long)unresolved);
    std::printf("EUD tables: %llu stage tables, %zu distinct contents, avg %.1f dwords; descriptor sets: %llu, %zu distinct\n",
                (unsigned long long)eud_total, eud_distinct, eud_total ? (double)eud_dw / eud_total : 0.0,
                (unsigned long long)desc_total, desc_distinct);
    auto dump = [](const char* name, const std::map<std::string, u32>& m) {
        std::printf("%s regions:", name);
        for (auto& [k, v] : m) std::printf(" [%s]=%u", k.c_str(), v);
        std::printf("\n");
    };
    dump("T#", tex_regions);
    dump("CB", cb_regions);
    dump("VB", vb_regions);
    dump("RO buffer", buf_regions);

    // Shader slot census: which usage types the frame's programs use.
    std::map<u32, u32> slot_types;
    for (auto& [a, s] : shaders)
        for (auto& sl : s.slots) ++slot_types[sl.type << 8 | (sl.reg >= 16)];
    std::printf("input usage slots (type/inEUD:count):");
    for (auto [k, v] : slot_types) std::printf(" %02x/%u:%u", k >> 8, k & 1, v);
    std::printf("\n");

    // Mid-frame ctx samples against the draws that reached the GPU.
    for (auto& b : img.blocks) {
        if (b.tag != TagCtx) continue;
        const u8* c = b.bytes.data();
        auto q = [&](u32 o) { return *(const u64*)(c + o); };
        auto w = [&](u32 o) { return *(const u32*)(c + o); };
        u64 ps_obj = q(0x98), vs_obj = q(0x260), eud_ps = q(0x240), eud_vs = q(0x408), cb_ps = q(0x248), cb_vs = q(0x410);
        auto isa = [&](u64 prog) {
            const u8* p = img.Ptr(prog + 8, 8);
            return p ? ((u64)((const u32*)p)[0] | (u64)((const u32*)p)[1] << 32) << 8 : 0;
        };
        std::printf("\nctx %llx%s: cmd %llx dirty %x blend %x raster %x depth %x | PS obj %llx isa %llx eud %llx (%u dw) cb %llx"
                    " | VS obj %llx isa %llx eud %llx (%u dw) cb %llx | vb map %04x %04x %04x %04x\n",
                    (unsigned long long)b.addr, b.addr == m.delta + kMainCtx ? " (main)" : "", (unsigned long long)q(0x10),
                    w(0x64), w(0x48), w(0x4C), w(0x54), (unsigned long long)ps_obj, (unsigned long long)isa(ps_obj),
                    (unsigned long long)eud_ps, *(const u16*)(c + 0xA0), (unsigned long long)cb_ps, (unsigned long long)vs_obj,
                    (unsigned long long)isa(vs_obj), (unsigned long long)eud_vs, *(const u16*)(c + 0x268), (unsigned long long)cb_vs,
                    *(const u16*)(c + 0x38A), *(const u16*)(c + 0x38C), *(const u16*)(c + 0x38E), *(const u16*)(c + 0x390));
        u32 hits = 0, ps_dw = *(const u16*)(c + 0xA0);
        if (!ps_dw || ps_dw == 0xFFFF) {
            std::printf("  -> PS without EUD or metadata being parsed (bind in progress): no table to match\n");
            continue;
        }
        for (size_t i = 0; i < draws.size(); ++i) {
            if (ps[i].eud != eud_ps) continue;
            auto& d = draws[i];
            if (hits++ < 3)
                std::printf("  -> draw %zu seg %u: PS isa %s, VS isa %s, VS eud %s, count %u\n", i, d.seg,
                            d.ps == isa(ps_obj) ? "match" : "DIFF", d.vs == isa(vs_obj) ? "match" : "DIFF",
                            vs[i].eud == eud_vs ? "match" : "DIFF", d.count);
        }
        if (!hits) std::printf("  -> no draw uses this PS table (not committed yet at sample time)\n");
        for (auto& t : img.blocks)
            if (t.tag == TagCtxTable && (t.addr == eud_ps || t.addr == eud_vs)) {
                const u8* fin = img.Ptr(t.addr, t.bytes.size());
                u32 dw = *(const u16*)(c + (t.addr == eud_ps ? 0xA0 : 0x268));
                bool same = fin && !std::memcmp(fin, t.bytes.data(), std::min<size_t>(dw * 4ull, t.bytes.size()));
                std::printf("  %s table at sample time vs submitted: %s\n", t.addr == eud_ps ? "PS" : "VS", same ? "identical" : "changed later");
            }
    }

    if (csv) {
        FILE* f = std::fopen(csv, "w");
        std::fprintf(f, "i,seg,op,count,inst,vs,ps,state,rt0,z,prim,vs_eud,vs_eud_dw,vs_t,vs_s,vs_cb,vs_vb,ps_eud,ps_eud_dw,ps_t,ps_s,ps_cb,unres,t0_base,t0_fmt,t0_w,t0_h\n");
        for (size_t i = 0; i < draws.size(); ++i) {
            auto& d = draws[i];
            auto& a = vs[i];
            auto& b = ps[i];
            Tex t0 = b.textures.empty() ? Tex{} : b.textures[0];
            std::fprintf(f, "%zu,%u,%02x,%u,%u,%llx,%llx,%016llx,%llx,%llx,%u,%llx,%u,%u,%u,%u,%u,%llx,%u,%u,%u,%u,%u,%llx,%u/%u,%u,%u\n", i, d.seg,
                         d.op, d.count, d.instances, (unsigned long long)d.vs, (unsigned long long)d.ps,
                         (unsigned long long)d.state_hash, (unsigned long long)d.rt0, (unsigned long long)d.zbase, d.prim,
                         (unsigned long long)a.eud, a.eud_dwords, a.tex, a.samp, a.cb, a.vb, (unsigned long long)b.eud,
                         b.eud_dwords, b.tex, b.samp, b.cb, a.unresolved + b.unresolved, (unsigned long long)t0.base, t0.dfmt,
                         t0.nfmt, t0.w, t0.h);
        }
        std::fclose(f);
        // Distinct resources of the frame, one per line, for diffs between captures.
        f = std::fopen((std::string(csv) + ".sets").c_str(), "w");
        for (u64 a : progs_vs) std::fprintf(f, "vs %llx\n", (unsigned long long)a);
        for (u64 a : progs_ps) std::fprintf(f, "ps %llx\n", (unsigned long long)a);
        std::set<std::string> texs;
        for (size_t i = 0; i < draws.size(); ++i)
            for (auto* st : {&vs[i], &ps[i]})
                for (auto& t : st->textures) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "tex %llx %ux%u fmt %u/%u type %u tile %u%s", (unsigned long long)t.base, t.w,
                                  t.h, t.dfmt, t.nfmt, t.type, t.tile, targets.count(t.base) ? " target" : "");
                    texs.insert(buf);
                }
        for (auto& s : texs) std::fprintf(f, "%s\n", s.c_str());
        for (u64 s : samp_all) std::fprintf(f, "samp %016llx\n", (unsigned long long)s);
        std::fclose(f);
        std::printf("per-draw CSV -> %s (+.sets)\n", csv);
    }
    return 0;
}

// Locates persistent copies of the frame's image T#s in live memory: exact 16-byte T# heads (views) and qwords equal
// to an image base (resident blocks). Raw hits go to <out> before any summary.
static int Find(const char* cap, const char* out) {
    Image img;
    if (!Load(cap, img)) return std::printf("cannot load %s\n", cap), 1;
    std::vector<Draw> draws;
    std::map<u32, u32> ops;
    Replay(img, draws, ops);
    std::unordered_map<u64, ShaderInfo> shaders;
    std::unordered_set<u64> targets, bases;
    std::set<std::array<u32, 4>> heads;
    for (auto& d : draws) targets.insert(d.rt0), targets.insert(d.zbase);
    for (auto& d : draws)
        for (auto [prog, user] : {std::pair{d.vs, d.vs_user}, std::pair{d.ps, d.ps_user}}) {
            if (!prog) continue;
            auto it = shaders.find(prog);
            if (it == shaders.end()) it = shaders.emplace(prog, ParseShader(img, prog)).first;
            for (auto& t : DecodeStage(img, it->second, user).textures)
                if (t.base && !targets.count(t.base)) bases.insert(t.base), heads.insert({t.raw[0], t.raw[1], t.raw[2], t.raw[3]});
        }
    std::printf("%zu image bases, %zu distinct T# heads (render targets excluded)\n", bases.size(), heads.size());

    DWORD pid = FindPid(L"gow3.exe");
    Process p;
    p.h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p.h) return std::printf("OpenProcess failed\n"), 1;
    std::vector<Region> regs;
    for (auto& r : Regions(p.h))
        if (r.protect == PAGE_READWRITE && r.type != MEM_IMAGE) regs.push_back(r);
    // Chunks of 32 MB over all threads.
    struct Chunk {
        u64 base, size;
    };
    std::vector<Chunk> chunks;
    for (auto& r : regs)
        for (u64 o = 0; o < r.size; o += 32ull << 20) chunks.push_back({r.base + o, std::min<u64>(32ull << 20, r.size - o)});
    std::vector<std::vector<std::pair<u64, u32>>> found(chunks.size());  // (address, kind 0=T# head 1=base qword)
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < chunks.size();) {
                auto m = p.ReadRange(chunks[i].base, chunks[i].size);
                for (size_t o = 0; o + 16 <= m.size(); o += 8) {
                    u64 v = *(const u64*)&m[o];
                    if (bases.count(v)) found[i].push_back({chunks[i].base + o, 1});
                    const u32* w = (const u32*)&m[o];
                    if (w[1] >> 20 && heads.count({w[0], w[1], w[2], w[3]})) found[i].push_back({chunks[i].base + o, 0});
                }
            }
        });
    for (auto& t : pool) t.join();

    FILE* f = std::fopen(out, "w");
    std::map<std::string, std::array<u32, 2>> by_region;
    u64 hits = 0;
    for (auto& v : found)
        for (auto [a, kind] : v) {
            auto bytes = p.ReadRange(a & ~0xFull, 0x80);
            std::fprintf(f, "%llx %s", (unsigned long long)a, kind ? "base" : "tsharp");
            for (size_t k = 0; k < 0x80; k += 4) std::fprintf(f, " %08x", *(const u32*)&bytes[k]);
            std::fprintf(f, "\n");
            ++by_region[RegionName(regs, a)][kind];
            ++hits;
        }
    std::fclose(f);
    std::printf("%llu hits (raw -> %s)\n", (unsigned long long)hits, out);
    for (auto& [r, n] : by_region) std::printf("  [%s] T# heads %u, base qwords %u\n", r.c_str(), n[0], n[1]);
    return 0;
}

// ---------------------------------------------------------------- record: every frame from launch, lossless
// File: "GOW3REC1", then records [RecHdr][zstd payload] in any order. A pack holds new unique 4 KB pages (ids are
// consecutive); a frame holds RecFrameHdr, Meta and its blocks as page id lists, XOR-coded against the previous frame's
// ids of the same block, so frames decode in sequence order.

constexpr u32 kPage = 0x1000, kPackPages = 256;
enum RecKind : u32 { RecPack = 1, RecFrame = 2 };
struct RecHdr {
    u32 kind, count;  // pack: count = pages
    u64 id, raw, comp;  // pack: first page id; frame: sequence
};
struct RecFrameHdr {
    u64 time_us;
    u32 flags, suspended_us, new_pages, nblocks;  // flags 1 = no submit (boot/loading snapshot)
    u64 raw_bytes;
};
struct RecBlockHdr {
    u32 tag, xored;
    u64 addr, size;
};

template <class T> struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<T> q;
    size_t cap;
    bool closed = false;
    explicit Queue(size_t c) : cap(c) {}
    void Push(T v) {
        std::unique_lock l(m);
        cv.wait(l, [&] { return q.size() < cap; });
        q.push_back(std::move(v));
        cv.notify_all();
    }
    bool Pop(T& v) {
        std::unique_lock l(m);
        cv.wait(l, [&] { return !q.empty() || closed; });
        if (q.empty()) return false;
        v = std::move(q.front());
        q.pop_front();
        cv.notify_all();
        return true;
    }
    void Close() {
        std::lock_guard l(m);
        closed = true;
        cv.notify_all();
    }
};

struct H128 {
    size_t operator()(const XXH128_hash_t& h) const { return h.low64; }
    bool operator()(const XXH128_hash_t& a, const XXH128_hash_t& b) const { return a.low64 == b.low64 && a.high64 == b.high64; }
};

static std::atomic<bool> g_stop{false};

static std::vector<std::array<u64, 2>> HeapSpans(const Image& img);

static int Record(const char* out_path, bool running, u64 max_frames, bool heap) {
    SetConsoleCtrlHandler([](DWORD) -> BOOL { return g_stop = true, TRUE; }, TRUE);
    // A new process unless --running: an instance left from an earlier run is not the one being launched.
    DWORD old_pid = running ? 0 : FindPid(L"gow3.exe"), pid = 0;
    std::printf("waiting for %sgow3.exe (Ctrl+C stops)\n", old_pid ? "a new " : "");
    while (!g_stop && (!(pid = FindPid(L"gow3.exe")) || pid == old_pid)) Sleep(1);
    Process p;
    p.h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME | SYNCHRONIZE, FALSE, pid);
    if (!p.h) return std::printf("OpenProcess failed %lu\n", GetLastError()), 1;
    auto exited = [&] { return WaitForSingleObject(p.h, 0) == WAIT_OBJECT_0; };
    while (!g_stop && !exited() && !p.delta) {
        for (auto& r : Regions(p.h))
            if (r.size == kDataSize) p.delta = r.base - kDataVa;
        if (!p.delta) Sleep(1);
    }
    if (!p.delta) return std::printf("gow3.exe ended before the EBOOT .data was mapped\n"), 1;
    FILE* f = std::fopen(out_path, "wb");
    if (!f) return std::printf("cannot write %s\n", out_path), 1;
    std::fwrite("GOW3REC1", 1, 8, f);
    std::printf("pid %lu delta +0x%llx -> %s\n", pid, (unsigned long long)p.delta, out_path);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto suspend = (NtProc)GetProcAddress(nt, "NtSuspendProcess");
    auto resume = (NtProc)GetProcAddress(nt, "NtResumeProcess");

    // Compressors: any number of threads, records land in any order.
    struct Job {
        RecHdr hdr;
        std::vector<u8> raw;
    };
    Queue<Job> jobs(64);
    std::mutex fmu;
    std::atomic<u64> file_bytes{8};
    std::vector<std::thread> zpool;
    for (unsigned t = 0; t < std::max(2u, std::thread::hardware_concurrency() - 2); ++t)
        zpool.emplace_back([&] {
            std::vector<u8> out;
            for (Job j; jobs.Pop(j);) {
                out.resize(ZSTD_compressBound(j.raw.size()));
                size_t n = ZSTD_compress(out.data(), out.size(), j.raw.data(), j.raw.size(), 1);
                if (ZSTD_isError(n)) std::printf("zstd: %s\n", ZSTD_getErrorName(n)), std::abort();
                j.hdr.comp = n;
                std::lock_guard l(fmu);
                std::fwrite(&j.hdr, sizeof(j.hdr), 1, f);
                std::fwrite(out.data(), 1, n, f);
                file_bytes += sizeof(j.hdr) + n;
            }
        });

    // Store: one thread owns the page dictionary and the previous ids per block.
    struct RawFrame {
        RecFrameHdr hdr;
        Meta meta;
        std::vector<Block> blocks;
    };
    Queue<RawFrame> frames(6);  // full queue: the game stays suspended until the store catches up
    std::atomic<u64> unique_pages{0}, raw_total{0};
    std::thread store([&] {
        std::unordered_map<XXH128_hash_t, u32, H128, H128> dict;
        std::map<std::pair<u32, u64>, std::vector<u32>> prev;
        std::vector<u8> pack;
        u64 pack_first = 0, seq = 0;
        auto flush = [&] {
            if (pack.empty()) return;
            u32 n = (u32)(pack.size() / kPage);
            jobs.Push({{RecPack, n, pack_first, pack.size(), 0}, std::move(pack)});
            pack_first += n;
            pack.clear();
        };
        for (RawFrame fr; frames.Pop(fr);) {
            std::vector<u8> desc(sizeof(RecFrameHdr) + sizeof(Meta));
            for (auto& b : fr.blocks) {
                u64 size = b.bytes.size();
                size_t n = (size + kPage - 1) / kPage;
                b.bytes.resize(n * kPage);
                std::vector<u32> ids(n);
                for (size_t i = 0; i < n; ++i) {
                    const u8* pg = b.bytes.data() + i * kPage;
                    auto [it, fresh] = dict.try_emplace(XXH3_128bits(pg, kPage), (u32)dict.size());
                    if (fresh) {
                        ++fr.hdr.new_pages;
                        pack.insert(pack.end(), pg, pg + kPage);
                        if (pack.size() == kPackPages * kPage) flush();
                    }
                    ids[i] = it->second;
                }
                fr.hdr.raw_bytes += size;
                auto& old = prev[{b.tag, b.addr}];
                RecBlockHdr bh{b.tag, old.size() == n, b.addr, size};
                desc.insert(desc.end(), (u8*)&bh, (u8*)(&bh + 1));
                for (size_t i = 0; i < n; ++i) {
                    u32 v = ids[i] ^ (bh.xored ? old[i] : 0);
                    desc.insert(desc.end(), (u8*)&v, (u8*)(&v + 1));
                }
                old = std::move(ids);
                PoolPut(std::move(b.bytes));
            }
            fr.hdr.nblocks = (u32)fr.blocks.size();
            std::memcpy(desc.data(), &fr.hdr, sizeof(RecFrameHdr));
            std::memcpy(desc.data() + sizeof(RecFrameHdr), &fr.meta, sizeof(Meta));
            unique_pages += fr.hdr.new_pages;
            raw_total += fr.hdr.raw_bytes;
            jobs.Push({{RecFrame, 0, seq++, desc.size(), 0}, std::move(desc)});
        }
        flush();
    });

    // Poll: the chain head (ctx+0x40) moves right after each submit; the ring cursor is tracked per slice for the
    // frame's extent. Without a submit for 2 ms (boot, loading) a snapshot without chain is taken instead.
    const u64 ctx = p.delta + kMainCtx, ring = p.delta + kRingGlobals;
    u64 head = p.Get<u64>(ctx + 0x40), ring_max[4] = {}, last_slice = ~0ull;
    auto t0 = std::chrono::steady_clock::now(), last = t0;
    u64 nframes = 0, nidle = 0, gaps = 0, susp_sum = 0, susp_max = 0, wait_max = 0;
    u32 last_counter = 0;
    // The poll loop never writes the console: the game logs to the same console, and a printf blocked behind it let
    // the game run past whole submits (lost frames). A printer thread shows notes and progress instead.
    std::mutex log_m;
    std::string notes;
    std::atomic<u64> a_records{0};
    std::atomic<u32> a_counter{0};
    std::atomic<bool> loop_done{false};
    auto note = [&](const char* fmt, u32 a, u32 b) {
        char s[96];
        std::snprintf(s, sizeof(s), fmt, a, b);
        std::lock_guard l(log_m);
        notes += s;
    };
    auto flush_notes = [&] {
        std::string s;
        std::lock_guard l(log_m);
        s.swap(notes);
        std::fputs(s.c_str(), stdout);
    };
    std::thread printer([&] {
        for (u64 shown = 0; !loop_done; Sleep(100)) {
            flush_notes();
            if (u64 n = a_records; n / 1000 != shown / 1000)
                shown = n, std::printf("%llu records, frame %u, %llu MB raw, %llu MB unique, %llu MB file\n", (unsigned long long)n,
                                       (u32)a_counter, (unsigned long long)(raw_total >> 20),
                                       (unsigned long long)(unique_pages * kPage >> 20), (unsigned long long)(file_bytes >> 20));
        }
    });
    for (u64 it = 0; !g_stop && nframes < max_frames; ++it) {
        u64 h = p.Get<u64>(ctx + 0x40), rs[3];
        if (p.Read(ring + 0x58, rs, sizeof(rs))) {
            u64 s = rs[0] & 3;
            if (s != last_slice) ring_max[s] = 0, last_slice = s;
            ring_max[s] = std::max(ring_max[s], rs[2]);
        }
        auto now = std::chrono::steady_clock::now();
        bool submit = h && h != head, idle = !submit && now - last > std::chrono::milliseconds(2);
        if (!submit && !idle) {
            if (!(it & 1023) && exited()) break;
            continue;
        }
        if (exited()) break;
        suspend(p.h);
        auto ts = std::chrono::steady_clock::now();
        if (submit) h = p.Get<u64>(ctx + 0x40);
        RawFrame fr{};
        bool chain_ok = ReadFrame(p, submit ? head : 0, h, true, ring_max, fr.blocks, fr.meta);
        if (heap && submit && chain_ok) {  // replay the frame now, still suspended, to find the heap it reads
            Image img;
            img.meta = fr.meta, img.blocks = std::move(fr.blocks), img.Index();
            const auto spans = HeapSpans(img);
            fr.blocks = std::move(img.blocks);
            const size_t first = fr.blocks.size();
            for (auto [lo, hi] : spans) fr.blocks.push_back({TagHeap, lo, PoolGet(hi - lo)});
            ReadBlocks(p, fr.blocks, first);
        }
        auto tr = std::chrono::steady_clock::now();
        fr.meta.suspended_us = fr.hdr.suspended_us = (u32)std::chrono::duration_cast<std::chrono::microseconds>(tr - ts).count();
        fr.hdr.time_us = std::chrono::duration_cast<std::chrono::microseconds>(ts - t0).count();
        fr.hdr.flags = !submit;
        u32 counter = fr.meta.frame_counter;
        frames.Push(std::move(fr));
        resume(p.h);
        auto wait_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tr).count();
        susp_sum += (u64)std::chrono::duration_cast<std::chrono::microseconds>(tr - ts).count() + wait_us;
        susp_max = std::max<u64>(susp_max, std::chrono::duration_cast<std::chrono::microseconds>(tr - ts).count() + wait_us);
        wait_max = std::max(wait_max, wait_us);
        if (submit) {
            if (last_counter && counter != last_counter + 1 && gaps++ < 20) note("gap: frame counter %u -> %u\n", last_counter, counter);
            if (!chain_ok) note("frame %u: chain BROKEN\n", counter, 0);
            last_counter = counter;
            head = h;
            ++nframes;
        } else {
            ++nidle;
        }
        a_records = nframes + nidle;
        a_counter = counter;
        last = std::chrono::steady_clock::now();
    }
    loop_done = true;
    printer.join();
    flush_notes();
    frames.Close();
    store.join();
    jobs.Close();
    for (auto& t : zpool) t.join();
    std::fclose(f);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("done: %llu frames, %llu snapshots without submit, %llu frame-counter gaps, %.1f s\n"
                "  suspended avg %llu us, max %llu us (store backpressure max %llu us)\n"
                "  %llu MB raw, %llu MB unique pages, %llu MB file -> %s\n",
                (unsigned long long)nframes, (unsigned long long)nidle, (unsigned long long)gaps, secs,
                (unsigned long long)(susp_sum / std::max<u64>(1, nframes + nidle)), (unsigned long long)susp_max,
                (unsigned long long)wait_max, (unsigned long long)(raw_total >> 20),
                (unsigned long long)(unique_pages * kPage >> 20), (unsigned long long)(file_bytes >> 20), out_path);
    return 0;
}

// ---------------------------------------------------------------- recording reader

struct RecFile {
    struct Pack {
        u64 first, count, off, comp, raw;
    };
    struct FrameRef {
        u64 seq, off, comp, raw;
    };
    FILE* f = nullptr;
    std::mutex fm, cm;
    std::vector<Pack> packs;  // by first page id
    std::vector<FrameRef> frames;  // by sequence
    std::unordered_map<size_t, std::shared_ptr<std::vector<u8>>> cache;  // decompressed packs, FIFO up to 2 GB
    std::deque<size_t> order;

    bool Open(const char* path) {
        if (!(f = std::fopen(path, "rb"))) return false;
        char magic[8];
        if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "GOW3REC1", 8)) return false;
        for (RecHdr h; std::fread(&h, sizeof(h), 1, f) == 1;) {
            u64 off = _ftelli64(f);
            if (h.kind == RecPack) packs.push_back({h.id, h.count, off, h.comp, h.raw});
            else if (h.kind == RecFrame) frames.push_back({h.id, off, h.comp, h.raw});
            if (_fseeki64(f, h.comp, SEEK_CUR)) break;
        }
        std::sort(packs.begin(), packs.end(), [](auto& a, auto& b) { return a.first < b.first; });
        std::sort(frames.begin(), frames.end(), [](auto& a, auto& b) { return a.seq < b.seq; });
        return true;
    }
    std::vector<u8> Payload(u64 off, u64 comp, u64 raw) {
        std::vector<u8> z(comp), out(raw);
        {
            std::lock_guard l(fm);
            _fseeki64(f, off, SEEK_SET);
            if (std::fread(z.data(), 1, comp, f) != comp) out.clear();
        }
        if (!out.empty() && ZSTD_isError(ZSTD_decompress(out.data(), raw, z.data(), comp))) out.clear();
        return out;
    }
    void Page(u32 id, u8* out) {
        auto it = std::upper_bound(packs.begin(), packs.end(), (u64)id, [](u64 v, const Pack& p) { return v < p.first; });
        if (it == packs.begin()) return void(std::memset(out, 0, kPage));
        if (--it; id >= it->first + it->count) return void(std::memset(out, 0, kPage));
        size_t i = it - packs.begin();
        std::shared_ptr<std::vector<u8>> data;
        {
            std::lock_guard l(cm);
            if (auto c = cache.find(i); c != cache.end()) data = c->second;
        }
        if (!data) {
            data = std::make_shared<std::vector<u8>>(Payload(it->off, it->comp, it->raw));
            std::lock_guard l(cm);
            if (cache.emplace(i, data).second) order.push_back(i);
            while (order.size() > (2048u << 20) / (kPackPages * kPage)) cache.erase(order.front()), order.pop_front();
        }
        if ((id - it->first + 1) * kPage <= data->size()) std::memcpy(out, data->data() + (id - it->first) * kPage, kPage);
        else std::memset(out, 0, kPage);
    }
};

struct FrameDesc {
    RecFrameHdr hdr{};
    Meta meta{};
    std::vector<RecBlockHdr> blocks;
    std::vector<std::vector<u32>> ids;
};

// Frames must be decoded in sequence: block ids are XOR-coded against the previous frame's.
struct DescDecoder {
    std::map<std::pair<u32, u64>, std::vector<u32>> prev;
    bool Next(RecFile& r, size_t i, FrameDesc& d) {
        auto raw = r.Payload(r.frames[i].off, r.frames[i].comp, r.frames[i].raw);
        if (raw.size() < sizeof(RecFrameHdr) + sizeof(Meta)) return false;
        std::memcpy(&d.hdr, raw.data(), sizeof(RecFrameHdr));
        std::memcpy(&d.meta, raw.data() + sizeof(RecFrameHdr), sizeof(Meta));
        size_t o = sizeof(RecFrameHdr) + sizeof(Meta);
        d.blocks.clear();
        d.ids.clear();
        for (u32 k = 0; k < d.hdr.nblocks; ++k) {
            RecBlockHdr bh;
            std::memcpy(&bh, raw.data() + o, sizeof(bh));
            o += sizeof(bh);
            size_t n = (bh.size + kPage - 1) / kPage;
            std::vector<u32> ids(n);
            std::memcpy(ids.data(), raw.data() + o, n * 4);
            o += n * 4;
            auto& old = prev[{bh.tag, bh.addr}];
            if (bh.xored)
                for (size_t j = 0; j < n; ++j) ids[j] ^= old[j];
            old = ids;
            d.blocks.push_back(bh);
            d.ids.push_back(std::move(ids));
        }
        return true;
    }
};

static void BuildImage(RecFile& r, const FrameDesc& d, Image& img) {
    img.meta = d.meta;
    img.blocks.clear();
    img.blocks.push_back({TagMeta, 0, std::vector<u8>((const u8*)&d.meta, (const u8*)(&d.meta + 1))});
    for (size_t k = 0; k < d.blocks.size(); ++k) {
        Block b{d.blocks[k].tag, d.blocks[k].addr, std::vector<u8>(d.ids[k].size() * kPage)};
        for (size_t j = 0; j < d.ids[k].size(); ++j) r.Page(d.ids[k][j], b.bytes.data() + j * kPage);
        b.bytes.resize(d.blocks[k].size);
        img.blocks.push_back(std::move(b));
    }
    img.Index();
}

// One CSV row per recorded frame with the analyze census numbers; batches of frames on all cores.
static int Timeline(const char* path, const char* csv) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    FILE* out = std::fopen(csv, "w");
    if (!out) return std::printf("cannot write %s\n", csv), 1;
    std::fprintf(out, "seq,frame,idle,time_ms,suspended_us,new_kb,raw_kb,draws,dispatches,segments,vs,ps,t_refs,t_distinct,"
                      "s_distinct,cb,vb,unresolved,ring_kb,arena0_kb,arena1_kb,arena2_kb,regions\n");
    DescDecoder dec;
    std::vector<FrameDesc> batch;
    std::vector<std::string> rows;
    for (size_t b0 = 0; b0 < r.frames.size(); b0 += batch.size()) {
        batch.resize(std::min<size_t>(256, r.frames.size() - b0));
        for (size_t i = 0; i < batch.size(); ++i)
            if (!dec.Next(r, b0 + i, batch[i])) return std::printf("frame record %zu unreadable\n", b0 + i), 1;
        rows.assign(batch.size(), {});
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
            pool.emplace_back([&] {
                Image img;
                for (size_t i; (i = next++) < batch.size();) {
                    auto& d = batch[i];
                    BuildImage(r, d, img);
                    std::vector<Draw> draws;
                    std::map<u32, u32> ops;
                    Replay(img, draws, ops);
                    std::unordered_map<u64, ShaderInfo> shaders;
                    std::unordered_set<u64> vs, ps, tex, samp;
                    u32 ndraw = 0, ncomp = 0;
                    u64 t_refs = 0, cb = 0, vb = 0, unres = 0;
                    for (auto& dr : draws) {
                        dr.compute ? ++ncomp : ++ndraw;
                        for (auto [prog, user, set] : {std::tuple{dr.vs, dr.vs_user, &vs}, {dr.ps, dr.compute ? dr.vs_user : dr.ps_user, &ps}}) {
                            if (!prog) continue;
                            set->insert(prog);
                            auto it = shaders.find(prog);
                            if (it == shaders.end()) it = shaders.emplace(prog, ParseShader(img, prog)).first;
                            auto st = DecodeStage(img, it->second, user);
                            t_refs += st.textures.size(), cb += st.cb, vb += st.vb, unres += st.unresolved;
                            for (auto& t : st.textures) tex.insert(t.base);
                            for (u64 s : st.samplers) samp.insert(s);
                        }
                    }
                    u64 kb[4] = {}, nreg = 0, segs = 0;
                    int arena = 0;
                    for (auto& b : img.blocks) {
                        if (b.tag == TagRing) kb[0] = b.bytes.size() >> 10;
                        if (b.tag == TagArena && arena < 3) kb[1 + arena++] = b.bytes.size() >> 10;
                        if (b.tag == TagRegions) nreg = b.bytes.size() / sizeof(Region);
                        if (b.tag == TagChain) segs = b.bytes.size() / sizeof(Run);
                    }
                    char row[512];
                    std::snprintf(row, sizeof(row), "%llu,%u,%u,%.3f,%u,%llu,%llu,%u,%u,%llu,%zu,%zu,%llu,%zu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                                  (unsigned long long)r.frames[b0 + i].seq, d.meta.frame_counter, d.hdr.flags, d.hdr.time_us / 1000.0,
                                  d.hdr.suspended_us, (unsigned long long)d.hdr.new_pages * 4, (unsigned long long)(d.hdr.raw_bytes >> 10),
                                  ndraw, ncomp, (unsigned long long)segs, vs.size(), ps.size(), (unsigned long long)t_refs, tex.size(),
                                  samp.size(), (unsigned long long)cb, (unsigned long long)vb, (unsigned long long)unres,
                                  (unsigned long long)kb[0], (unsigned long long)kb[1], (unsigned long long)kb[2], (unsigned long long)kb[3],
                                  (unsigned long long)nreg);
                    rows[i] = row;
                }
            });
        for (auto& t : pool) t.join();
        for (auto& s : rows) std::fputs(s.c_str(), out);
    }
    std::fclose(out);
    std::printf("%zu frames, %zu packs -> %s\n", r.frames.size(), r.packs.size(), csv);
    return 0;
}

// Whole-recording census for the translation layer: resource lifetimes keyed the way a cache would key them (program
// address -> ISA content, texture base -> descriptor shape), session-wide sampler/pipeline totals, where buffers live,
// and every non-draw PM4 packet with its subtype. Frames are decoded in parallel batches and merged in order.
struct CensusFrame {
    u64 seq = 0, time_us = 0;
    u32 counter = 0, draws = 0, dispatches = 0;
    std::vector<std::array<u64, 3>> progs;  // addr, ISA xxh3, stage (0 VS, 1 PS, 2 CS, 3 fetch) | code bytes << 8
    std::vector<std::array<u64, 3>> texs;   // base, shape key, target this frame
    std::unordered_set<u64> samplers, pipes, states, targets;
    std::map<std::string, u64> ops;  // "op" or "op:subtype"
    u64 vb[3] = {}, cb[3] = {}, buf[3] = {};  // ring, CPU arena, other (asset heap)
};

static void CensusOne(const Image& img, CensusFrame& cf) {
    std::vector<Draw> draws;
    std::map<u32, u32> ops;
    Replay(img, draws, ops);
    const Meta& m = img.meta;
    std::vector<std::pair<u64, u64>> arenas;
    for (auto& b : img.blocks)
        if (b.tag == TagArena) arenas.push_back({b.addr, b.addr + (16ull << 20)});
    auto where = [&](u64 a) {
        if (a >= m.slice_begin && a < m.slice_end) return 0;
        for (auto [lo, hi] : arenas)
            if (a >= lo && a < hi) return 1;
        return 2;
    };
    std::unordered_map<u64, ShaderInfo> shaders;
    auto prog = [&](u64 a, u64 stage) -> const ShaderInfo& {
        auto it = shaders.find(a);
        if (it != shaders.end()) return it->second;
        auto& s = shaders[a] = ParseShader(img, a);
        u64 n = s.ok ? s.code_bytes : 0x100;
        const u8* p = img.Ptr(a, n);
        cf.progs.push_back({a, p ? XXH3_64bits(p, n) : 0, stage | n << 8});
        return s;
    };
    for (auto& d : draws) cf.targets.insert(d.rt0), cf.targets.insert(d.zbase);
    for (auto& d : draws) {
        d.compute ? ++cf.dispatches : ++cf.draws;
        cf.states.insert(d.state_hash);
        cf.pipes.insert(Mix(Mix(d.vs, d.ps), d.state_hash));
        for (auto [a, user, stage] : {std::tuple{d.vs, d.vs_user, 0ull}, {d.ps, d.compute ? d.vs_user : d.ps_user, d.compute ? 2ull : 1ull}}) {
            if (!a) continue;
            auto st = DecodeStage(img, prog(a, stage), user);
            if (st.fetch && !shaders.count(st.fetch)) {
                u32 n = FetchBytes(img, st.fetch);
                const u8* p = n ? img.Ptr(st.fetch, n) : nullptr;
                shaders[st.fetch] = {};
                cf.progs.push_back({st.fetch, p ? XXH3_64bits(p, n) : 0, 3 | (u64)n << 8});
            }
            for (auto& t : st.textures) {
                u64 shape = (u64)t.dfmt | (u64)t.nfmt << 6 | (u64)t.w << 10 | (u64)t.h << 24 | (u64)t.type << 38 | (u64)t.tile << 42;
                cf.texs.push_back({t.base, shape, cf.targets.count(t.base)});
            }
            for (u64 s : st.samplers) cf.samplers.insert(s);
            for (u64 v : st.vbs) ++cf.vb[where(v)];
            for (u64 v : st.cbs) ++cf.cb[where(v)];
            for (u64 v : st.bufs) ++cf.buf[where(v)];
        }
    }
    // Non-draw packets with their subtype, in chain order.
    const Block* chain = img.Find(TagChain);
    for (size_t ri = 0; chain && ri < chain->bytes.size() / sizeof(Run); ++ri) {
        const Run& r = ((const Run*)chain->bytes.data())[ri];
        const u32* d = (const u32*)img.Ptr(r.begin, r.end - r.begin);
        size_t n = (r.end - r.begin) / 4;
        for (size_t i = 0; d && i < n;) {
            if (d[i] >> 30 != 3) {
                ++i;
                continue;
            }
            u32 cnt = ((d[i] >> 16) & 0x3FFF) + 1, op = (d[i] >> 8) & 0xFF;
            const u32* b = d + i + 1;
            char key[64];
            auto addr_class = [&](u64 a) {
                u64 labels = img.Find(TagLabels) ? img.Find(TagLabels)->addr : 0;
                return a >= labels && a < labels + 0x1000 ? "labels" : where(a) == 0 ? "ring" : where(a) == 1 ? "arena" : "other";
            };
            if (i + cnt >= n) break;
            switch (op) {
            case 0x10: case 0x69: case 0x76: case 0x79: case 0x27: case 0x2D: case 0x35: case 0x15: key[0] = 0; break;
            case 0x46: std::snprintf(key, sizeof(key), "46 EVENT_WRITE type %02x", b[0] & 0x3F); break;
            case 0x47: std::snprintf(key, sizeof(key), "47 EOP type %02x data %u int %u -> %s", b[0] & 0x3F, b[2] >> 29, (b[2] >> 24) & 3,
                                     addr_class(b[1] | (u64)(b[2] & 0xFFFF) << 32)); break;
            case 0x48: std::snprintf(key, sizeof(key), "48 EOS type %02x -> %s", b[0] & 0x3F, addr_class(b[1] | (u64)(b[2] & 0xFFFF) << 32)); break;
            case 0x3C: std::snprintf(key, sizeof(key), "3c WAIT_REG_MEM fn %u %s -> %s", b[0] & 7, b[0] & 0x10 ? "mem" : "reg",
                                     b[0] & 0x10 ? addr_class(b[1] | (u64)(b[2] & 0xFFFF) << 32) : "reg"); break;
            case 0x20: std::snprintf(key, sizeof(key), "20 SET_PREDICATION op %u bool %u -> %s", (b[1] >> 16) & 7, (b[1] >> 8) & 1,
                                     addr_class((b[0] & ~0xFull) | (u64)(b[1] & 0xFF) << 32)); break;
            case 0x58: std::snprintf(key, sizeof(key), "58 ACQUIRE_MEM coher %08x", b[0]); break;
            case 0x2F: std::snprintf(key, sizeof(key), "2f NUM_INSTANCES %s", b[0] > 1 ? ">1" : "1"); break;
            case 0x37: std::snprintf(key, sizeof(key), "37 WRITE_DATA dst %u", (b[0] >> 8) & 0xF); break;
            default: std::snprintf(key, sizeof(key), "%02x", op);
            }
            if (key[0]) ++cf.ops[key];
            i += cnt + 1;
        }
    }
}

// GPU-order walk of one frame: where each predication's query was written, RT changes (event 0x2A, only 011a3320)
// between them, and how much index work runs predicated.
static int Predication(const char* path) {
    Image img;
    if (!Load(path, img)) return 1;
    const Block* chain = img.Find(TagChain);
    if (!chain) return std::printf("no chain\n"), 1;
    std::unordered_map<u64, u64> end_at;  // query base -> rt change count when its end was dumped
    u64 rt = 0, pred_on = 0, found = 0, same_rt = 0, one_rt = 0, more_rt = 0, idx_all = 0, idx_pred = 0;
    u64 draws_all = 0, draws_pred = 0, queries = 0, runs = 0;
    bool inside = false, last_was_pred = false;
    for (size_t ri = 0; ri < chain->bytes.size() / sizeof(Run); ++ri) {
        const Run& r = ((const Run*)chain->bytes.data())[ri];
        const u32* d = (const u32*)img.Ptr(r.begin, r.end - r.begin);
        size_t n = (r.end - r.begin) / 4;
        for (size_t i = 0; d && i < n;) {
            if (d[i] >> 30 != 3) {
                ++i;
                continue;
            }
            u32 cnt = ((d[i] >> 16) & 0x3FFF) + 1, op = (d[i] >> 8) & 0xFF;
            const u32* b = d + i + 1;
            if (i + cnt >= n) break;
            u64 idx = op == 0x27 ? b[3] : op == 0x35 ? b[2] : op == 0x2D ? b[0] : ~0ull;
            if (idx != ~0ull) {
                idx_all += idx, ++draws_all;
                if (inside) idx_pred += idx, ++draws_pred, runs += !last_was_pred;
                last_was_pred = inside;
            }
            if (op == 0x46 && (b[0] & 0x3F) == 0x2A) ++rt;
            if (op == 0x46 && (b[0] & 0x3F) == 0x39) {
                u64 a = (b[1] | (u64)(b[2] & 0xFFFF) << 32) & ~0x7ull;
                if (a & 8) end_at[a - 8] = rt, ++queries;
            }
            if (op == 0x20) {
                inside = (b[1] >> 8) & 1;
                if (inside) {
                    ++pred_on;
                    u64 a = (b[0] & ~0xFull) | (u64)(b[1] & 0xFF) << 32;
                    auto it = end_at.find(a);
                    if (it != end_at.end()) {
                        ++found;
                        u64 k = rt - it->second;
                        ++(k == 0 ? same_rt : k == 1 ? one_rt : more_rt);
                    }
                }
            }
            i += cnt + 1;
        }
    }
    std::printf("queries ended %llu, predications %llu (query found earlier in frame: %llu)\n", queries, pred_on, found);
    std::printf("RT changes between query end and predication: 0 -> %llu, 1 -> %llu, >1 -> %llu\n", same_rt, one_rt, more_rt);
    std::printf("contiguous predicated runs %llu\n", runs);
    std::printf("draws predicated %llu of %llu, indices predicated %llu of %llu (%.1f%%)\n", draws_pred, draws_all, idx_pred,
                idx_all, idx_all ? 100.0 * idx_pred / idx_all : 0.0);
    return 0;
}

// Bytes the frame's GPU work reads (buffer V# contents, index buffers, descriptor tables), merged per class:
// 0 ring slice, 1 current double arena, 2 other double arena, 3 1 MB arena, 4 everything else (asset heap).
// blk = the CPU-written blocks of classes 0..3 as captured {base, bytes}.
struct FrameSpans {
    std::vector<std::array<u64, 2>> cls[5];  // merged [lo, hi)
    std::array<u64, 2> blk[4] = {};
    u64 bytes[5] = {}, capped = 0;
    std::vector<std::array<u64, 2>> tex;       // texture base levels [lo, hi), unmerged
    std::vector<std::array<u64, 2>> tex_full;  // whole textures (every level, layer and face), over-estimated
};

// Base mip level footprint of a T# (pitch x height x bits per texel); enough to watch its memory for writes.
static u64 TexBytes(const Tex& t) {
    static const u8 bits[64] = {0, 8, 16, 16, 32, 32, 32, 32, 32, 32, 32, 64, 64, 96, 128, 0, 16, 16, 16, 16, 32, 32, 64, 0, 0,
                                0, 0,  0,  0,  0,  0,  0,  8,  8,  16, 4,  8,  8,  4,  8,  8,  8};
    u64 pitch = std::max<u64>(((t.raw[4] >> 13) & 0x3FFF) + 1, t.w), b = bits[t.dfmt] ? bits[t.dfmt] : 32;
    return std::min<u64>(pitch * t.h * b / 8, 64ull << 20);
}

// Whole texture: base level x layers (x6 for cubes) x2 for the mip chain and tile padding; reading too much is harmless.
static u64 TexFootprint(const Tex& t) {
    const u64 layers = t.type == 10 || t.type >= 12 ? (t.raw[4] & 0x1FFF) + 1 : 1;
    return std::min<u64>(TexBytes(t) * layers * (t.type == 11 ? 6 : 1) * 2, 256ull << 20);
}

static FrameSpans Spans(const Image& img) {
    FrameSpans fs;
    const Meta& m = img.meta;
    const Block* data = img.Find(TagData);
    u32 cur = data ? *(const u32*)(data->bytes.data() + (0x02A7805C - kDataVa)) & 1 : 0;
    u32 ai = 0;
    for (auto& b : img.blocks) {
        if (b.tag == TagRing) fs.blk[0] = {b.addr, b.bytes.size()};
        if (b.tag == TagArena && ai < 3) fs.blk[ai == 2 ? 3 : ai == cur ? 1 : 2] = {b.addr, b.bytes.size()}, ++ai;
    }
    auto end_of = [&](int c) { return c == 0 ? m.slice_end : fs.blk[c][0] + (c == 3 ? 1ull << 20 : 16ull << 20); };
    auto add = [&](u64 a, u64 n) {
        if (!a || !n) return;
        int c = 4;
        if (a >= m.slice_begin && a < m.slice_end) c = 0;
        for (int k = 1; k < 4 && c == 4; ++k)
            if (fs.blk[k][0] && a >= fs.blk[k][0] && a < end_of(k)) c = k;
        u64 hi = c < 4 ? std::min(a + n, end_of(c)) : a + std::min<u64>(n, 64ull << 20);
        fs.capped += c == 4 && n > (64ull << 20);
        fs.cls[c].push_back({a, hi});
    };
    std::vector<Draw> draws;
    std::map<u32, u32> ops;
    Replay(img, draws, ops);
    std::unordered_map<u64, ShaderInfo> shaders;
    for (auto& d : draws) {
        add(d.ib, d.ib_bytes);
        for (auto [prog, user] : {std::pair{d.vs, d.vs_user}, {d.ps, d.compute ? d.vs_user : d.ps_user}}) {
            if (!prog) continue;
            auto it = shaders.find(prog);
            if (it == shaders.end()) it = shaders.emplace(prog, ParseShader(img, prog)).first;
            auto st = DecodeStage(img, it->second, user);
            for (auto [a, n] : st.spans) add(a, n);
            for (auto& t : st.textures)
                if (t.base) fs.tex.push_back({t.base, t.base + TexBytes(t)}), fs.tex_full.push_back({t.base, t.base + TexFootprint(t)});
        }
    }
    for (int c = 0; c < 5; ++c) {
        auto& v = fs.cls[c];
        std::sort(v.begin(), v.end());
        size_t o = 0;
        for (auto& s : v)
            if (o && s[0] <= v[o - 1][1]) v[o - 1][1] = std::max(v[o - 1][1], s[1]);
            else v[o++] = s;
        v.resize(o);
        for (auto& s : v) fs.bytes[c] += s[1] - s[0];
    }
    return fs;
}

// Asset heap ranges a frame's draws read, page aligned and merged: buffers (class 4) and whole textures outside the
// CPU-written blocks.
static std::vector<std::array<u64, 2>> HeapSpans(const Image& img) {
    auto fs = Spans(img);
    auto v = fs.cls[4];
    for (auto& t : fs.tex_full) {
        bool cpu = t[0] >= img.meta.slice_begin && t[0] < img.meta.slice_end;
        for (int k = 1; k < 4 && !cpu; ++k) cpu = fs.blk[k][0] && t[0] >= fs.blk[k][0] && t[0] < fs.blk[k][0] + fs.blk[k][1];
        if (!cpu) v.push_back(t);
    }
    for (auto& s : v) s[0] &= ~0xFFFull, s[1] = (s[1] + 0xFFF) & ~0xFFFull;
    std::sort(v.begin(), v.end());
    size_t o = 0;
    for (auto& s : v)
        if (o && s[0] <= v[o - 1][1]) v[o - 1][1] = std::max(v[o - 1][1], s[1]);
        else v[o++] = s;
    v.resize(o);
    return v;
}

// One CSV row per submitted frame: CPU-written block sizes against what the GPU actually reads from them.
static int Volume(const char* path, const char* csv) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    FILE* out = std::fopen(csv, "w");
    if (!out) return std::printf("cannot write %s\n", csv), 1;
    std::fprintf(out, "seq,time_ms,draws,ring_kb,arena_cur_kb,arena_1m_kb,ref_ring_kb,ref_arena_cur_kb,ref_arena_other_kb,"
                      "ref_arena_1m_kb,ref_heap_kb,spans_cpu,spans_heap,capped\n");
    DescDecoder dec;
    std::vector<FrameDesc> batch;
    std::vector<std::string> rows;
    for (size_t b0 = 0; b0 < r.frames.size(); b0 += batch.size()) {
        batch.resize(std::min<size_t>(256, r.frames.size() - b0));
        for (size_t i = 0; i < batch.size(); ++i)
            if (!dec.Next(r, b0 + i, batch[i])) return std::printf("frame record %zu unreadable\n", b0 + i), 1;
        rows.assign(batch.size(), {});
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
            pool.emplace_back([&] {
                Image img;
                for (size_t i; (i = next++) < batch.size();) {
                    BuildImage(r, batch[i], img);
                    if (!img.Find(TagChain)) continue;
                    std::vector<Draw> draws;
                    std::map<u32, u32> ops;
                    Replay(img, draws, ops);
                    FrameSpans fs = Spans(img);
                    char row[384];
                    std::snprintf(row, sizeof(row), "%llu,%.3f,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%zu,%llu\n",
                                  (unsigned long long)r.frames[b0 + i].seq, batch[i].hdr.time_us / 1000.0, draws.size(),
                                  fs.blk[0][1] >> 10, fs.blk[1][1] >> 10, fs.blk[3][1] >> 10, fs.bytes[0] >> 10, fs.bytes[1] >> 10,
                                  fs.bytes[2] >> 10, fs.bytes[3] >> 10, fs.bytes[4] >> 10,
                                  fs.cls[0].size() + fs.cls[1].size() + fs.cls[2].size() + fs.cls[3].size(), fs.cls[4].size(),
                                  (unsigned long long)fs.capped);
                    rows[i] = row;
                }
            });
        for (auto& t : pool) t.join();
        for (auto& s : rows) std::fputs(s.c_str(), out);
    }
    std::fclose(out);
    std::printf("%zu records -> %s\n", r.frames.size(), csv);
    return 0;
}

// Live: does asset memory change outside loads? Every submit, hash (64 KB chunks) the heap memory the frame's GPU work
// reads: buffer/index/table spans of class 4 and texture base levels. With the null GPU nothing on the GPU writes, so a
// changed chunk is a CPU write. Reports chunks that changed since they were last seen.
static int HeapWatch(u32 frames) {
    DWORD pid = FindPid(L"gow3.exe");
    if (!pid) return std::printf("gow3.exe not running\n"), 1;
    Process p;
    p.h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!p.h) return std::printf("OpenProcess failed %lu\n", GetLastError()), 1;
    for (auto& r : Regions(p.h))
        if (r.size == kDataSize) p.delta = r.base - kDataVa;
    if (!p.delta) return std::printf("EBOOT .data region not found\n"), 1;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto suspend = (NtProc)GetProcAddress(nt, "NtSuspendProcess");
    auto resume = (NtProc)GetProcAddress(nt, "NtResumeProcess");
    const u64 ctx = p.delta + kMainCtx;
    constexpr u64 kChunk = 64 << 10;
    std::unordered_map<u64, u64> seen;               // chunk -> hash
    std::map<u64, std::array<u64, 3>> changed;       // chunk -> {times changed, texture?, first frame}
    std::unordered_map<u64, std::vector<u8>> last;   // contents of chunks that changed, to locate the written bytes
    std::map<u64, std::array<u64, 3>> written;       // chunk -> {min offset, max offset + 1, bytes differing last time}
    std::map<u64, std::set<std::pair<u64, u64>>> covers;  // chunk -> GPU-read spans overlapping it (last frame seen)
    u64 h0 = p.Get<u64>(ctx + 0x40), watched_sum = 0, frames_with_change = 0;
    double ms_sum = 0;
    std::printf("pid %lu, watching %u frames\n", pid, frames);
    for (u32 f = 0; f < frames;) {
        u64 h1 = p.Get<u64>(ctx + 0x40);
        if (h1 == h0) continue;
        suspend(p.h);
        auto t0 = std::chrono::steady_clock::now();
        h1 = p.Get<u64>(ctx + 0x40);
        Image img;
        bool ok = ReadFrame(p, h0, h1, false, nullptr, img.blocks, img.meta);
        img.Index();
        FrameSpans fs = Spans(img);
        // Chunks to hash: heap spans and texture base levels; chunk -> is texture.
        std::unordered_map<u64, bool> want;
        for (auto [lo, hi] : fs.cls[4])
            for (u64 c = lo & ~(kChunk - 1); c < hi; c += kChunk) want.emplace(c, false);
        for (auto [lo, hi] : fs.tex)
            for (u64 c = lo & ~(kChunk - 1); c < hi; c += kChunk) want[c] = true;
        std::vector<std::pair<u64, bool>> list(want.begin(), want.end());
        std::vector<u64> hash(list.size());
        std::vector<size_t> idx(list.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t i) {
            thread_local std::vector<u8> buf(kChunk);
            p.ReadInto(list[i].first, buf.data(), kChunk);
            hash[i] = XXH3_64bits(buf.data(), kChunk);
        });
        std::unordered_map<u64, std::vector<u8>> now;  // current bytes of chunks already known to change
        for (auto& [a, c] : changed)
            if (want.count(a)) now[a] = p.ReadRange(a, kChunk);
        resume(p.h);
        u32 n_changed = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            auto [it, fresh] = seen.try_emplace(list[i].first, hash[i]);
            if (!fresh && it->second != hash[i]) {
                auto& c = changed[list[i].first];
                if (!c[0]) c[2] = f;
                ++c[0], c[1] = list[i].second, ++n_changed;
            }
            it->second = hash[i];
        }
        for (auto& [a, bytes] : now) {
            auto& old = last[a];
            if (old.size() == bytes.size()) {
                u64 lo = kChunk, hi = 0, n = 0;
                for (u64 o = 0; o < kChunk; ++o)
                    if (old[o] != bytes[o]) lo = std::min(lo, o), hi = o + 1, ++n;
                if (n) {
                    auto& w = written[a];
                    if (!w[1]) w[0] = lo;
                    w[0] = std::min(w[0], lo), w[1] = std::max(w[1], hi), w[2] = n;
                }
            }
            old = std::move(bytes);
            auto& cv = covers[a];
            cv.clear();
            for (auto [lo, hi] : fs.cls[4])
                if (lo < a + kChunk && hi > a) cv.insert({lo, hi - lo});
            for (auto [lo, hi] : fs.tex)
                if (lo < a + kChunk && hi > a) cv.insert({lo, hi - lo});
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms_sum += ms, watched_sum += list.size(), frames_with_change += n_changed != 0;
        if (f % 20 == 0 || n_changed)
            std::printf("frame %u%s: %zu chunks (%.0f MB) watched, %u changed, suspended %.0f ms\n", f, ok ? "" : " (chain?)",
                        list.size(), list.size() * 64 / 1024.0, n_changed, ms);
        h0 = h1, ++f;
    }
    std::printf("\n%u frames, avg %.0f MB watched/frame, avg suspension %.0f ms, %llu frames with changes, %zu distinct chunks changed\n",
                frames, watched_sum * 64.0 / 1024 / frames, ms_sum / frames, (unsigned long long)frames_with_change, changed.size());
    for (auto& [a, c] : changed) {
        auto w = written[a];
        std::printf("  0x%llx %s: changed %llu times, first at frame %llu; written range +0x%llx..+0x%llx, %llu bytes differ\n",
                    (unsigned long long)a, c[1] ? "texture" : "buffer", (unsigned long long)c[0], (unsigned long long)c[2],
                    (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2]);
        for (auto [lo, n] : covers[a]) std::printf("      read span 0x%llx size 0x%llx\n", (unsigned long long)lo, (unsigned long long)n);
    }
    return 0;
}

// One frame's CPU-written blocks and the spans the GPU reads from them, for the Vulkan transfer bench:
// "GOW3PAY1", u32 nblocks, per block {u32 class, u32 nspans, u64 bytes, bytes..., spans as u64 {offset, size}...}.
static int Payload(const char* cap, const char* out_path) {
    Image img;
    if (!Load(cap, img)) return std::printf("cannot load %s\n", cap), 1;
    FrameSpans fs = Spans(img);
    FILE* out = std::fopen(out_path, "wb");
    if (!out) return std::printf("cannot write %s\n", out_path), 1;
    std::fwrite("GOW3PAY1", 1, 8, out);
    u32 n = 0;
    for (int c = 0; c < 4; ++c) n += fs.blk[c][0] != 0;
    std::fwrite(&n, 4, 1, out);
    for (u32 c = 0; c < 4; ++c) {
        if (!fs.blk[c][0]) continue;
        const u8* p = img.Ptr(fs.blk[c][0], fs.blk[c][1]);
        u32 ns = (u32)fs.cls[c].size();
        u64 bytes = fs.blk[c][1];
        std::fwrite(&c, 4, 1, out), std::fwrite(&ns, 4, 1, out), std::fwrite(&bytes, 8, 1, out);
        std::fwrite(p, 1, bytes, out);
        for (auto [lo, hi] : fs.cls[c]) {
            u64 o = lo - fs.blk[c][0], sz = std::min(hi, fs.blk[c][0] + bytes) - lo;
            if (lo >= fs.blk[c][0] + bytes) o = sz = 0;  // read past the captured part: recorded as empty
            std::fwrite(&o, 8, 1, out), std::fwrite(&sz, 8, 1, out);
        }
        std::printf("class %u: block %llu KB, %u spans, %llu KB read\n", c, (unsigned long long)(bytes >> 10), ns,
                    (unsigned long long)(fs.bytes[c] >> 10));
    }
    std::printf("heap read %llu KB in %zu spans\n", (unsigned long long)(fs.bytes[4] >> 10), fs.cls[4].size());
    std::fclose(out);
    return 0;
}

static int Census(const char* path, const char* prefix) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    std::string pre = prefix;
    struct Life {
        u64 first, last, frames = 0;
        std::map<u64, u64> variants;  // content/shape -> frames
        u64 info = 0, target = 0;
    };
    std::map<u64, Life> progs, texs;
    std::unordered_set<u64> samplers, pipes, states, targets;
    std::map<std::string, std::array<u64, 4>> ops;  // total, frames, first seq, last seq
    FILE* ff = std::fopen((pre + ".frames.csv").c_str(), "w");
    if (!ff) return std::printf("cannot write %s.frames.csv\n", prefix), 1;
    std::fprintf(ff, "seq,frame,time_ms,draws,dispatches,progs,new_progs,new_isa_at_known_addr,texs,new_tex,new_shape_at_known_base,"
                     "new_pipes,new_samplers,new_states,vb_ring,vb_arena,vb_heap,cb_ring,cb_other,buf_arena,buf_heap\n");
    DescDecoder dec;
    std::vector<FrameDesc> batch;
    std::vector<CensusFrame> out;
    u64 submits = 0;
    for (size_t b0 = 0; b0 < r.frames.size(); b0 += batch.size()) {
        batch.resize(std::min<size_t>(256, r.frames.size() - b0));
        for (size_t i = 0; i < batch.size(); ++i)
            if (!dec.Next(r, b0 + i, batch[i])) return std::printf("frame record %zu unreadable\n", b0 + i), 1;
        out.assign(batch.size(), {});
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
            pool.emplace_back([&] {
                Image img;
                for (size_t i; (i = next++) < batch.size();) {
                    if (batch[i].hdr.flags) continue;  // no submit: nothing drawn
                    BuildImage(r, batch[i], img);
                    out[i].seq = r.frames[b0 + i].seq;
                    out[i].time_us = batch[i].hdr.time_us;
                    out[i].counter = batch[i].meta.frame_counter;
                    CensusOne(img, out[i]);
                }
            });
        for (auto& t : pool) t.join();
        for (size_t i = 0; i < batch.size(); ++i) {
            if (batch[i].hdr.flags) continue;
            auto& c = out[i];
            ++submits;
            u64 np = 0, nv = 0, nt = 0, ns = 0, npipe = 0, nsamp = 0, nst = 0;
            std::unordered_set<u64> seen_p, seen_t;
            for (auto& [a, h, info] : c.progs) {
                auto [it, fresh] = progs.try_emplace(a, Life{c.seq, c.seq});
                auto& l = it->second;
                np += fresh, nv += !fresh && !l.variants.count(h);
                l.last = c.seq, l.info = info;
                if (seen_p.insert(a).second) ++l.frames;
                ++l.variants[h];
            }
            for (auto& [a, shape, tgt] : c.texs) {
                auto [it, fresh] = texs.try_emplace(a, Life{c.seq, c.seq});
                auto& l = it->second;
                nt += fresh, ns += !fresh && !l.variants.count(shape);
                l.last = c.seq, l.target |= tgt;
                if (seen_t.insert(a).second) ++l.frames;
                ++l.variants[shape];
            }
            for (u64 v : c.pipes) npipe += pipes.insert(v).second;
            for (u64 v : c.samplers) nsamp += samplers.insert(v).second;
            for (u64 v : c.states) nst += states.insert(v).second;
            for (u64 v : c.targets) targets.insert(v);
            for (auto& [k, v] : c.ops) {
                auto [it, fresh] = ops.try_emplace(k, std::array<u64, 4>{0, 0, c.seq, c.seq});
                it->second[0] += v, ++it->second[1], it->second[3] = c.seq;
            }
            std::fprintf(ff, "%llu,%u,%.3f,%u,%u,%zu,%llu,%llu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                         (unsigned long long)c.seq, c.counter, c.time_us / 1000.0, c.draws, c.dispatches, seen_p.size(),
                         (unsigned long long)np, (unsigned long long)nv, seen_t.size(), (unsigned long long)nt, (unsigned long long)ns,
                         (unsigned long long)npipe, (unsigned long long)nsamp, (unsigned long long)nst, (unsigned long long)c.vb[0],
                         (unsigned long long)c.vb[1], (unsigned long long)c.vb[2], (unsigned long long)c.cb[0],
                         (unsigned long long)(c.cb[1] + c.cb[2]), (unsigned long long)c.buf[1], (unsigned long long)c.buf[2]);
        }
    }
    std::fclose(ff);
    auto lives = [&](const char* name, std::map<u64, Life>& m, bool is_prog) {
        FILE* f = std::fopen((pre + name).c_str(), "w");
        std::fprintf(f, is_prog ? "addr,stage,code_bytes,first_seq,last_seq,frames,variants\n" : "base,first_seq,last_seq,frames,shapes,target,shape_list\n");
        for (auto& [a, l] : m) {
            if (is_prog) {
                std::fprintf(f, "%llx,%s,%llu,%llu,%llu,%llu,%zu\n", (unsigned long long)a, (const char*[]){"VS", "PS", "CS", "FS"}[l.info & 3],
                             (unsigned long long)(l.info >> 8), (unsigned long long)l.first, (unsigned long long)l.last,
                             (unsigned long long)l.frames, l.variants.size());
            } else {
                std::fprintf(f, "%llx,%llu,%llu,%llu,%zu,%llu,", (unsigned long long)a, (unsigned long long)l.first, (unsigned long long)l.last,
                             (unsigned long long)l.frames, l.variants.size(), (unsigned long long)l.target);
                for (auto& [s, n] : l.variants)
                    std::fprintf(f, " %ux%u/f%u.%u/t%u/m%u:%llu", (u32)(s >> 10 & 0x3FFF), (u32)(s >> 24 & 0x3FFF), (u32)(s & 0x3F),
                                 (u32)(s >> 6 & 0xF), (u32)(s >> 38 & 0xF), (u32)(s >> 42 & 0x1F), (unsigned long long)n);
                std::fprintf(f, "\n");
            }
        }
        std::fclose(f);
    };
    lives(".programs.csv", progs, true);
    lives(".textures.csv", texs, false);
    FILE* fs = std::fopen((pre + ".summary.txt").c_str(), "w");
    u64 prog_multi = 0, tex_multi = 0, tex_targets = 0;
    for (auto& [a, l] : progs) prog_multi += l.variants.size() > 1;
    for (auto& [a, l] : texs) tex_multi += l.variants.size() > 1, tex_targets += l.target != 0;
    std::fprintf(fs, "%llu submitted frames of %zu records\n", (unsigned long long)submits, r.frames.size());
    std::fprintf(fs, "programs (incl. fetch shaders): %zu addresses, %llu with more than one ISA content\n", progs.size(), (unsigned long long)prog_multi);
    std::fprintf(fs, "textures: %zu bases, %llu with more than one shape, %llu ever a render/depth target\n", texs.size(),
                 (unsigned long long)tex_multi, (unsigned long long)tex_targets);
    std::fprintf(fs, "session distinct: samplers %zu, pipeline keys %zu, state combos %zu, RT/Z bases %zu\n", samplers.size(),
                 pipes.size(), states.size(), targets.size());
    std::fprintf(fs, "\nnon-draw packets (total, frames, first seq, last seq):\n");
    for (auto& [k, v] : ops)
        std::fprintf(fs, "  %-52s %10llu %7llu %7llu %7llu\n", k.c_str(), (unsigned long long)v[0], (unsigned long long)v[1],
                     (unsigned long long)v[2], (unsigned long long)v[3]);
    std::fclose(fs);
    std::printf("%llu frames -> %s.{frames.csv,programs.csv,textures.csv,summary.txt}\n", (unsigned long long)submits, prefix);
    return 0;
}

// Aliasing census: every surface the GPU writes (colour/depth/stencil targets, RW images and buffers) and every later
// read of that memory (texture T#, buffer V#, RW resource) in GPU order, across frames. A read with the writer's
// layout is plain render-to-texture; another format at the same layout needs a view; anything else needs a copy.
namespace AliasCensus {
struct Shape {
    u32 fmt, nfmt, pitch, h, tile, type;  // tile 0xFF = unknown (depth/stencil, buffers)
};
struct Ev {
    u8 write, kind;  // kinds: 0 colour, 1 depth, 2 stencil, 3 RW image, 4 RW buffer, 5 texture, 6 buffer
    u64 base, bytes, prog;
    Shape s;
};
struct Surf {
    u64 end, prog, seq;
    u8 kind;
    Shape s;
};
const char* const kKinds[] = {"colour", "depth", "stencil", "rw-image", "rw-buffer", "texture", "buffer"};
const u8 kBits[64] = {0, 8, 16, 16, 32, 32, 32, 32, 32, 32, 32, 64, 64, 96, 128, 0, 16, 16, 16, 16, 32, 32, 64, 0, 0,
                      0, 0,  0,  0,  0,  0,  0,  8,  8,  16, 4,  8,  8,  4,  8,  8,  8};

std::vector<Ev> FrameEvents(const Image& img) {
    std::vector<Ev> ev;
    std::vector<Draw> draws;
    std::map<u32, u32> ops;
    Replay(img, draws, ops);
    std::unordered_map<u64, ShaderInfo> shaders;
    auto tex_shape = [](const Tex& t) {
        return Shape{t.dfmt, t.nfmt, std::max<u32>(((t.raw[4] >> 13) & 0x3FFF) + 1, t.w), t.h, t.tile, t.type};
    };
    for (auto& d : draws) {
        u64 writer = d.ps ? d.ps : d.vs;
        for (auto [prog, user] : {std::pair{d.vs, d.vs_user}, {d.ps, d.compute ? d.vs_user : d.ps_user}}) {
            if (!prog) continue;
            auto it = shaders.find(prog);
            if (it == shaders.end()) it = shaders.emplace(prog, ParseShader(img, prog)).first;
            auto st = DecodeStage(img, it->second, user);
            for (auto& t : st.textures)
                if (t.base) ev.push_back({0, 5, t.base, TexBytes(t), prog, tex_shape(t)});
            for (auto [a, n] : st.spans)
                if (a && n) ev.push_back({0, 6, a, n, prog, {0, 0, 0, 0, 0xFF, 0}});
            for (auto& t : st.rw_textures)
                if (t.base) ev.push_back({1, 3, t.base, TexBytes(t), prog, tex_shape(t)});
            for (auto [a, n] : st.rw_bufs)
                if (a && n) ev.push_back({1, 4, a, n, prog, {0, 0, 0, 0, 0xFF, 0}});
        }
        if (d.compute) continue;
        for (u32 i = 0; i < 8; ++i) {
            const u32* cb = d.cb[i];
            u32 fmt = cb[4] >> 2 & 0x1F, pitch = ((cb[1] & 0x7FF) + 1) * 8;
            if (!fmt || !(d.cb_mask >> (i * 4) & 0xF) || !cb[0]) continue;
            u64 pixels = ((u64)(cb[2] & 0x3FFFFF) + 1) * 64, layers = ((cb[3] >> 13) & 0x7FF) - (cb[3] & 0x7FF) + 1;
            u64 bytes = pixels * (kBits[fmt] ? kBits[fmt] : 32) / 8 * (1u << (cb[5] >> 12 & 7)) * layers;
            u32 nfmt = cb[4] >> 8 & 7;  // CB NUMBER_TYPE; its SRGB (6) is the T# NUM_FORMAT 9
            ev.push_back({1, 0, (u64)cb[0] << 8, bytes, writer, {fmt, nfmt == 6 ? 9u : nfmt, pitch, u32(pixels / pitch), cb[5] & 0x1F, 0}});
        }
        u32 zfmt = d.db[0] & 3, pitch = ((d.db[6] & 0x7FF) + 1) * 8, h = ((d.db[6] >> 11 & 0x7FF) + 1) * 8;
        if (zfmt && (d.depth_control & 4) && d.db[4])
            ev.push_back({1, 1, (u64)d.db[4] << 8, (u64)pitch * h * (zfmt == 1 ? 2 : 4), writer, {zfmt == 1 ? 2u : 4u, zfmt == 1 ? 0u : 7u, pitch, h, 0xFF, 0}});
        if ((d.db[1] & 1) && (d.depth_control & 1) && d.db[5])
            ev.push_back({1, 2, (u64)d.db[5] << 8, (u64)pitch * h, writer, {1, 0, pitch, h, 0xFF, 0}});
    }
    return ev;
}

int Run(const char* path, const char* out_path) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    std::map<u64, Surf> live;  // GPU-written surfaces by base, non-overlapping
    struct Agg {
        u64 reads = 0, frames = 0, first = 0, last = 0, example = 0, last_frame = ~0ull;
    };
    std::map<std::string, Agg> found;
    u64 submits = 0, matched = 0, plain = 0;
    DescDecoder dec;
    std::vector<FrameDesc> batch;
    std::vector<std::vector<Ev>> evs;
    for (size_t b0 = 0; b0 < r.frames.size(); b0 += batch.size()) {
        batch.resize(std::min<size_t>(256, r.frames.size() - b0));
        for (size_t i = 0; i < batch.size(); ++i)
            if (!dec.Next(r, b0 + i, batch[i])) return std::printf("frame record %zu unreadable\n", b0 + i), 1;
        evs.assign(batch.size(), {});
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
            pool.emplace_back([&] {
                Image img;
                for (size_t i; (i = next++) < batch.size();)
                    if (!batch[i].hdr.flags) BuildImage(r, batch[i], img), evs[i] = FrameEvents(img);
            });
        for (auto& t : pool) t.join();
        for (size_t i = 0; i < batch.size(); ++i) {
            if (batch[i].hdr.flags) continue;
            ++submits;
            u64 seq = r.frames[b0 + i].seq;
            for (auto& e : evs[i]) {
                auto first = live.upper_bound(e.base);
                if (first != live.begin() && std::prev(first)->second.end > e.base) --first;
                auto last = first;
                while (last != live.end() && last->first < e.base + e.bytes) ++last;
                if (e.write) {
                    live.erase(first, last);
                    live[e.base] = {e.base + e.bytes, e.prog, seq, e.kind, e.s};
                    continue;
                }
                for (auto it = first; it != last; ++it) {
                    const Surf& w = it->second;
                    const u32 wb = w.kind == 4 ? 0 : kBits[w.s.fmt], rb = e.kind == 6 ? 0 : kBits[e.s.fmt];
                    const bool same_base = it->first == e.base;
                    // Target heights are the padded allocation: a read of the top rows keeps the layout.
                    const bool layout = same_base && wb == rb && w.s.pitch == e.s.pitch && e.s.h <= w.s.h &&
                                        (w.s.tile == e.s.tile || w.s.tile == 0xFF || e.s.tile == 0xFF);
                    ++matched;
                    if (layout && w.s.fmt == e.s.fmt && w.s.nfmt == e.s.nfmt) { ++plain; continue; }
                    const char* rel = e.kind == 6 || w.kind == 4 ? "raw-bytes" : layout ? "view" : same_base ? "copy" : "copy-subrange";
                    char key[384];
                    std::snprintf(key, sizeof(key),
                                  "%-13s %-9s f%u.%u %ux%u t%u -> %-7s f%u.%u %ux%u t%u ty%u | %s | off %s | reader %llx writer %llx",
                                  rel, kKinds[w.kind], w.s.fmt, w.s.nfmt, w.s.pitch, w.s.h, w.s.tile, kKinds[e.kind], e.s.fmt, e.s.nfmt,
                                  e.s.pitch, e.s.h, e.s.tile, e.s.type, w.seq == seq ? "same frame" : "older frame",
                                  same_base ? "0" : "inside", (unsigned long long)e.prog, (unsigned long long)w.prog);
                    auto& a = found[key];
                    if (!a.reads) a.first = seq, a.example = e.base;
                    ++a.reads, a.last = seq;
                    if (a.last_frame != seq) ++a.frames, a.last_frame = seq;
                }
            }
        }
        std::printf("\r%zu / %zu records", b0 + batch.size(), r.frames.size());
    }
    std::vector<std::pair<std::string, Agg>> rows(found.begin(), found.end());
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second.frames > y.second.frames; });
    FILE* f = std::fopen(out_path, "w");
    if (!f) return std::printf("\ncannot write %s\n", out_path), 1;
    std::fprintf(f, "%llu submitted frames; reads of GPU-written memory: %llu, plain render-to-texture %llu, aliased %llu (%zu cases)\n",
                 (unsigned long long)submits, (unsigned long long)matched, (unsigned long long)plain,
                 (unsigned long long)(matched - plain), rows.size());
    std::map<std::string, std::array<u64, 2>> by_rel;  // "relation writer -> reader kind" -> cases, reads
    for (auto& [k, a] : rows) {
        std::istringstream in(k);
        std::string rel, wk, wf, wd, wt, arrow, rk;
        in >> rel >> wk >> wf >> wd >> wt >> arrow >> rk;
        auto& c = by_rel[rel + " " + wk + " -> " + rk];
        ++c[0], c[1] += a.reads;
    }
    for (auto& [k, c] : by_rel)
        std::fprintf(f, "  %-40s cases %6llu reads %10llu\n", k.c_str(), (unsigned long long)c[0], (unsigned long long)c[1]);
    std::fprintf(f, "frames reads first_seq last_seq example_base | relation writer -> reader\n");
    for (auto& [k, a] : rows)
        std::fprintf(f, "%6llu %8llu %6llu %6llu %10llx | %s\n", (unsigned long long)a.frames, (unsigned long long)a.reads,
                     (unsigned long long)a.first, (unsigned long long)a.last, (unsigned long long)a.example, k.c_str());
    std::fclose(f);
    std::printf("\n%llu frames, %zu aliasing cases -> %s\n", (unsigned long long)submits, rows.size(), out_path);
    return 0;
}
} // namespace AliasCensus

// ISA census over every distinct program a recording uses (by content): which GCN encodings/opcodes occur and how
// much control flow there is. Sizes a minimal translator.
namespace IsaStat {
// (encoding, opcode) of a CI instruction and its size in dwords.
std::pair<std::pair<const char*, u32>, int> Decode(const u32* w) {
    const u32 x = w[0];
    const int lit = (x & 0x1FF) == 0xFF;
    if (x >> 23 == 0x17D) return {{"sop1", x >> 8 & 0xFF}, 1 + ((x & 0xFF) == 0xFF)};
    if (x >> 23 == 0x17E) return {{"sopc", x >> 16 & 0x7F}, 1 + ((x & 0xFF) == 0xFF || (x >> 8 & 0xFF) == 0xFF)};
    if (x >> 23 == 0x17F) return {{"sopp", x >> 16 & 0x7F}, 1};
    if (x >> 28 == 0xB) return {{"sopk", x >> 23 & 0x1F}, 1 + ((x >> 23 & 0x1F) == 0x15)};
    if (x >> 30 == 2) return {{"sop2", x >> 23 & 0x7F}, 1 + ((x & 0xFF) == 0xFF || (x >> 8 & 0xFF) == 0xFF)};
    if (x >> 27 == 0x18) return {{"smrd", x >> 22 & 0x1F}, 1 + ((x & 0x1FF) == 0xFF)};
    if (x >> 25 == 0x3F) return {{"vop1", x >> 9 & 0xFF}, 1 + lit};
    if (x >> 25 == 0x3E) return {{"vopc", x >> 17 & 0xFF}, 1 + lit};
    if (x >> 31 == 0) {
        const u32 op = x >> 25 & 0x3F;
        return {{"vop2", op}, 1 + (lit || op == 0x20 || op == 0x21)};  // v_madmk/madak carry a literal
    }
    switch (x >> 26) {
    case 0x34: return {{"vop3", x >> 17 & 0x1FF}, 2};
    case 0x32: return {{"vintrp", x >> 16 & 3}, 1};
    case 0x36: return {{"ds", x >> 18 & 0xFF}, 2};
    case 0x38: return {{"mubuf", x >> 18 & 0x7F}, 2};
    case 0x3A: return {{"mtbuf", x >> 16 & 7}, 2};
    case 0x3C: return {{"mimg", x >> 18 & 0x7F}, 2};
    case 0x3E: return {{"exp", 0}, 2};
    }
    return {{"unknown", x >> 26}, 1};
}

int Run(const char* path, const char* out_path) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    std::mutex m;
    std::unordered_map<u64, std::pair<u32, std::vector<u32>>> progs;  // ISA xxh3 -> stage, code
    DescDecoder dec;
    std::vector<FrameDesc> batch;
    for (size_t b0 = 0; b0 < r.frames.size(); b0 += batch.size()) {
        batch.resize(std::min<size_t>(256, r.frames.size() - b0));
        for (size_t i = 0; i < batch.size(); ++i)
            if (!dec.Next(r, b0 + i, batch[i])) return std::printf("frame record %zu unreadable\n", b0 + i), 1;
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
            pool.emplace_back([&] {
                Image img;
                for (size_t i; (i = next++) < batch.size();) {
                    if (batch[i].hdr.flags) continue;
                    BuildImage(r, batch[i], img);
                    std::vector<Draw> draws;
                    std::map<u32, u32> ops;
                    Replay(img, draws, ops);
                    std::unordered_set<u64> seen;
                    for (auto& d : draws)
                        for (auto [a, stage] : {std::pair{d.vs, 0u}, {d.ps, d.compute ? 2u : 1u}}) {
                            if (!a || !seen.insert(a).second) continue;
                            auto si = ParseShader(img, a);
                            const u8* p = si.ok ? img.Ptr(a, si.code_bytes) : nullptr;
                            if (!p) continue;
                            const u64 h = XXH3_64bits(p, si.code_bytes);
                            std::scoped_lock l(m);
                            if (!progs.count(h)) progs[h] = {stage, std::vector<u32>((const u32*)p, (const u32*)(p + si.code_bytes))};
                        }
                }
            });
        for (auto& t : pool) t.join();
        std::printf("\r%zu / %zu records, %zu programs", b0 + batch.size(), r.frames.size(), progs.size());
    }
    std::map<std::pair<std::string, u32>, std::array<u64, 2>> opc;  // (enc, op) -> instances, programs
    u64 n_inst = 0, with_branch = 0, with_loop = 0, with_exec = 0, unknown = 0, branches = 0, max_branches = 0;
    std::map<u32, u64> stages;
    for (auto& [h, sp] : progs) {
        auto& [stage, code] = sp;
        ++stages[stage];
        std::set<std::pair<std::string, u32>> here;
        u64 br = 0;
        bool loop = false, exec = false;
        for (size_t i = 0; i < code.size();) {
            auto [k, n] = Decode(&code[i]);
            ++n_inst, ++opc[{k.first, k.second}][0], here.insert({k.first, k.second});
            unknown += !std::strcmp(k.first, "unknown");
            const bool sopp = !std::strcmp(k.first, "sopp");
            if (sopp && (k.second == 2 || (k.second >= 4 && k.second <= 9))) {
                ++br;
                loop |= (int16_t)(code[i] & 0xFFFF) < 0;
            }
            // *_saveexec (sop1 0x24..0x2B on CI) and writes to EXEC (sdst 126) by scalar ops
            if (!std::strcmp(k.first, "sop1") && k.second >= 0x24 && k.second <= 0x2B) exec = true;
            if ((!std::strcmp(k.first, "sop1") || !std::strcmp(k.first, "sop2")) && (code[i] >> 16 & 0x7F) == 126) exec = true;
            i += n;
            if (sopp && k.second == 1) break;  // s_endpgm
        }
        for (auto& k : here) ++opc[k][1];
        branches += br, max_branches = std::max(max_branches, br);
        with_branch += br > 0, with_loop += loop, with_exec += exec;
    }
    FILE* o = std::fopen(out_path, "w");
    if (!o) return std::printf("\ncannot write %s\n", out_path), 1;
    std::fprintf(o, "distinct programs by content: %zu (VS %llu, PS %llu, CS %llu); instructions %llu; unknown encodings %llu\n",
                 progs.size(), (unsigned long long)stages[0], (unsigned long long)stages[1], (unsigned long long)stages[2],
                 (unsigned long long)n_inst, (unsigned long long)unknown);
    std::fprintf(o, "control flow: programs with branches %llu, with backward branches %llu, writing EXEC %llu; branches total %llu, max %llu\n",
                 (unsigned long long)with_branch, (unsigned long long)with_loop, (unsigned long long)with_exec,
                 (unsigned long long)branches, (unsigned long long)max_branches);
    std::map<std::string, u32> per_enc;
    for (auto& [k, v] : opc) ++per_enc[k.first];
    std::fprintf(o, "distinct opcodes %zu:", opc.size());
    for (auto& [e, n] : per_enc) std::fprintf(o, " %s %u", e.c_str(), n);
    std::fprintf(o, "\n\nenc op instances programs\n");
    std::vector<std::pair<std::pair<std::string, u32>, std::array<u64, 2>>> rows(opc.begin(), opc.end());
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second[1] > y.second[1]; });
    for (auto& [k, v] : rows) std::fprintf(o, "%-7s %3u %9llu %6llu\n", k.first.c_str(), k.second, (unsigned long long)v[0], (unsigned long long)v[1]);
    std::fclose(o);
    std::printf("\n%zu programs -> %s\n", progs.size(), out_path);
    return 0;
}
} // namespace IsaStat

// Shadow trace (runtime GOW3_SHADOW=1) analysis: loading timeline and texture address reuse across level sections.
namespace ShadowScan {
struct Record {
    u64 time_ns;
    u32 kind, x;
    u64 a, b;
    u32 data[8];
};
enum Kind : u32 { View = 1, Rebase, Release, GpuMove, SectionOpen, WadOpen, Tick };

int Run(const char* path, const char* out_path) {
    FILE* f = std::fopen(path, "rb");
    char magic[8];
    if (!f || std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "GOW3SHD1", 8)) return std::printf("cannot load %s\n", path), 1;
    std::vector<Record> rs;
    for (Record r; std::fread(&r, sizeof(r), 1, f) == 1;) rs.push_back(r);
    std::fclose(f);
    std::stable_sort(rs.begin(), rs.end(), [](auto& x, auto& y) { return x.time_ns < y.time_ns; });
    FILE* o = std::fopen(out_path, "w");
    if (!o) return std::printf("cannot write %s\n", out_path), 1;
    auto name = [](const Record& r) { return std::string((const char*)r.data, strnlen((const char*)r.data, 32)); };
    auto secs = [](u64 ns) { return ns / 1e9; };
    // Timeline: loading intervals from the section flag, fps from the engine frame counter, names as they open.
    std::fprintf(o, "== timeline (s)\n");
    u64 last_tick_t = 0, last_frames = 0, load_start = 0;
    u32 loading = 0;
    std::vector<std::pair<u64, std::string>> sections;  // start time, name
    u64 views = 0, rebases = 0, releases = 0, moves = 0, move_bytes = 0;
    for (auto& r : rs) {
        switch (r.kind) {
        case Tick:
            if (r.x != loading) {
                if (r.x) load_start = r.time_ns;
                else std::fprintf(o, "%9.2f  loading ended after %.2f s\n", secs(r.time_ns), secs(r.time_ns - load_start));
                if (r.x) std::fprintf(o, "%9.2f  loading started\n", secs(r.time_ns));
                loading = r.x;
            }
            if (last_tick_t && r.time_ns / 10'000'000'000 != last_tick_t / 10'000'000'000)
                std::fprintf(o, "%9.2f  fps %.0f\n", secs(r.time_ns), (r.a - last_frames) / secs(r.time_ns - last_tick_t));
            if (!last_tick_t || r.time_ns / 10'000'000'000 != last_tick_t / 10'000'000'000) last_tick_t = r.time_ns, last_frames = r.a;
            break;
        case SectionOpen:
            sections.push_back({r.time_ns, name(r)});
            std::fprintf(o, "%9.2f  section %s (%u, %llu)\n", secs(r.time_ns), name(r).c_str(), r.x, (unsigned long long)r.a);
            break;
        case WadOpen: std::fprintf(o, "%9.2f  wad %s\n", secs(r.time_ns), name(r).c_str()); break;
        case View: ++views; break;
        case Rebase: ++rebases; break;
        case Release: ++releases; break;
        case GpuMove: ++moves, move_bytes += r.x; break;
        }
    }
    // Address reuse: per T# base, the shapes it carried and in which sections.
    struct Use {
        std::map<u64, std::set<size_t>> shapes;  // shape -> sections
    };
    std::map<u64, Use> bases;
    size_t sec = 0;
    for (auto& r : rs) {
        while (sec < sections.size() && sections[sec].first <= r.time_ns) ++sec;
        if (r.kind != View) continue;
        Tex t = DecodeT(r.data);
        if (!t.base) continue;
        u64 shape = (u64)t.dfmt | (u64)t.nfmt << 6 | (u64)t.w << 10 | (u64)t.h << 24 | (u64)t.type << 38 | (u64)t.tile << 42;
        bases[t.base].shapes[shape].insert(sec);
    }
    u64 multi_sec = 0, multi_shape = 0, cross = 0;
    for (auto& [b, u] : bases) {
        std::set<size_t> all;
        for (auto& [s, ss] : u.shapes) all.insert(ss.begin(), ss.end());
        multi_sec += all.size() > 1, multi_shape += u.shapes.size() > 1;
        // Another shape at the same base in a different section: the image must be recreated, not reused.
        if (u.shapes.size() > 1) {
            std::set<size_t> first = u.shapes.begin()->second;
            for (auto it = std::next(u.shapes.begin()); it != u.shapes.end(); ++it)
                for (size_t s : it->second) cross += !first.count(s);
        }
    }
    std::fprintf(o, "\n== totals\nrecords %zu, views %llu, rebases %llu, releases %llu, gpu moves %llu (%.1f MB)\n", rs.size(),
                 (unsigned long long)views, (unsigned long long)rebases, (unsigned long long)releases, (unsigned long long)moves, move_bytes / 1048576.0);
    std::fprintf(o, "texture bases %zu: in more than one section %llu, with more than one shape %llu, shape changes across sections %llu\n",
                 bases.size(), (unsigned long long)multi_sec, (unsigned long long)multi_shape, (unsigned long long)cross);
    // A new shape at a base is safe for the image table only if the old texture's block was released or moved by
    // compaction in between (rebase: new base = old base - sign-extended delta).
    std::map<u64, std::vector<std::pair<u64, u64>>> timeline;  // base -> (time, shape or 0 = released/moved)
    for (auto& r : rs) {
        if (r.kind == View) {
            Tex t = DecodeT(r.data);
            if (t.base) timeline[t.base].push_back({r.time_ns, (u64)t.dfmt | (u64)t.nfmt << 6 | (u64)t.w << 10 | (u64)t.h << 24 | (u64)t.type << 38 | (u64)t.tile << 42});
        } else if (r.kind == Release && r.x <= 1 && r.b) {
            timeline[r.b].push_back({r.time_ns, 0});
        } else if (r.kind == Rebase && r.b) {
            timeline[r.b].push_back({r.time_ns, 0});
            timeline[r.b - (u64)(int64_t)(int32_t)r.x].push_back({r.time_ns, 0});
        }
    }
    u64 safe = 0, unexplained = 0;
    for (auto& [b, v] : timeline) {
        std::stable_sort(v.begin(), v.end());
        u64 shape = 0;
        bool freed = false;
        for (auto [t, s] : v) {
            if (!s) { freed = true; continue; }
            if (shape && s != shape) {
                freed ? ++safe : ++unexplained;
                if (!freed) std::fprintf(o, "unexplained shape change at %llx, %.2f s\n", (unsigned long long)b, t / 1e9);
            }
            shape = s, freed = false;
        }
    }
    std::fprintf(o, "shape changes at a base: after release/move %llu, without %llu\n", (unsigned long long)safe, (unsigned long long)unexplained);
    std::fprintf(o, "\n== bases with more than one shape (base: shape@sections)\n");
    for (auto& [b, u] : bases) {
        if (u.shapes.size() < 2) continue;
        std::fprintf(o, "%llx:", (unsigned long long)b);
        for (auto& [s, ss] : u.shapes) {
            std::fprintf(o, " %ux%u/f%u.%u/t%u@", (u32)(s >> 10 & 0x3FFF), (u32)(s >> 24 & 0x3FFF), (u32)(s & 0x3F), (u32)(s >> 6 & 0xF), (u32)(s >> 42 & 0x1F));
            for (size_t x : ss) std::fprintf(o, "%zu,", x);
        }
        std::fprintf(o, "\n");
    }
    std::fclose(o);
    std::printf("%zu records, %zu sections, %zu texture bases -> %s\n", rs.size(), sections.size(), bases.size(), out_path);
    return 0;
}
} // namespace ShadowScan

// Plan phase 1: the draw shadow (GOW3_DRAWLOG=1 -> _rehooked_/draws-*.bin) against the GPU-order replay of a
// recording of the same run. Frames pair by the submit's chain heads, draws by packet address. Per draw it checks the
// programs read from the ctx, the user data each ctx applied from its own stream (only registers the stage meta
// references count) and the fixed state fields of the ctx against the registers the GPU would see.
namespace DrawCheck {
struct Record {
    u64 pkt, ctx;
    u32 kind, tid, dirty, resync;
    u64 isa[2], pgm[2];
    u32 user[2][16];
    u32 set[2], used[2];
    u32 field[8];
    u32 reg[12], reg_set;
    u32 pad[7];
};
static_assert(sizeof(Record) == 320);
enum Kind : u32 { DrawIndex = 1, DrawAuto, Dispatch, Submit, Lost };
const char* kRegNames[12]{"CB_TARGET_MASK", "CB_SHADER_MASK", "DB_DEPTH_CONTROL", "CB_COLOR_CONTROL", "CB_BLEND_CONTROL?203",
                          "PA_SU_SC_MODE_CNTL", "CB_BLEND0_CONTROL", "DB_STENCIL_CONTROL", "DB_STENCILREFMASK",
                          "DB_STENCILREFMASK_BF", "CB_COLOR0_BASE", "DB_Z_READ_BASE"};
// ctx field -> context register it should equal at the draw (index into kShadowRegs).
constexpr std::pair<u32, u32> kFieldRegs[]{{0, 6}, {1, 5}, {2, 2}, {3, 8}, {4, 9}, {5, 7}};
const char* kFieldNames[]{"+0x48 blend", "+0x4C raster", "+0x54 depth", "+0x58 stencil ref", "+0x5C stencil ref bf", "+0x60 stencil"};

// User data registers a program reads, from its OrbShdr slots (what the GPU program really uses).
u32 UsedRegs(const ShaderInfo& si) {
    u32 m = 0;
    for (auto& s : si.slots) {
        if (s.reg >= 16) continue;
        const u32 n = s.type == 0 || s.type == 4 ? (s.flags & 1 ? 8 : 4) : s.type <= 3 ? 4 : s.type >= 0x12 && s.type <= 0x1B ? 2 : 1;
        m |= ((1u << n) - 1) << s.reg;
    }
    return m & 0xFFFF;
}

struct Stats {
    u64 frames = 0, oracle = 0, matched = 0, by_op_unmatched[256]{}, dirty_nonzero = 0, resync[4]{};
    u64 isa_ctx_diff[2][2]{}, pgm_local_diff[2]{}, pgm_local_unset[2]{}, meta_used_diff[2]{};  // [dirty != 0][stage]
    u64 ud_used = 0, ud_eq = 0, ud_diff = 0, ud_inherited = 0, ud_inherited_eq0 = 0, cs_set = 0, cs_diff = 0;
    u64 reg_eq[2][12]{}, reg_diff[2][12]{}, reg_unset[2][12]{}, reg_unset_differs[2][12]{}, field_eq[2][6]{}, field_diff[2][6]{};  // [job ctx]
    std::map<u32, u64> inherited_by_reg, diff_by_reg;  // dirty != 0 << 16 | stage << 8 | reg
    std::vector<std::string> examples, cs_examples;
    void Add(const Stats& o) {
        frames += o.frames, oracle += o.oracle, matched += o.matched, dirty_nonzero += o.dirty_nonzero;
        for (int i = 0; i < 256; ++i) by_op_unmatched[i] += o.by_op_unmatched[i];
        for (int i = 0; i < 4; ++i) resync[i] += o.resync[i];
        for (int i = 0; i < 2; ++i) {
            pgm_local_diff[i] += o.pgm_local_diff[i], pgm_local_unset[i] += o.pgm_local_unset[i], meta_used_diff[i] += o.meta_used_diff[i];
            for (int s = 0; s < 2; ++s) isa_ctx_diff[i][s] += o.isa_ctx_diff[i][s];
            for (int k = 0; k < 6; ++k) field_eq[i][k] += o.field_eq[i][k], field_diff[i][k] += o.field_diff[i][k];
        }
        ud_used += o.ud_used, ud_eq += o.ud_eq, ud_diff += o.ud_diff, ud_inherited += o.ud_inherited, ud_inherited_eq0 += o.ud_inherited_eq0;
        cs_set += o.cs_set, cs_diff += o.cs_diff;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 12; ++i)
                reg_eq[c][i] += o.reg_eq[c][i], reg_diff[c][i] += o.reg_diff[c][i], reg_unset[c][i] += o.reg_unset[c][i],
                    reg_unset_differs[c][i] += o.reg_unset_differs[c][i];
        for (auto [k, v] : o.inherited_by_reg) inherited_by_reg[k] += v;
        for (auto [k, v] : o.diff_by_reg) diff_by_reg[k] += v;
        for (auto& e : o.examples)
            if (examples.size() < 60) examples.push_back(e);
        for (auto& e : o.cs_examples)
            if (cs_examples.size() < 20) cs_examples.push_back(e);
    }
    void Example(const char* fmt, auto... args) { Push(examples, 60, fmt, args...); }
    static void Push(std::vector<std::string>& v, size_t cap, const char* fmt, auto... args) {
        if (v.size() >= cap) return;
        char s[256];
        std::snprintf(s, sizeof(s), fmt, args...);
        v.push_back(s);
    }
};

void Compare(const Record& r, const Draw& d, const u32 (&used)[2], bool job, u64 head, u64 seq, Stats& st) {
    ++st.matched;
    const int dirty = r.dirty != 0;
    st.dirty_nonzero += dirty;
    for (int b = 0; b < 2; ++b) st.resync[b + 1] += r.resync >> b & 1;
    if (d.compute) {
        if (r.pgm[1] != d.ps) ++st.pgm_local_diff[1];
        for (u32 k = 0; k < 16; ++k)
            if (r.set[1] >> k & 1 && used[0] >> k & 1) {
                ++st.cs_set;
                if (r.user[1][k] == d.vs_user[k]) continue;
                ++st.cs_diff;
                Stats::Push(st.cs_examples, 20, "seq %llu dispatch %u user[%u] local %08x gpu %08x (set %04x used %04x dirty %x tid %u ctx %llx cs %llx)",
                            (unsigned long long)seq, d.index, k, r.user[1][k], d.vs_user[k], r.set[1], used[0], r.dirty, r.tid,
                            (unsigned long long)r.ctx, (unsigned long long)d.ps);
            }
        return;
    }
    const u64 oracle_prog[2]{d.ps, d.vs};
    const u32* oracle_user[2]{d.ps_user, d.vs_user};
    for (int s = 0; s < 2; ++s) {
        if (r.isa[s] != oracle_prog[s]) {
            ++st.isa_ctx_diff[dirty][s];
            if (!dirty)
                st.Example("seq %llu draw %u %s ISA ctx %llx gpu %llx (tid %u)", (unsigned long long)seq, d.index, s ? "VS" : "PS",
                           (unsigned long long)r.isa[s], (unsigned long long)oracle_prog[s], r.tid);
        }
        if (r.pgm[s] != oracle_prog[s]) r.pgm[s] ? ++st.pgm_local_diff[s] : ++st.pgm_local_unset[s];
        st.meta_used_diff[s] += !dirty && r.used[s] != used[s];
        for (u32 k = 0; k < 16; ++k) {
            if (!(used[s] >> k & 1)) continue;
            ++st.ud_used;
            if (r.set[s] >> k & 1) {
                if (r.user[s][k] == oracle_user[s][k]) ++st.ud_eq;
                else {
                    ++st.ud_diff, ++st.diff_by_reg[dirty << 16 | s << 8 | k];
                    st.Example("seq %llu draw %u %s user[%u] local %08x gpu %08x (dirty %x tid %u ctx %llx)", (unsigned long long)seq, d.index,
                               s ? "VS" : "PS", k, r.user[s][k], oracle_user[s][k], r.dirty, r.tid, (unsigned long long)r.ctx);
                }
            } else {
                ++st.ud_inherited, st.ud_inherited_eq0 += oracle_user[s][k] == 0;
                ++st.inherited_by_reg[dirty << 16 | s << 8 | k];
            }
        }
    }
    // Only state the draw depends on: stencil with STENCIL_ENABLE, blend 0 with blending on RT0.
    const bool stencil = d.reg[2] & 1, blend = ((d.reg[6] | r.reg[6]) >> 30 & 1) && (d.reg[0] & 0xF);
    for (int i = 0; i < 12; ++i) {
        const u32 reg = kShadowRegs[i];
        if (((reg == 0x10b || reg == 0x10c || reg == 0x10d) && !stencil) || (reg == 0x1e0 && !blend)) continue;
        if (!(r.reg_set >> i & 1)) ++st.reg_unset[job][i];
        else if (r.reg[i] == d.reg[i]) ++st.reg_eq[job][i];
        else {
            ++st.reg_diff[job][i];
            if (st.reg_diff[job][i] <= 2)
                st.Example("seq %llu draw %u op %02x %s ctx %03x %s local %08x gpu %08x (dirty %x tid %u, packet at head+%llx)", (unsigned long long)seq,
                           d.index, d.op, job ? "job" : "main", reg, kRegNames[i], r.reg[i], d.reg[i], r.dirty, r.tid,
                           (unsigned long long)(r.pkt - head));
        }
    }
    for (int i = 0; i < 6; ++i) {
        const auto [f, g] = kFieldRegs[i];
        if (r.field[f] == d.reg[g]) ++st.field_eq[dirty][i];
        else {
            ++st.field_diff[dirty][i];
            if (!dirty && st.field_diff[0][i] <= 3)
                st.Example("seq %llu draw %u ctx %s %08x gpu %s %08x (dirty %x)", (unsigned long long)seq, d.index, kFieldNames[i], r.field[f],
                           kRegNames[g], d.reg[g], r.dirty);
        }
    }
}

int Run(const char* rec_path, const char* log_path, const char* out_path) {
    FILE* f = std::fopen(log_path, "rb");
    char magic[8];
    if (!f || std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "GOW3DRW1", 8)) return std::printf("cannot load %s\n", log_path), 1;
    std::vector<Record> rs;
    for (Record r; std::fread(&r, sizeof(r), 1, f) == 1;) rs.push_back(r);
    std::fclose(f);
    std::vector<size_t> markers;
    std::unordered_map<u64, std::vector<u32>> by_pkt;
    u64 lost = 0, kinds[6]{};
    for (size_t i = 0; i < rs.size(); ++i) {
        ++kinds[std::min<u32>(rs[i].kind, 5)];
        if (rs[i].kind == Submit) markers.push_back(i);
        else if (rs[i].kind == Lost) lost = rs[i].pkt;
        else by_pkt[rs[i].pkt].push_back((u32)i);
    }
    std::printf("draw log: %zu records (%llu indexed, %llu auto, %llu dispatch, %zu submits), %llu lost\n", rs.size(),
                (unsigned long long)kinds[DrawIndex], (unsigned long long)kinds[DrawAuto], (unsigned long long)kinds[Dispatch],
                markers.size(), (unsigned long long)lost);
    RecFile r;
    if (!r.Open(rec_path)) return std::printf("cannot load %s\n", rec_path), 1;
    // Pair frames with submits in order: same heads, the next marker after the last paired one.
    DescDecoder dec;
    struct Job { FrameDesc d; u64 seq; size_t lo, hi; };
    std::vector<Job> jobs;
    size_t m = 0, unpaired = 0;
    for (size_t i = 0; i < r.frames.size(); ++i) {
        Job j;
        if (!dec.Next(r, i, j.d)) return std::printf("frame record %zu unreadable\n", i), 1;
        if (j.d.hdr.flags & 1) continue;
        size_t k = m;
        while (k < markers.size() && !(rs[markers[k]].ctx == j.d.meta.new_head && rs[markers[k]].pkt == j.d.meta.old_head)) ++k;
        if (k == markers.size()) { ++unpaired; continue; }
        j.seq = r.frames[i].seq, j.hi = markers[k], j.lo = k >= 2 ? markers[k - 2] : 0;
        m = k + 1;
        jobs.push_back(std::move(j));
    }
    std::printf("recording: %zu frames, %zu paired with a submit, %zu not paired\n", r.frames.size(), jobs.size(), unpaired);
    std::vector<Stats> per(jobs.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::thread::hardware_concurrency(); ++t)
        pool.emplace_back([&] {
            Image img;
            for (size_t i; (i = next++) < jobs.size();) {
                auto& j = jobs[i];
                std::vector<Draw> draws;
                std::map<u32, u32> ops;
                // The GPU keeps registers across frames: warm the replay with the previous frame's end state.
                std::vector<u32> carry;
                if (i) BuildImage(r, jobs[i - 1].d, img), Replay(img, draws, ops, &carry), draws.clear();
                BuildImage(r, j.d, img);
                Replay(img, draws, ops, &carry);
                Stats& st = per[i];
                ++st.frames;
                std::unordered_map<u64, u32> used_of;
                auto used = [&](u64 prog) {
                    auto it = used_of.find(prog);
                    return it != used_of.end() ? it->second : used_of[prog] = UsedRegs(ParseShader(img, prog));
                };
                for (auto& d : draws) {
                    ++st.oracle;
                    const Record* best = nullptr;
                    if (auto it = by_pkt.find(d.pkt); it != by_pkt.end())
                        for (u32 idx : it->second)
                            if (idx > j.lo && idx < j.hi) best = &rs[idx];
                    if (!best) { ++st.by_op_unmatched[d.op]; continue; }
                    const u32 u[2]{d.ps ? used(d.ps) : 0u, d.vs ? used(d.vs) : 0u};
                    Compare(*best, d, u, best->ctx != 0x02A78B58 + j.d.meta.delta, j.d.meta.old_head, j.seq, st);
                }
            }
        });
    for (auto& t : pool) t.join();
    Stats all;
    for (auto& s : per) all.Add(s);
    FILE* o = std::fopen(out_path, "w");
    if (!o) return std::printf("cannot write %s\n", out_path), 1;
    auto pct = [](u64 a, u64 b) { return b ? 100.0 * a / b : 0.0; };
    std::fprintf(o, "frames %llu, oracle draws+dispatches %llu, matched %llu (%.3f%%), log lost %llu, unpaired frames %zu\n",
                 (unsigned long long)all.frames, (unsigned long long)all.oracle, (unsigned long long)all.matched, pct(all.matched, all.oracle),
                 (unsigned long long)lost, unpaired);
    std::fprintf(o, "unmatched by packet op:");
    for (int i = 0; i < 256; ++i)
        if (all.by_op_unmatched[i]) std::fprintf(o, " %02x=%llu", i, (unsigned long long)all.by_op_unmatched[i]);
    std::fprintf(o, "\ndirty != 0 at the draw: %llu; stream resync %llu, packet not found %llu\n", (unsigned long long)all.dirty_nonzero,
                 (unsigned long long)all.resync[1], (unsigned long long)all.resync[2]);
    std::fprintf(o, "ISA from ctx program objects != GPU: dirty == 0: PS %llu, VS %llu; dirty != 0: PS %llu, VS %llu\n",
                 (unsigned long long)all.isa_ctx_diff[0][0], (unsigned long long)all.isa_ctx_diff[0][1], (unsigned long long)all.isa_ctx_diff[1][0],
                 (unsigned long long)all.isa_ctx_diff[1][1]);
    std::fprintf(o, "stage meta used mask != program slots (dirty == 0): PS %llu, VS %llu\n", (unsigned long long)all.meta_used_diff[0],
                 (unsigned long long)all.meta_used_diff[1]);
    std::fprintf(o, "PGM from the ctx stream != GPU: PS %llu, VS/CS %llu; not written by the ctx: PS %llu, VS %llu\n",
                 (unsigned long long)all.pgm_local_diff[0], (unsigned long long)all.pgm_local_diff[1], (unsigned long long)all.pgm_local_unset[0],
                 (unsigned long long)all.pgm_local_unset[1]);
    std::fprintf(o, "user data referenced by the meta: %llu; written by the ctx and equal %llu, different %llu; inherited %llu (%llu of them zero)\n",
                 (unsigned long long)all.ud_used, (unsigned long long)all.ud_eq, (unsigned long long)all.ud_diff, (unsigned long long)all.ud_inherited,
                 (unsigned long long)all.ud_inherited_eq0);
    std::fprintf(o, "compute user data used by the program and written by the ctx %llu, different %llu\n", (unsigned long long)all.cs_set,
                 (unsigned long long)all.cs_diff);
    for (auto& e : all.cs_examples) std::fprintf(o, "  %s\n", e.c_str());
    for (auto [title, map] : {std::pair{"inherited", &all.inherited_by_reg}, {"different", &all.diff_by_reg}}) {
        std::fprintf(o, "%s user data by register (d = draw with dirty != 0):", title);
        for (auto [k, v] : *map) std::fprintf(o, " %s%s%u=%llu", k >> 16 ? "d" : "", k >> 8 & 1 ? "VS" : "PS", k & 0xff, (unsigned long long)v);
        std::fprintf(o, "\n");
    }
    std::fprintf(o, "\ncontext register from the ctx stream, only where the draw depends on it\n"
                    "(main ctx: equal / different / not written; job ctx: equal / different / not written):\n");
    for (int i = 0; i < 12; ++i)
        std::fprintf(o, "  %03x %-22s %9llu %9llu %9llu   %9llu %9llu %9llu\n", kShadowRegs[i], kRegNames[i], (unsigned long long)all.reg_eq[0][i],
                     (unsigned long long)all.reg_diff[0][i], (unsigned long long)all.reg_unset[0][i], (unsigned long long)all.reg_eq[1][i],
                     (unsigned long long)all.reg_diff[1][i], (unsigned long long)all.reg_unset[1][i]);
    std::fprintf(o, "\nctx field vs GPU register (dirty == 0: equal / different; dirty != 0: equal / different):\n");
    for (int i = 0; i < 6; ++i)
        std::fprintf(o, "  %-22s %-20s %10llu %10llu %10llu %10llu\n", kFieldNames[i], kRegNames[kFieldRegs[i].second],
                     (unsigned long long)all.field_eq[0][i], (unsigned long long)all.field_diff[0][i], (unsigned long long)all.field_eq[1][i],
                     (unsigned long long)all.field_diff[1][i]);
    std::fprintf(o, "\nexamples:\n");
    for (auto& e : all.examples) std::fprintf(o, "  %s\n", e.c_str());
    std::fclose(o);
    std::printf("matched %llu of %llu -> %s\n", (unsigned long long)all.matched, (unsigned long long)all.oracle, out_path);
    return 0;
}
} // namespace DrawCheck

// Phase 2: every distinct (program, draw state) of a recording through the GCN translator, glslang and the SPIR-V validator.
namespace Xlat {
struct Item {
    Gcn::Input in;  // spans set from code/fetch when translated
    std::vector<u32> code, fetch;
    std::vector<Gcn::SlotSpec> spec;
    u64 prog = 0;
    u32 draws = 0;
};
struct Result {
    bool ok = false;
    std::string error, detail, glsl;
    double ms[3]{};  // translate, glslang (+ spirv-opt), validate
    size_t spv = 0;
};

// Descriptor dwords at `p`: user data, then 64-bit pointers loaded from guest memory.
static const u32* Resolve(const Image& img, const u32* user, const Gcn::Path& p, u32 n) {
    if (p.reg + (p.depth ? 2u : n) > 16) return nullptr;
    const u32* at = user + p.reg;
    for (u32 d = 0; d < p.depth && at; ++d)
        at = (const u32*)img.Ptr(((u64)(at[1] & 0xFFFF) << 32 | at[0]) + p.offset[d], d + 1 < p.depth ? 8 : n * 4);
    return at;
}

// Fetch shader a VS calls through s_swappc_b64, its address in user data.
static u64 FetchAddr(const std::vector<u32>& code, const u32* user) {
    for (size_t i = 0; i < code.size() && code[i] != 0xBF810000; i += IsaStat::Decode(&code[i]).second)
        if (code[i] >> 23 == 0x17D && (code[i] >> 8 & 0xFF) == 33) {
            const u32 s = code[i] & 0xFF;
            return s < 15 ? (u64)(user[s + 1] & 0xFFFF) << 32 | user[s] : 0;
        }
    return 0;
}

static void WriteFileAll(const std::string& path, const std::string& s) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) std::fwrite(s.data(), 1, s.size(), f), std::fclose(f);
}

// Error kind without the program counter, to group failures.
static std::string Kind(std::string e) {
    if (auto p = e.find(" at 0x"); p != std::string::npos) e.resize(p);
    if (e.rfind("gcn:", 0) == 0)
        if (auto p = e.find(": ", 4); p != std::string::npos) e = e.substr(p + 2);
    return e.substr(0, e.find('\n'));
}

int Run(const char* path, const char* out_dir, int mode) {  // spirv-opt: 0 none, 1 light, 2 performance
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    CreateDirectoryA(out_dir, nullptr);
    std::vector<FrameDesc> descs(r.frames.size());
    DescDecoder dec;
    for (size_t i = 0; i < descs.size(); ++i)
        if (!dec.Next(r, i, descs[i])) return std::printf("frame record %zu unreadable\n", i), 1;

    const unsigned nt = std::thread::hardware_concurrency();
    std::vector<std::unordered_map<u64, Item>> local(nt);
    std::vector<std::map<std::string, u64>> skipped(nt);
    std::atomic<size_t> next{0}, done{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nt; ++t)
        pool.emplace_back([&, t] {
            Image img;
            auto& items = local[t];
            auto& skip = skipped[t];
            std::unordered_map<u64, std::vector<Gcn::Slot>> slots_of;  // analysis key -> slots
            for (size_t i; (i = next++) < descs.size(); ++done) {
                if (descs[i].hdr.flags) continue;
                std::vector<Draw> draws;
                std::map<u32, u32> ops;
                std::vector<u32> carry;  // the GPU keeps registers across frames
                if (i && !descs[i - 1].hdr.flags) BuildImage(r, descs[i - 1], img), Replay(img, draws, ops, &carry), draws.clear();
                BuildImage(r, descs[i], img);
                Replay(img, draws, ops, &carry);
                std::unordered_map<u64, std::pair<u64, std::vector<u32>>> progs;  // address -> content hash, code
                auto stage_item = [&](const Draw& d, Gcn::Stage st, u64 addr, const u32* user, const u32* rsrc) {
                    auto pit = progs.find(addr);
                    if (pit == progs.end()) {
                        auto si = ParseShader(img, addr);
                        const u8* p = si.ok ? img.Ptr(addr, si.code_bytes) : nullptr;
                        if (!p) return void(++skip["program unreadable"]);
                        pit = progs.emplace(addr, std::pair{XXH3_64bits(p, si.code_bytes), std::vector<u32>((const u32*)p, (const u32*)(p + si.code_bytes))}).first;
                    }
                    const auto& [hash, code] = pit->second;
                    Item it;
                    it.in.stage = st, it.in.rsrc1 = rsrc[0], it.in.rsrc2 = rsrc[1];
                    if (st == Gcn::Stage::Vertex)
                        if (u64 fa = FetchAddr(code, user))
                            if (u32 n = FetchBytes(img, fa)) it.fetch.assign((const u32*)img.Ptr(fa, n), (const u32*)img.Ptr(fa, n) + n / 4);
                    if (st == Gcn::Stage::Pixel) {
                        it.in.ps_input_ena = d.ps_ena, it.in.ps_input_addr = d.ps_addr, it.in.col_format = d.col_format;
                        std::memcpy(it.in.ps_input_cntl, d.ps_cntl, sizeof(d.ps_cntl));
                        for (u32 k = 0; k < 8; ++k) {
                            const u32 nt_ = d.cb[k][4] >> 8 & 7;
                            it.in.mrt_class[k] = d.col_format >> (4 * k) & 0xF ? nt_ == 4 ? 1 : nt_ == 5 ? 2 : 0 : 0;
                        }
                    }
                    if (st == Gcn::Stage::Compute) std::copy_n(d.threads, 3, it.in.threads);
                    const u64 fhash = it.fetch.empty() ? 0 : XXH3_64bits(it.fetch.data(), it.fetch.size() * 4);
                    const u64 akey = Mix(Mix(Mix(hash, fhash), it.in.rsrc2), (u64)st);
                    auto sit = slots_of.find(akey);
                    if (sit == slots_of.end()) {
                        it.in.code = code, it.in.fetch = it.fetch;
                        sit = slots_of.emplace(akey, Gcn::Analyze(it.in).slots).first;
                    }
                    for (auto& s : sit->second) {
                        const u32* desc = Resolve(img, user, s.path, 4);
                        if (!desc) return void(++skip["descriptor unreadable"]);
                        it.spec.push_back(s.kind == Gcn::SlotKind::Sampler ? Gcn::SamplerSpec(desc) : Gcn::TextureSpec(desc));
                    }
                    it.in.code = {}, it.in.fetch = {};
                    std::vector<u32> kw{it.in.rsrc1, it.in.ps_input_ena, it.in.ps_input_addr, it.in.col_format, it.in.threads[0], it.in.threads[1], it.in.threads[2]};
                    kw.insert(kw.end(), it.in.ps_input_cntl, it.in.ps_input_cntl + 32);
                    kw.resize(kw.size() + 2), std::memcpy(&kw[kw.size() - 2], it.in.mrt_class, 8);
                    const u64 key = XXH3_64bits_withSeed(kw.data(), kw.size() * 4, Mix(akey, XXH3_64bits(it.spec.data(), it.spec.size() * sizeof(Gcn::SlotSpec))));
                    auto [e, fresh] = items.try_emplace(key);
                    if (fresh) it.code = code, it.prog = hash, e->second = std::move(it);
                    ++e->second.draws;
                };
                for (auto& d : draws) {
                    if (d.compute) {
                        if (d.ps) stage_item(d, Gcn::Stage::Compute, d.ps, d.vs_user, d.rsrc[0]);
                        continue;
                    }
                    if (d.vs) stage_item(d, Gcn::Stage::Vertex, d.vs, d.vs_user, d.rsrc[0]);
                    if (d.ps) stage_item(d, Gcn::Stage::Pixel, d.ps, d.ps_user, d.rsrc[1]);
                }
                if (done % 64 == 0) std::printf("\r%zu / %zu frames", done.load(), descs.size());
            }
        });
    for (auto& t : pool) t.join();
    std::unordered_map<u64, Item> items;
    std::map<std::string, u64> skip;
    for (unsigned t = 0; t < nt; ++t) {
        for (auto& [k, v] : local[t]) {
            auto [e, fresh] = items.try_emplace(k);
            if (fresh) e->second = std::move(v);
            else e->second.draws += v.draws;
        }
        for (auto& [k, v] : skipped[t]) skip[k] += v;
    }
    std::printf("\r%zu frames: %zu distinct (program, state) items\n", descs.size(), items.size());

    std::vector<Item*> list;
    for (auto& [k, v] : items) list.push_back(&v);
    std::vector<Result> res(list.size());
    next = 0, pool.clear();
    for (unsigned t = 0; t < nt; ++t)
        pool.emplace_back([&] {
            shaderc_compiler_t cc = shaderc_compiler_initialize();
            shaderc_compile_options_t opt = shaderc_compile_options_initialize();
            shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
            shaderc_compile_options_set_target_spirv(opt, shaderc_spirv_version_1_6);
            shaderc_compile_options_set_optimization_level(opt, mode == 2 ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);
            spv_context vc = spvContextCreate(SPV_ENV_VULKAN_1_3);
            // Light: SSA plus dead code only; the driver compiler does the rest.
            spv_optimizer_t* light = spvOptimizerCreate(SPV_ENV_VULKAN_1_3);
            // A pass that reports (inliner warnings) calls the consumer; left unset it throws bad_function_call.
            spvOptimizerSetMessageConsumer(light, [](spv_message_level_t, const char*, const spv_position_t*, const char*) {});
            const char* flags[]{"--inline-entry-points-exhaustive", "--eliminate-local-single-block", "--eliminate-local-single-store", "--ssa-rewrite",
                                "--eliminate-dead-code-aggressive"};
            spvOptimizerRegisterPassesFromFlags(light, flags, std::size(flags));
            spv_optimizer_options lopt = spvOptimizerOptionsCreate();
            spvOptimizerOptionsSetRunValidator(lopt, false);
            for (size_t i; (i = next++) < list.size();) {
                Item& it = *list[i];
                Result& o = res[i];
                it.in.code = it.code, it.in.fetch = it.fetch;
                auto t0 = std::chrono::steady_clock::now();
                auto lap = [&](int k) {
                    const auto t = std::chrono::steady_clock::now();
                    o.ms[k] = std::chrono::duration<double, std::milli>(t - t0).count(), t0 = t;
                };
                std::string err;
                std::vector<u32> words;
                std::uint64_t mask;
                const bool compiled = Gcn::EmitSpirv(Gcn::Analyze(it.in), it.in, it.spec, words, mask, err);
                lap(0);
                if (!compiled) {
                    o.error = "translate: " + Kind(err), o.detail = err;
                    continue;
                }
                if (mode == 1) {
                    spv_binary out = nullptr;
                    if (spvOptimizerRun(light, words.data(), words.size(), &out, lopt) != SPV_SUCCESS || !out) o.error = "spirv-opt light failed";
                    else words.assign(out->code, out->code + out->wordCount);
                    spvBinaryDestroy(out);
                }
                lap(1);
                if (compiled && o.error.empty()) {
                    spv_const_binary_t bin{words.data(), words.size()};
                    spv_diagnostic dg = nullptr;
                    if (spvValidate(vc, &bin, &dg) != SPV_SUCCESS) o.detail = dg ? dg->error : "?", o.error = "spirv-val: " + Kind(o.detail);
                    else o.ok = true;
                    spvDiagnosticDestroy(dg);
                    o.spv = words.size() * 4;
                }
                lap(2);
            }
            spvOptimizerOptionsDestroy(lopt);
            spvOptimizerDestroy(light);
            spvContextDestroy(vc);
            shaderc_compile_options_release(opt);
            shaderc_compiler_release(cc);
        });
    for (auto& t : pool) t.join();

    // Report: totals per stage, programs whose every item compiled, failures grouped by kind with one dumped example each.
    static constexpr const char* stage_name[]{"vs", "ps", "cs"};
    u64 ok[3]{}, total[3]{}, draws_ok = 0, draws_all = 0;
    std::map<std::pair<int, u64>, bool> prog_ok;
    std::map<std::string, std::pair<u64, size_t>> fails;  // kind -> items, example
    double ms[3]{}, ms_max[3]{};
    size_t spv = 0, glsl = 0, dispatch = 0, slowest = 0;
    bool sample[3]{};
    for (size_t i = 0; i < list.size(); ++i) {
        const int s = (int)list[i]->in.stage;
        ++total[s], draws_all += list[i]->draws;
        for (int k = 0; k < 3; ++k) ms[k] += res[i].ms[k], ms_max[k] = std::max(ms_max[k], res[i].ms[k]);
        if (res[i].ms[1] == ms_max[1]) slowest = i;
        auto [p, fresh] = prog_ok.try_emplace({s, list[i]->prog}, true);
        p->second = p->second && res[i].ok;
        dispatch += res[i].glsl.find("switch (pc_)") != std::string::npos;
        if (res[i].ok) {
            ++ok[s], draws_ok += list[i]->draws, spv += res[i].spv, glsl += res[i].glsl.size();
            if (!sample[s]) sample[s] = true, WriteFileAll(std::format("{}\\ok-{}.glsl", out_dir, stage_name[s]), res[i].glsl);
            continue;
        }
        auto [f, first] = fails.try_emplace(res[i].error, std::pair<u64, size_t>{0, i});
        ++f->second.first;
    }
    FILE* o = std::fopen(std::format("{}\\report.txt", out_dir).c_str(), "w");
    if (!o) return std::printf("cannot write report in %s\n", out_dir), 1;
    u64 n_ok = ok[0] + ok[1] + ok[2];
    std::fprintf(o, "recording %s: %zu frames, %zu items (VS %llu, PS %llu, CS %llu)\n", path, descs.size(), list.size(), (unsigned long long)total[0],
                 (unsigned long long)total[1], (unsigned long long)total[2]);
    std::fprintf(o, "compiled and valid: %llu / %zu items (VS %llu, PS %llu, CS %llu); draw stages covered %.4f%%\n", (unsigned long long)n_ok, list.size(),
                 (unsigned long long)ok[0], (unsigned long long)ok[1], (unsigned long long)ok[2], draws_all ? 100.0 * draws_ok / draws_all : 0.0);
    u64 progs_ok = 0;
    for (auto& [k, v] : prog_ok) progs_ok += v;
    std::fprintf(o, "programs with every item valid: %llu / %zu\n", (unsigned long long)progs_ok, prog_ok.size());
    const char* step[]{"translate", mode == 2 ? "glslang + spirv-opt -O" : mode == 1 ? "glslang + light opt" : "glslang (no opt)", "validate"};
    for (int k = 0; k < 3; ++k)
        std::fprintf(o, "%-20s total %8.0f ms (%u threads), mean %7.2f ms, max %8.1f ms\n", step[k], ms[k], nt, list.empty() ? 0.0 : ms[k] / list.size(), ms_max[k]);
    std::fprintf(o, "mean GLSL %zu B, SPIR-V %zu B; items with unstructured branches (pc dispatch loop) %zu\n", n_ok ? glsl / n_ok : 0, n_ok ? spv / n_ok : 0,
                 dispatch);
    if (!list.empty()) WriteFileAll(std::format("{}\\slowest-{}.glsl", out_dir, stage_name[(int)list[slowest]->in.stage]), res[slowest].glsl);
    for (auto& [k, v] : skip) std::fprintf(o, "skipped draw stages, %s: %llu\n", k.c_str(), (unsigned long long)v);
    std::fprintf(o, "\nfailures by kind (items, example file)\n");
    u32 n = 0;
    for (auto& [kind, v] : fails) {
        const auto file = std::format("fail-{:03}.glsl", n++);
        const Result& x = res[v.second];
        WriteFileAll(std::format("{}\\{}", out_dir, file), std::format("/* {}\n{}\n*/\n{}", x.error, x.detail, x.glsl));
        std::fprintf(o, "%6llu  %s  %s\n", (unsigned long long)v.first, file.c_str(), kind.c_str());
    }
    std::fclose(o);
    std::printf("valid %llu / %zu items, %zu failure kinds -> %s\\report.txt\n", (unsigned long long)n_ok, list.size(), fails.size(), out_dir);
    return 0;
}
} // namespace Xlat

// Writes recorded frame <seq> as a normal .cap for analyze/find.
static int Extract(const char* path, u64 seq, const char* out_path) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    if (seq >= r.frames.size()) return std::printf("only %zu frames\n", r.frames.size()), 1;
    DescDecoder dec;
    FrameDesc d;
    for (size_t i = 0; i <= seq; ++i)
        if (!dec.Next(r, i, d)) return std::printf("frame record %zu unreadable\n", i), 1;
    Image img;
    BuildImage(r, d, img);
    FILE* f = std::fopen(out_path, "wb");
    if (!f) return std::printf("cannot write %s\n", out_path), 1;
    std::fwrite("GOW3CAP1", 1, 8, f);
    for (auto& b : img.blocks) {
        u64 hdr[3] = {b.tag, b.addr, b.bytes.size()};
        std::fwrite(hdr, 8, 3, f);
        std::fwrite(b.bytes.data(), 1, b.bytes.size(), f);
    }
    std::fclose(f);
    std::printf("seq %llu: frame %u%s, t %.3f ms -> %s\n", (unsigned long long)seq, d.meta.frame_counter,
                d.hdr.flags ? " (no submit)" : "", d.hdr.time_us / 1000.0, out_path);
    return 0;
}

// ---------------------------------------------------------------- offline Vulkan replay (plan phases 3-5)

namespace ReplayVk {
// PNG with stored (uncompressed) deflate blocks: no zlib dependency, files are only for inspection.
static void WritePng(const std::string& path, u32 w, u32 h, const std::vector<u8>& rgba) {
    static u32 crc_table[256];
    if (!crc_table[1])
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    auto be = [](std::vector<u8>& v, u32 x) { v.insert(v.end(), {(u8)(x >> 24), (u8)(x >> 16), (u8)(x >> 8), (u8)x}); };
    std::vector<u8> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const std::vector<u8>& data) {
        be(out, (u32)data.size());
        const size_t at = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        u32 c = ~0u;
        for (size_t i = at; i < out.size(); ++i) c = crc_table[(c ^ out[i]) & 0xFF] ^ (c >> 8);
        be(out, ~c);
    };
    std::vector<u8> ihdr;
    be(ihdr, w), be(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    std::vector<u8> raw;
    raw.reserve((size_t)h * (w * 4 + 1));
    for (u32 y = 0; y < h; ++y) raw.push_back(0), raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4);
    std::vector<u8> z{0x78, 0x01};
    for (size_t o = 0; o < raw.size() || o == 0;) {
        const u16 n = (u16)std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n == raw.size()), z.insert(z.end(), {(u8)n, (u8)(n >> 8), (u8)~n, (u8)(~n >> 8)});
        z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
        if ((o += n) == raw.size()) break;
    }
    u32 a = 1, b = 0;
    for (u8 c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    be(z, b << 16 | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    if (FILE* f = std::fopen(path.c_str(), "wb")) std::fwrite(out.data(), 1, out.size(), f), std::fclose(f);
}

// Guest memory of recorded frame `seq` at `addr`: dwords as hex and float.
static int Peek(const char* path, u64 seq, u64 addr, u64 bytes) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    DescDecoder dec;
    FrameDesc d;
    for (u64 i = 0; i <= seq; ++i)
        if (!dec.Next(r, i, d)) return std::printf("frame record %llu unreadable\n", (unsigned long long)i), 1;
    Image img;
    BuildImage(r, d, img);
    const u32* p = (const u32*)img.Ptr(addr, bytes);
    if (!p) return std::printf("%llx+%llx not in frame %llu\n", (unsigned long long)addr, (unsigned long long)bytes, (unsigned long long)seq), 1;
    for (u64 o = 0; o < bytes / 4; o += 4) {
        std::printf("%llx:", (unsigned long long)(addr + o * 4));
        for (u64 k = o; k < std::min(o + 4, bytes / 4); ++k) std::printf(" %08x", p[k]);
        for (u64 k = o; k < std::min(o + 4, bytes / 4); ++k) std::printf(" %g", std::bit_cast<float>(p[k]));
        std::printf("\n");
    }
    return 0;
}

// Runs recorded frames [from, to] (every `step`-th) through Gpu::Renderer, then writes every render target as PNG.
static int Run(const char* path, u64 from, u64 to, const char* out_dir, bool validation, u64 stop, u64 step, bool perf, u64 repeat, bool live) {
    RecFile r;
    if (!r.Open(path)) return std::printf("cannot load %s\n", path), 1;
    to = std::min<u64>(to, r.frames.size() - 1);
    CreateDirectoryA(out_dir, nullptr);
    Gpu::Renderer gpu;
    std::string err;
    if (!gpu.Init(validation, err)) return std::printf("vulkan: %s\n", err.c_str()), 1;
    if (const char* a = std::getenv("GOW3_ANISO")) gpu.SetAnisotropy(unsigned(std::atoi(a)));  // the menu's texture filtering (live: 4/8/16)
    // --live: the live game's callbacks, so their costs show offline. The tracker scans page states lock-free and locks
    // only for new pages (as GuestMemory::Track); the frame's GPU writes are copied back into host memory.
    static const std::unique_ptr<std::atomic<u8>[]> known(new std::atomic<u8>[1ull << 23]{});
    static std::mutex known_lock;
    static std::vector<u8> sink;
    static u64 rays = 0;  // fxGodRays' query blocks (gpu_thread's kGodRays)
    if (live) {
        gpu.SetTracker([](u64 lo, u64 hi) {
            static const bool temp_lock = std::getenv("GOW3_TEMP_TRACKLOCK");  // TEMP perf: the old global lock + set
            static std::unordered_set<u64> temp_set;
            if (temp_lock) {
                std::scoped_lock lock(known_lock);
                if (!temp_set.insert(lo * 31 + hi).second) return false;
            }
            u64 p = lo >> 12;
            const u64 e = std::min((hi + 0xFFF) >> 12, 1ull << 23);
            while (p < e && known[p].load(std::memory_order_acquire)) ++p;
            if (p >= e) return false;
            std::scoped_lock lock(known_lock);
            for (; p < e; ++p) known[p].store(1, std::memory_order_release);
            return false;
        });
        gpu.SetReadback([](u64 a, const void* data, u64 size) {
            if (a - rays < 0x2000) std::printf("query block %llx: %llu samples\n", (unsigned long long)a, (unsigned long long)((const u64*)data)[1]);
            if (sink.size() < size) sink.resize(size);
            return Gpu::PieceHash::Copy(sink.data(), data, size);
        });
    }
    // A range runs in batch: every frame is traced, its lines are folded into classes (summary.txt, frames.csv) and dropped.
    // --perf: untraced, as the live game outside an F9 dump (no per-draw probes, no report classes).
    const bool batch = from != to && !perf;
    gpu.GetStats().trace = !perf;
    gpu.GetStats().stop = stop;
    DescDecoder dec;
    FrameDesc d;
    Image img;
    double ms_build = 0, ms_submit = 0;
    const auto t_start = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count(); };
    struct Class {
        u64 n = 0, frames = 0, last = ~0ull;
        std::string first;  // seq:draw
    };
    std::map<std::string, Class> classes;
    std::map<std::string, std::pair<u64, u64>> vs_clip;  // VS -> (draws all clipped, draws rasterized)
    std::string csv = "seq,draws,skipped,all_clipped,notes\n";
    u64 skipped_before = 0;
    auto add = [&](const std::string& key, u64 seq, size_t draw) {
        Class& c = classes[key];
        ++c.n;
        if (c.last != seq) ++c.frames, c.last = seq;
        if (c.first.empty()) c.first = std::format("{}:{}", seq, draw);
    };
    auto norm = [](std::string_view t) {  // addresses -> '#', so one class per cause
        std::string o;
        for (size_t k = 0; k < t.size();) {
            size_t e = k;
            while (e < t.size() && std::isxdigit((u8)t[e])) ++e;
            if (e - k >= 5) o += '#', k = e;
            else o += t[k++];
        }
        return o;
    };
    auto fold = [&](u64 seq) {
        auto& s = gpu.GetStats();
        u64 all_clipped = 0, notes = 0;
        for (size_t k = 0; k < s.trace_lines.size(); ++k) {
            const std::string& l = s.trace_lines[k];
            for (size_t p = l.find(" !"); p != std::string::npos; p = l.find(" !", p + 2)) {
                const size_t e = std::min(l.find(" !", p + 2), l.find(" |", p + 2));
                add("note: " + norm(l.substr(p + 2, e == std::string::npos ? std::string::npos : e - p - 2)), seq, k), ++notes;
            }
            for (size_t p = l.find(" | V"); p != std::string::npos; p = l.find(" | V", p + 4)) {
                const std::string seg = l.substr(p + 3, l.find(" |", p + 3) - p - 3);
                for (const char* st : {" unreadable", " zero", " not-mirrored"})
                    if (seg.ends_with(st) && seg.find(" 0 ") == std::string::npos) add(std::format("V# {} ({})", st + 1, seg.substr(0, 2)), seq, k);
                if (seg.find(" BAD ") != std::string::npos) {  // TEMP: non-geometry floats, per program and V# register
                    const size_t v = l.find(" vs ");
                    add(std::format("V# BAD floats ({}) vs {} ps {}", seg.substr(0, seg.find(' ', 4)), v == std::string::npos ? "?" : l.substr(v + 4, l.find(' ', v + 4) - v - 4),
                                    l.find(" ps ") == std::string::npos ? "?" : l.substr(l.find(" ps ") + 4, l.find(' ', l.find(" ps ") + 4) - l.find(" ps ") - 4)),
                        seq, k);
                }
            }
            if (const size_t b = l.find(" BOOM "); b != std::string::npos) {  // TEMP: exploded vertices, per program
                const size_t v = l.find(" vs "), q = l.find(" ps ");
                add(std::format("VS output exploded vs {} ps {}", v == std::string::npos ? "?" : l.substr(v + 4, l.find(' ', v + 4) - v - 4),
                                q == std::string::npos ? "?" : l.substr(q + 4, l.find(' ', q + 4) - q - 4)),
                    seq, k);
                std::printf("BOOM seq %llu draw %zu:%s\n", (unsigned long long)seq, k, l.substr(b).c_str());
            }
            if (const size_t c = l.find("|| prims "); c != std::string::npos) {
                u64 prims = 0, vsn = 0, cin = 0, cout = 0, fs = 0, px = 0;
                std::sscanf(l.c_str() + c, "|| prims %llu vs %llu clip %llu/%llu fs %llu px %llu", &prims, &vsn, &cin, &cout, &fs, &px);
                const size_t v = l.find(" vs ");
                const std::string vs = v == std::string::npos ? "?" : l.substr(v + 4, l.find(' ', v + 4) - v - 4);
                if (cin && !cout) add("all primitives clipped", seq, k), ++all_clipped, ++vs_clip[vs].first;
                else if (cout) ++vs_clip[vs].second;
                if (fs && !px) add("fragments, no sample passes depth/stencil", seq, k);
            }
        }
        csv += std::format("{},{},{},{},{}\n", seq, s.trace_lines.size(), s.skipped - skipped_before, all_clipped, notes);
        skipped_before = s.skipped;
        s.trace_lines.clear();
    };
    // --repeat N: the range runs N times (steady-state measurement: shaders and images already exist after the first);
    // a single frame is decoded once and submitted N times.
    const u64 reps = from == to ? 1 : repeat, again = from == to ? repeat : 1;
    for (u64 rep = 0; rep < reps; ++rep, dec = DescDecoder{})
    for (u64 i = 0; i <= to; ++i) {
        if (!dec.Next(r, i, d)) return std::printf("frame record %llu unreadable\n", (unsigned long long)i), 1;
        if (i < from || d.hdr.flags || (i - from) % step) continue;
        if (!batch) std::printf("[%.1f s] frame %llu decoded\n", since(), (unsigned long long)i);
        BuildImage(r, d, img);
        const Block* chain = img.Find(TagChain);
        if (!chain) continue;
        for (u64 k = 0; k < again; ++k) {
        auto t0 = std::chrono::steady_clock::now();
        for (auto& b : img.blocks)
            if (b.addr && (b.tag == TagCmd || b.tag == TagRing || b.tag == TagShader || b.tag == TagData || b.tag == TagArena || b.tag == TagHeap))
                gpu.Write(b.addr, b.bytes.data(), b.bytes.size());
        std::vector<Gpu::Segment> segs;
        for (auto run = (const ::Run*)chain->bytes.data(); run < (const ::Run*)(chain->bytes.data() + chain->bytes.size()); ++run)
            if (auto p = (const u32*)img.Ptr(run->begin, run->end - run->begin)) segs.push_back({p, (size_t)(run->end - run->begin) / 4});
        if (const u64* p = live ? (const u64*)img.Ptr(d.meta.delta + 0x0146A8E8, 8) : nullptr; p && *p) gpu.SetQueries(rays = *p, *p + 0x2000);
        if (const float* dt = (const float*)img.Ptr(d.meta.delta + 0x013cdca0, 4)) gpu.SetMotionBlurFrameTime(*dt);
        auto t1 = std::chrono::steady_clock::now();
        gpu.Submit(segs, [&](u64 a, u64 n) -> const void* { return img.Ptr(a, n); });
        auto t2 = std::chrono::steady_clock::now();
        ms_build += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms_submit += std::chrono::duration<double, std::milli>(t2 - t1).count();
        const auto& s = gpu.GetStats();
        std::printf("seq %llu: %zu segments, draws %llu skipped %llu shaders %llu (fail %llu) targets %llu textures %llu uploaded %llu MB, %.1f ms (record %.2f gpu %.2f)"
                    " GPU xfer %.3f copy %.3f render %.3f readback %.3f barriers %llu+%llu walk %.3f blocks %.3f\n",  // TEMP perf: the retired frame's GPU timestamps
                    (unsigned long long)i, segs.size(), (unsigned long long)s.draws, (unsigned long long)s.skipped,
                    (unsigned long long)s.shaders, (unsigned long long)s.shader_fail, (unsigned long long)s.targets, (unsigned long long)s.textures,
                    (unsigned long long)(s.uploaded >> 20), std::chrono::duration<double, std::milli>(t2 - t1).count(), s.record_ms, s.gpu_ms,
                    s.temp_ms[8], s.temp_ms[9], s.temp_ms[10], s.temp_ms[11], (unsigned long long)s.temp_barriers[0], (unsigned long long)s.temp_barriers[1],
                    s.temp_ms[1], s.temp_ms[2]);
        if (batch) fold(i);
        }
    }
    if (batch) {
        std::vector<std::pair<std::string, Class>> v(classes.begin(), classes.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.n > b.second.n; });
        std::string sum = std::format("frames {}..{} step {}\n{:>8} {:>6}  {:<12} class\n", from, to, step, "count", "frames", "first");
        for (auto& [k, c] : v) sum += std::format("{:>8} {:>6}  {:<12} {}\n", c.n, c.frames, c.first, k);
        sum += "VS never rasterized in the range (all draws clipped):\n";
        for (auto& [vs, c] : vs_clip)
            if (c.first && !c.second) sum += std::format("  {} x{}\n", vs, c.first);
        std::printf("%s", sum.c_str());
        Xlat::WriteFileAll(std::string(out_dir) + "\\summary.txt", sum);
        Xlat::WriteFileAll(std::string(out_dir) + "\\frames.csv", csv);
    }
    std::string report;
    const auto& s = gpu.GetStats();
    report += std::format("frames {}..{}: draws {} dispatches {} skipped {} shaders {} (fail {}) targets {} uploaded {} MB validation {}\n"
                          "build {:.1f} ms, submit {:.1f} ms (totals)\n",
                          from, to, s.draws, s.dispatches, s.skipped, s.shaders, s.shader_fail, s.targets, s.uploaded >> 20, s.validation,
                          ms_build, ms_submit);
    for (auto& n : s.notes) report += n + "\n";
    std::printf("[%.1f s] frames done, reading targets\n", since());
    for (auto& t : gpu.ReadTargets()) {
        const std::string name = std::format("{}\\{}-{:x}-{}x{}-f{}.png", out_dir, t.depth ? "z" : "rt", t.base, t.w, t.h, t.format);
        WritePng(name, t.w, t.h, t.rgba);
        size_t touched = 0;  // pixels that differ from the creation clear (black, depth far = white)
        for (size_t p = 0; p < t.rgba.size(); p += 4) touched += t.depth ? t.rgba[p] != 255 : (t.rgba[p] | t.rgba[p + 1] | t.rgba[p + 2]) != 0;
        report += std::format("{} touched {}\n", name, touched);
    }
    std::printf("%s[%.1f s] targets written\n", report.c_str(), since());
    if (std::getenv("GOW3_DUMP_TEX")) {  // every guest texture, mips 0 and 2: tex\<base>-m<mip>-<w>x<h>-f<VkFormat>.png
        const std::string dir = std::string(out_dir) + "\\tex";
        CreateDirectoryA(dir.c_str(), nullptr);
        const auto tex = gpu.ReadTextures();
        for (auto& t : tex) WritePng(std::format("{}\\{:x}-m{}-{}x{}-f{}.png", dir, t.base, t.level, t.w, t.h, t.format), t.w, t.h, t.rgba);
        std::printf("[%.1f s] %zu texture mips written\n", since(), tex.size());
    }
    for (size_t i = 0; i < s.trace_lines.size(); ++i) report += std::format("draw {} {}\n", i, s.trace_lines[i]);
    for (const auto& [name, code] : s.trace_spirv) Xlat::WriteFileAll(std::string(out_dir) + "\\" + name + ".spv", code);
    if (FILE* f = std::fopen((std::string(out_dir) + "\\report.txt").c_str(), "wb")) std::fwrite(report.data(), 1, report.size(), f), std::fclose(f);
    return 0;
}
} // namespace ReplayVk

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // progress stays visible through tee/pipes
    if (argc >= 6 && !std::strcmp(argv[1], "replay")) {
        bool validate = false, perf = false, live = false;
        u64 stop = ~0ull, step = 1, repeat = 1;
        for (int i = 6; i < argc; ++i)
            if (!std::strcmp(argv[i], "--validate")) validate = true;
            else if (!std::strcmp(argv[i], "--perf")) perf = true;
            else if (!std::strcmp(argv[i], "--live")) live = true;
            else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::max<u64>(1, std::strtoull(argv[++i], nullptr, 10));
            else if (!std::strcmp(argv[i], "--stop") && i + 1 < argc) stop = std::strtoull(argv[++i], nullptr, 10);
            else if (!std::strcmp(argv[i], "--step") && i + 1 < argc) step = std::max<u64>(1, std::strtoull(argv[++i], nullptr, 10));
        return ReplayVk::Run(argv[2], std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 10), argv[5], validate, stop, step, perf, repeat, live);
    }
    if (argc >= 6 && !std::strcmp(argv[1], "peek"))
        return ReplayVk::Peek(argv[2], std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 16), std::strtoull(argv[5], nullptr, 0));
    if (argc >= 4 && !std::strcmp(argv[1], "find")) return Find(argv[2], argv[3]);
    if (argc >= 3 && !std::strcmp(argv[1], "capture")) return Capture(argv[2], false);
    if (argc >= 3 && !std::strcmp(argv[1], "sample")) return Capture(argv[2], true);
    if (argc >= 3 && !std::strcmp(argv[1], "analyze")) return Analyze(argv[2], argc > 3 ? argv[3] : nullptr);
    if (argc >= 3 && !std::strcmp(argv[1], "record")) {
        bool running = false, heap = false;
        u64 max_frames = ~0ull;
        for (int i = 3; i < argc; ++i)
            if (!std::strcmp(argv[i], "--running")) running = true;
            else if (!std::strcmp(argv[i], "--heap")) heap = true;
            else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        return Record(argv[2], running, max_frames, heap);
    }
    if (argc >= 4 && !std::strcmp(argv[1], "timeline")) return Timeline(argv[2], argv[3]);
    if (argc >= 4 && !std::strcmp(argv[1], "census")) return Census(argv[2], argv[3]);
    if (argc >= 5 && !std::strcmp(argv[1], "extract")) return Extract(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4]);
    if (argc >= 3 && !std::strcmp(argv[1], "predication")) return Predication(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "volume")) return Volume(argv[2], argv[3]);
    if (argc >= 4 && !std::strcmp(argv[1], "payload")) return Payload(argv[2], argv[3]);
    if (argc >= 3 && !std::strcmp(argv[1], "heapwatch")) return HeapWatch((u32)std::strtoul(argv[2], nullptr, 10));
    if (argc >= 4 && !std::strcmp(argv[1], "alias")) return AliasCensus::Run(argv[2], argv[3]);
    if (argc >= 4 && !std::strcmp(argv[1], "shadow")) return ShadowScan::Run(argv[2], argv[3]);
    if (argc >= 4 && !std::strcmp(argv[1], "isastat")) return IsaStat::Run(argv[2], argv[3]);
    if (argc >= 5 && !std::strcmp(argv[1], "drawcheck")) return DrawCheck::Run(argv[2], argv[3], argv[4]);
    if (argc >= 4 && !std::strcmp(argv[1], "xlat")) return Xlat::Run(argv[2], argv[3], argc < 5 ? 2 : !std::strcmp(argv[4], "--O0") ? 0 : !std::strcmp(argv[4], "--light") ? 1 : 2);
    std::printf("usage: framecap capture|sample <out.cap> | analyze <in.cap> [draws.csv] | find <in.cap> <out.txt>\n"
                "       record <out.rec> [--running] [--frames N] [--heap] | timeline <in.rec> <out.csv> | extract <in.rec> <seq> <out.cap>\n"
                "       census <in.rec> <prefix> | predication <in.cap> | volume <in.rec> <out.csv> | payload <in.cap> <out.bin>\n"
                "       heapwatch <frames> | alias <in.rec> <out.txt> | shadow <shadow.bin> <out.txt>\n"
                "       isastat <in.rec> <out.txt> | drawcheck <in.rec> <draws.bin> <out.txt> | xlat <in.rec> <out_dir>\n"
                "       replay <in.rec> <from_seq> <to_seq> <out_dir> [--validate] [--stop <draw>]\n");
    return 1;
}
