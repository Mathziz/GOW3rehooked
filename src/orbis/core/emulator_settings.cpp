// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "orbis/common/path_util.h"
#include "orbis/common/util.h"
#include "orbis/logging/log.h"
#include "emulator_settings.h"

// ── Singleton storage ─────────────────────────────────────────────────
std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::s_instance = nullptr;
std::mutex EmulatorSettingsImpl::s_mutex;

EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;

std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    std::lock_guard lock(s_mutex);
    if (!s_instance)
        s_instance = std::make_shared<EmulatorSettingsImpl>();
    return s_instance;
}

void EmulatorSettingsImpl::SetInstance(std::shared_ptr<EmulatorSettingsImpl> instance) {
    std::lock_guard lock(s_mutex);
    s_instance = std::move(instance);
}

// --------------------
// General helpers
// --------------------
bool EmulatorSettingsImpl::AddGameInstallDir(const std::filesystem::path& dir, bool enabled) {
    for (const auto& d : m_general.install_dirs)
        if (d.path == dir)
            return false;
    m_general.install_dirs.push_back({dir, enabled});
    return true;
}

std::vector<std::filesystem::path> EmulatorSettingsImpl::GetGameInstallDirs() const {
    std::vector<std::filesystem::path> out;
    for (const auto& d : m_general.install_dirs)
        if (d.enabled)
            out.push_back(d.path);
    return out;
}

const std::vector<GameInstallDir>& EmulatorSettingsImpl::GetAllGameInstallDirs() const {
    return m_general.install_dirs;
}

void EmulatorSettingsImpl::SetAllGameInstallDirs(const std::vector<GameInstallDir>& dirs) {
    m_general.install_dirs = dirs;
}

void EmulatorSettingsImpl::RemoveGameInstallDir(const std::filesystem::path& dir) {
    auto iterator =
        std::find_if(m_general.install_dirs.begin(), m_general.install_dirs.end(),
                     [&dir](const GameInstallDir& install_dir) { return install_dir.path == dir; });
    if (iterator != m_general.install_dirs.end()) {
        m_general.install_dirs.erase(iterator);
    }
}

void EmulatorSettingsImpl::SetGameInstallDirEnabled(const std::filesystem::path& dir,
                                                    bool enabled) {
    auto iterator =
        std::find_if(m_general.install_dirs.begin(), m_general.install_dirs.end(),
                     [&dir](const GameInstallDir& install_dir) { return install_dir.path == dir; });
    if (iterator != m_general.install_dirs.end()) {
        iterator->enabled = enabled;
    }
}

void EmulatorSettingsImpl::SetGameInstallDirs(
    const std::vector<std::filesystem::path>& dirs_config) {
    m_general.install_dirs.clear();
    m_general.install_dirs.reserve(dirs_config.size());
    for (const auto& dir : dirs_config) {
        m_general.install_dirs.push_back({dir, true});
    }
}

const std::vector<bool> EmulatorSettingsImpl::GetGameInstallDirsEnabled() {
    std::vector<bool> enabled_dirs;
    enabled_dirs.reserve(m_general.install_dirs.size());
    for (const auto& dir : m_general.install_dirs) {
        enabled_dirs.push_back(dir.enabled);
    }
    return enabled_dirs;
}

std::filesystem::path EmulatorSettingsImpl::GetHomeDir() {
    if (m_general.home_dir.empty()) {
        return Common::FS::GetUserPath(Common::FS::PathType::HomeDir);
    }
    return m_general.home_dir;
}

void EmulatorSettingsImpl::SetHomeDir(const std::filesystem::path& dir) {
    m_general.home_dir = dir;
}

std::filesystem::path EmulatorSettingsImpl::GetSysModulesDir() {
    if (m_general.sys_modules_dir.empty()) {
        return Common::FS::GetUserPath(Common::FS::PathType::SysModuleDir);
    }
    return m_general.sys_modules_dir;
}

void EmulatorSettingsImpl::SetSysModulesDir(const std::filesystem::path& dir) {
    m_general.sys_modules_dir = dir;
}

std::filesystem::path EmulatorSettingsImpl::GetFontsDir() {
    if (m_general.font_dir.empty()) {
        return Common::FS::GetUserPath(Common::FS::PathType::FontsDir);
    }
    return m_general.font_dir;
}

void EmulatorSettingsImpl::SetFontsDir(const std::filesystem::path& dir) {
    m_general.font_dir = dir;
}

std::filesystem::path EmulatorSettingsImpl::GetAddonInstallDir() {
    if (m_general.addon_install_dir.empty()) {
        return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "addcont";
    }
    return m_general.addon_install_dir;
}

void EmulatorSettingsImpl::SetAddonInstallDir(const std::filesystem::path& dir) {
    m_general.addon_install_dir = dir;
}

void EmulatorSettingsImpl::SetDefaultValues() {
    m_general = GeneralSettings{};
    m_network = NetworkSettings{};
    m_log = LogSettings{};
    m_debug = DebugSettings{};
    m_input = InputSettings{};
    m_audio = AudioSettings{};
    m_gpu = GPUSettings{};
    m_vulkan = VulkanSettings{};
}
