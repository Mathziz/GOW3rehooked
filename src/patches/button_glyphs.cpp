// SPDX-License-Identifier: GPL-2.0-or-later
#include "patches/button_glyphs.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <exception>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "input/input_handler.h"
#include "main/host.h"
#include "orbis/common/util.h"
#include "orbis/core/file_sys/fs.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

namespace Gow3 {
namespace FileOverlay {
namespace {
struct Replacement {
    u64 offset;
    std::vector<u8> bytes;
};
struct File {
    std::string guest;  // upper case
    std::vector<Replacement> replacements;
    std::atomic<bool> logged{};
};
// Written once before the game runs, then only read.
std::vector<std::unique_ptr<File>> files;
std::atomic<bool> any{};

std::string Upper(std::string_view text) {
    std::string upper(text);
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    return upper;
}
} // namespace

void Add(std::string_view guest, u64 offset, std::vector<u8> bytes) {  // before the game runs: no reader yet
    const std::string key = Upper(guest);
    auto it = std::ranges::find(files, key, [](const auto& f) { return f->guest; });
    if (it == files.end()) {
        files.push_back(std::make_unique<File>());
        files.back()->guest = key;
        it = files.end() - 1;
    }
    (*it)->replacements.push_back({offset, std::move(bytes)});
    any.store(true, std::memory_order_release);
}

void Apply(std::string_view guest_path, u64 offset, void* data, u64 size) {
    if (!any.load(std::memory_order_acquire) || !size) return;
    for (const auto& file : files) {
        if (file->guest.size() != guest_path.size()) continue;
        bool same = true;
        for (size_t i = 0; same && i < guest_path.size(); ++i)
            same = std::toupper(static_cast<unsigned char>(guest_path[i])) == file->guest[i];
        if (!same) continue;
        u64 patched = 0;
        for (const auto& r : file->replacements) {
            const u64 begin = std::max(offset, r.offset), end = std::min(offset + size, r.offset + r.bytes.size());
            if (begin >= end) continue;
            std::memcpy(static_cast<u8*>(data) + (begin - offset), r.bytes.data() + (begin - r.offset), end - begin);
            patched += end - begin;
        }
        if (patched && !file->logged.exchange(true))
            Log(std::format("Button icons: {} read with the keyboard prompts", guest_path));
        return;
    }
}
} // namespace FileOverlay

namespace ButtonGlyphs {
namespace {
// -- What is drawn --------------------------------------------------------------------------------------------------------
// Inline: the 64 px icons inside text; Qte: the large prompt on black; Atlas: two prompts, the first upside down in the top
// half (the shoulder buttons).
enum class Form { Inline, Qte, Atlas };
struct Texture { const char* id; Form form; const char* first; const char* second; };
// The first ten characters of the texture's WAD entry name (TX<hash>.<hash>.dds) and the output of the input config it shows.
constexpr Texture textures[]{
    {"TX1BBACF4D", Form::Inline, "cross", nullptr},    {"TXBA40AFD2", Form::Inline, "circle", nullptr},
    {"TX64F3D812", Form::Inline, "square", nullptr},   {"TX573C282A", Form::Inline, "triangle", nullptr},
    {"TX91A47980", Form::Inline, "l1", nullptr},       {"TX5CED5AE9", Form::Inline, "r1", nullptr},
    {"TX6FC83ACE", Form::Inline, "l2", nullptr},       {"TX67E86446", Form::Inline, "r2", nullptr},
    {"TX9AE3BA81", Form::Inline, "l3", nullptr},       {"TXFB33513A", Form::Inline, "r3", nullptr},
    {"TX192AE9D1", Form::Inline, "options", nullptr},  {"TXD43D1443", Form::Inline, "touchpad_right", nullptr},
    {"TX7FF61BF7", Form::Qte, "cross", nullptr},       {"TX9D590CC8", Form::Qte, "circle", nullptr},
    {"TX1F5FC983", Form::Qte, "square", nullptr},      {"TXE8889579", Form::Qte, "triangle", nullptr},
    {"TXFB91136E", Form::Atlas, "l2", "l1"},           {"TX0FFF3CCB", Form::Atlas, "r2", "r1"},
    // The keyboard's own: the two left stick icons, the right stick and the D-pad.
    {"TXDCA1B818", Form::Inline, "lstick", nullptr},   {"TXE2BB1428", Form::Inline, "lstick", nullptr},
    {"TX77AAD542", Form::Inline, "rstick", nullptr},   {"TXBF2035D0", Form::Inline, "dpad", nullptr},
};
constexpr const char* wads[]{"/app0/EXEC/WAD/PS4_LE/R_UI.WAD", "/app0/EXEC/WAD/PS4_LE/R_PERM.WAD"};
constexpr int K = 4;          // drawn this many times larger, then reduced
constexpr int kMaxSide = 4096; // a larger texture header comes from a damaged file and is left alone

// What a key cap shows: text, an arrow (or the four), or a mouse with a button lit.
enum class Icon { None, Up, Down, Left, Right, Arrows, MouseLeft, MouseRight, MouseMiddle };
struct Label {
    std::string text;
    Icon icon = Icon::None;
    bool Wide() const { return icon == Icon::None && text.size() > 2; }
    bool Empty() const { return icon == Icon::None && text.empty(); }
};

Label KeyLabel(const std::string& token) {
    struct Name { const char* token; const char* text; };
    static constexpr Name names[]{
        {"space", "SPACE"}, {"enter", "ENTER"}, {"kpenter", "ENTER"}, {"tab", "TAB"}, {"backspace", "BKSP"}, {"delete", "DEL"},
        {"insert", "INS"}, {"home", "HOME"}, {"end", "END"}, {"pgup", "PG UP"}, {"pgdown", "PG DN"}, {"lshift", "SHIFT"},
        {"rshift", "R SHIFT"}, {"lctrl", "CTRL"}, {"rctrl", "R CTRL"}, {"lalt", "ALT"}, {"ralt", "ALT GR"}, {"lwin", "WIN"},
        {"rwin", "WIN"}, {"lmeta", "WIN"}, {"rmeta", "WIN"}, {"capslock", "CAPS"}, {"escape", "ESC"}, {"printscreen", "PRTSC"},
        {"scrolllock", "SCRLK"}, {"pausebreak", "PAUSE"}, {"grave", "`"}, {"minus", "-"}, {"equals", "="}, {"lbracket", "["},
        {"rbracket", "]"}, {"backslash", "\\"}, {"semicolon", ";"}, {"apostrophe", "'"}, {"comma", ","}, {"period", "."},
        {"slash", "/"}, {"kpperiod", "NUM ."}, {"kpslash", "NUM /"}, {"kpasterisk", "NUM *"}, {"kpminus", "NUM -"},
        {"kpplus", "NUM +"}, {"kpequals", "NUM ="}, {"sidebuttonback", "M4"}, {"sidebuttonforward", "M5"},
    };
    if (token == "up") return {{}, Icon::Up};
    if (token == "down") return {{}, Icon::Down};
    if (token == "left") return {{}, Icon::Left};
    if (token == "right") return {{}, Icon::Right};
    if (token == "leftbutton") return {{}, Icon::MouseLeft};
    if (token == "rightbutton") return {{}, Icon::MouseRight};
    if (token == "middlebutton") return {{}, Icon::MouseMiddle};
    for (const auto& name : names)
        if (token == name.token) return {name.text};
    if (token.size() == 3 && token.starts_with("kp") && std::isdigit(static_cast<unsigned char>(token[2]))) return {"NUM " + token.substr(2)};
    std::string text = token;
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return {text};
}

// The four keys of a stick or of the D-pad (up, left, down, right) as one label: WASD, the four arrows, or the keys joined.
std::optional<Label> FourKeys(const char* up, const char* left, const char* down, const char* right) {
    std::array<std::string, 4> keys;
    const char* outputs[]{up, left, down, right};
    for (int i = 0; i < 4; ++i) {
        const auto bound = Input::KeyboardBindings(outputs[i]);
        if (bound.empty()) return std::nullopt;
        keys[i] = bound.front();
    }
    if (keys == std::array<std::string, 4>{"up", "left", "down", "right"}) return Label{{}, Icon::Arrows};
    std::array<std::string, 4> texts;
    bool letters = true;
    for (int i = 0; i < 4; ++i) {
        const Label label = KeyLabel(keys[i]);
        if (label.icon != Icon::None) return std::nullopt;  // an arrow among letters has no short name
        texts[i] = label.text;
        letters &= label.text.size() == 1;
    }
    std::string text;
    for (const auto& t : texts) text += (text.empty() || letters ? "" : "/") + t;
    return Label{text};
}

std::optional<Label> LabelOf(const char* output) {
    if (!std::strcmp(output, "lstick")) return FourKeys("axis_left_y_minus", "axis_left_x_minus", "axis_left_y_plus", "axis_left_x_plus");
    if (!std::strcmp(output, "rstick")) {
        // The right stick evades: the Evade key does the same with the keyboard.
        if (const auto dodge = Input::KeyboardBindings("dodge"); !dodge.empty()) return KeyLabel(dodge.front());
        return FourKeys("axis_right_y_minus", "axis_right_x_minus", "axis_right_y_plus", "axis_right_x_plus");
    }
    if (!std::strcmp(output, "dpad")) return FourKeys("pad_up", "pad_left", "pad_down", "pad_right");
    const auto bound = Input::KeyboardBindings(output);
    if (bound.empty()) return std::nullopt;
    return KeyLabel(bound.front());
}

// -- Drawing -------------------------------------------------------------------------------------------------------------
struct Color { float r, g, b, a; };
constexpr Color Ink{240 / 255.f, 240 / 255.f, 240 / 255.f, 1.f};

// Premultiplied RGBA, drawn with hard edges at K times the size; Reduce() averages it down, which smooths the edges.
struct Canvas {
    int w{}, h{};
    std::vector<float> px;
    Canvas(int width, int height, Color fill = {0, 0, 0, 0}) : w(width), h(height), px(size_t(width) * height * 4) {
        for (size_t i = 0; i < px.size(); i += 4) px[i] = fill.r * fill.a, px[i + 1] = fill.g * fill.a, px[i + 2] = fill.b * fill.a, px[i + 3] = fill.a;
    }
    void Blend(int x, int y, Color c, float coverage) {
        if (x < 0 || y < 0 || x >= w || y >= h || coverage <= 0.f) return;
        float* p = &px[(size_t(y) * w + x) * 4];
        const float a = c.a * std::min(coverage, 1.f), keep = 1.f - a;
        p[0] = c.r * a + p[0] * keep, p[1] = c.g * a + p[1] * keep, p[2] = c.b * a + p[2] * keep, p[3] = a + p[3] * keep;
    }
    // Every pixel whose center the shape holds, in a color that may vary with y.
    template <typename Inside, typename Paint>
    void Fill(float x0, float y0, float x1, float y1, Inside inside, Paint paint) {
        for (int y = std::max(0, int(y0)); y < std::min(h, int(std::ceil(y1)) + 1); ++y)
            for (int x = std::max(0, int(x0)); x < std::min(w, int(std::ceil(x1)) + 1); ++x)
                if (inside(x + 0.5f, y + 0.5f)) Blend(x, y, paint(y + 0.5f), 1.f);
    }
    void Over(const Canvas& top, int dx = 0, int dy = 0) {
        for (int y = 0; y < top.h; ++y) {
            if (y + dy < 0 || y + dy >= h) continue;
            for (int x = 0; x < top.w; ++x) {
                if (x + dx < 0 || x + dx >= w) continue;
                const float* s = &top.px[(size_t(y) * top.w + x) * 4];
                float* d = &px[(size_t(y + dy) * w + x + dx) * 4];
                const float keep = 1.f - s[3];
                for (int i = 0; i < 4; ++i) d[i] = s[i] + d[i] * keep;
            }
        }
    }
    Canvas Reduce(int factor) const {
        Canvas out(std::max(1, w / factor), std::max(1, h / factor));
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x) {
                float sum[4]{};
                int n = 0;
                for (int sy = y * factor; sy < std::min(h, (y + 1) * factor); ++sy)
                    for (int sx = x * factor; sx < std::min(w, (x + 1) * factor); ++sx, ++n)
                        for (int i = 0; i < 4; ++i) sum[i] += px[(size_t(sy) * w + sx) * 4 + i];
                for (int i = 0; i < 4; ++i) out.px[(size_t(y) * out.w + x) * 4 + i] = n ? sum[i] / float(n) : 0.f;
            }
        return out;
    }
    Canvas Rotated180() const {
        Canvas out(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(&out.px[(size_t(h - 1 - y) * w + (w - 1 - x)) * 4], &px[(size_t(y) * w + x) * 4], 4 * sizeof(float));
        return out;
    }
    // Three box blurs, close to a Gaussian of `sigma` pixels.
    void Blur(float sigma) {
        const int radius = std::max(1, int(std::lround(std::sqrt(12.f * sigma * sigma / 3.f + 1.f) * 0.5f)));
        std::vector<float> line;
        for (int pass = 0; pass < 3; ++pass) {
            for (const bool horizontal : {true, false}) {
                const int lines = horizontal ? h : w, length = horizontal ? w : h;
                line.resize(size_t(length) * 4);
                for (int l = 0; l < lines; ++l) {
                    const auto at = [&](int i) -> float* { return horizontal ? &px[(size_t(l) * w + i) * 4] : &px[(size_t(i) * w + l) * 4]; };
                    for (int i = 0; i < length; ++i) std::memcpy(&line[size_t(i) * 4], at(i), 4 * sizeof(float));
                    float sum[4]{};
                    for (int i = -radius; i <= radius; ++i)
                        if (i >= 0 && i < length) for (int c = 0; c < 4; ++c) sum[c] += line[size_t(i) * 4 + c];
                    const float scale = 1.f / float(2 * radius + 1);
                    for (int i = 0; i < length; ++i) {
                        float* d = at(i);
                        for (int c = 0; c < 4; ++c) d[c] = sum[c] * scale;
                        const int out_i = i - radius, in_i = i + radius + 1;
                        for (int c = 0; c < 4; ++c) {
                            if (out_i >= 0) sum[c] -= line[size_t(out_i) * 4 + c];
                            if (in_i < length) sum[c] += line[size_t(in_i) * 4 + c];
                        }
                    }
                }
            }
        }
    }
};

bool InRoundedRect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    r = std::min({r, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f});
    const float cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
void FillRoundedRect(Canvas& c, float x0, float y0, float x1, float y1, float r, Color color) {
    c.Fill(x0, y0, x1, y1, [&](float x, float y) { return InRoundedRect(x, y, x0, y0, x1, y1, r); }, [&](float) { return color; });
}
void StrokeRoundedRect(Canvas& c, float x0, float y0, float x1, float y1, float r, float width, Color color) {
    c.Fill(x0, y0, x1, y1, [&](float x, float y) {
        return InRoundedRect(x, y, x0, y0, x1, y1, r) &&
               !InRoundedRect(x, y, x0 + width, y0 + width, x1 - width, y1 - width, std::max(0.f, r - width));
    }, [&](float) { return color; });
}
void FillTriangle(Canvas& c, std::array<std::pair<float, float>, 3> p, Color color) {
    const auto edge = [](std::pair<float, float> a, std::pair<float, float> b, float x, float y) {
        return (b.first - a.first) * (y - a.second) - (b.second - a.second) * (x - a.first);
    };
    float x0 = p[0].first, x1 = x0, y0 = p[0].second, y1 = y0;
    for (const auto& [x, y] : p) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    c.Fill(x0, y0, x1, y1, [&](float x, float y) {
        const float a = edge(p[0], p[1], x, y), b = edge(p[1], p[2], x, y), d = edge(p[2], p[0], x, y);
        return (a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0);
    }, [&](float) { return color; });
}

struct Font {
    std::vector<u8> data;
    stbtt_fontinfo info{};
    bool ok = false;
};
const Font& LabelFont() {
    static const Font font = [] {
        Font f;
        std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
        char windows[MAX_PATH]{};
        const auto fonts = std::filesystem::path(std::string_view(windows, GetWindowsDirectoryA(windows, MAX_PATH))) / "Fonts";
        for (const char* name : {"seguisb.ttf", "segoeuib.ttf", "arialbd.ttf", "segoeui.ttf", "arial.ttf"}) candidates.push_back(fonts / name);
#else
        for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                 "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                 "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                 "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"})
            candidates.emplace_back(path);
#endif
        for (const auto& path : candidates) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            f.data.assign(std::istreambuf_iterator<char>(in), {});
            if (f.data.size() > 1024 && stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
                f.ok = true;
                break;
            }
        }
        return f;
    }();
    return font;
}

// The coverage of `text` at `px` pixels, cut to its ink.
struct Mask { int w{}, h{}; std::vector<u8> a; };
Mask TextMask(const Font& font, std::string_view text, float px) {
    const float scale = stbtt_ScaleForPixelHeight(&font.info, px);
    struct Placed { int cp, x, y, w, h; float shift; };
    std::vector<Placed> glyphs;
    float pen = 0.f;
    int left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
    for (size_t i = 0; i < text.size(); ++i) {
        const int cp = static_cast<unsigned char>(text[i]);
        int advance{}, bearing{};
        stbtt_GetCodepointHMetrics(&font.info, cp, &advance, &bearing);
        const float shift = pen - std::floor(pen);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&font.info, cp, scale, scale, shift, 0.f, &x0, &y0, &x1, &y1);
        if (x1 > x0 && y1 > y0) {
            const int gx = int(std::floor(pen)) + x0;
            glyphs.push_back({cp, gx, y0, x1 - x0, y1 - y0, shift});
            left = std::min(left, gx), right = std::max(right, gx + x1 - x0), top = std::min(top, y0), bottom = std::max(bottom, y1);
        }
        pen += float(advance) * scale;
        if (i + 1 < text.size()) pen += float(stbtt_GetCodepointKernAdvance(&font.info, cp, static_cast<unsigned char>(text[i + 1]))) * scale;
    }
    Mask mask;
    if (glyphs.empty()) return mask;
    mask.w = right - left, mask.h = bottom - top;
    mask.a.assign(size_t(mask.w) * mask.h, 0);
    std::vector<u8> bitmap;
    for (const auto& g : glyphs) {
        bitmap.assign(size_t(g.w) * g.h, 0);
        stbtt_MakeCodepointBitmapSubpixel(&font.info, bitmap.data(), g.w, g.h, g.w, scale, scale, g.shift, 0.f, g.cp);
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                u8& d = mask.a[size_t(g.y - top + y) * mask.w + (g.x - left + x)];
                d = std::max(d, bitmap[size_t(y) * g.w + x]);
            }
    }
    return mask;
}

// The label centered in the area: text shrunk until it fits, an arrow, or a mouse.
void DrawLabel(Canvas& c, float x0, float y0, float x1, float y1, const Label& label, Color color) {
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, r = std::min(x1 - x0, y1 - y0) * 0.5f;
    const auto arrow = [&](float ax, float ay, float k, float ox, float oy) {
        FillTriangle(c, {{{ox + ax * r * 0.8f * k, oy + ay * r * 0.8f * k},
                          {ox - ax * r * 0.5f * k - ay * r * 0.7f * k, oy - ay * r * 0.5f * k - ax * r * 0.7f * k},
                          {ox - ax * r * 0.5f * k + ay * r * 0.7f * k, oy - ay * r * 0.5f * k + ax * r * 0.7f * k}}}, color);
    };
    switch (label.icon) {
    case Icon::Up: return arrow(0, -1, 1, cx, cy);
    case Icon::Down: return arrow(0, 1, 1, cx, cy);
    case Icon::Left: return arrow(-1, 0, 1, cx, cy);
    case Icon::Right: return arrow(1, 0, 1, cx, cy);
    case Icon::Arrows: {
        constexpr std::pair<float, float> directions[]{{0.f, -1.f}, {0.f, 1.f}, {-1.f, 0.f}, {1.f, 0.f}};
        for (const auto& [ax, ay] : directions) arrow(ax, ay, 0.5f, cx + ax * r * 0.5f, cy + ay * r * 0.5f);
        return;
    }
    case Icon::MouseLeft: case Icon::MouseRight: case Icon::MouseMiddle: {
        const float h = std::min(y1 - y0, (x1 - x0) * 1.35f), w = h * 0.68f, line = std::max(1.f, h * 0.075f);
        const float bx0 = cx - w * 0.5f, bx1 = cx + w * 0.5f, by0 = cy - h * 0.5f, by1 = cy + h * 0.5f, split = by0 + h * 0.43f;
        const float round = w * 0.5f;
        if (label.icon != Icon::MouseMiddle) { // the button: the body's quarter above the split
            const bool lit_left = label.icon == Icon::MouseLeft;
            c.Fill(bx0, by0, bx1, split, [&](float x, float y) {
                return InRoundedRect(x, y, bx0, by0, bx1, by1, round) && y < split && (lit_left ? x < cx : x > cx);
            }, [&](float) { return color; });
        }
        StrokeRoundedRect(c, bx0, by0, bx1, by1, round, line, color);
        FillRoundedRect(c, bx0, split - line * 0.5f, bx1, split + line * 0.5f, 0.f, color);
        FillRoundedRect(c, cx - line * 0.5f, by0, cx + line * 0.5f, split, 0.f, color);
        const float wx0 = cx - w * 0.11f, wx1 = cx + w * 0.11f, wy0 = by0 + h * 0.12f, wy1 = by0 + h * 0.32f;  // the wheel
        if (label.icon == Icon::MouseMiddle) FillRoundedRect(c, wx0, wy0, wx1, wy1, w * 0.11f, color);
        else StrokeRoundedRect(c, wx0, wy0, wx1, wy1, w * 0.11f, line * 0.8f, color);
        return;
    }
    case Icon::None: break;
    }
    const Font& font = LabelFont();
    if (!font.ok || label.text.empty()) return;
    float size = y1 - y0;
    Mask mask = TextMask(font, label.text, size);
    while ((mask.w > x1 - x0 || mask.h > y1 - y0) && size > 6.f) size *= 0.92f, mask = TextMask(font, label.text, size);
    const int ox = int(std::lround(cx - mask.w * 0.5f)), oy = int(std::lround(cy - mask.h * 0.5f));
    for (int y = 0; y < mask.h; ++y)
        for (int x = 0; x < mask.w; ++x) c.Blend(ox + x, oy + y, color, mask.a[size_t(y) * mask.w + x] / 255.f);
}

// An icon as the game draws them inside text: a dark key cap, lit from above, with the label on it.
Canvas DrawInline(const Label& label, int width, int height) {
    const int W = width * K, H = height * K;
    Canvas c(W, H);
    float x0, y0, x1, y1;
    if (!label.Wide()) {
        const float d = float(std::min(W, H) - 2 * K);
        x0 = (W - d) * 0.5f, y0 = (H - d) * 0.5f, x1 = x0 + d, y1 = y0 + d;
    } else {
        const float bh = float(std::min(H - 2 * K, 36 * K));
        x0 = 2.f * K, y0 = (H - bh) * 0.5f, x1 = float(W - 2 * K), y1 = y0 + bh;
    }
    const float radius = 5.f * K, top = y0, span = std::max(1.f, y1 - y0);
    c.Fill(x0, y0, x1, y1, [&](float x, float y) { return InRoundedRect(x, y, x0, y0, x1, y1, radius); }, [&](float y) {
        const float t = std::clamp((y - top) / span, 0.f, 1.f);
        return Color{(84 + (26 - 84) * t) / 255.f, (84 + (26 - 84) * t) / 255.f, (86 + (28 - 86) * t) / 255.f, 1.f};
    });
    StrokeRoundedRect(c, x0, y0, x1, y1, radius, 1.5f * K, {14 / 255.f, 14 / 255.f, 14 / 255.f, 1.f});
    const float fx = label.Wide() ? 0.10f : 0.12f, fy = label.Wide() ? 0.16f : 0.14f;
    const float ix = (x1 - x0) * fx, iy = (y1 - y0) * fy;
    DrawLabel(c, x0 + ix, y0 + iy, x1 - ix, y1 - iy, label, Ink);
    return c.Reduce(K);
}

// A quick-time prompt: the key cap's outline and label, bright on black with a glow (the game adds these to the picture).
Canvas DrawPrompt(const Label& label, int width, int height) {
    const int W = width * K, H = height * K;
    const float side = float(std::min(W, H));
    Canvas sharp(W, H);
    StrokeRoundedRect(sharp, W * 0.06f, H * 0.06f, W * 0.94f, H * 0.94f, side * 0.1f, std::max(1.f, side * 0.035f), Ink);
    DrawLabel(sharp, W * 0.15f, H * 0.22f, W * 0.85f, H * 0.78f, label, Ink);
    const Canvas reduced = sharp.Reduce(K);
    Canvas glow = reduced;  // blurred at the final size: the same picture, a sixteenth of the work
    glow.Blur(2.5f);
    Canvas image(width, height, {0, 0, 0, 1});
    image.Over(glow);
    image.Over(reduced);
    return image;
}

Canvas Render(const Texture& texture, int width, int height, const Label& first, const Label& second) {
    if (texture.form == Form::Inline) return DrawInline(first, width, height);
    if (texture.form == Form::Qte) return DrawPrompt(first, width, height);
    // The shoulder atlas: the prompts sit about 52 px from the midline of a 256 px texture, not in the middle of each half.
    Canvas image(width, height, {0, 0, 0, 1});
    const int half = height / 2, offset = height * 12 / 256;
    for (int row = 0; row < 2; ++row) {
        Canvas part(width, half, {0, 0, 0, 1});
        part.Over(DrawPrompt(row == 0 ? first : second, width, half), 0, -offset);
        image.Over(row == 0 ? part.Rotated180() : part, 0, row * half);
    }
    return image;
}

// -- DXT -----------------------------------------------------------------------------------------------------------------
u16 To565(const float c[3]) {
    const auto q = [](float v, int bits) { return u16(std::clamp(int(std::lround(v * float((1 << bits) - 1))), 0, (1 << bits) - 1)); };
    return u16(q(c[0], 5) << 11 | q(c[1], 6) << 5 | q(c[2], 5));
}
void From565(u16 v, int out[3]) {
    const int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    out[0] = r << 3 | r >> 2, out[1] = g << 2 | g >> 4, out[2] = b << 3 | b >> 2;
}

// A 4x4 block of 8-bit straight RGBA: endpoints along the colors' main axis, four-color mode.
void EncodeColor(const u8 (&block)[16][4], u8* out) {
    float mean[3]{};
    for (const auto& p : block) for (int c = 0; c < 3; ++c) mean[c] += p[c] / 255.f / 16.f;
    float cov[6]{};
    for (const auto& p : block) {
        const float d[3]{p[0] / 255.f - mean[0], p[1] / 255.f - mean[1], p[2] / 255.f - mean[2]};
        cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2], cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
    }
    float axis[3]{0.577f, 0.577f, 0.577f};
    for (int i = 0; i < 8; ++i) {
        const float n[3]{cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                         cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length < 1e-9f) break;
        for (int c = 0; c < 3; ++c) axis[c] = n[c] / length;
    }
    float lo = 1e9f, hi = -1e9f;
    for (const auto& p : block) {
        const float t = (p[0] / 255.f - mean[0]) * axis[0] + (p[1] / 255.f - mean[1]) * axis[1] + (p[2] / 255.f - mean[2]) * axis[2];
        lo = std::min(lo, t), hi = std::max(hi, t);
    }
    const float inset = (hi - lo) / 32.f;
    lo += inset, hi -= inset;
    float e0[3], e1[3];
    for (int c = 0; c < 3; ++c) e0[c] = std::clamp(mean[c] + axis[c] * hi, 0.f, 1.f), e1[c] = std::clamp(mean[c] + axis[c] * lo, 0.f, 1.f);
    u16 c0 = To565(e0), c1 = To565(e1);
    if (c0 < c1) std::swap(c0, c1);
    u32 indices = 0;
    if (c0 != c1) {
        int p[4][3];
        From565(c0, p[0]), From565(c1, p[1]);
        for (int c = 0; c < 3; ++c) p[2][c] = (2 * p[0][c] + p[1][c]) / 3, p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
        for (int i = 0; i < 16; ++i) {
            int best = 0, best_d = INT32_MAX;
            for (int j = 0; j < 4; ++j) {
                const int dr = block[i][0] - p[j][0], dg = block[i][1] - p[j][1], db = block[i][2] - p[j][2];
                const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < best_d) best_d = d, best = j;
            }
            indices |= u32(best) << (2 * i);
        }
    }
    out[0] = u8(c0), out[1] = u8(c0 >> 8), out[2] = u8(c1), out[3] = u8(c1 >> 8);
    for (int i = 0; i < 4; ++i) out[4 + i] = u8(indices >> (8 * i));
}

void EncodeAlpha(const u8 (&block)[16][4], u8* out) {
    u8 a0 = 0, a1 = 255;
    for (const auto& p : block) a0 = std::max(a0, p[3]), a1 = std::min(a1, p[3]);
    u64 indices = 0;
    if (a0 != a1) {
        int palette[8]{a0, a1};
        for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, best_d = INT32_MAX;
            for (int j = 0; j < 8; ++j)
                if (const int d = std::abs(block[i][3] - palette[j]); d < best_d) best_d = d, best = j;
            indices |= u64(best) << (3 * i);
        }
    }
    out[0] = a0, out[1] = a1;
    for (int i = 0; i < 6; ++i) out[2 + i] = u8(indices >> (8 * i));
}

// One mip level: straight 8-bit pixels (DXT1 flattened on black), block by block.
void EncodeLevel(const Canvas& level, bool dxt5, std::vector<u8>& out) {
    const int bw = (level.w + 3) / 4, bh = (level.h + 3) / 4;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            u8 block[16][4];
            for (int i = 0; i < 16; ++i) {
                const int x = std::min(level.w - 1, bx * 4 + i % 4), y = std::min(level.h - 1, by * 4 + i / 4);
                const float* p = &level.px[(size_t(y) * level.w + x) * 4];
                const float a = std::clamp(p[3], 0.f, 1.f), unpremultiply = dxt5 && a > 1e-4f ? 1.f / a : 1.f;
                for (int c = 0; c < 3; ++c) block[i][c] = u8(std::clamp(int(std::lround(p[c] * unpremultiply * 255.f)), 0, 255));
                block[i][3] = u8(std::lround(a * 255.f));
            }
            u8 bytes[16];
            if (dxt5) EncodeAlpha(block, bytes), EncodeColor(block, bytes + 8);
            else EncodeColor(block, bytes);
            out.insert(out.end(), bytes, bytes + (dxt5 ? 16 : 8));
        }
}

// The picture and its smaller levels (each half the previous), exactly `length` bytes as in the original; empty otherwise.
std::vector<u8> Encode(Canvas level, bool dxt5, size_t length) {
    std::vector<u8> out;
    while (out.size() < length) {
        EncodeLevel(level, dxt5, out);
        if (level.w == 1 && level.h == 1) break;
        Canvas half(std::max(1, level.w / 2), std::max(1, level.h / 2));
        for (int y = 0; y < half.h; ++y)
            for (int x = 0; x < half.w; ++x)
                for (int c = 0; c < 4; ++c) {
                    float sum = 0;
                    int n = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(level.w - 1, x * 2 + dx), sy = std::min(level.h - 1, y * 2 + dy);
                            sum += level.px[(size_t(sy) * level.w + sx) * 4 + c], ++n;
                        }
                    half.px[(size_t(y) * half.w + x) * 4 + c] = sum / float(n);
                }
        level = std::move(half);
    }
    if (out.size() != length) out.clear();
    return out;
}

// -- WADs ----------------------------------------------------------------------------------------------------------------
u32 U32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

// Entries of a 0x40 header (u16 tag, u16 flags, u32 size, name[56]) and `size` bytes padded to 16; the EntityCount marker of
// level WADs (tag 0) carries a count in its size and no data. Draws the prompts found; returns how many.
int PatchWad(const char* guest, const std::filesystem::path& host, const Label* labels, const Label* seconds) {
    std::ifstream file(host, std::ios::binary);
    if (!file) return 0;
    file.seekg(0, std::ios::end);
    const u64 file_size = u64(file.tellg());
    int count = 0;
    u8 header[0x40];
    std::vector<u8> data;
    for (u64 offset = 0; offset + 0x40 <= file_size;) {
        file.seekg(std::streamoff(offset));
        if (!file.read(reinterpret_cast<char*>(header), 0x40)) break;
        const u16 tag = u16(header[0] | header[1] << 8);
        u64 size = U32(header + 4);
        const char* raw = reinterpret_cast<const char*>(header + 8);
        const std::string name(raw, std::find(raw, raw + 56, '\0'));
        if (tag == 0 && name == "EntityCount") size = 0;
        if (offset + 0x40 + size > file_size) break;
        const u64 start = offset + 0x40, end = start + size;
        offset = start + ((size + 15) & ~u64(15));
        if (name.size() < 14 || !name.ends_with(".dds")) continue;
        const auto texture = std::ranges::find_if(textures, [&](const Texture& t) { return name.compare(0, 10, t.id) == 0; });
        if (texture == std::end(textures)) continue;
        const size_t index = size_t(texture - std::begin(textures));
        if (labels[index].Empty() || (texture->form == Form::Atlas && seconds[index].Empty()) || size > (64u << 20)) continue;
        data.resize(size);
        file.seekg(std::streamoff(start));
        if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size))) break;
        // The DDS magic lies inside the entry, after a short prefix.
        static constexpr char kMagic[] = "DDS ";
        const auto magic = std::search(data.begin(), data.end(), kMagic, kMagic + 4);
        const size_t dds = size_t(magic - data.begin());
        if (magic == data.end() || dds + 128 > data.size()) continue;
        const bool dxt1 = !std::memcmp(&data[dds + 84], "DXT1", 4), dxt5 = !std::memcmp(&data[dds + 84], "DXT5", 4);
        const int height = int(U32(&data[dds + 12])), width = int(U32(&data[dds + 16]));
        if ((!dxt1 && !dxt5) || width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
            Log(std::format("Button icons: {} is not a DXT texture of a usable size; left as it is", name));
            continue;
        }
        std::vector<u8> pixels = Encode(Render(*texture, width, height, labels[index], seconds[index]), dxt5, data.size() - dds - 128);
        if (pixels.empty()) {
            Log(std::format("Button icons: {} ({}x{}) has mip levels of another size; left as it is", name, width, height));
            continue;
        }
        FileOverlay::Add(guest, start + dds + 128, std::move(pixels));
        ++count;
    }
    return count;
}
} // namespace

void Prepare(const std::filesystem::path& game, const Config& config) {
    if (config.button_icons == ButtonIcons::PlayStation) return;
    try {
        if (config.button_icons == ButtonIcons::Auto) {
            int gamepads = 0;
            if (SDL_JoystickID* ids = SDL_GetGamepads(&gamepads)) SDL_free(ids);
            if (gamepads > 0) {
                Log("Button icons: a gamepad is connected; the game keeps its PlayStation prompts");
                return;
            }
        }
        if (!LabelFont().ok) {
            Log("Button icons: no font found for the key labels; the game keeps its PlayStation prompts");
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        std::vector<Label> labels(std::size(textures)), seconds(std::size(textures));
        for (size_t i = 0; i < std::size(textures); ++i) {
            if (const auto label = LabelOf(textures[i].first)) labels[i] = *label;
            if (textures[i].second)
                if (const auto label = LabelOf(textures[i].second)) seconds[i] = *label;
        }
        auto* mounts = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        int total = 0;
        std::string found;
        for (const char* wad : wads) {
            const int count = PatchWad(wad, mounts->GetHostPath(wad), labels.data(), seconds.data());
            total += count;
            found += std::format("{}{} in {}", found.empty() ? "" : ", ", count, std::filesystem::path(wad).filename().string());
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        Log(std::format("Button icons: keyboard prompts, {} textures drawn ({}) in {} ms{}", total, found, ms,
                        total ? "" : "; the game folder has none of the known prompt textures"));
        (void)game;
    } catch (const std::exception& error) {
        Log(std::string("Button icons: ") + error.what() + "; the game keeps its PlayStation prompts");
    }
}
} // namespace ButtonGlyphs
} // namespace Gow3
