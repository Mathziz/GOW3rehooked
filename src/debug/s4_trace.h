// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "debug/debug.h"
#include "orbis/common/types.h"

// Debug S4 (GOW3_S4=1): which CPU code touches memory the GPU writes. Each submit's written colour/depth/stencil surfaces
// (context registers at each draw) and EVENT_WRITE destinations stay PAGE_NOACCESS until the next submit; the first CPU
// access to each page is recorded with its instruction address, then the page is reopened.
// Report: <port>/s4-gpu-written.txt, rewritten every 2 s.
namespace Gow3::S4 {
inline const bool enabled = DebugDeep("GOW3_S4");
void Start();                                                 // the access handler and the report thread
void Stop();                                                  // stops the report thread and folds s4-gpu-written.txt into the session .dmp archive
void Packet(u32 header, const u32* p, u32 payload);           // each type 3 PM4 packet the GPU thread walks
void Arm();                                                   // end of a submit: reopen the previous pages, arm this submit's surfaces
} // namespace Gow3::S4
