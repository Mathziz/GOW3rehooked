// SPDX-License-Identifier: GPL-2.0-or-later
// The full-screen message shown before the game starts (a missing game, a crashed last run): a title, a question and up to two
// notes, answered with Cross (Enter, click) or Circle (Escape, click). Without `choice` there is a single OK button.
#pragma once

namespace Gow3::Prompt {
enum class Answer { Pending, Yes, No };
struct Content {
    const char *title, *question, *notes[2];  // string literals; an empty note is skipped
    bool choice;                              // Yes / No instead of OK
};
void Show(const Content& content);
Answer Result();
} // namespace Gow3::Prompt
