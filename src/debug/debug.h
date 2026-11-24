// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// Diagnostics. GOW3_DEBUG=1 (or --debug) turns on, by itself, every diagnostic that keeps the game playable: the log,
// timers, GPU timestamps, frame recorder, render probe, long-frame probe, fault diagnostics, labels and video trace (DebugAuto;
// NAME=0 turns one off). GOW3_DEBUG=2 or NAME=1 turns on the deep/heavy ones (DebugDeep: texture dumping in F9, Khronos
// validation, exploded vertices (BOOM), light log, shadow trace, NVIDIA diagnostics config (TEMP_FAULT=1), S4, TEMP_STATS,
// GPU_PROF, DRAWLOG, BARSITES, COPYLOG). Other
// parameterized options (TRACE_EVERY, DUMP_STOP, the probes and the experiments) remain opt-in via DebugOption. The variable is
// read once, before main, and is constant afterwards. Without it no other GOW3_* diagnostic variable is even read, and each
// diagnostic costs the one predicted branch of GOW3_DIAG (or of DebugEnabled()).
namespace Gow3 {
// True when the command line has --debug (main/host.cpp).
bool DebugArgument();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // the values are only read, never kept across changes
inline char DebugMode() {
    static const char mode = [] {
#ifdef GOW3_OFFLINE
        return '1'; // tools/framecap: the offline harness always runs the diagnostics
#else
        const char* set = std::getenv("GOW3_DEBUG");
        return set && (*set == '1' || *set == '2') && !set[1] ? *set : DebugArgument() ? '1' : '\0';
#endif
    }();
    return mode;
}
inline const char* DebugOption(const char* name) { return DebugMode() ? std::getenv(name) : nullptr; }
#pragma clang diagnostic pop
inline bool DebugEnabled() { return DebugMode() != '\0'; }
inline int DebugLevel() { return DebugMode() ? DebugMode() - '0' : 0; }
inline bool DebugFlag(const char* name) { const char* v = DebugOption(name); return v && !(v[0] == '0' && !v[1]); }
// On in every debug run unless NAME=0.
inline bool DebugAuto(const char* name) { const char* v = DebugOption(name); return DebugMode() && !(v && v[0] == '0' && !v[1]); }
// On automatically in GOW3_DEBUG=2 (unless NAME=0); opt-in in GOW3_DEBUG=1.
inline bool DebugDeep(const char* name) {
    const char* v = DebugOption(name);
    if (v && v[0] == '0' && !v[1]) return false;
    return DebugLevel() >= 2 || v != nullptr;
}
// `GOW3_DIAG statement;` runs the statement only in a debug run.
#define GOW3_DIAG if (::Gow3::DebugEnabled()) [[unlikely]]
// The clock of a diagnostic timer: no clock read outside a debug run (the interval it feeds is not used there).
inline std::chrono::steady_clock::time_point DiagNow() { return DebugMode() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; }
inline double DiagMs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
// steady_clock in ns (QPC, the clock of ETW traces) for the records the tools read.
inline std::uint64_t DiagClock() { return std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }

// Debug: time the calling thread spent in the page tracker's write-fault handler; the game thread's frame sample takes it.
inline thread_local std::int64_t fault_ns{};
struct FaultTimer {
    std::chrono::steady_clock::time_point start = DiagNow();
    ~FaultTimer() { if (DebugEnabled()) fault_ns += (std::chrono::steady_clock::now() - start).count(); }
};
// The function inside a host call wrapper's signature (HOST_CALL), or the text itself.
std::string_view HostCallName(std::string_view signature);
// Debug: records an event begun at `start` once it lasted `threshold_ms`; `what` must outlive the report.
void Spike(const char* what, std::chrono::steady_clock::time_point start, double threshold_ms, std::uint64_t detail = 0);
// Spikes recorded since the last call, by start time relative to `since`; empty when none.
std::string TakeSpikes(std::chrono::steady_clock::time_point since);
} // namespace Gow3
