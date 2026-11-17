// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>

namespace Gow3 {
// Gives gow3.exe the icon of the game's icon0.png (Explorer, shortcuts, taskbar pins). The icon is not shipped with the
// port: it is written into the running executable once per icon file, and failures are silent (the default icon stays).
void EmbedGameIcon(const std::filesystem::path& icon_png);
}
