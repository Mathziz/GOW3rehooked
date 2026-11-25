// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>

struct SDL_Window;

namespace Gow3::SaveImportUi {
void Offer(SDL_Window* window, const std::filesystem::path& game, bool manual);
bool Active();
bool Update(); // true once the success screen has finished
void RestartFailed();
void WaitForCopy();
} // namespace Gow3::SaveImportUi
