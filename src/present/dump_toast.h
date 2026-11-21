// SPDX-License-Identifier: GPL-2.0-or-later
// Debug runs: a small toast in the window's bottom-left corner while an F9 frame dump is being saved (a spinning ring), then
// "saved" for 3 s. The dump is complete only once it stops spinning: a game closed earlier loses the images still encoding.
#pragma once

namespace Gow3::DumpToast {
// A part of the dump starts (the traced frames, or one traced frame's images) and ends; the toast spins while any is open.
// Any thread.
void Begin();
void End();
} // namespace Gow3::DumpToast
