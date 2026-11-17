// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/frame_recorder.h"
#include <chrono>
#include <ctime>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include "debug/cpu_probe.h"
#include "debug/dump_archive.h"
#include "orbis/core/tls.h"
#include "debug/video_trace.h"
#include "gpu/gpu_thread.h"
#include "main/host.h"

namespace Gow3::Recorder {
namespace {
std::mutex names_mutex;
std::vector<std::string> host_names{""};  // id 0: no host call
std::unordered_map<const char*, u32> host_ids;

u32 HostCallId(const char* signature) {
    std::scoped_lock lock(names_mutex);
    const auto [it, added] = host_ids.try_emplace(signature, u32(host_names.size()));
    if (added) host_names.emplace_back(HostCallName(signature));
    return it->second;
}
}

GameThread SampleGameThread() {
    // The thread's cycle counter against the clock, measured once (a core's clock varies a little: this is a ratio).
    static const double cycles_per_ms = [] {
        ULONG64 c0, c1;
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        const u64 t0 = DiagClock();
        while (DiagClock() - t0 < 2'000'000) {}
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        return double(c1 - c0) / (double(DiagClock() - t0) * 1e-6);
    }();
    static thread_local ULONG64 previous;
    Core::timed_host_calls = true;
    ULONG64 cycles;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    const Core::HostCallTotals t = Core::TakeHostCallTotals();
    const GameThread sample{previous ? float(double(cycles - previous) / cycles_per_ms) : 0.f, float(double(t.ns) * 1e-6),
                            float(double(t.longest_ns) * 1e-6), float(double(std::exchange(fault_ns, 0)) * 1e-6), t.longest ? HostCallId(t.longest) : 0u};
    previous = cycles;
    CpuProbe::SubmitStarted();
    return sample;
}
} // namespace Gow3::Recorder

namespace Gow3::GpuThread {
void RecordFlip(s64 late_ns, u32 pending, u32 stale_us, u32 flip_us, u32 event_us) {
    if (!DebugEnabled()) return;
    auto& f = Recorder::flips.Next();
    f.at = DiagClock(), f.late_ns = late_ns, f.pending = pending, f.stale_us = stale_us, f.flip_us = flip_us, f.event_us = event_us;
    Recorder::flips.Commit();
}

void RecordPresent(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point acquired) {
    if (!DebugEnabled()) return;
    auto& p = Recorder::presents.Next();
    p.start = u64(start.time_since_epoch().count()), p.acquired = u64(acquired.time_since_epoch().count()), p.end = DiagClock();
    Recorder::presents.Commit();
}

void WriteFrames() {
    VideoTrace::Write();
    using namespace Recorder;
    if (!DebugEnabled() || !frames.Count()) return;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const std::string name = std::format("frames-{}.bin", stamp);
    // Header: magic, record sizes, record counts; then the frames, flips, presents and compiles, oldest first.
    const u32 sizes[4]{sizeof(Frame), sizeof(Flip), sizeof(Present), sizeof(Compile)};
    const u64 ends[4]{frames.End(), flips.End(), presents.End(), compiles.End()};
    const u64 counts[4]{std::min(ends[0], kRing), std::min(ends[1], kRing), std::min(ends[2], kRing), std::min(ends[3], kRing)};
    std::vector<u8> bin;
    bin.reserve(8 + sizeof(sizes) + sizeof(counts) + counts[0] * sizeof(Frame) + counts[1] * sizeof(Flip) +
                counts[2] * sizeof(Present) + counts[3] * sizeof(Compile));
    const auto append_raw = [&](const void* p, size_t n) {
        const auto* b = static_cast<const u8*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    append_raw("GOW3FRM2", 8);
    append_raw(sizes, sizeof(sizes));
    append_raw(counts, sizeof(counts));
    frames.AppendTo(bin, ends[0]);
    flips.AppendTo(bin, ends[1]);
    presents.AppendTo(bin, ends[2]);
    compiles.AppendTo(bin, ends[3]);
    DumpArchive::Add(name, bin);

    std::string names_txt; // host call ids of the frames' `game.longest`, one per line
    {
        std::scoped_lock lock(names_mutex);
        for (size_t i = 1; i < host_names.size(); ++i) names_txt += std::format("{}\t{}\n", i, host_names[i]);
    }
    if (!names_txt.empty()) DumpArchive::Add(name + ".hle", names_txt);
    Log(std::format("Frame recorder: {} frames, {} flips, {} presents, {} compiles staged as {} ({})", counts[0], counts[1],
                    counts[2], counts[3], name, DumpArchive::Path().string()));
    CpuProbe::Write(name);
}
} // namespace Gow3::GpuThread
