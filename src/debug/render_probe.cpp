// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/render_probe.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#ifndef GOW3_OFFLINE
#include "debug/dump_archive.h"
#endif
#include "main/host.h"

namespace Gow3::RenderProbe {
Frame active;
namespace {
std::unique_ptr<Record[]> ring;
std::mutex publish;
u64 count{};
bool frozen{};
unsigned Region(u64 a, u64 n) {
    for (unsigned i = 0; i < active.regions; ++i)
        if (a >= active.ranges[i].first && a + n <= active.ranges[i].second) return i;
    return kRegions;
}
}
void Bind(unsigned thread) {
    if (!DebugEnabled()) {
        current = nullptr;
        return;
    }
    current = &active.threads[thread], current->tid = Gow3::ThreadId();
}
void Begin(u64 frame, std::span<const std::pair<u64, u64>> regions) {
    if (!DebugEnabled()) {
        current = nullptr;
        return;
    }
    if (!ring) ring = std::make_unique<Record[]>(kFrames);
    active.n = frame, active.start = Clock();
    active.regions = unsigned(std::min<size_t>(regions.size(), kRegions));
    std::copy_n(regions.begin(), active.regions, active.ranges);
    for (Thread& t : active.threads)
        if (t.tid) t = {};
    Bind(kGpu);
}
void Piece(u64 address, u64 bytes, Outcome outcome, bool changed_during_copy, bool fused, u64 staged_bytes) {
    if (!current) return;
    Memory& m = current->memory[Region(address, bytes)];
    if (changed_during_copy) ++m.changed_during_copy;
    else {
        ++m.pieces[outcome], m.bytes[outcome] += bytes;
        if (fused && outcome != InFlight) m.fused_bytes += bytes;
        if (fused) m.subpage_saved_bytes += bytes - staged_bytes;
        else m.unused_staging_bytes += staged_bytes;
        if (!fused && (outcome == New || outcome == Changed)) m.reread_bytes += bytes;
    }
}
void ExcludeArena(u64 lo, u64 hi) {
    if (current && hi > lo) current->memory[Region(lo, hi - lo)].excluded_arena_bytes += hi - lo;
}
void End() {
    if (!DebugEnabled() || !ring) return;
    active.end = Clock();
    std::scoped_lock lock(publish);
    if (frozen) return;
    Record& f = ring[count++ % kFrames];
    static_cast<Header&>(f) = active;
    std::fill_n(f.memory, kRegions + 1, Memory{});
    // Only the participating threads: avoid touching the unused slots on this CPU.
    for (unsigned i = 0; i < kThreads; ++i)
        if (const Thread& t = active.threads[i]; t.tid) {
            f.threads[i] = t;
            for (unsigned r = 0; r <= kRegions; ++r) {
                for (unsigned k = 0; k < Outcomes; ++k) f.memory[r].pieces[k] += t.memory[r].pieces[k], f.memory[r].bytes[k] += t.memory[r].bytes[k];
                f.memory[r].changed_during_copy += t.memory[r].changed_during_copy;
                f.memory[r].fused_bytes += t.memory[r].fused_bytes;
                f.memory[r].reread_bytes += t.memory[r].reread_bytes;
                f.memory[r].unused_staging_bytes += t.memory[r].unused_staging_bytes;
                f.memory[r].excluded_arena_bytes += t.memory[r].excluded_arena_bytes;
                f.memory[r].subpage_saved_bytes += t.memory[r].subpage_saved_bytes;
            }
        } else f.threads[i].tid = 0;
}
void Write() {
#ifdef GOW3_OFFLINE
    return;
#else
    if (!DebugEnabled() || !ring) return;
    u64 end;
    {
        std::scoped_lock lock(publish);
        frozen = true, end = count;
    }
    if (!end) return;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    Gow3::LocalTime(tm, now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const std::string memory_name = std::format("render-{}.memory.csv", stamp);
    const std::string recorder_name = std::format("render-{}.recorders.csv", stamp);
    // Outcomes compare against the last mirrored hash at this address, including any reoffers in the same submit.
    std::string memory_csv = "frame,start_ns,end_ns,lo_hex,hi_hex,new_pieces,new_bytes,changed_pieces,changed_bytes,same_pieces,same_bytes,inflight_pieces,inflight_bytes,changed_during_copy,fused_bytes,reread_bytes,unused_staging_bytes,excluded_arena_bytes,subpage_saved_bytes\n";
    std::string recorder_csv = "frame,start_ns,end_ns,tid,cpu_mask_hex,blocks,block_ns,recent_hits";
    for (const char* p : {"hash", "apply", "draw", "shader", "identify", "state", "issue"})
        recorder_csv += std::format(",{0}_calls,{0}_samples,{0}_ns", p);
    for (const char* c : {"tables", "fixed", "shaders", "clip", "viewport", "raster", "bias", "depth", "bounds", "scissor", "stencil", "blend", "index", "draw"})
        recorder_csv += std::format(",cmd_{}", c);
    recorder_csv += '\n';
    for (u64 i = end > kFrames ? end - kFrames : 0; i < end; ++i) {
        const Record& f = ring[i % kFrames];
        for (const Work& t : f.threads) {
            if (!t.tid) continue;
            recorder_csv += std::format("{},{},{},{},{:x},{},{},{}", f.n, f.start, f.end, t.tid, t.cpu_mask, t.blocks, t.block_ns, t.recent_hits);
            for (const Metric& m : t.phase) recorder_csv += std::format(",{},{},{}", m.calls, m.samples, m.ns);
            for (u64 c : t.commands) recorder_csv += std::format(",{}", c);
            recorder_csv += '\n';
        }
        for (unsigned r = 0; r <= kRegions; ++r) {
            const Memory& m = f.memory[r];
            if (!m.pieces[New] && !m.pieces[Changed] && !m.pieces[Same] && !m.pieces[InFlight] && !m.excluded_arena_bytes) continue;
            const auto [lo, hi] = r < f.regions ? f.ranges[r] : std::pair<u64, u64>{};
            memory_csv += std::format("{},{},{},{:x},{:x}", f.n, f.start, f.end, lo, hi);
            for (unsigned k = 0; k < Outcomes; ++k) memory_csv += std::format(",{},{}", m.pieces[k], m.bytes[k]);
            memory_csv += std::format(",{},{},{},{},{},{}\n", m.changed_during_copy, m.fused_bytes, m.reread_bytes, m.unused_staging_bytes, m.excluded_arena_bytes, m.subpage_saved_bytes);
        }
    }
    DumpArchive::Add(memory_name, memory_csv);
    DumpArchive::Add(recorder_name, recorder_csv);
    Log(std::format("Render probe: last {} of {} submits; Draw sampled 1/64, phase times inclusive; staged {} and {} ({})", std::min<u64>(end, kFrames), end, memory_name, recorder_name, DumpArchive::Path().string()));
#endif
}
} // namespace Gow3::RenderProbe
