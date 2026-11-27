// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
struct SDL_Window;
union SDL_Event;
struct VkCommandBuffer_T;
struct VkQueue_T;
namespace ImGui::Core {
void Initialize(SDL_Window* window);
void SetMouseCursorHidden(bool hidden);
bool ProcessEvent(const SDL_Event& event);
void NewFrame();
// Records the last UI frame into a dynamic rendering pass; `queue` is idled before textures are destroyed.
void Render(VkCommandBuffer_T* cmd, VkQueue_T* queue);
void AcquireGamepadInputCapture();
void ReleaseGamepadInputCapture();
bool IsGamepadInputCaptured();
// Whether the prompts show the gamepad's buttons (else the keyboard's keys): the last device used, or the gamepad for the
// rest of the run once the two alternate quickly (a pad mapped to the mouse).
bool GamepadPrompts();
bool PlayStationPrompts();  // the last gamepad is a DualShock 3, DualShock 4 or DualSense (else Xbox-style prompts)
void DrawLayers();
// Any layer is up: UI frames are built (passive layers such as the FPS counter included).
bool MustKeepDrawing();
// A layer that takes input or animates on its own is up: the UI gets the input and is presented without game frames.
bool HasInteractiveLayers();
}
