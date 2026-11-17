// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
constexpr uint32_t Gow3Inspect = 1;
constexpr uint32_t Gow3ValidateRuntime = 2;
// Runs the runtime (entry.cpp) for the game folder; returns only on failure, with the reason in Gow3LastError().
int Gow3Run(const wchar_t* executable_directory, const wchar_t* game_directory, uint32_t flags);
const wchar_t* Gow3LastError();
