// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <system_error>

#include "orbis/logging/log.h"
#include "orbis/common/path_util.h"
#include "orbis/core/file_sys/backends/host_fs.h"
#include "orbis/core/file_sys/fs.h"
#include "orbis/core/file_sys/ifile.h"

namespace Core::FileSys {

bool IsZArchiveFile(const std::filesystem::path&) {
    return false;
}

std::filesystem::path StripZArchiveExtension(const std::filesystem::path& path) {
    return path;
}

std::unique_ptr<IBackend> OpenGameBackend(const std::filesystem::path& root) {
    const auto resolved = ResolveGameRoot(root);
    if (!resolved.has_value()) {
        return nullptr;
    }

    return std::make_unique<HostFsBackend>(*resolved, true);
}

std::optional<std::vector<u8>> ReadGameFile(const std::filesystem::path& game_root,
                                            std::string_view rel_path) {
    const auto backend = OpenGameBackend(game_root);
    if (!backend) {
        return std::nullopt;
    }
    return backend->ReadFile(rel_path);
}

std::optional<std::filesystem::path> ResolveGameFilePath(const std::filesystem::path& game_root,
                                                         std::string_view rel_path) {
    const auto backend = OpenGameBackend(game_root);
    if (!backend) {
        return std::nullopt;
    }

    if (const auto host_root = backend->RootHostPath()) {
        std::filesystem::path full_path = *host_root / rel_path;
        std::error_code ec;
        if (std::filesystem::is_regular_file(full_path, ec) && !ec) {
            return full_path;
        }
    }
    return std::nullopt;
}

u64 GetGameRootSize(const std::filesystem::path& game_root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(game_root, ec) || ec) {
        return 0ull;
    }

    u64 total = 0;
    for (std::filesystem::recursive_directory_iterator
             it(game_root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !entry_ec) {
            const auto size = it->file_size(entry_ec);
            if (!entry_ec) {
                total += static_cast<u64>(size);
            }
        }
    }
    return total;
}

} // namespace Core::FileSys
