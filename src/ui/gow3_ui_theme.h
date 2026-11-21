// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>
#include "ui/imgui_core.h"

namespace Gow3::UI {

inline ImFont* g_font_cinzel = nullptr;
inline ImFont* g_font_cinzel_dec = nullptr;
inline ImFont* g_font_body = nullptr;
inline ImFont* g_font_pad = nullptr;

namespace Theme {
    constexpr ImU32 GoldBronze      = IM_COL32(201, 158, 75, 255);       // #c99e4b
    constexpr ImU32 GoldLight       = IM_COL32(254, 243, 199, 255);      // #fef3c7
    constexpr ImU32 GoldMuted       = IM_COL32(168, 138, 77, 255);       // #a88a4d
    constexpr ImU32 GoldVein        = IM_COL32(201, 158, 75, 14);        // 5.5% opacity marble vein

    constexpr ImU32 BloodCore       = IM_COL32(239, 68, 68, 255);        // #ef4444
    constexpr ImU32 BloodMid        = IM_COL32(185, 28, 28, 255);        // #b91c1c
    constexpr ImU32 BloodDeep       = IM_COL32(80, 7, 7, 255);           // #500707
    constexpr ImU32 BloodTrenchDark = IM_COL32(13, 3, 3, 255);           // #0d0303
    constexpr ImU32 BloodTrenchMid  = IM_COL32(30, 7, 5, 255);           // #1e0705

    constexpr ImU32 ObsidianBg      = IM_COL32(5, 1, 1, 255);             // #050101
    constexpr ImU32 TextMain        = IM_COL32(245, 238, 220, 255);       // #f5eedc
    constexpr ImU32 TextMuted       = IM_COL32(156, 145, 131, 255);       // #9c9183

    constexpr ImU32 PS4Blue         = IM_COL32(93, 151, 245, 255);        // #5d97f5 Cross
    constexpr ImU32 PS4Red          = IM_COL32(242, 90, 90, 255);         // #f25a5a Circle
    constexpr ImU32 PS4Pink         = IM_COL32(226, 140, 190, 255);       // #e28cbe Square
    constexpr ImU32 PS4Green        = IM_COL32(94, 204, 160, 255);        // #5eCCa0 Triangle
    constexpr ImU32 PS4BtnBg        = IM_COL32(21, 21, 26, 255);          // #15151a
    constexpr ImU32 PS4BtnBorder    = IM_COL32(54, 54, 68, 255);          // #363644
}

// Draw true continuous Greek meander ribbon (uniform spacing, zero overlap, zero gaps)
inline void DrawGreekMeander(ImDrawList* list, ImVec2 pos, float width, float height, ImU32 color = Theme::GoldBronze, float thickness = 1.8f) {
    if (width <= 0.0f || height <= 0.0f) return;
    const float S = height / 4.0f;
    const float unit_w = 5.0f * S;
    const int count = std::max(1, static_cast<int>(width / unit_w));
    const float total_w = count * unit_w;
    const float x_start = pos.x + (width - total_w) * 0.5f;

    std::vector<ImVec2> pts;
    pts.reserve(count * 9 + 2);

    for (int i = 0; i < count; ++i) {
        const float x = x_start + i * unit_w;
        const float y_bot = pos.y + height;
        const float y_top = pos.y;
        const float y_mid_bot = pos.y + 3.0f * S;
        const float y_mid_top = pos.y + 1.5f * S;

        if (i == 0) {
            pts.push_back({x, y_bot});
        }
        pts.push_back({x + S, y_bot});
        pts.push_back({x + S, y_top});
        pts.push_back({x + 4.0f * S, y_top});
        pts.push_back({x + 4.0f * S, y_mid_bot});
        pts.push_back({x + 2.0f * S, y_mid_bot});
        pts.push_back({x + 2.0f * S, y_mid_top});
        pts.push_back({x + 3.0f * S, y_mid_top});
        pts.push_back({x + 3.0f * S, y_bot});
        pts.push_back({x + 5.0f * S, y_bot});
    }

    if (pts.size() > 1) {
        list->AddPolyline(pts.data(), static_cast<int>(pts.size()), color, 0, thickness);
    }
}

inline void DrawSoftGlow(ImDrawList* list, ImVec2 center, float radius, ImU32 rgb, float opacity) {
    constexpr int segments = 48, rings = 10;
    const ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
    list->PrimReserve(segments * (rings * 6 - 3), 1 + segments * rings);
    const ImDrawIdx base = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
    list->PrimWriteVtx(center, uv, rgb | (static_cast<ImU32>(opacity) << IM_COL32_A_SHIFT));
    for (int ring = 1; ring <= rings; ++ring) {
        const float t = static_cast<float>(ring) / rings;
        const float falloff = (1.0f - t * t) * (1.0f - t * t);
        const ImU32 color = rgb | (static_cast<ImU32>(opacity * falloff) << IM_COL32_A_SHIFT);
        for (int i = 0; i < segments; ++i) {
            const float angle = 6.28318530718f * i / segments;
            list->PrimWriteVtx({center.x + radius * t * std::cos(angle),
                                center.y + radius * t * std::sin(angle)}, uv, color);
        }
    }
    for (int i = 0; i < segments; ++i) {
        list->PrimWriteIdx(base);
        list->PrimWriteIdx(base + 1 + i);
        list->PrimWriteIdx(base + 1 + (i + 1) % segments);
    }
    for (int ring = 1; ring < rings; ++ring) {
        const int inner = 1 + (ring - 1) * segments, outer = inner + segments;
        for (int i = 0; i < segments; ++i) {
            const int next = (i + 1) % segments;
            list->PrimWriteIdx(base + inner + i); list->PrimWriteIdx(base + outer + i); list->PrimWriteIdx(base + outer + next);
            list->PrimWriteIdx(base + inner + i); list->PrimWriteIdx(base + outer + next); list->PrimWriteIdx(base + inner + next);
        }
    }
}

inline void DrawOmegaSymbol(ImDrawList* list, ImVec2 center, float radius, ImU32 color = Theme::GoldBronze, float pulse = 0.5f, bool with_glow = true) {
    if (radius <= 0.0f) return;

    if (with_glow) {
        const float p = std::clamp(pulse, 0.0f, 1.0f);
        DrawSoftGlow(list, center, radius * 1.45f, IM_COL32(201, 130, 65, 0), 28.0f + 8.0f * p);
    }

    // Render the authentic Cinzel Decorative Omega glyph
    const float font_size = radius * 2.2f;
    ImFont* font = g_font_cinzel_dec ? g_font_cinzel_dec : (g_font_cinzel ? g_font_cinzel : ImGui::GetFont());

    const char* omega_str = "\xCE\xA9"; // UTF-8 Greek Omega (U+03A9)
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, omega_str);
    const ImVec2 text_pos{center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f};

    // Soft subtle shadow
    list->AddText(font, font_size, {text_pos.x + 1.0f, text_pos.y + 1.5f}, IM_COL32(0, 0, 0, 220), omega_str);
    // Golden Omega
    list->AddText(font, font_size, text_pos, color, omega_str);
}

enum class PS4Symbol { Cross, Circle, Square, Triangle };

inline void DrawPadFaceButton(ImDrawList* list, ImVec2 center, float radius, PS4Symbol symbol) {
    // Outer disc
    list->AddCircleFilled(center, radius, Theme::PS4BtnBg, 32);
    // Outer border
    list->AddCircle(center, radius, Theme::PS4BtnBorder, 32, 1.5f);
    // Subtle inner bevel
    list->AddCircle(center, radius - 1.2f, IM_COL32(255, 255, 255, 22), 32, 1.0f);

    const float d = radius * 0.44f;
    if (!ImGui::Core::PlayStationPrompts()) {
        const float scale = radius * 0.50f;
        const ImVec2 origin{std::round(center.x * 2) * 0.5f, std::round(center.y * 2) * 0.5f};
        const auto point = [&](float x, float y) { return origin + ImVec2{x * scale, y * scale}; };
        // Filled contours give the letters one continuous edge at their small in-game size.
        const auto fill = [&](ImU32 color, std::initializer_list<ImVec2> contour) {
            for (const auto p : contour) list->PathLineTo(point(p.x, p.y));
            list->PathFillConcave(color);
        };
        switch (symbol) {
        case PS4Symbol::Cross:
            fill(IM_COL32(123, 185, 100, 255), {{-0.21f, -1}, {0.21f, -1}, {0.98f, 1}, {0.57f, 1}, {0.40f, 0.54f},
                                               {-0.40f, 0.54f}, {-0.57f, 1}, {-0.98f, 1}});
            list->AddTriangleFilled(point(-0.27f, 0.18f), point(0, -0.53f), point(0.27f, 0.18f), Theme::PS4BtnBg);
            break;
        case PS4Symbol::Circle: {
            list->PathLineTo(point(-0.79f, -1));
            list->PathLineTo(point(0.06f, -1));
            list->PathBezierCubicCurveTo(point(0.50f, -1), point(0.74f, -0.81f), point(0.74f, -0.54f), 6);
            list->PathBezierCubicCurveTo(point(0.74f, -0.34f), point(0.63f, -0.17f), point(0.43f, -0.08f), 6);
            list->PathBezierCubicCurveTo(point(0.69f, -0.04f), point(0.85f, 0.15f), point(0.85f, 0.42f), 6);
            list->PathBezierCubicCurveTo(point(0.85f, 0.80f), point(0.57f, 1), point(0.09f, 1), 6);
            list->PathLineTo(point(-0.79f, 1));
            list->PathFillConcave(IM_COL32(221, 106, 109, 255));
            const auto counter = [&](float top, float bottom, float right) {
                list->PathLineTo(point(-0.39f, top));
                list->PathLineTo(point(0.03f, top));
                list->PathBezierCubicCurveTo(point(right, top), point(right, bottom), point(0.03f, bottom), 8);
                list->PathLineTo(point(-0.39f, bottom));
                list->PathFillConvex(Theme::PS4BtnBg);
            };
            counter(-0.68f, -0.20f, 0.50f);
            counter(0.12f, 0.68f, 0.63f);
            break;
        }
        case PS4Symbol::Square:
            fill(Theme::PS4Blue, {{-0.92f, -1}, {-0.45f, -1}, {0, -0.34f}, {0.45f, -1}, {0.92f, -1}, {0.24f, 0},
                                 {0.92f, 1}, {0.45f, 1}, {0, 0.34f}, {-0.45f, 1}, {-0.92f, 1}, {-0.24f, 0}});
            break;
        case PS4Symbol::Triangle:
            fill(IM_COL32(223, 186, 89, 255), {{-0.93f, -1}, {-0.47f, -1}, {0, -0.25f}, {0.47f, -1}, {0.93f, -1},
                                             {0.20f, 0.13f}, {0.20f, 1}, {-0.20f, 1}, {-0.20f, 0.13f}});
            break;
        }
    } else if (symbol == PS4Symbol::Cross) {
        list->AddLine({center.x - d, center.y - d}, {center.x + d, center.y + d}, Theme::PS4Blue, 2.6f);
        list->AddLine({center.x + d, center.y - d}, {center.x - d, center.y + d}, Theme::PS4Blue, 2.6f);
    } else if (symbol == PS4Symbol::Triangle) {
        list->AddTriangle({center.x, center.y - d * 1.15f}, {center.x + d * 1.1f, center.y + d * 0.85f}, {center.x - d * 1.1f, center.y + d * 0.85f}, Theme::PS4Green, 2.6f);
    } else if (symbol == PS4Symbol::Square) {
        list->AddRect({center.x - d, center.y - d}, {center.x + d, center.y + d}, Theme::PS4Pink, 0.0f, 0, 2.6f);
    } else {
        list->AddCircle(center, d, Theme::PS4Red, 24, 2.6f);
    }
}

// A keyboard key, drawn like the pad's buttons: a cap of height `h` over its darker side, as wide as its label needs (at
// least square, the label shrunk to fit `max_w`), or an arrow for an arrow key (`arrow` its direction).
inline float KeyCapWidth(const char* label, float h, ImFont* font) {
    return std::max(h, font->CalcTextSizeA(h * 0.46f, FLT_MAX, 0.0f, label).x + h * 0.62f);
}
inline void DrawKeyCap(ImDrawList* list, ImVec2 c, float h, const char* label, ImFont* font, float max_w = FLT_MAX, ImVec2 arrow = {}) {
    const bool arrowed = arrow.x != 0.0f || arrow.y != 0.0f;
    const float w = std::min(arrowed ? h : KeyCapWidth(label, h, font), max_w), side = h * 0.1f, round = h * 0.22f;
    const ImVec2 a{c.x - w * 0.5f, c.y - h * 0.5f}, b{c.x + w * 0.5f, c.y + h * 0.5f - side};
    list->AddRectFilled({a.x, a.y + side}, {b.x, b.y + side}, Theme::PS4BtnBorder, round);
    list->AddRectFilled(a, b, Theme::PS4BtnBg, round);
    list->AddRect(a, b, Theme::PS4BtnBorder, round, 0, 1.5f);
    list->AddRect(a + ImVec2{1.2f, 1.2f}, b - ImVec2{1.2f, 1.2f}, IM_COL32(255, 255, 255, 22), round, 0, 1.0f);
    const ImVec2 mid{c.x, (a.y + b.y) * 0.5f};
    if (arrowed) {
        const float half = h * 0.24f, head = h * 0.24f;
        const ImVec2 normal{-arrow.y, arrow.x}, tip = mid + arrow * half;
        list->AddLine(mid - arrow * half, tip - arrow * (head * 0.5f), Theme::GoldLight, std::max(1.6f, h * 0.08f));
        list->AddTriangleFilled(tip, tip - arrow * head + normal * (head * 0.62f), tip - arrow * head - normal * (head * 0.62f), Theme::GoldLight);
        return;
    }
    float size = h * 0.46f;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
    if (extent.x > w - h * 0.3f) size *= (w - h * 0.3f) / extent.x, extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
    list->AddText(font, size, mid - extent * 0.5f, Theme::GoldLight, label);
}

// A mouse of height `h` with `button` lit (ImGuiMouseButton: left, right, middle; 3 the side button nearer the wrist).
inline void DrawMouseGlyph(ImDrawList* list, ImVec2 c, float h, int button) {
    const float w = h * 0.66f, r = w * 0.5f, split = c.y - h * 0.08f, line = std::max(1.2f, h * 0.05f);
    const ImVec2 a{c.x - w * 0.5f, c.y - h * 0.5f}, b{c.x + w * 0.5f, c.y + h * 0.5f};
    if (button == 3) list->AddRectFilled({a.x - w * 0.12f, c.y - h * 0.02f}, {a.x + 1.0f, c.y + h * 0.2f}, Theme::GoldLight, w * 0.05f);
    list->AddRectFilled(a, b, Theme::PS4BtnBg, r);
    if (button == 0) list->AddRectFilled(a, {c.x, split}, Theme::GoldLight, r, ImDrawFlags_RoundCornersTopLeft);
    if (button == 1) list->AddRectFilled({c.x, a.y}, {b.x, split}, Theme::GoldLight, r, ImDrawFlags_RoundCornersTopRight);
    list->AddRect(a, b, Theme::PS4BtnBorder, r, 0, 1.5f);
    list->AddLine({a.x, split}, {b.x, split}, Theme::PS4BtnBorder, line);
    list->AddLine({c.x, a.y}, {c.x, split}, Theme::PS4BtnBorder, line);
    const ImVec2 wa{c.x - w * 0.1f, a.y + h * 0.12f}, wb{c.x + w * 0.1f, a.y + h * 0.3f};  // the wheel
    list->AddRectFilled(wa, wb, button == 2 ? Theme::GoldLight : Theme::PS4BtnBg, w * 0.1f);
    list->AddRect(wa, wb, button == 2 ? Theme::GoldLight : Theme::GoldMuted, w * 0.1f, 0, line);
}

// A prompt: the gamepad button of radius `radius`, or with the keyboard and mouse the key that does the same in every
// screen (Enter, Escape, Delete, Insert), then its label. Laid out from its left edge, centered on `y`.
inline const char* KeyOf(PS4Symbol symbol) {
    return symbol == PS4Symbol::Cross ? "ENTER" : symbol == PS4Symbol::Circle ? "ESC" : symbol == PS4Symbol::Square ? "DEL" : "INS";
}
inline float HintGlyphWidth(PS4Symbol symbol, float radius, ImFont* font) {
    return ImGui::Core::GamepadPrompts() ? radius * 2.0f : KeyCapWidth(KeyOf(symbol), radius * 1.7f, font);
}
inline float HintWidth(PS4Symbol symbol, float radius, const char* label, ImFont* font, float size) {
    return HintGlyphWidth(symbol, radius, font) + (label && *label ? size * 0.55f + font->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x : 0.0f);
}
inline void DrawHint(ImDrawList* list, float left, float y, float radius, PS4Symbol symbol, const char* label, ImFont* font, float size, ImU32 color = Theme::GoldLight) {
    const bool gamepad = ImGui::Core::GamepadPrompts();
    const float w = gamepad ? radius * 2.0f : KeyCapWidth(KeyOf(symbol), radius * 1.7f, font);
    if (gamepad) DrawPadFaceButton(list, {left + radius, y}, radius, symbol);
    else DrawKeyCap(list, {left + w * 0.5f, y}, radius * 1.7f, KeyOf(symbol), font);
    if (label && *label) list->AddText(font, size, {left + w + size * 0.55f, y - size * 0.55f}, color, label);
}

inline ImU32 Fade(ImU32 color, float alpha) {
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(color >> IM_COL32_A_SHIFT) * std::clamp(alpha, 0.0f, 1.0f)) << IM_COL32_A_SHIFT);
}

// The card of a list row: a resting basalt slab that becomes the blood-red focused card as t goes from 0 to 1.
inline void DrawFocusCard(ImDrawList* draw, ImVec2 min, ImVec2 max, float t, float S) {
    draw->AddRectFilled(min, max, Fade(IM_COL32(18, 10, 8, 160), 1.0f - t), 3.0f * S);
    draw->AddRect(min, max, Fade(IM_COL32(201, 158, 75, 45), 1.0f - t), 3.0f * S, 0, 1.0f);
    draw->AddLine({min.x + 1.0f, min.y + 1.5f}, {min.x + 1.0f, max.y - 1.5f}, Fade(IM_COL32(201, 158, 75, 85), 1.0f - t), 2.5f);
    if (t <= 0.0f) return;
    draw->AddRectFilledMultiColor(min, max, Fade(IM_COL32(163, 22, 16, 235), t), Fade(IM_COL32(75, 12, 10, 190), t),
                                  Fade(IM_COL32(40, 6, 6, 120), t), Fade(IM_COL32(115, 17, 14, 220), t));
    draw->AddRect(min, max, Fade(Theme::GoldBronze, t), 3.0f * S, 0, 1.8f);
    draw->AddLine({min.x + 1.0f, min.y + 1.5f}, {min.x + 1.0f, max.y - 1.5f}, Fade(Theme::GoldLight, t), 4.5f);
    draw->AddRect(min - ImVec2{2, 2}, max + ImVec2{2, 2}, Fade(IM_COL32(185, 28, 28, 75), t), 4.0f * S, 0, 2.0f);
}

// The other controller inputs, drawn like the face buttons. `h` is the height; the shoulder, options and touchpad buttons are
// wider than the round ones.
enum class PadGlyph { Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Options, Touchpad,
                      DpadUp, DpadDown, DpadLeft, DpadRight, StickUp, StickDown, StickLeft, StickRight };

inline float PadGlyphWidth(PadGlyph glyph, float h) {
    switch (glyph) {
    case PadGlyph::L1: case PadGlyph::R1: case PadGlyph::L2: case PadGlyph::R2: return h * 1.9f;
    case PadGlyph::Options: return h * 1.5f;
    case PadGlyph::Touchpad: return h * 2.1f;
    default: return h;
    }
}

inline void DrawPadGlyph(ImDrawList* list, ImVec2 c, float h, PadGlyph glyph) {
    const float r = h * 0.5f;
    const bool ps = ImGui::Core::PlayStationPrompts();
    const auto label = [&](const char* text, float size) {
        // These labels have two characters. Center their ink bounds instead of the font's line box.
        auto* baked = g_font_pad->GetFontBaked(size);
        const ImFontGlyph first = *baked->FindGlyph(text[0]);
        const auto* second = baked->FindGlyph(text[1]);
        const ImVec2 min{std::min(first.X0, first.AdvanceX + second->X0), std::min(first.Y0, second->Y0)};
        const ImVec2 max{std::max(first.X1, first.AdvanceX + second->X1), std::max(first.Y1, second->Y1)};
        const ImVec2 origin = c - (min + max) * (0.5f * size / baked->Size);
        list->AddText(g_font_pad, size, {std::round(origin.x), std::round(origin.y)}, Theme::GoldLight, text);
    };
    const auto body = [&](float w, float height) { // the rounded button body
        const ImVec2 a{c.x - w * 0.5f, c.y - height * 0.5f}, b{c.x + w * 0.5f, c.y + height * 0.5f};
        list->AddRectFilled(a, b, Theme::PS4BtnBg, height * 0.4f);
        list->AddRect(a, b, Theme::PS4BtnBorder, height * 0.4f, 0, 1.5f);
        list->AddRect(a + ImVec2{1.2f, 1.2f}, b - ImVec2{1.2f, 1.2f}, IM_COL32(255, 255, 255, 22), height * 0.4f, 0, 1.0f);
        return a;
    };
    const auto triangle = [&](ImVec2 dir, float tip, float base, float half) { // points along dir
        const ImVec2 side{-dir.y, dir.x};
        list->AddTriangleFilled(c + dir * tip, c + dir * base + side * half, c + dir * base - side * half, Theme::GoldLight);
    };
    const float line = std::max(1.5f, h * 0.07f);
    switch (glyph) {
    case PadGlyph::Cross: return DrawPadFaceButton(list, c, r, PS4Symbol::Cross);
    case PadGlyph::Circle: return DrawPadFaceButton(list, c, r, PS4Symbol::Circle);
    case PadGlyph::Square: return DrawPadFaceButton(list, c, r, PS4Symbol::Square);
    case PadGlyph::Triangle: return DrawPadFaceButton(list, c, r, PS4Symbol::Triangle);
    case PadGlyph::L1: case PadGlyph::R1: case PadGlyph::L2: case PadGlyph::R2: {
        constexpr const char* names[][4]{{"LB", "RB", "LT", "RT"}, {"L1", "R1", "L2", "R2"}};
        body(h * 1.9f, h * 0.72f);
        return label(names[ps][int(glyph) - int(PadGlyph::L1)], h * 0.5f);
    }
    case PadGlyph::L3: case PadGlyph::R3: {
        constexpr const char* names[][2]{{"LS", "RS"}, {"L3", "R3"}};
        list->AddCircleFilled(c, r, Theme::PS4BtnBg, 32);
        list->AddCircle(c, r, Theme::PS4BtnBorder, 32, 1.5f);
        return label(names[ps][glyph == PadGlyph::R3], h * 0.46f);
    }
    case PadGlyph::Options:
        body(h * 1.5f, h * 0.64f);
        for (int k = -1; k <= 1; ++k) list->AddLine({c.x - h * 0.22f, c.y + k * h * 0.13f}, {c.x + h * 0.22f, c.y + k * h * 0.13f}, Theme::GoldLight, line);
        return;
    case PadGlyph::Touchpad: {
        const ImVec2 a = body(h * 2.1f, h * 0.82f);
        if (ps) {
            list->AddLine({c.x - h * 0.5f, a.y + h * 0.2f}, {c.x + h * 0.5f, a.y + h * 0.2f}, Theme::TextMuted, line);
        } else {
            const ImVec2 half{h * 0.19f, h * 0.14f}, offset{h * 0.10f, h * 0.07f};
            list->AddRect(c - offset - half, c - offset + half, Theme::GoldLight, 0.0f, 0, line);
            list->AddRectFilled(c + offset - half, c + offset + half, Theme::PS4BtnBg);
            list->AddRect(c + offset - half, c + offset + half, Theme::GoldLight, 0.0f, 0, line);
        }
        return;
    }
    case PadGlyph::DpadUp: case PadGlyph::DpadDown: case PadGlyph::DpadLeft: case PadGlyph::DpadRight: {
        const float arm = h * 0.36f;
        for (const auto [grow, color] : {std::pair{1.4f, Theme::PS4BtnBorder}, std::pair{0.0f, Theme::PS4BtnBg}}) { // outline, then fill
            list->AddRectFilled({c.x - r - grow, c.y - arm * 0.5f - grow}, {c.x + r + grow, c.y + arm * 0.5f + grow}, color, 2.0f);
            list->AddRectFilled({c.x - arm * 0.5f - grow, c.y - r - grow}, {c.x + arm * 0.5f + grow, c.y + r + grow}, color, 2.0f);
        }
        const ImVec2 dir = glyph == PadGlyph::DpadUp ? ImVec2{0, -1} : glyph == PadGlyph::DpadDown ? ImVec2{0, 1} : glyph == PadGlyph::DpadLeft ? ImVec2{-1, 0} : ImVec2{1, 0};
        return triangle(dir, r * 0.82f, r * 0.42f, arm * 0.3f);
    }
    default: { // the stick pushed in a direction
        const ImVec2 dir = glyph == PadGlyph::StickUp ? ImVec2{0, -1} : glyph == PadGlyph::StickDown ? ImVec2{0, 1} : glyph == PadGlyph::StickLeft ? ImVec2{-1, 0} : ImVec2{1, 0};
        list->AddCircleFilled(c, r, Theme::PS4BtnBg, 32);
        list->AddCircle(c, r, Theme::PS4BtnBorder, 32, 1.5f);
        list->AddCircle(c - dir * (r * 0.12f), r * 0.42f, Theme::PS4BtnBorder, 24, 1.5f);
        return triangle(dir, r * 0.86f, r * 0.58f, r * 0.2f);
    }
    }
}

// Draw refined dark basalt background with warm crimson radial underglow and subtle golden marble veins
inline void DrawBasaltBackground(ImDrawList* list, ImVec2 pos, ImVec2 size) {
    // 1. Base Obsidian
    list->AddRectFilled(pos, pos + size, Theme::ObsidianBg);

    // 2. Radial Crimson Underglow
    const ImVec2 center{pos.x + size.x * 0.35f, pos.y + size.y * 0.45f};
    DrawSoftGlow(list, center, std::max(size.x, size.y) * 0.48f, IM_COL32(58, 15, 12, 0), 55.0f);

    // 3. Subtle Golden Veins
    auto draw_vein = [&](const std::vector<ImVec2>& pts) {
        if (pts.size() > 1) {
            list->AddPolyline(pts.data(), static_cast<int>(pts.size()), Theme::GoldVein, 0, 1.3f);
        }
    };

    const float W = size.x, H = size.y;
    draw_vein({
        {pos.x + 0.025f * W, pos.y + 0.88f * H},
        {pos.x + 0.180f * W, pos.y + 0.72f * H},
        {pos.x + 0.350f * W, pos.y + 0.66f * H},
        {pos.x + 0.630f * W, pos.y + 0.50f * H},
        {pos.x + 0.960f * W, pos.y + 0.30f * H}
    });

    draw_vein({
        {pos.x + 0.350f * W, pos.y + 0.66f * H},
        {pos.x + 0.430f * W, pos.y + 0.80f * H},
        {pos.x + 0.580f * W, pos.y + 0.91f * H}
    });

    draw_vein({
        {pos.x + 0.630f * W, pos.y + 0.50f * H},
        {pos.x + 0.710f * W, pos.y + 0.25f * H},
        {pos.x + 0.880f * W, pos.y + 0.12f * H}
    });

    draw_vein({
        {pos.x + 0.110f * W, pos.y + 0.33f * H},
        {pos.x + 0.300f * W, pos.y + 0.36f * H},
        {pos.x + 0.430f * W, pos.y + 0.19f * H}
    });
}

inline ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto ch = [&](int shift) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(std::lround(ca + (cb - ca) * t));
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), ch(IM_COL32_A_SHIFT));
}

inline void DrawDpadHorizontalGlyph(ImDrawList* list, ImVec2 c, float h) {
    const float r = h * 0.5f, arm = h * 0.36f;
    for (const auto& [grow, color] : {std::pair{1.4f, Theme::PS4BtnBorder}, std::pair{0.0f, Theme::PS4BtnBg}}) {
        list->AddRectFilled({c.x - r - grow, c.y - arm * 0.5f - grow}, {c.x + r + grow, c.y + arm * 0.5f + grow}, color, 2.0f);
        list->AddRectFilled({c.x - arm * 0.5f - grow, c.y - r - grow}, {c.x + arm * 0.5f + grow, c.y + r + grow}, color, 2.0f);
    }
    list->AddTriangleFilled({c.x - r * 0.82f, c.y}, {c.x - r * 0.42f, c.y - arm * 0.30f},
                            {c.x - r * 0.42f, c.y + arm * 0.30f}, Theme::GoldLight);
    list->AddTriangleFilled({c.x + r * 0.82f, c.y}, {c.x + r * 0.42f, c.y + arm * 0.30f},
                            {c.x + r * 0.42f, c.y - arm * 0.30f}, Theme::GoldLight);
}

struct SliderHits {
    ImVec2 left_min, left_max;
    ImVec2 right_min, right_max;
    ImVec2 track_min, track_max;
};

inline SliderHits DrawDiamondSlider(ImDrawList* draw, ImVec2 box_min, ImVec2 box_max, float S, float focus_t,
                                    unsigned value, ImFont* font) {
    value = std::min(value, 100u);
    const float mid_y = (box_min.y + box_max.y) * 0.5f;
    const float pad = 10.0f * S, gap = 8.0f * S, tri_w = 9.0f * S, tri_hh = 6.0f * S, val_w = 46.0f * S;
    const float lx0 = box_min.x + pad, lx1 = lx0 + tri_w;
    const float rx1 = box_max.x - pad, rx0 = rx1 - tri_w;

    const ImU32 arrow_col = LerpColor(Fade(Theme::GoldBronze, 0.55f), Theme::GoldLight, focus_t);
    draw->AddTriangleFilled({lx0, mid_y}, {lx1, mid_y - tri_hh}, {lx1, mid_y + tri_hh}, arrow_col);
    draw->AddTriangleFilled({rx1, mid_y}, {rx0, mid_y + tri_hh}, {rx0, mid_y - tri_hh}, arrow_col);

    const std::string val_text = value == 0 ? "OFF" : std::to_string(value) + "%";
    const float val_size = 15.0f * S;
    const ImVec2 val_ext = font->CalcTextSizeA(val_size, FLT_MAX, 0.0f, val_text.c_str());
    const ImU32 val_col = value == 0 ? IM_COL32(221, 106, 109, 255) : Theme::GoldLight;
    draw->AddText(font, val_size, {rx0 - gap - val_ext.x, mid_y - val_ext.y * 0.5f}, val_col, val_text.c_str());

    const float tx0 = lx1 + gap + 4.0f * S;
    const float tx1 = std::max(tx0 + 16.0f * S, rx0 - gap - val_w - gap - 4.0f * S);
    const float half_h = 3.5f * S, ty0 = mid_y - half_h, ty1 = mid_y + half_h;
    const float frac = static_cast<float>(value) / 100.0f;
    const float fill_x = tx0 + (tx1 - tx0) * frac;

    for (int i = 0; i <= 10; ++i) {
        const float x = tx0 + (tx1 - tx0) * (static_cast<float>(i) / 10.0f);
        draw->AddLine({x, ty1 + 2.0f * S}, {x, ty1 + 6.0f * S}, Fade(Theme::GoldBronze, 0.35f), std::max(1.0f, 1.0f * S));
    }
    draw->AddRectFilled({tx0, ty0}, {tx1, ty1}, Theme::BloodTrenchDark, half_h);
    if (value > 0 && fill_x > tx0 + 1.0f) {
        draw->AddRectFilledMultiColor({tx0, ty0}, {fill_x, ty1}, IM_COL32(124, 90, 36, 255), IM_COL32(226, 187, 106, 255),
                                      IM_COL32(226, 187, 106, 255), IM_COL32(124, 90, 36, 255));
    }
    draw->AddRect({tx0, ty0}, {tx1, ty1}, Fade(Theme::GoldBronze, 0.35f), half_h, 0, std::max(1.0f, 1.0f * S));

    const ImVec2 tc{fill_x, mid_y};
    const float r = 9.2f * S;
    const ImVec2 top{tc.x, tc.y - r}, right{tc.x + r, tc.y}, bottom{tc.x, tc.y + r}, left{tc.x - r, tc.y};
    if (value > 0) {
        DrawSoftGlow(draw, tc, 14.0f * S, Theme::GoldBronze & ~IM_COL32_A_MASK, 28.0f + 42.0f * focus_t);
    }
    if (focus_t > 0.01f) {
        const float hr = r + 2.2f * S;
        draw->AddQuad({tc.x, tc.y - hr}, {tc.x + hr, tc.y}, {tc.x, tc.y + hr}, {tc.x - hr, tc.y},
                      Fade(Theme::GoldLight, 0.35f * focus_t), std::max(1.5f, 2.0f * S));
    }
    if (value == 0) {
        draw->AddQuadFilled(top, right, bottom, left, LerpColor(IM_COL32(74, 58, 34, 255), IM_COL32(107, 85, 48, 255), focus_t));
    } else {
        draw->AddTriangleFilled(left, top, right, IM_COL32(246, 220, 152, 255));
        draw->AddTriangleFilled(left, right, bottom, IM_COL32(182, 138, 63, 255));
    }
    draw->AddQuad(top, right, bottom, left, IM_COL32(58, 26, 12, 255), std::max(1.0f, 1.2f * S));

    return {
        {box_min.x, box_min.y}, {lx1 + gap * 0.5f, box_max.y},
        {rx0 - gap * 0.5f, box_min.y}, {box_max.x, box_max.y},
        {tx0, box_min.y}, {tx1, box_max.y},
    };
}

struct SelectorHits {
    ImVec2 left_min, left_max;
    ImVec2 right_min, right_max;
};

inline SelectorHits DrawToggleSelector(ImDrawList* draw, ImVec2 box_min, ImVec2 box_max, float S, float focus_t,
                                       bool on, bool available, ImFont* font) {
    const float mid_y = (box_min.y + box_max.y) * 0.5f;
    const float pad = 10.0f * S, gap = 8.0f * S, tri_w = 9.0f * S, tri_hh = 6.0f * S;
    const float lx0 = box_min.x + pad, lx1 = lx0 + tri_w;
    const float rx1 = box_max.x - pad, rx0 = rx1 - tri_w;

    const float arrow_alpha = available ? 1.0f : 0.38f;
    const ImU32 arrow_col = Fade(LerpColor(Fade(Theme::GoldBronze, 0.55f), Theme::GoldLight, focus_t), arrow_alpha);
    draw->AddTriangleFilled({lx0, mid_y}, {lx1, mid_y - tri_hh}, {lx1, mid_y + tri_hh}, arrow_col);
    draw->AddTriangleFilled({rx1, mid_y}, {rx0, mid_y + tri_hh}, {rx0, mid_y - tri_hh}, arrow_col);

    const char* label = on ? "ON" : "OFF";
    const float text_size = 16.0f * S;
    const ImVec2 ext = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, label);
    const ImVec2 center{(lx1 + rx0) * 0.5f, mid_y};
    if (on && available) {
        DrawSoftGlow(draw, center, 26.0f * S, Theme::GoldBronze & ~IM_COL32_A_MASK, 20.0f);
    }
    const ImU32 text_col = !available ? Fade(Theme::TextMuted, 0.38f) : on ? Theme::GoldLight : Theme::TextMuted;
    draw->AddText(font, text_size, {center.x - ext.x * 0.5f, center.y - ext.y * 0.5f}, text_col, label);

    return {
        {box_min.x, box_min.y}, {lx1 + gap, box_max.y},
        {rx0 - gap, box_min.y}, {box_max.x, box_max.y},
    };
}

} // namespace Gow3::UI
