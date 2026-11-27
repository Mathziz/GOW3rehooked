// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/fps_overlay.h"
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include "main/config.h"
#include "main/host.h"
#include "ui/gow3_ui_theme.h"
#include "ui/imgui_layer.h"

namespace Gow3::FpsOverlay {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<unsigned long long> flips{};
std::atomic<long long> last_flip_ns{}, longest_ns{};  // since the clock's epoch; the longest gap between flips this window
std::atomic<bool> shown{};

long long Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

class Layer final : public ImGui::Layer {
public:
    // Passive: the counter does not keep the UI awake, take input or present frames of its own.
    bool ShouldKeepDrawing() override { return false; }

    void Draw() override {
        const long long now = Now();
        if (now - window_start >= 500'000'000) {  // a new reading every half second
            const auto count = flips.load(std::memory_order_relaxed);
            const double seconds = double(now - window_start) / 1e9;
            fps = window_start ? double(count - window_flips) / seconds : 0.0;
            frame_ms = count > window_flips ? seconds * 1000.0 / double(count - window_flips) : 0.0;
            worst_ms = double(longest_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
            window_start = now, window_flips = count;
        }
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float u = std::min(size.x / 100.0f, size.y / 56.25f);
        auto* list = ImGui::GetForegroundDrawList();
        auto* font = UI::g_font_body ? UI::g_font_body : ImGui::GetFont();
        char big[16], small[48];
        std::snprintf(big, sizeof(big), "%.0f FPS", fps);
        std::snprintf(small, sizeof(small), "%.1f ms  ·  max %.0f ms", frame_ms, worst_ms);
        const float big_px = std::max(14.0f, 1.05f * u), small_px = std::max(11.0f, 0.68f * u), pad = std::max(5.0f, 0.4f * u);
        const ImVec2 big_size = font->CalcTextSizeA(big_px, FLT_MAX, 0.0f, big), small_size = font->CalcTextSizeA(small_px, FLT_MAX, 0.0f, small);
        const ImVec2 lo{std::round(0.8f * u), std::round(0.8f * u)};
        const ImVec2 hi{std::round(lo.x + std::max(big_size.x, small_size.x) + 2.0f * pad), std::round(lo.y + big_size.y + small_size.y + 2.0f * pad)};
        list->AddRectFilled(lo, hi, IM_COL32(5, 1, 1, 170), 3.0f);
        list->AddRectFilled(lo, {lo.x + std::max(2.0f, 0.12f * u), hi.y}, UI::Theme::GoldBronze);
        // Green at the selected rate or near it, gold a little below, red well below.
        const ImU32 color = fps >= target * 0.95 ? IM_COL32(150, 220, 120, 255) : fps >= target * 0.75 ? UI::Theme::GoldLight : IM_COL32(242, 110, 100, 255);
        list->AddText(font, big_px, {lo.x + pad + 1.0f, lo.y + pad + 1.0f}, IM_COL32(0, 0, 0, 200), big);
        list->AddText(font, big_px, {lo.x + pad, lo.y + pad}, color, big);
        list->AddText(font, small_px, {lo.x + pad, lo.y + pad + big_size.y}, UI::Theme::TextMuted, small);
    }

    double target = 60.0;

private:
    long long window_start{};
    unsigned long long window_flips{};
    double fps{}, frame_ms{}, worst_ms{};
};
Layer layer;
} // namespace

void Flip() {
    if (!shown.load(std::memory_order_relaxed)) return;
    flips.fetch_add(1, std::memory_order_relaxed);
    const long long now = Now(), previous = last_flip_ns.exchange(now, std::memory_order_relaxed);
    // A gap of more than two seconds is a pause (a menu, loading), not a frame.
    if (previous && now - previous < 2'000'000'000) {
        long long longest = longest_ns.load(std::memory_order_relaxed);
        while (now - previous > longest && !longest_ns.compare_exchange_weak(longest, now - previous, std::memory_order_relaxed)) {}
    }
}

void Show(bool show) {
    if (shown.exchange(show) == show) return;
    if (show) {
        layer.target = GameFrameRate(LoadConfig(PortDirectory() / PortDir::Settings / "config.cfg").fps);
        last_flip_ns = 0, longest_ns = 0;
        ImGui::Layer::AddLayer(&layer);
    } else {
        ImGui::Layer::RemoveLayer(&layer);
    }
}

void Toggle() {
    const bool show = !shown.load();
    Show(show);
    try {
        SaveConfigValue(PortDirectory() / PortDir::Settings / "config.cfg", "fps_overlay", show ? "true" : "false");
    } catch (const std::exception& error) {
        Log(std::string("FPS counter: could not save the choice: ") + error.what());
    }
}
} // namespace Gow3::FpsOverlay
