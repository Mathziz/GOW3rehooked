// SPDX-License-Identifier: GPL-2.0-or-later
// The game's own splash image (sce_sys/pic1.png, else pic0.png), drawn with ImGui from startup until the start menu, with the
// shader compilation progress in a footer while programs compile. With no image it is a black screen, only while compiling.
#pragma once

namespace Gow3::Splash {
// `logos`: the intro logos run, so the splash ends two seconds after the first non-black game frame. `wanted` false: no
// splash this run.
void Show(bool logos, bool wanted);
bool NeedsFrameProbe();
void NonBlackFrame();
// The first-boot brightness screen is up.
void Reveal();
// The start menu is up (its music opened): the splash goes away.
void Dismiss();
// True for a guest path that is the start menu's music.
bool IsStartMenuMusic(const char* path);
// The threads that keep the splash moving (the window's events and the presentation) run at the highest priority while it
// is up, above the shader build, then go back to the priority they had. Each one updates its own on every pass of its loop;
// once the splash is gone that is a test of a local flag, inlined.
struct Priority {
    int was = 0;
    bool raised = false, done = false;
    void Update() { if (!done) Step(); }
    void Step();
};
} // namespace Gow3::Splash
