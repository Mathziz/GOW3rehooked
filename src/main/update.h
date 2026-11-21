// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <string_view>

namespace Gow3::Update {
// Deletes gow3_update.exe, the separate updater of v0.1.5 and v0.1.6, and its backup. A file still running from the
// update that installed this runtime is deleted on a later start.
void RemoveRetiredUpdater(const std::filesystem::path& app);

// Offers the latest GitHub release of this CPU variant when it is newer than `tag`, and installs it in place. True: the
// caller exits, because the game restarted on the new runtime or the player closed the offer.
bool OfferAndInstall(const std::filesystem::path& app, const std::filesystem::path& game, std::wstring_view tag);
} // namespace Gow3::Update
