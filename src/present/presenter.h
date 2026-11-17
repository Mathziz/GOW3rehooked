// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "orbis/common/types.h"
#include "main/config.h"
struct SDL_Window;

// Vulkan swapchain: the flipped display buffer as the renderer drew it (letterboxed), then the system dialogs (ImGui).
// Presents once per completed VideoOut flip, and continuously while a dialog is open.
namespace Gow3::Presenter {
void Initialize(SDL_Window* window, const Config& config);
// Guest address of the display buffer now on screen (0: none); `bgra` when its memory order is B, G, R, A.
void Flipped(u64 address, bool bgra, u64 frame_id);
void BeforeInput();
u64 EndSimulation();
void BeginRender(u64 frame_id);
void EndRender(u64 frame_id);
u64 FrameForFlip();
void UpdateDisplay();
s64 TakeVblankCorrection();
} // namespace Gow3::Presenter
