// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/prompt.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "ui/gow3_ui_theme.h"
#include "ui/imgui_layer.h"

namespace Gow3::Prompt {
namespace {
std::atomic<Answer> answer{Answer::Pending};

class Layer final : public ImGui::Layer {
public:
    explicit Layer(const Content& content) : content(content) {}

    void Draw() override {
        using namespace ImGui;
        const ImVec2 size = GetIO().DisplaySize;
        const float cqw = std::min(size.x / 100.0f, size.y / 56.25f);
        auto* list = GetBackgroundDrawList();
        auto* font = UI::g_font_cinzel ? UI::g_font_cinzel : GetFont();
        auto* body = UI::g_font_body ? UI::g_font_body : font;
        UI::DrawBasaltBackground(list, {0, 0}, size);

        const auto centered = [&](ImFont* f, float px, float y, ImU32 color, const char* text, bool shadow = false) {
            const float x = (size.x - f->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x) * 0.5f;
            if (shadow) list->AddText(f, px, {x + 1.0f, y + 1.5f}, IM_COL32(0, 0, 0, 220), text);
            list->AddText(f, px, {x, y}, color, text);
        };
        const float title_size = std::max(18.0f, 2.0f * cqw), ask_size = std::max(15.0f, 1.45f * cqw);
        const float note_size = std::max(13.0f, 1.05f * cqw), radius = std::max(13.0f, 1.2f * cqw), label_size = std::max(15.0f, 1.3f * cqw);
        const float omega_r = std::max(16.0f, 2.0f * cqw);
        float y = size.y * 0.5f - 12.0f * cqw;
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(GetTime()) * 2.1f);
        UI::DrawOmegaSymbol(list, {size.x * 0.5f, y + omega_r}, omega_r, UI::Theme::GoldBronze, pulse, true);
        y += omega_r * 2.0f + 2.0f * cqw;
        centered(font, title_size, y, UI::Theme::GoldLight, content.title, true);
        y += title_size + 2.2f * cqw;
        centered(font, ask_size, y, UI::Theme::TextMain, content.question);
        y += ask_size + 1.8f * cqw;
        for (const char* note : content.notes) {
            if (!*note) continue;
            centered(body, note_size, y, UI::Theme::TextMuted, note);
            y += note_size * 1.5f;
        }
        y += 4.0f * cqw - note_size * 0.5f;

        struct Button { UI::PS4Symbol symbol; const char* label; Answer result; ImGuiKey pad, key; };
        const Button choices[]{{UI::PS4Symbol::Cross, "YES", Answer::Yes, ImGuiKey_GamepadFaceDown, ImGuiKey_Enter},
                               {UI::PS4Symbol::Circle, "NO", Answer::No, ImGuiKey_GamepadFaceRight, ImGuiKey_Escape}};
        const Button ok[]{{UI::PS4Symbol::Cross, "OK", Answer::Yes, ImGuiKey_GamepadFaceDown, ImGuiKey_Enter}};
        const Button* buttons = content.choice ? choices : ok;
        const int count = content.choice ? 2 : 1;
        const float gap = 6.0f * cqw;
        float widths[2], total = gap * (count - 1);
        for (int i = 0; i < count; ++i) total += widths[i] = UI::HintWidth(buttons[i].symbol, radius, buttons[i].label, font, label_size);
        float x = (size.x - total) * 0.5f;
        Answer chosen = Answer::Pending;
        for (int i = 0; i < count; ++i) {
            const auto& b = buttons[i];
            UI::DrawHint(list, x, y + radius, radius, b.symbol, b.label, font, label_size);
            const ImRect rect{{x, y}, {x + widths[i], y + radius * 2.0f}};
            if (armed && (IsKeyPressed(b.pad, false) || IsKeyPressed(b.key, false) || (IsMouseClicked(ImGuiMouseButton_Left) && rect.Contains(GetIO().MousePos))))
                chosen = b.result;
            x += widths[i] + gap;
        }
        // A message with nothing to choose is closed by either button.
        if (!content.choice && armed && (IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || IsKeyPressed(ImGuiKey_Escape, false))) chosen = Answer::Yes;
        // The press that was held when the window opened must not answer it.
        if (!armed) armed = !IsKeyDown(ImGuiKey_GamepadFaceDown) && !IsKeyDown(ImGuiKey_GamepadFaceRight) && !IsKeyDown(ImGuiKey_Enter) && !IsKeyDown(ImGuiKey_Escape);
        if (chosen != Answer::Pending) answer = chosen;
    }

private:
    Content content;
    bool armed{};
};
} // namespace

void Show(const Content& content) {
    static Layer layer(content);
    ImGui::Layer::AddLayer(&layer);
}
Answer Result() { return answer; }
} // namespace Gow3::Prompt
