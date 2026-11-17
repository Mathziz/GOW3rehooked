// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// GOW3 interface at the upstream path: the standalone runtime has no debugger UI that pauses guests.
#pragma once

struct GuestDebugState {
    bool IsGuestThreadsPaused() const { return false; }
};
inline constexpr GuestDebugState DebugState{};
