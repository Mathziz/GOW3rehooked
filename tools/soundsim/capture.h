// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>

namespace SoundSim {
void ReportGaiaCapture(const std::filesystem::path& path);
void ReportStereo(const std::filesystem::path& path);
}
