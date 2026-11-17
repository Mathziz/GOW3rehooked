// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "orbis/common/path_util.h"
#include <array>
#include <stdexcept>

namespace Common::FS {
static std::array<std::filesystem::path, static_cast<size_t>(PathType::Count)> paths;
bool ValidatePath(const std::filesystem::path& path) { return !path.empty(); }
std::string PathToUTF8String(const std::filesystem::path& path) {
    auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
const std::filesystem::path& GetUserPath(PathType type) { return paths.at(static_cast<size_t>(type)); }
std::string GetUserPathString(PathType type) { return PathToUTF8String(GetUserPath(type)); }
void SetUserPath(PathType type, const std::filesystem::path& path) {
    if (!path.is_absolute()) throw std::runtime_error("Orbis host path must be absolute");
    paths.at(static_cast<size_t>(type)) = path;
}
}
