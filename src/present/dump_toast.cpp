// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/dump_toast.h"
#include "ui/translation.h"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include "main/host.h"
#include "ui/gow3_ui_theme.h"
#include "ui/imgui_layer.h"

namespace Gow3::DumpToast {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kTurn = 0.9;                             // seconds per spinner turn
constexpr float kFadeIn = 0.18f, kHold = 3.0f, kFadeOut = 0.4f;  // seconds: appearing, "saved" shown, disappearing
constexpr float kArc = 250.0f / 360.0f;                    // of the ring, the spinning arc
constexpr float kTau = 6.2831853f;

// The parts open and the layer's state: Begin and End on the GPU thread and the image threads, Draw on the window's.
std::mutex mutex;
unsigned busy = 0;
bool up = false;
Clock::time_point shown, saved;  // the toast appeared; the last part ended (while busy is 0)

float Seconds(Clock::duration d) { return std::chrono::duration<float>(d).count(); }

ImU32 Faded(ImU32 color, float alpha) {
    const ImU32 a = ImU32(float(color >> IM_COL32_A_SHIFT & 0xFF) * alpha + 0.5f);
    return (color & ~IM_COL32_A_MASK) | a << IM_COL32_A_SHIFT;
}

class Layer final : public ImGui::Layer {
public:
    void Draw() override {
        const auto now = Clock::now();
        bool done;
        float alpha;
        double turns;
        {
            std::scoped_lock lock(mutex);
            done = busy == 0;
            alpha = std::min(1.0f, Seconds(now - shown) / kFadeIn);
            turns = std::chrono::duration<double>(now - shown).count() / kTurn;
            if (done) {
                const float left = kHold + kFadeOut - Seconds(now - saved);
                if (left <= 0) {
                    up = false;
                    RemoveLayer(this);
                    return;
                }
                alpha = std::min(alpha, left / kFadeOut);
            }
        }
        // Sizes in the splash's cqw: a hundredth of a 16:9 window's width.
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float u = std::min(size.x / 100.0f, size.y / 56.25f);
        auto* list = ImGui::GetForegroundDrawList();  // no window: the toast takes no input
        auto* font = UI::g_font_cinzel ? UI::g_font_cinzel : ImGui::GetFont();
        auto* body = UI::g_font_body ? UI::g_font_body : font;
        // The folder only: a large dump is split into several .zip parts.
        static const std::string folder = std::string(PortDir::Root) + "/";
        const char* title = UI::Tr(done ? "Debug dump saved" : "Saving debug dump");
        const char* note = done ? folder.c_str() : UI::Tr("Don't close the game");

        const float title_px = std::max(12.0f, 0.8f * u), note_px = std::max(11.0f, 0.62f * u);
        const ImVec2 title_size = font->CalcTextSizeA(title_px, FLT_MAX, 0.0f, title);
        const ImVec2 note_size = body->CalcTextSizeA(note_px, FLT_MAX, 0.0f, note);
        const float border = std::max(1.0f, 0.05f * u), bar = std::max(2.0f, 0.16f * u), stroke = std::max(2.0f, 0.12f * u);
        const float box = 0.75f * u, gap = 0.65f * u, lead = 0.12f * u;  // box: half the spinner's square; lead: title to note
        const float text_h = title_size.y + lead + note_size.y;
        const float w = bar + 0.7f * u + 2.0f * box + gap + std::max(title_size.x, note_size.x) + 0.95f * u;
        const float h = std::max(2.0f * box, text_h) + 1.0f * u;
        const ImVec2 lo{std::round(1.6f * u), std::round(size.y - 1.6f * u - h)}, hi{std::round(lo.x + w), std::round(lo.y + h)};

        list->AddRectFilled(lo, hi, Faded(UI::Theme::ObsidianBg, alpha * 220 / 255));
        list->AddRect(lo, hi, Faded(UI::Theme::GoldBronze, alpha * 140 / 255), 0.0f, border);
        list->AddRectFilled(lo, {lo.x + bar, hi.y}, Faded(UI::Theme::GoldBronze, alpha));

        // The spinner: a faint ring (0.8 of its square) with a 250° arc turning on it; once saved, the whole ring and a check.
        const ImVec2 c{lo.x + bar + 0.7f * u + box, (lo.y + hi.y) * 0.5f};
        const float r = 0.8f * box;
        if (!done) {
            list->AddCircle(c, r, Faded(UI::Theme::GoldBronze, alpha * 50 / 255), 48, stroke);
            const float start = float(turns - std::floor(turns)) * kTau - kTau / 4;
            list->PathArcTo(c, r, start, start + kArc * kTau, 40);
            list->PathStroke(Faded(UI::Theme::GoldBronze, alpha), stroke);
        } else {
            list->AddCircle(c, r, Faded(UI::Theme::GoldBronze, alpha), 48, stroke);
            const float s = box / 50.0f;  // the check's points on a 100-unit square
            const ImVec2 check[]{{c.x - 19 * s, c.y + 2 * s}, {c.x - 6 * s, c.y + 15 * s}, {c.x + 19 * s, c.y - 13 * s}};
            list->AddPolyline(check, 3, Faded(UI::Theme::GoldLight, alpha), stroke * 1.15f);
        }

        const float x = c.x + box + gap, y = std::round(c.y - text_h * 0.5f);
        list->AddText(font, title_px, {x + 1.0f, y + 1.5f}, Faded(IM_COL32(0, 0, 0, 220), alpha), title);
        list->AddText(font, title_px, {x, y}, Faded(UI::Theme::GoldLight, alpha), title);
        list->AddText(body, note_px, {x, y + title_size.y + lead}, Faded(UI::Theme::TextMuted, alpha), note);
    }
};
Layer layer;
} // namespace

void Begin() {
    std::scoped_lock lock(mutex);
    ++busy;
    if (!up) up = true, shown = Clock::now(), ImGui::Layer::AddLayer(&layer);
}

void End() {
    std::scoped_lock lock(mutex);
    if (busy && --busy == 0) saved = Clock::now();
}
} // namespace Gow3::DumpToast
