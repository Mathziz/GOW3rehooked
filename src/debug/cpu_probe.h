// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include "debug/debug.h"
#include "orbis/common/types.h"

// Debug runs: the submitting game thread sampled only in its frames longer than kSlowNs; GOW3_TEMP_PROBE=1 instead samples it
// (0) and the GPU thread (1) every ~0.5 ms: RIP, the host return
// addresses above it (the first guest one ends the walk), whether it ran since the sample before. Written with the frame
// recording as <frames>.probe (samples), <frames>.probe.frames (per GPU frame image timers) and <frames>.probe.txt (thread
// names, eboot base, modules). Every call is a no-op while the probe is off.
// TEMP GOW3_TEMP_PROBE=blocks: the GPU thread and the recorders, every ~0.5 ms, only while a frame's parallel recording has run
// longer than 3 ms; <frames>.blocks.txt lists each such phase's samples with their host stacks, symbolized.
namespace Gow3::CpuProbe {
inline const bool enabled = DebugAuto("GOW3_TEMP_PROBE");
constexpr u32 kThreads = 2 + 31;    // game thread, GPU thread, recorders (2 + k)
void Start();
void Register(u32 thread);          // the calling thread is sample thread 0 (submitting game thread), 1 (GPU thread) or 2 + recorder
void BlocksBegin();                 // TEMP: the parallel recording of a frame starts / ends
void BlocksEnd();
void SubmitStarted();               // the game thread submitted: its next frame starts now (GOW3_TEMP_PROBE=slow samples it once it runs long)
void Frame(u64 n);                  // per GPU frame: the image and check timers since the frame before
void Write(const std::string& path);
} // namespace Gow3::CpuProbe
