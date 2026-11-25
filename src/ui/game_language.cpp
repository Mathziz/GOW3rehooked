// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_language.h"

#include <array>
#include <atomic>
#include <fstream>
#include <optional>

#include "main/host.h"
#include "orbis/core/emulator_settings.h"

namespace Gow3::UI {
namespace {
std::atomic<GameLanguage> language{GameLanguage::English};
GameLanguage launch_language{GameLanguage::English};
constexpr std::array<std::string_view, static_cast<size_t>(GameLanguage::Count)> wad_codes{
    "EN", "BR", "DE", "ES", "FR", "IT", "JA", "KO", "LA", "NL", "PL", "PT", "RU", "TR", "ZH",
};

std::optional<GameLanguage> ConsoleLanguage(int id) {
    using enum GameLanguage;
    constexpr GameLanguage languages[]{
        Japanese, English, French, Spanish, German, Italian, Dutch, Portuguese, Russian, Korean, Chinese,
        Chinese, Count, Count, Count, Count, Polish, BrazilianPortuguese, English, Turkish, LatinAmericanSpanish,
    };
    if (id < 0 || size_t(id) >= std::size(languages) || languages[id] == Count) return {};
    return languages[id];
}
}

GameLanguage TextLanguage() { return language.load(std::memory_order_relaxed); }
GameLanguage StartupTextLanguage() { return launch_language; }

void InitializeTextLanguage(const std::filesystem::path& game) {
    auto selected = ConsoleLanguage(EmulatorSettings.GetConsoleLanguage()).value_or(GameLanguage::English);
    const auto file = game / PortDir::Root / PortDir::Saves / "USERDATA" / "SAVEDATA";
    std::ifstream prefs(file, std::ios::binary);
    // The 1.02 preferences record is 32 bytes. Version 10 stores the text language at byte 12,
    // followed by the spoken language at byte 13.
    std::array<unsigned char, 32> data{};
    if (prefs.read(reinterpret_cast<char*>(data.data()), data.size()) && prefs.peek() == std::char_traits<char>::eof() &&
        data[0] == 10 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
        selected = ConsoleLanguage(data[12]).value_or(selected);
    }
    launch_language = selected;
    language.store(selected, std::memory_order_relaxed);
}

void LanguageWadOpened(std::string_view path) {
    // The game reloads its resident text WAD on a language change. DLC and audio use other paths.
    constexpr std::string_view prefix = "/EXEC/WAD/PS4_LE/R_LANG_";
    if (path.starts_with("/app0/")) path.remove_prefix(5);
    else if (path.starts_with("/hostapp/")) path.remove_prefix(8);
    else return;
    if (path.size() != prefix.size() + 6 || !path.starts_with(prefix) || !path.ends_with(".WAD")) return;
    const auto code = path.substr(prefix.size(), 2);
    if (code == "UK") return language.store(GameLanguage::English, std::memory_order_relaxed);
    for (size_t i = 0; i < wad_codes.size(); ++i) {
        if (wad_codes[i] == code) return language.store(static_cast<GameLanguage>(i), std::memory_order_relaxed);
    }
}

} // namespace Gow3::UI
