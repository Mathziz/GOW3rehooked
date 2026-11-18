// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Gow3::SaveImport {
enum class Source { ShadPS4, SpartanPort };

struct Slot {
    std::filesystem::path name;
    std::filesystem::file_time_type time;
};

struct SaveSet {
    std::filesystem::path root;
    std::vector<Slot> slots;
    size_t conflicts{};
};

bool WasOffered();
void MarkOffered();
std::optional<SaveSet> Discover(Source source, const std::filesystem::path& game,
                                const std::filesystem::path& selected = {});
size_t Import(Source source, const SaveSet& saves);
} // namespace Gow3::SaveImport
