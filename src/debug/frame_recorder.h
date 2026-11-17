// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>
#include "debug/debug.h"
#include "orbis/common/types.h"

// Frame recorder: per frame, the game's submit and the GPU thread's phases, stalls and counts; per flip and present,
// their times (steady_clock ns: QPC, the clock of ETW traces); per program compiled, what made it a new variant. Plain
// stores into fixed rings, one writer each, written to frames-<time>.bin when the window closes (debug runs), read by
// tools\framestat. The rings exist only in debug runs, and callers check DebugEnabled() before using them.
namespace Gow3::Recorder {
constexpr u64 kRing = 1 << 16;
// What the game's submitting thread did since its previous submit: CPU time it ran, time inside host calls (HLE) and
// the longest of them (an id into the names written beside the recording).
struct GameThread {
    float cpu_ms, host_ms, longest_ms, fault_ms;  // fault_ms: in the page tracker's write-fault handler
    u32 longest;
};
// Sample taken by that thread at each submit (a debug run): its CPU time, host call totals; turns host call timing on.
GameThread SampleGameThread();
struct Frame {
    u64 n, submit, captured, start, rendered, processed;
    GameThread game;
    float ms[16];  // BeginFrame, chain, then Stats::temp_ms
    float compile_wait_ms, bind_ms, evict_ms, label_wait_ms;
    u32 compiles, compile_waits, binds, evicts, label_waits, faults, queries, draws;
    u32 reaped;  // Stats::reaped, reap_max_ms
    float reap_max_ms;
};
struct Flip {
    u64 at;
    s64 late_ns;  // the vblank thread's wake-up after its deadline
    u32 pending, stale_us, flip_us, event_us;  // stale_us: already overdue when the sleep was planned; the previous pass's work
};
struct Present {
    u64 start, acquired, end;
};
struct Compile {  // Gpu::Stats::Compile, of the frame `n` whose Submit returned it
    u64 n, prog, key;
    u32 stage, changed, slot, us, was[2], now[2];
};
template <class T> struct Ring {
    std::unique_ptr<T[]> v{DebugEnabled() ? new T[kRing] : nullptr};
    std::atomic<u64> n{0};
    T& Next() { return v[n.load(std::memory_order_relaxed) % kRing] = {}; }
    void Commit() { n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
    u64 End() const { return n.load(std::memory_order_acquire); }
    // The records before `end` (End() when the header was written: the threads go on recording), oldest first.
    void Write(FILE* f, u64 end) const {
        for (u64 i = end > kRing ? end - kRing : 0; i < end;) {
            const u64 run = std::min(end - i, kRing - i % kRing);
            std::fwrite(&v[i % kRing], sizeof(T), run, f), i += run;
        }
    }
    void AppendTo(std::vector<u8>& out, u64 end) const {
        for (u64 i = end > kRing ? end - kRing : 0; i < end;) {
            const u64 run = std::min(end - i, kRing - i % kRing);
            const auto* bytes = reinterpret_cast<const u8*>(&v[i % kRing]);
            out.insert(out.end(), bytes, bytes + run * sizeof(T));
            i += run;
        }
    }
    u64 Count() const { return std::min(n.load(std::memory_order_acquire), kRing); }
};
inline Ring<Frame> frames;
inline Ring<Flip> flips;
inline Ring<Present> presents;
inline Ring<Compile> compiles;
} // namespace Gow3::Recorder
