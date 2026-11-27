// SPDX-License-Identifier: GPL-2.0-or-later
// The frame rate in the window's top-left corner (F10, or fps_overlay=true in config.cfg): the game's frames per second, the
// average frame time and the longest frame of the last half second. It takes no input and adds no presents: it is drawn with
// the game's own frames.
#pragma once

namespace Gow3::FpsOverlay {
// A frame of the game reached the display (VideoOut flip). Any thread.
void Flip();
// Shows or hides the counter; Toggle also saves the choice in config.cfg. The window's thread.
void Show(bool shown);
void Toggle();
} // namespace Gow3::FpsOverlay
