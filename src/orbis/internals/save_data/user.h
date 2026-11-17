// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "orbis/internals/system/userservice.h"

// GOW3 is single-player: one fixed user on pad port 1. Its id names the save directory
// (SaveData/<id>/<serial>), so it keeps the value saves were written under.
namespace Libraries::SaveData::User {

constexpr s32 Id = 1000;
constexpr const char* Name = "shadPS4";
constexpr s32 Color = 1; // OrbisUserServiceUserColor::Blue
inline std::atomic_bool logged_in{};

/// Logs the user in once; later calls (a pad connected after boot) do nothing.
inline void Login() {
    if (!logged_in.exchange(true)) {
        UserService::AddUserServiceEvent({UserService::OrbisUserServiceEventType::Login, Id});
    }
}

} // namespace Libraries::SaveData::User
