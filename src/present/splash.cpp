// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/splash.h"
#include <imgui.h>
#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <optional>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include "orbis/common/elf_info.h"
#include "orbis/core/emulator_settings.h"
#include "ui/gow3_ui_theme.h"
#include "ui/game_language.h"
#include "ui/imgui_layer.h"
#include "ui/imgui_texture.h"
#include "ui/translation.h"
#include "main/host.h"
#include "gpu/shaders/shader_aot.h"
#include "gpu/shaders/shader_cache.h"
#include "present/window.h"

namespace Gow3::Splash {
namespace {
// Before applying its saved preferences the guest opens the console's language WAD.
const char* StartupTr(const char* english) { return UI::Tr(UI::StartupTextLanguage(), english); }
std::atomic<bool> start_menu;
enum class State { Pending, Up, Gone };
std::atomic<State> state{State::Pending};  // of the layer
std::atomic<bool> probe_frames;
std::atomic<std::chrono::steady_clock::rep> first_frame{};

std::string Duration(double seconds) {
    unsigned s = unsigned(std::ceil(seconds));
    if (s >= 60) s = (s + 2) / 5 * 5;  // a long wait is not known to the second
    return std::format("{}:{:02}", s / 60, s % 60);
}

std::string FormatCount(u64 val) {
    std::string s = std::to_string(val);
    for (int n = static_cast<int>(s.length()) - 3; n > 0; n -= 3) s.insert(n, 1, ' ');
    return s;
}


bool AotRunning() {
    auto& cache = Gpu::ShaderCache::Instance();
    return cache.Enabled() && cache.Kind() == Gpu::CacheKind::Aot && Gpu::Aot::phase.load(std::memory_order_acquire) != Gpu::Aot::Phase::Finished;
}

class Layer final : public ImGui::Layer {
public:
    Layer(ImGui::RefCountedTexture texture, bool logos) : texture(std::move(texture)), logos(logos) {}
    void Draw() override {
        if (removed) return;
        // Without an image the splash exists only to carry the shader compilation overlay.
        const auto first = first_frame.load(std::memory_order_acquire);
        const bool intro_ready = first && std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(first) >= std::chrono::seconds(2);
        if (start_menu.load() || (logos && intro_ready) || (!texture && !AotRunning())) {
            probe_frames.store(false, std::memory_order_release);
            removed = true, texture = {};
            state.store(State::Gone, std::memory_order_release);
            RemoveLayer(this);
            return;
        }
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        auto* list = ImGui::GetBackgroundDrawList();
        list->AddRectFilled({}, size, IM_COL32_BLACK);
        if (texture) {
            const auto image = texture.GetTexture();
            const float scale = std::min(size.x / image.width, size.y / image.height);
            const ImVec2 origin{(size.x - image.width * scale) / 2, (size.y - image.height * scale) / 2};
            list->AddImage(image.im_id, origin, {size.x - origin.x, size.y - origin.y});
        }
        DrawProgress(list, size);
    }

private:
    void DrawProgress(ImDrawList* list, ImVec2 size) {
        if (!AotRunning()) return;
        auto& cache = Gpu::ShaderCache::Instance();
        using Gpu::Aot::Phase;
        Phase stage;
        Gpu::ShaderCache::Progress p, base;
        do {
            stage = Gpu::Aot::phase.load(std::memory_order_acquire);
            if (stage == Phase::Finished) return;
            base = Gpu::Aot::phase_start[static_cast<unsigned>(stage)];
            p = cache.Snapshot();
        } while (stage != Gpu::Aot::phase.load(std::memory_order_acquire));
        const auto now = std::chrono::steady_clock::now();
        if (!phase || stage != current) phase = now, current = stage, samples.clear(), finish = -1, last = 0, fraction = 0;
        const double elapsed = std::chrono::duration<double>(now - *phase).count();
        const auto delta = [](auto value, auto start) { return value >= start ? value - start : 0; };
        const bool is_pipelines = stage == Phase::Pipelines;
        const auto total = is_pipelines ? delta(p.pipelines_total, base.pipelines_total) : delta(p.total, base.total);
        const auto done = std::min(is_pipelines ? delta(p.pipelines_done, base.pipelines_done) : delta(p.done, base.done), total);
        const auto total_weight = is_pipelines ? total : delta(p.total_weight, base.total_weight);
        const auto done_weight = is_pipelines ? done : std::min(delta(p.done_weight, base.done_weight), total_weight);
        const bool measured = stage == Phase::Index || stage == Phase::Recipes || stage == Phase::Variants || stage == Phase::Pipelines;
        // A stage with nothing to measure keeps an empty bar (a sweeping glow while it runs); finalizing shows it full.
        if (measured && total_weight) fraction = double(done_weight) / total_weight;
        else if (stage == Phase::Finalizing) fraction = 1;
        const bool indeterminate = !measured && stage != Phase::Finalizing;

        if (measured && total && done < total) {
            if (samples.empty() || elapsed - samples.back().first >= 0.1) samples.push_back({elapsed, double(done_weight)});
            while (samples.size() > 2 && elapsed - samples.front().first > 10) samples.pop_front();
            const double span = samples.back().first - samples.front().first, work = samples.back().second - samples.front().second;
            if (span >= 2 && work > 0) {
                const double estimate = elapsed + double(total_weight - done_weight) * span / work;
                finish = finish < 0 ? estimate : finish + (estimate - finish) * (1 - std::exp(-(elapsed - last) / 4));
            }
        }
        last = elapsed;

        const bool counting = measured && total && done < total;
        const std::string status = !counting ? StartupTr(stage == Phase::Finalizing ? "Saving the cache…" : "Preparing…")
                                   : finish < 0 ? StartupTr("Estimating…")
                                   : fmt::format(fmt::runtime(StartupTr("Time remaining: {}")), Duration(std::max(0.0, finish - elapsed)));
        const auto count = measured && total ? std::format("{} / {}", FormatCount(done), FormatCount(total)) : std::string{};

        const std::string stage_label = stage == Phase::Index      ? StartupTr("Stage I · Reading files")
                                      : stage == Phase::Recipes    ? StartupTr("Stage II · Shader recipes")
                                      : stage == Phase::Catalog    ? StartupTr("Stage III · Reading the catalog")
                                      : stage == Phase::Variants   ? StartupTr("Stage IV · Shaders")
                                      : stage == Phase::Pipelines  ? StartupTr("Stage V · Pipelines")
                                      : stage == Phase::Finalizing ? fmt::format(fmt::runtime(StartupTr("Stage {} · Finalizing")),
                                            Gpu::Aot::has_pipelines_phase.load(std::memory_order_acquire) ? "VI" : "V")
                                      : StartupTr("Starting");

        const float H = size.y, W = size.x;
        const float cqw = std::min(W / 100.0f, H / 56.25f);

        // 1. Smooth deep gradient scrim covering the bottom 64% of the screen
        const ImU32 c0 = IM_COL32(5, 1, 1, 0);
        const ImU32 c1 = IM_COL32(5, 1, 1, 64);
        const ImU32 c2 = IM_COL32(5, 1, 1, 166);
        const ImU32 c3 = IM_COL32(5, 1, 1, 235);
        const ImU32 c4 = IM_COL32(5, 1, 1, 255);

        const float scrim_top = H * 0.360f;
        list->AddRectFilledMultiColor({0, scrim_top}, {W, scrim_top + 0.128f * H}, c0, c0, c1, c1);
        list->AddRectFilledMultiColor({0, scrim_top + 0.128f * H}, {W, scrim_top + 0.243f * H}, c1, c1, c2, c2);
        list->AddRectFilledMultiColor({0, scrim_top + 0.243f * H}, {W, scrim_top + 0.352f * H}, c2, c2, c3, c3);
        list->AddRectFilledMultiColor({0, scrim_top + 0.352f * H}, {W, scrim_top + 0.461f * H}, c3, c3, c4, c4);
        list->AddRectFilled({0, scrim_top + 0.461f * H}, {W, H}, c4);

        auto* font = UI::g_font_cinzel ? UI::g_font_cinzel : ImGui::GetFont();
        const auto calc_w = [&](float px, const char* txt) { return font->CalcTextSizeA(px, FLT_MAX, 0, txt).x; };
        auto* body = UI::g_font_body ? UI::g_font_body : font;
        const auto body_w = [&](float px, const char* txt) { return body->CalcTextSizeA(px, FLT_MAX, 0, txt).x; };

        // 2. Geometry and Spacing calculations
        const float col_w = std::min(W * 0.82f, 58.0f * cqw);
        const float col_left = (W - col_w) * 0.5f;

        const float small_size = std::max(12.0f, 0.98f * cqw);
        const float title_size = std::max(18.0f, 1.75f * cqw);
        const float eta_size   = std::max(12.0f, 0.9f * cqw);
        const float bar_h      = std::max(7.0f, 0.65f * cqw);

        const float bottom_pad = 3.4f * cqw;
        const float y_eta = H - bottom_pad - eta_size;
        const float bar_bot = y_eta - 1.1f * cqw;
        const float bar_top = bar_bot - bar_h;
        const float subrow_y = bar_top - 0.9f * cqw - small_size;
        const float title_y  = subrow_y - 1.6f * cqw - title_size;

        const float omega_r = std::max(14.0f, 1.7f * cqw);
        const float omega_y = title_y - 0.6f * cqw - omega_r;

        const float meander_h = std::max(10.0f, 1.15f * cqw);
        const float meander_w = std::min(W * 0.86f, 64.0f * cqw);
        const float meander_y = (omega_y - omega_r) - 1.4f * cqw - meander_h;
        const float meander_x = (W - meander_w) * 0.5f;

        // Draw Meander ribbon
        UI::DrawGreekMeander(list, {meander_x, meander_y}, meander_w, meander_h, UI::Theme::GoldBronze, 1.8f);

        // Draw Omega symbol with compact aura (Fundo 5 pulse)
        const float time_val = static_cast<float>(ImGui::GetTime());
        const float pulse = 0.5f + 0.5f * std::sin(time_val * 2.1f);
        UI::DrawOmegaSymbol(list, {W * 0.5f, omega_y}, omega_r, UI::Theme::GoldBronze, pulse, true);

        // Draw Title: COMPILING SHADERS
        const char* title_str = StartupTr("Compiling shaders");
        const float title_w = calc_w(title_size, title_str);
        // Shadow
        list->AddText(font, title_size, {(W - title_w) * 0.5f + 1.0f, title_y + 1.5f}, IM_COL32(0, 0, 0, 220), title_str);
        list->AddText(font, title_size, {(W - title_w) * 0.5f, title_y}, UI::Theme::GoldLight, title_str);

        // Draw Subrow: Stage name on left, count on right
        list->AddText(body, small_size, {col_left, subrow_y}, UI::Theme::TextMain, stage_label.c_str());
        if (!count.empty()) {
            const float count_w = body_w(small_size, count.c_str());
            list->AddText(body, small_size, {col_left + col_w - count_w, subrow_y}, UI::Theme::TextMain, count.c_str());
        }

        // Draw Arterial Blood Trench Progress Bar (Clean, no drip pins)
        // Trench background
        list->AddRectFilledMultiColor({col_left, bar_top}, {col_left + col_w, bar_bot},
            UI::Theme::BloodTrenchDark, UI::Theme::BloodTrenchDark,
            UI::Theme::BloodTrenchMid, UI::Theme::BloodTrenchMid);

        // Liquid blood fill
        const float fill_w = std::clamp(col_w * static_cast<float>(fraction), 0.0f, col_w);
        if (indeterminate) {
            const float segment = col_w * 0.22f, t = std::fmod(time_val / 1.8f, 1.0f);
            const float from = col_left - segment + t * (col_w + segment);
            const float left = std::max(col_left, from), right = std::min(col_left + col_w, from + segment);
            if (right > left)
                list->AddRectFilledMultiColor({left, bar_top}, {right, bar_bot}, UI::Theme::BloodCore, UI::Theme::BloodCore, UI::Theme::BloodDeep, UI::Theme::BloodDeep);
        } else if (fill_w > 1.0f) {
            list->AddRectFilledMultiColor({col_left, bar_top}, {col_left + fill_w, bar_bot},
                UI::Theme::BloodCore, UI::Theme::BloodCore,
                UI::Theme::BloodDeep, UI::Theme::BloodDeep);

            const float reference_radius = std::max(bar_h * 1.1f, col_w * 0.035f);
            const float glow_radius = reference_radius * 2.125f;
            const float sweep = std::fmod(time_val / 1.5f, 1.0f);
            const float eased = sweep * sweep * (3.0f - 2.0f * sweep);
            const float glow_x = col_left - reference_radius + eased * (col_w + 2.0f * reference_radius);
            constexpr float stops[] = {-1.0f, -0.5f, -0.12f, 0.12f, 0.5f, 1.0f};
            constexpr float opacity[] = {0.0f, 0.18f, 1.0f, 1.0f, 0.18f, 0.0f};
            for (int i = 0; i < 5; ++i) {
                const float from = glow_x + stops[i] * glow_radius;
                const float to = glow_x + stops[i + 1] * glow_radius;
                const float left = std::max(col_left, from);
                const float right = std::min(col_left + fill_w, to);
                if (right <= left) continue;
                const auto color = [&](float x) {
                    const float a = opacity[i] + (opacity[i + 1] - opacity[i]) * ((x - from) / (to - from));
                    return IM_COL32(255, 255, 255, static_cast<int>(255.0f * a));
                };
                const ImU32 c0 = color(left), c1 = color(right);
                list->AddRectFilledMultiColor({left, bar_top}, {right, bar_bot}, c0, c1, c1, c0);
            }
        }

        // Gold bronze border
        list->AddRect({col_left, bar_top}, {col_left + col_w, bar_bot}, UI::Theme::GoldBronze, 1.0f, 0, 1.5f);

        // Status below the bar: the estimate for the stage, or what it is doing
        list->AddText(body, eta_size, {(W - body_w(eta_size, status.c_str())) * 0.5f, y_eta}, UI::Theme::TextMain, status.c_str());

        if (stage != Phase::Finalizing) DrawSkip(list, size, cqw, y_eta + eta_size * 0.5f, small_size);
    }

    // Holding Circle (or Escape) closes the ring around its button; a full ring leaves the build and restarts the game with JIT
    // shaders for that run (the saved setting stays AOT).
    void DrawSkip(ImDrawList* list, ImVec2 size, float cqw, float center_y, float label_size) {
        constexpr float hold_seconds = 1.2f;
        auto* font = UI::g_font_cinzel ? UI::g_font_cinzel : ImGui::GetFont();
        const char* label = StartupTr(skipping ? "Restarting…" : "Hold to skip");
        // The ring goes around Circle, or around the Escape key's cap with the keyboard and mouse.
        const float radius = std::max(11.0f, 0.95f * cqw), glyph_w = UI::HintGlyphWidth(UI::PS4Symbol::Circle, radius, font);
        const float ring = (ImGui::Core::GamepadPrompts() ? radius : std::hypot(glyph_w * 0.5f, radius * 0.85f)) + std::max(3.0f, 0.3f * cqw);
        const float gap = label_size * 0.9f;  // from the ring, not the button, to the text
        const float width = ring * 2.0f + gap + font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label).x;
        const ImVec2 center{size.x - 4.0f * cqw - width + ring, center_y};

        if (!skipping) {
            const bool held = ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyDown(ImGuiKey_Escape);
            const float step = ImGui::GetIO().DeltaTime;
            hold = held ? std::min(1.0f, hold + step / hold_seconds) : std::max(0.0f, hold - step * 3.0f);
            if (hold >= 1.0f && Window::RequestJitRestart()) skipping = true;
        }
        const float thickness = std::max(2.0f, 0.2f * cqw);
        list->AddCircle(center, ring, IM_COL32(201, 158, 75, 50), 48, thickness);
        if (hold > 0.0f) {
            constexpr float start = -1.5707963f;  // the top, filling clockwise
            list->PathArcTo(center, ring, start, start + 6.2831853f * hold, 48);
            list->PathStroke(UI::Theme::GoldLight, 0, thickness);
        }
        UI::DrawHint(list, center.x - glyph_w * 0.5f, center.y, radius, UI::PS4Symbol::Circle, nullptr, font, label_size);
        list->AddText(font, label_size, {center.x + ring + gap, center_y - label_size * 0.55f}, UI::Theme::GoldLight, label);
    }

    ImGui::RefCountedTexture texture;
    bool logos, removed{};
    std::optional<std::chrono::steady_clock::time_point> phase;
    Gpu::Aot::Phase current = Gpu::Aot::Phase::Waiting;
    std::deque<std::pair<double, double>> samples;
    double finish = -1, last = 0;
    double fraction = 0;
    float hold = 0;      // how much of the skip ring is closed
    bool skipping{};     // the restart was requested
};
std::optional<Layer> layer;
} // namespace

void Show(bool logos, bool wanted) {
    if (!wanted) return state.store(State::Gone, std::memory_order_release);
    ImGui::RefCountedTexture texture;
    for (const char* name : {"pic1.png", "pic0.png"}) {
        const auto image = Common::ElfInfo::Instance().GetGameFolder() / "sce_sys" / name;
        if (std::filesystem::exists(image) && (texture = ImGui::RefCountedTexture::DecodePngFile(image))) break;
        Log(std::string("Cannot load ") + name);
    }
    // No image: a black screen only while the shader compilation overlay has something to show.
    if (!texture && !AotRunning()) {
        state.store(State::Gone, std::memory_order_release);
        return Log("No splash image; starting without a splash screen");
    }
    EmulatorSettings.SetShowSplash(true);
    probe_frames.store(logos, std::memory_order_release);
    layer.emplace(std::move(texture), logos);
    state.store(State::Up, std::memory_order_release);
    ImGui::Layer::AddLayer(&*layer);
}

// The splash is shown once (or never): once it is gone, the priority is restored and nothing is checked again.
void Priority::Step() {
    const State now = state.load(std::memory_order_acquire);
    if (now == State::Up && !raised) {
#ifdef _WIN32
        was = GetThreadPriority(GetCurrentThread()), raised = true;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
        raised = true;
#endif
    } else if (now == State::Gone) {
#ifdef _WIN32
        if (raised) SetThreadPriority(GetCurrentThread(), was);
#endif
        done = true;
    }
}

bool NeedsFrameProbe() { return probe_frames.load(std::memory_order_acquire) && !first_frame.load(std::memory_order_acquire); }
void NonBlackFrame() {
    auto expected = std::chrono::steady_clock::rep{};
    first_frame.compare_exchange_strong(expected, std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
}

void Reveal() { start_menu = true; }
void Dismiss() {
    Reveal();
    MarkLaunchReady();
    Window::NotifyMainMenuReady();
}

bool IsStartMenuMusic(const char* path) { return std::strstr(path, "MENU_LOOP") != nullptr; }
} // namespace Gow3::Splash
