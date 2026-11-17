// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include "debug/debug.h"

// CPU renderer measurements (active in debug runs, GOW3_DEBUG >= 1, and offline tools). No extra guest reads or runtime I/O.
namespace Gow3::RenderProbe {
using u64 = std::uint64_t;
inline u64 Clock() { return u64(std::chrono::steady_clock::now().time_since_epoch().count()); }
constexpr unsigned kRegions = 10, kThreads = 33, kGpu = kThreads - 1, kFrames = 4096;
enum Phase { Hash, Apply, Draw, Shader, Identify, State, Issue, Phases };
enum Outcome { New, Changed, Same, InFlight, Outcomes };
enum Command { Tables, Fixed, Shaders, Clip, Viewport, Raster, Bias, Depth, Bounds, Scissor, Stencil, Blend, Index, DrawCall, Commands };
struct Metric { u64 calls{}, samples{}, ns{}; };
struct Memory { u64 pieces[Outcomes]{}, bytes[Outcomes]{}, changed_during_copy{}, fused_bytes{}, reread_bytes{}, unused_staging_bytes{}, excluded_arena_bytes{}, subpage_saved_bytes{}; };
struct Work {
    u64 tid{}, cpu_mask{}, blocks{}, block_ns{}, recent_hits{};
    Metric phase[Phases];
    u64 commands[Commands]{};
};
struct alignas(64) Thread : Work {
    Memory memory[kRegions + 1];  // final bucket: outside the frame regions, or straddling their bounds
};
struct Header {
    u64 n{}, start{}, end{};
    unsigned regions{};
    std::pair<u64, u64> ranges[kRegions];
};
struct Frame : Header { Thread threads[kThreads]; };
struct Record : Header { Work threads[kThreads]; Memory memory[kRegions + 1]; };
extern Frame active;
inline thread_local Thread* current = nullptr;
inline thread_local bool sampled_draw = false;
void Bind(unsigned thread);
void Begin(u64 frame, std::span<const std::pair<u64, u64>> regions);
void End();
void Write();  // window thread, at close: freezes the ring before doing I/O
void Piece(u64 address, u64 bytes, Outcome outcome, bool changed_during_copy = false, bool fused = false, u64 staged_bytes = 0);
void ExcludeArena(u64 lo, u64 hi);  // deferred arena bytes [lo, hi) that no work of the frame read (not hashed or staged)
inline void Cmd(Command group, u64 n = 1) { if (current) current->commands[group] += n; }

struct Scope {
    Metric* metric = nullptr;
    u64 start{};
    explicit Scope(Phase phase, bool sample = sampled_draw) {
        if (current) {
            Metric& m = current->phase[phase];
            ++m.calls;
            if (sample) metric = &m, ++m.samples, start = Clock();
        }
    }
    void End() { if (metric) metric->ns += Clock() - start, metric = nullptr; }
    ~Scope() { End(); }
};
struct DrawScope {
    // Rotate the sampling phase each frame to avoid repeatedly timing only the same draws.
    bool previous = sampled_draw;
    Scope timer{Draw, current && ((current->phase[Draw].calls + active.n * 17) & 63) == 0};
    DrawScope() { sampled_draw = timer.metric != nullptr; }
    ~DrawScope() { sampled_draw = previous; }
};
} // namespace Gow3::RenderProbe
