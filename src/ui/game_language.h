// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string_view>

namespace Gow3::UI {

enum class GameLanguage { English, BrazilianPortuguese, German, Spanish, French, Italian, Japanese,
                          Korean, LatinAmericanSpanish, Dutch, Polish, Portuguese, Russian, Turkish,
                          Chinese, Count };

GameLanguage TextLanguage();
GameLanguage StartupTextLanguage();
void InitializeTextLanguage(const std::filesystem::path& game);
void LanguageWadOpened(std::string_view path);

} // namespace Gow3::UI
