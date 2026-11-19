// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/imgui_layer.h"
#include "ui/imgui_texture.h"
#include "ui/imgui_core.h"
#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <imgui_internal.h>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include "main/host.h"
#include "cinzel_font_data.h"
#include "gow3_ui_theme.h"

namespace ImGui {
namespace Core::TextureManager {
struct Inner {
    std::atomic<u32> references{1};
    RefCountedTexture::Image image;
    ImTextureData texture;
    std::vector<u8> pixels;
    bool registered = false;  // with the ImGui context (NewFrame's thread only)
    ~Inner() { texture.Pixels = nullptr; }  // pixels owns the CPU storage
};
} // namespace Core::TextureManager
// Textures are created by any thread, registered with the context and uploaded by the backend in NewFrame / Render, and
// destroyed there once nothing refers to them.
static std::mutex texture_mutex;
static std::vector<std::unique_ptr<Core::TextureManager::Inner>> textures;
RefCountedTexture::RefCountedTexture() : inner(nullptr) {}
RefCountedTexture::RefCountedTexture(Core::TextureManager::Inner* texture) : inner(texture) {}
RefCountedTexture::RefCountedTexture(const RefCountedTexture& other) : inner(other.inner) {
    if (inner) ++inner->references;
}
RefCountedTexture::RefCountedTexture(RefCountedTexture&& other) noexcept : inner(std::exchange(other.inner, nullptr)) {}
RefCountedTexture& RefCountedTexture::operator=(const RefCountedTexture& other) {
    if (this != &other) { RefCountedTexture copy(other); std::swap(inner, copy.inner); }
    return *this;
}
RefCountedTexture& RefCountedTexture::operator=(RefCountedTexture&& other) noexcept {
    if (this != &other) { RefCountedTexture moved(std::move(other)); std::swap(inner, moved.inner); }
    return *this;
}
RefCountedTexture::~RefCountedTexture() { if (inner) --inner->references; }
RefCountedTexture::operator bool() const { return inner != nullptr; }
RefCountedTexture::Image RefCountedTexture::GetTexture() const { return inner ? inner->image : Image{}; }
RefCountedTexture RefCountedTexture::DecodePngTexture(std::vector<u8> data) {
    if (data.size() > INT_MAX) return {};
    int width{}, height{}, channels{};
    auto* pixels = stbi_load_from_memory(data.data(), int(data.size()), &width, &height, &channels, 4);
    if (!pixels) return {};
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(pixels, stbi_image_free);
    auto texture = std::make_unique<Core::TextureManager::Inner>();
    texture->pixels.assign(pixels, pixels + size_t(width) * height * 4);
    auto& image = texture->texture;
    image.Format = ImTextureFormat_RGBA32, image.Width = width, image.Height = height, image.BytesPerPixel = 4;
    image.Pixels = texture->pixels.data(), image.UseColors = true;
    image.SetStatus(ImTextureStatus_WantCreate);
    texture->image = {image.GetTexRef(), u32(width), u32(height)};
    const RefCountedTexture result(texture.get());
    std::scoped_lock lock(texture_mutex);
    textures.push_back(std::move(texture));
    return result;
}
RefCountedTexture RefCountedTexture::DecodePngFile(const std::filesystem::path& path) { return DecodePngTexture(Gow3::ReadFile(path)); }

// NewFrame (ui_mutex held): registers the new textures, retires the unreferenced ones and drops the destroyed.
static void UpdateTextures() {
    std::scoped_lock lock(texture_mutex);
    std::erase_if(textures, [](auto& t) {
        if (!t->registered) {
            if (!t->references) return true;
            RegisterUserTexture(&t->texture), t->registered = true;
        } else if (!t->references) {
            if (t->texture.Status == ImTextureStatus_Destroyed) return UnregisterUserTexture(&t->texture), true;
            t->texture.WantDestroyNextFrame = true;
        }
        return false;
    });
}

// Textures nobody refers to any more still need a frame for the backend to destroy them, even with no layer left to draw.
static std::atomic<bool> retiring;
static void UpdateRetiring() {
    std::scoped_lock lock(texture_mutex);
    retiring = std::ranges::any_of(textures, [](auto& t) { return t->registered && !t->references; });
}

static std::mutex layer_mutex;
static std::vector<Layer*> layers;
static std::vector<std::pair<bool, Layer*>> layer_changes;
static std::atomic<u32> captured;
static std::atomic<bool> ui_capture;
static std::atomic<bool> ui_visible;
static std::mutex ui_mutex;
void Layer::AddLayer(Layer* layer) {
    std::scoped_lock lock(layer_mutex);
    layer_changes.emplace_back(true, layer);
    SDL_Event event{}; event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}
void Layer::RemoveLayer(Layer* layer) {
    std::scoped_lock lock(layer_mutex);
    layer_changes.emplace_back(false, layer);
    SDL_Event event{}; event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}
namespace Core {
void Initialize(SDL_Window* window) {
    CreateContext();
    auto& io = GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = io.LogFilename = nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0370, 0x03FF, // Greek and Coptic (Ω 0x03A9)
        0x2000, 0x206F, // General Punctuation (· bullet)
        0,
    };
    Gow3::UI::g_font_cinzel = io.Fonts->AddFontFromMemoryTTF(
        (void*)Cinzel_Bold_data, Cinzel_Bold_size, 20.0f, &cfg, ranges);
    Gow3::UI::g_font_cinzel_dec = io.Fonts->AddFontFromMemoryTTF(
        (void*)CinzelDecorative_Bold_data, CinzelDecorative_Bold_size, 38.0f, &cfg, ranges);

#ifdef _WIN32
    char windows[MAX_PATH]{};
    const auto fonts = std::filesystem::path(std::string_view(windows, GetWindowsDirectoryA(windows, MAX_PATH))) / "Fonts";
    const auto body = fonts / "segoeui.ttf", pad = fonts / "seguisb.ttf";
#else
    std::filesystem::path body, pad;
    struct FontPair { const char* regular; const char* bold; };
    constexpr FontPair kFontPairs[] = {
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
        {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"},
        {"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"},
        {"/usr/share/fonts/truetype/DejaVuSans.ttf", "/usr/share/fonts/truetype/DejaVuSans-Bold.ttf"},
        {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf"},
        {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"},
        {"/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/google-noto/NotoSans-Bold.ttf"},
        {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"},
        {"/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf"},
    };
    for (const auto& pair : kFontPairs) {
        if (std::filesystem::exists(pair.regular)) {
            body = pair.regular;
            if (std::filesystem::exists(pair.bold)) pad = pair.bold;
            break;
        }
    }
#endif
    if (std::filesystem::exists(body)) Gow3::UI::g_font_body = io.Fonts->AddFontFromFileTTF(body.string().c_str());
    if (!Gow3::UI::g_font_body) Gow3::UI::g_font_body = io.Fonts->AddFontDefaultVector();
    if (std::filesystem::exists(pad)) Gow3::UI::g_font_pad = io.Fonts->AddFontFromFileTTF(pad.string().c_str());
    if (!Gow3::UI::g_font_pad) Gow3::UI::g_font_pad = Gow3::UI::g_font_body;
    StyleColorsDark();
    GetStyle().FontSizeBase = 20.f;
    if (!ImGui_ImplSDL3_InitForVulkan(window)) throw std::runtime_error("Initialize dialog input");
}
void SetMouseCursorHidden(bool hidden) {
    std::scoped_lock lock(ui_mutex);
    if (hidden) GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange, SDL_HideCursor();
    else GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange, SDL_ShowCursor();
}
// The prompts follow the device of the last input (the event thread only writes). Four changes within three seconds pin the
// gamepad until the game closes: a pad mapped to the mouse, or both used at once, would make every prompt flicker.
static std::atomic<bool> gamepad_prompts;
static std::atomic<bool> playstation_prompts;
static void TrackPromptDevice(const SDL_Event& e) {
    static bool pinned = false, used = false;
    static SDL_JoystickID last_gamepad{};
    static Uint64 changes[4]{};  // the times of the last changes, a ring
    static unsigned next = 0;
    const auto mouse = [](SDL_MouseID id) { return id != SDL_TOUCH_MOUSEID && id != SDL_PEN_MOUSEID; };
    int pad = -1;  // 1 gamepad, 0 keyboard and mouse
    SDL_JoystickID gamepad = last_gamepad;
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad) gamepad = e.gdevice.which;
        if (!used) pad = 1;
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad == e.gdevice.which) {
            SDL_JoystickID* remaining = SDL_GetGamepads(nullptr);
            gamepad = remaining ? remaining[0] : 0;
            SDL_free(remaining);
        }
        if (!gamepad) pad = 0, pinned = false;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: gamepad = e.gbutton.which, pad = 1; break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: gamepad = e.gtouchpad.which, pad = 1; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(int(e.gaxis.value)) > 16000) gamepad = e.gaxis.which, pad = 1;
        break;
    case SDL_EVENT_KEY_DOWN: if (!e.key.repeat) pad = 0; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: if (mouse(e.button.which)) pad = 0; break;
    case SDL_EVENT_MOUSE_WHEEL: if (mouse(e.wheel.which)) pad = 0; break;
    case SDL_EVENT_MOUSE_MOTION: if (mouse(e.motion.which) && std::abs(e.motion.xrel) + std::abs(e.motion.yrel) >= 2.0f) pad = 0; break;
    default: return;
    }
    if (gamepad != last_gamepad) {
        last_gamepad = gamepad;
        const SDL_GamepadType type = gamepad ? SDL_GetRealGamepadTypeForID(gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
        playstation_prompts.store(type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5,
                                  std::memory_order_relaxed);
    }
    if (pad < 0 || pinned) return;
    used |= e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED;
    if (gamepad_prompts.exchange(pad == 1, std::memory_order_relaxed) == (pad == 1) || !used) return;
    const Uint64 now = SDL_GetTicks();
    const Uint64 oldest = std::exchange(changes[next], now);
    next = (next + 1) % std::size(changes);
    if (gamepad && oldest && now - oldest <= 3000) pinned = true, gamepad_prompts.store(true, std::memory_order_relaxed);
}
bool GamepadPrompts() { return gamepad_prompts.load(std::memory_order_relaxed); }
bool PlayStationPrompts() { return playstation_prompts.load(std::memory_order_relaxed); }
bool ProcessEvent(const SDL_Event& event) {
    TrackPromptDevice(event);
    const bool active = MustKeepDrawing();
    if (!active && event.type != SDL_EVENT_GAMEPAD_ADDED && event.type != SDL_EVENT_GAMEPAD_REMOVED &&
        event.type != SDL_EVENT_MOUSE_BUTTON_UP) return false;
    std::scoped_lock lock(ui_mutex);
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (!active) { GetIO().ClearInputKeys(); GetIO().ClearEventsQueue(); }
    return ui_capture && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_WHEEL ||
        event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
}
void NewFrame() {
    std::scoped_lock lock(ui_mutex);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    UpdateTextures();
    ImGui::NewFrame();
    DrawLayers();
    ImGui::Render();
    UpdateRetiring();
    ui_visible = GetDrawData()->CmdListsCount != 0;
    ui_capture = MustKeepDrawing() && (GetIO().NavActive || GetIO().WantCaptureMouse || GetIO().WantCaptureKeyboard);
    if (!MustKeepDrawing()) { GetIO().ClearInputKeys(); GetIO().ClearEventsQueue(); }
}
void Render(VkCommandBuffer cmd, VkQueue queue) {
    if (!ui_visible && !retiring) return;
    std::scoped_lock lock(ui_mutex);
    auto* data = GetDrawData();
    if (!data || !data->Textures) return;
    // Font atlas pages may still be read by the previous presentation.
    if (std::ranges::any_of(*data->Textures, [](const auto* texture) { return texture->Status == ImTextureStatus_WantDestroy; })) vkQueueWaitIdle(queue);
    if (data->CmdListsCount) return ImGui_ImplVulkan_RenderDrawData(data, cmd);
    for (auto* texture : *data->Textures)  // nothing to draw: only the textures' creations and destructions
        if (texture->Status != ImTextureStatus_OK) ImGui_ImplVulkan_UpdateTexture(texture);
}
void AcquireGamepadInputCapture() { ++captured; }
void ReleaseGamepadInputCapture() {
    auto count = captured.load(std::memory_order_relaxed);
    while (count && !captured.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {}
}
bool IsGamepadInputCaptured() { return captured != 0 || ui_capture; }
bool MustKeepDrawing() {
    std::scoped_lock lock(layer_mutex);
    return !layers.empty() || !layer_changes.empty() || retiring;
}
void DrawLayers() {
    {
        std::scoped_lock lock(layer_mutex);
        for (auto [add, layer] : layer_changes) {
            if (add) {
                if (std::ranges::find(layers, layer) == layers.end()) layers.push_back(layer);
            } else {
                std::erase(layers, layer);
            }
        }
        layer_changes.clear();
    }
    for (auto* layer : layers) layer->Draw();
}
}
}
