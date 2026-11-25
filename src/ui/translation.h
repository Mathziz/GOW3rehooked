// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Gow3::UI {

enum class GameLanguage;

// All callers pass a NUL-terminated source phrase or an empty string.
const char* Tr(const char* english);
const char* Tr(GameLanguage language, const char* english);

} // namespace Gow3::UI
