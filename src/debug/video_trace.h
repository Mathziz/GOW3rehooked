// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "debug/debug.h"
#include "orbis/common/types.h"

// Debug runs (GOW3_VIDEO_TRACE=0 off): the life of each cutscene frame, from the decoder to the readback, plus the audio queue and
// flow around it, in ring buffers written to video-trace.csv, video-audio-trace.csv and video-audio-flow.csv when the
// window closes. The buffers exist only when the trace is on; callers check `Enabled()`.
namespace Gow3::VideoTrace {
inline bool Enabled() {
    static const bool enabled = DebugAuto("GOW3_VIDEO_TRACE");
    return enabled;
}
struct Event { u64 time, address, serial, tag; u32 kind, width, height, queue; };
enum Ring { Decoded, Selected, Rendered, Textures, Copies, Readbacks };
void Add(Ring ring, Event event);
void Write();
} // namespace Gow3::VideoTrace
