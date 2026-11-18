// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/prompt.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include "ui/gow3_ui_theme.h"
#include "ui/imgui_layer.h"

namespace Gow3::Prompt {
namespace {
Answer answer{Answer::Pending};

class Layer final : public ImGui::Layer {
public:
    void SetContent(Content next) {
        content = std::move(next);
        answer = Answer::Pending;
        armed = false;
    }

    void Draw() override {
        using namespace ImGui;
        const ImVec2 size = GetIO().DisplaySize;
        const float cqw = std::min(size.x / 100.0f, size.y / 56.25f);
        auto* list = GetBackgroundDrawList();
        auto* font = UI::g_font_cinzel ? UI::g_font_cinzel : GetFont();
        auto* body = UI::g_font_body ? UI::g_font_body : font;
        UI::DrawBasaltBackground(list, {0, 0}, size);

        const auto centered = [&](ImFont* f, float px, float y, ImU32 color, const std::string& text, bool shadow = false) {
            const float wrap = std::max(1.0f, size.x * 0.84f);
            const ImVec2 extent = f->CalcTextSizeA(px, FLT_MAX, wrap, text.c_str());
            const float x = (size.x - extent.x) * 0.5f;
            if (shadow) list->AddText(f, px, {x + 1.0f, y + 1.5f}, IM_COL32(0, 0, 0, 220), text.c_str(), nullptr, wrap);
            list->AddText(f, px, {x, y}, color, text.c_str(), nullptr, wrap);
            return extent.y;
        };
        const float title_size = std::max(18.0f, 2.0f * cqw), ask_size = std::max(15.0f, 1.45f * cqw);
        const float note_size = std::max(13.0f, 1.05f * cqw);
        const float button_size = std::max(15.0f, 1.3f * cqw);
        const float button_scale = content.step_layout ? std::min(1.0f, size.x / (40.0f * button_size)) : 1.0f;
        const float label_size = button_size * button_scale, radius = std::max(13.0f, 1.2f * cqw) * button_scale;
        const float omega_r = std::max(16.0f, 2.0f * cqw);
        float y = size.y * 0.5f - 12.0f * cqw;
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(GetTime()) * 2.1f);
        UI::DrawOmegaSymbol(list, {size.x * 0.5f, y + omega_r}, omega_r, UI::Theme::GoldBronze, pulse, true);
        y += omega_r * 2.0f + 2.0f * cqw;
        y += centered(font, title_size, y, UI::Theme::GoldLight, content.title, true) + 2.2f * cqw;
        y += centered(content.step_layout ? body : font, ask_size, y, UI::Theme::TextMain, content.question) + 1.8f * cqw;
        const float notes_top = y;
        for (const auto& note : content.notes) {
            if (note.empty()) continue;
            y += centered(body, note_size, y, UI::Theme::TextMuted, note) + 0.5f * note_size;
        }
        if (content.step_layout) y = std::max(y, notes_top + content.notes.size() * 1.5f * note_size);
        y += (content.step_layout ? 2.5f : 4.0f) * cqw - note_size * 0.5f;
        if (content.status) return;

        struct Button { UI::PS4Symbol symbol; const char* label; Answer result; ImGuiKey pad, key; };
        const bool custom = !content.actions[0].empty();
        const Button buttons[]{
            {UI::PS4Symbol::Cross, custom ? content.actions[0].c_str() : content.choice ? "YES" : "OK",
             Answer::Yes, ImGuiKey_GamepadFaceDown, ImGuiKey_Enter},
            {UI::PS4Symbol::Square, content.actions[1].c_str(), Answer::No, ImGuiKey_GamepadFaceLeft, ImGuiKey_Delete},
            {UI::PS4Symbol::Circle, custom ? content.actions[2].c_str() : content.choice ? "NO" : "",
             custom ? Answer::Back : Answer::No, ImGuiKey_GamepadFaceRight, ImGuiKey_Escape},
        };
        const float gap = 6.0f * cqw, column = 9.0f * label_size;
        float widths[3]{}, total = -gap;
        for (size_t i = 0; i < std::size(buttons); ++i) {
            if (!*buttons[i].label) continue;
            widths[i] = UI::HintWidth(buttons[i].symbol, radius, buttons[i].label, font, label_size);
            total += widths[i] + gap;
        }
        float x = (size.x - total) * 0.5f;
        Answer chosen = Answer::Pending;
        for (size_t i = 0; i < std::size(buttons); ++i) {
            if (!widths[i]) continue;
            const auto& b = buttons[i];
            ImRect rect{{x, y}, {x + widths[i], y + radius * 2.0f}};
            if (content.step_layout) {
                // Step pages center each key in a fixed column with its label below, so nothing moves between pages.
                const float center = size.x * 0.5f + (static_cast<float>(i) - 1.0f) * column;
                const float key = UI::HintGlyphWidth(b.symbol, radius, font);
                const float text = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, b.label).x;
                const float label_y = y + radius * 2.0f + 0.35f * label_size, half = std::max(key, text) * 0.5f;
                UI::DrawHint(list, center - key * 0.5f, y + radius, radius, b.symbol, nullptr, font, label_size);
                list->AddText(font, label_size, {center - text * 0.5f, label_y}, UI::Theme::GoldLight, b.label);
                rect = {{center - half, y}, {center + half, label_y + label_size}};
            } else {
                UI::DrawHint(list, x, y + radius, radius, b.symbol, b.label, font, label_size);
            }
            if (armed && (IsKeyPressed(b.pad, false) || IsKeyPressed(b.key, false) || (IsMouseClicked(ImGuiMouseButton_Left) && rect.Contains(GetIO().MousePos))))
                chosen = b.result;
            x += widths[i] + gap;
        }
        // A message with nothing to choose is closed by either button.
        if (!custom && !content.choice && armed &&
            (IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || IsKeyPressed(ImGuiKey_Escape, false))) chosen = Answer::Yes;
        // The press that was held when the window opened must not answer it.
        if (!armed) armed = std::ranges::none_of(buttons, [](const Button& b) { return IsKeyDown(b.pad) || IsKeyDown(b.key); });
        if (chosen != Answer::Pending) answer = chosen;
    }

private:
    Content content;
    bool armed{};
};
Layer layer;
bool visible{};
} // namespace

void Show(Content content) {
    layer.SetContent(std::move(content));
    if (!visible) ImGui::Layer::AddLayer(&layer);
    visible = true;
}
void Hide() {
    if (visible) ImGui::Layer::RemoveLayer(&layer);
    visible = false;
}
Answer Result() { return answer; }
} // namespace Gow3::Prompt
