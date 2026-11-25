// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string>

namespace Gow3::Prompt {
enum class Answer { Pending, Yes, No, Back };
struct Content {
    std::string title, question;
    std::array<std::string, 2> notes;
    bool choice{}; // Yes/No instead of OK
    std::array<std::string, 3> actions; // Custom Cross, Square and Circle labels
    bool status{}; // No buttons
    bool step_layout{};
    bool navigate{}; // Arrows or WASD select an answer, then Enter accepts it.
};
void Show(Content content);
void Hide();
Answer Result();
} // namespace Gow3::Prompt
