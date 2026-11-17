// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vector>
#include "gpu/buffers.h"
#include "gpu/vk_gpu.h"

// Debug frame dumps. F9 traces the next frame, GOW3_TEMP_TRACE_EVERY=N every Nth one: one line per draw and, for F9, each
// program's SPIR-V and the render targets as PNGs, in dumps\trace\HHMMSS. GOW3_DUMP_STOP=<draw> stops the dumped frame after
// that draw. Debug runs with GOW3_BOOM=1 log the draws whose vertices explode and the guest memory behind their loads, and the
// draws that read bytes the mirror did not get this frame (stale reads).
// The GPU thread calls these only in debug runs.
namespace Gow3::FrameDump {
void Begin(Gpu::Stats& stats);                        // before the frame's Submit: arms the renderer's trace
void End(Gpu::Renderer& renderer, Gpu::Stats& stats); // after it: writes what the trace collected
// After the frame's Submit, with its regions and what it offered to the mirror.
void Boom(Gpu::Stats& stats, const std::vector<GuestMemory::Range>& regions, const std::vector<GuestMemory::Range>& offer);
} // namespace Gow3::FrameDump
