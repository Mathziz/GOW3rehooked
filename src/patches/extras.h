// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include "main/config.h"
#include "orbis/common/types.h"
#include "orbis/core/module.h"

namespace Gow3::Extras {

enum class Toggle : size_t {
    InfiniteHealth,
    InfiniteMagic,
    InfiniteItem,
    InfiniteRage,
    MaxRedOrbs,
    InfiniteDoubleJumps,
    UnlockChaos,
    AutoQte,
    RemoveMotionBlur,
    RemoveDofSpartan,
    RemoveDofErick,
    Count
};

void LoadFromConfig(const Config& config);
void WriteToConfig(Config& config);
void InstallHooks(Core::Module& eboot);

u32 Vibration();
void SetVibration(u32 percent);

bool Enabled(Toggle toggle);
void SetEnabled(Toggle toggle, bool enabled);
bool Available(Toggle toggle);

void ResetAll();
void Save();

} // namespace Gow3::Extras
