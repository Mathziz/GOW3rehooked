// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
using Gow3PathChar = std::filesystem::path::value_type;
constexpr uint32_t Gow3Inspect = 1;
constexpr uint32_t Gow3ValidateRuntime = 2;
// Runs the runtime (entry.cpp) for the game folder; returns only on failure, with the reason in Gow3LastError().
int Gow3Run(const Gow3PathChar* executable_directory, const Gow3PathChar* game_directory, uint32_t flags);
const Gow3PathChar* Gow3LastError();
