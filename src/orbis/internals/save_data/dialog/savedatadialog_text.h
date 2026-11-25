// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

#include "ui/game_language.h"

namespace Libraries::SaveData::Dialog {

struct SaveText {
    const char *save_game, *load_game, *delete_save, *duplicate_save, *overwrite_save, *new_save;
    const char *empty_slot, *no_data, *auto_save, *manual_save, *corrupted;
    const char *difficulty, *play_time, *date_saved, *file_size, *folder, *condition, *save_count;
    const char *duplicate, *remove, *create, *select, *back, *open_folder, *confirm, *yes, *no, *ok, *cancel;
    const char *ask_save, *ask_load, *ask_delete, *ask_duplicate, *ask_overwrite;
    const char *saving, *loading, *deleting, *restoring, *saved, *loaded, *deleted;
    const char *corrupt_data, *corrupt_delete, *corrupt_create, *corrupt_restore, *no_space, *limit;
    const char *error, *unknown, *load_failed, *cannot_save, *cannot_load, *cannot_delete, *cannot_duplicate;
};

const SaveText& GetSaveText(Gow3::UI::GameLanguage language);

struct SaveMetadata {
    std::string_view difficulty;
    std::string_view play_time;
};

SaveMetadata ParseSaveMetadata(std::string_view details);
std::string_view StripAutosavePrefix(std::string_view subtitle);

} // namespace Libraries::SaveData::Dialog
