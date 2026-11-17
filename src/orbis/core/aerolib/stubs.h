// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "orbis/common/types.h"

namespace Core::AeroLib {

u64 GetStub(const char* nid);
void SetStubObserver(void (*observer)(const char* name, const char* nid, void* caller));

} // namespace Core::AeroLib
