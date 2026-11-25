// SPDX-License-Identifier: GPL-2.0-or-later
#include "translation.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "game_language.h"

namespace Gow3::UI {
namespace {
struct Phrase {
    std::string_view english;
    std::array<const char*, static_cast<size_t>(GameLanguage::Count) - 1> translations;
};
constexpr Phrase catalog[]{
#include "translations.inc"
};
static_assert(std::ranges::is_sorted(catalog, {}, &Phrase::english));
static_assert(std::ranges::all_of(catalog, [](const Phrase& phrase) {
    return std::ranges::all_of(phrase.translations, [](const char* text) { return text && *text; });
}));
}

const char* Tr(const char* english) { return Tr(TextLanguage(), english); }

const char* Tr(GameLanguage lang, const char* english) {
    if (!english || !*english) return english;
    if (lang == GameLanguage::English) return english;
    const auto row = std::ranges::lower_bound(catalog, std::string_view{english}, {}, &Phrase::english);
    if (row == std::end(catalog) || row->english != english) return english;
    return row->translations[static_cast<size_t>(lang) - 1];
}

} // namespace Gow3::UI
