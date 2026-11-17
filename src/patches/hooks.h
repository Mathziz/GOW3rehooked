// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "main/config.h"
#include "orbis/core/module.h"

namespace Gow3 {
// Install engine timing, gameplay, title menu, and optional diagnostic hooks.
void InstallHooks(Core::Module& eboot, const Config& config);
// Stops background diagnostic drain threads and folds their loose files into the session .dmp archive.
void StopDiagnosticHooks();
// TEMP shadow trace (GOW3_SHADOW=1): records WAD opens next to the texture lifetime events.
void ShadowFileOpen(const char* path);
}
