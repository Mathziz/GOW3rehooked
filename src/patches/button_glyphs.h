// SPDX-License-Identifier: GPL-2.0-or-later
// The game's own button prompts with the keys and mouse buttons of the controls: tutorials, the move list, quick-time events
// and the stick and D-pad icons. The prompts are DXT textures in R_UI.WAD (one more copy of a quick-time circle in R_PERM.WAD);
// at startup the ones in TEXTURES are drawn again at the same size and format, and the game reads those bytes in place of the
// originals (FileOverlay). The game folder is never written. Which textures are prompts, and how SpartanPort draws them, come
// from SpartanPort's launcher (launcher-rust/src/runner/glyphs.rs, GPL-2.0); the Muses' lyre keeps its PlayStation shapes.
#pragma once
#include <filesystem>
#include <string_view>
#include <vector>
#include "main/config.h"
#include "orbis/common/types.h"

namespace Gow3 {
namespace ButtonGlyphs {
// Draws the keyboard prompts when the setting asks for them (Auto: no gamepad connected now). Before the game runs.
void Prepare(const std::filesystem::path& game, const Config& config);
} // namespace ButtonGlyphs

namespace FileOverlay {
// Registers bytes that replace the file's from `offset` on. Before the game runs.
void Add(std::string_view guest_path, u64 offset, std::vector<u8> bytes);
// Replaces the bytes a read of the guest file returns where a replacement was registered; `offset` is the file position of
// data[0]. Called by every read of a regular file: a lookup only when some file has replacements.
void Apply(std::string_view guest_path, u64 offset, void* data, u64 size);
} // namespace FileOverlay
} // namespace Gow3
