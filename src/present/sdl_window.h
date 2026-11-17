// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// GOW3 interface at the upstream path: the window size used by input/input_mouse.cpp; kept current by
// window/window.cpp, which owns the SDL window and event loop.
#pragma once

#include "orbis/common/types.h"

namespace Frontend {
class WindowSDL {
public:
    s32 GetWidth() const { return width; }
    s32 GetHeight() const { return height; }
    s32 width{}, height{};
};
} // namespace Frontend
