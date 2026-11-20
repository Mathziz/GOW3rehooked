// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/window.h"
#include <SDL3/SDL.h>
#include <stb_image.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
#ifndef _WIN32
#include <linux/close_range.h>
#include <poll.h>
#endif
#include "orbis/common/elf_info.h"
#include "orbis/core/emulator_settings.h"
#include "ui/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input/input_mouse.h"
#include "orbis/internals/save_data/save_backup.h"
#include "main/host.h"
#include "present/presenter.h"
#include "present/prompt.h"
#include "present/save_import_ui.h"
#include "present/splash.h"
#include "gpu/gpu_thread.h"
#include "debug/dump_archive.h"
#include "debug/render_probe.h"
#include "debug/s4_trace.h"
#include "patches/hooks.h"
#include "gpu/device.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_recipes.h"
#include "present/sdl_window.h"

Frontend::WindowSDL* g_window; // read by input/input_mouse.cpp

namespace Gow3::Window {
namespace {
SDL_Window* window;
Uint32 restart_event, main_menu_event;
Frontend::WindowSDL input_window;

void SaveRestartSettings(const std::filesystem::path& game, const Config& config) {
    const auto path = game / PortDir::Root / PortDir::Settings / "config.cfg";
    const bool mechanism_changed = LoadConfig(path).shader_mechanism != config.shader_mechanism;
    SaveMenuConfig(path, config);
    if (mechanism_changed) ResetAotCooldown();
}

#ifdef _WIN32
struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::wstring QuotePath(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    const auto last = value.find_last_not_of(L'\\');
    value.append(last == value.npos ? value.size() : value.size() - last - 1, L'\\');
    return L'"' + value + L'"';
}

// The replacement game reads this variable and clears it: its shaders are JIT for that run only.
constexpr const wchar_t* JitOnceVariable = L"GOW3_JIT_ONCE";

// `jit_once`: the replacement runs with JIT shaders while the saved settings (AOT) stay as they are.
void Restart(const std::filesystem::path& game, const Config* config, bool jit_once) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle parent, cancel, ready;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value, SYNCHRONIZE, TRUE, 0) ||
        !(cancel.value = CreateEventW(&security, TRUE, FALSE, nullptr)) ||
        !(ready.value = CreateEventW(&security, TRUE, FALSE, nullptr)))
        throw std::runtime_error(HostError("Prepare restart handles"));
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        throw std::runtime_error(HostError("Prepare restart attributes"));
    const auto cleanup = std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&DeleteProcThreadAttributeList)>(attributes, DeleteProcThreadAttributeList);
    HANDLE inherited[]{parent.value, cancel.value, ready.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error(HostError("Prepare restart inheritance"));
    const auto executable = ExecutableDirectory() / L"gow3.exe";
    std::wstring command = QuotePath(executable) + L" --restart-after " +
        std::to_wstring(reinterpret_cast<uintptr_t>(parent.value)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(cancel.value)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(ready.value)) + L" --game-dir " + QuotePath(game);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (jit_once) SetEnvironmentVariableW(JitOnceVariable, L"1");  // inherited by the replacement only
    const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, nullptr, &startup.StartupInfo, &process);
    if (jit_once) SetEnvironmentVariableW(JitOnceVariable, nullptr);
    if (!started) throw std::runtime_error(HostError("Start replacement game"));
    Handle child, thread;
    child.value = process.hProcess;
    thread.value = process.hThread;
    const HANDLE waits[]{ready.value, child.value};
    if (WaitForMultipleObjects(2, waits, FALSE, 10000) != WAIT_OBJECT_0) {
        SetEvent(cancel.value);
        throw std::runtime_error("Replacement game could not prepare for restart");
    }
    try {
        if (config && !jit_once) SaveRestartSettings(game, *config);
    } catch (...) {
        SetEvent(cancel.value);
        throw;
    }
}

#else
constexpr const char* JitOnceVariable = "GOW3_JIT_ONCE";

[[noreturn]] void Restart(const std::filesystem::path& game, bool jit_once) {
    if (!jit_once || setenv(JitOnceVariable, "1", 1) == 0) {
        // Retain the AppImage mount while this process restarts, and drop all descriptors of the old runtime.
        if (close_range(3, ~0u, CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE) == 0)
            execl("/proc/self/exe", ExecutableName, "--game-dir", game.c_str(), nullptr);
    }
    const auto error = HostError("Start replacement game");
    Log(error);
    try { FlushLog(PortDirectory() / "gow3.log"); } catch (...) {}
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "God of War III", error.c_str(), window);
    ExitImmediately(1);
}
#endif

// Event forwarding mirrors src/sdl_window.cpp (WindowSDL::WaitEvent, OnKeyboardMouseInput, OnGamepadEvent,
// InitTimers); bindings come from the upstream input config (user/input_config/*.ini).
Uint32 SDLCALL PollController(void* controller, SDL_TimerID, Uint32 interval) {
    static_cast<Input::GameController*>(controller)->PollState();
    return interval;
}

Uint32 SDLCALL WheelOff(void* event, SDL_TimerID, Uint32) {
    auto* off = static_cast<SDL_Event*>(event);
    off->type = SDL_EVENT_MOUSE_WHEEL_OFF;
    SDL_PushEvent(off);
    delete off;
    return 0;
}

void OnInput(const SDL_Event& event) {
    if (event.type == SDL_EVENT_MOUSE_WHEEL) SDL_AddTimer(33, WheelOff, new SDL_Event(event));
    if (Input::UpdatePressedKeys(Input::InputBinding::GetInputEventFromSDLEvent(event))) Input::ActivateOutputsFromInputs();
}

void OnGamepad(Input::GameControllers& controllers, const SDL_Event& event) {
    const auto pad = [&](SDL_JoystickID id) {
        const u8 index = controllers.GetGamepadIndexFromJoystickId(id);
        return index < 5 ? controllers[index] : nullptr;
    };
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (auto* c = pad(event.gsensor.which)) {
            if (event.gsensor.sensor == SDL_SENSOR_GYRO) c->UpdateGyro(event.gsensor.data);
            else if (event.gsensor.sensor == SDL_SENSOR_ACCEL) c->UpdateAcceleration(event.gsensor.data);
        }
        return;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        if (auto* c = pad(event.gtouchpad.which))
            c->SetTouchpadState(event.gtouchpad.finger, event.type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP, event.gtouchpad.x,
                                event.gtouchpad.y);
        return;
    default:
        // The touchpad button keeps its fixed binding so touchpad handling stays intact.
        if (event.type != SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
            if (auto* c = pad(event.gbutton.which))
                c->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            return;
        }
        OnInput(event);
    }
}

void UpdateWindowSize() { SDL_GetWindowSizeInPixels(window, &input_window.width, &input_window.height); }

// The window and its taskbar button take the game's own icon, read from its folder (nothing of it ships with the port).
void SetGameIcon() {
    std::ifstream file(Common::ElfInfo::Instance().GetGameFolder() / "sce_sys" / "icon0.png", std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    int width{}, height{}, channels{};
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return Log("No game icon for the window");
    if (SDL_Surface* icon = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4)) {
        SDL_SetWindowIcon(window, icon); // copied by SDL
        SDL_DestroySurface(icon);
    }
    stbi_image_free(pixels);
}

// The display mode of Exclusive, and of Borderless with VRR.
// - Size: Borderless keeps the desktop's. Exclusive takes the smallest size of the display's own shape (scaled to the panel
//   undistorted) that holds the whole picture: the game's resolution if listed (a DSR size too), else one the picture is
//   letterboxed in (480p: usually 1280x720), never larger than the desktop's.
// - Refresh rate: with VRR the highest of that size (variable refresh then follows the game below it). Else the frame rate, else
//   the highest multiple of it (40 FPS: 120 or 240 Hz, each frame shown for a whole number of refreshes), else the highest below
//   it, else the lowest.
void SetDisplayMode(const Config& config) {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    int count{};
    const std::unique_ptr<SDL_DisplayMode*, decltype(&SDL_free)> list(SDL_GetFullscreenDisplayModes(display, &count), SDL_free);
    if (!desktop || !list) return Log("Display: no display modes, the desktop's is kept");
    const std::span modes(list.get(), size_t(count));  // largest first, the refresh rates of a size highest first
    const auto& game = Resolutions[config.resolution];
    int w = desktop->w, h = desktop->h;
    for (const SDL_DisplayMode* m : modes)  // ends on the smallest that holds it
        if (config.display_mode == DisplayMode::Fullscreen && m->w >= game.width && m->h >= game.height && (m->w <= desktop->w || (m->w == game.width && m->h == game.height)) &&
            std::abs(float(m->w) * float(desktop->h) / (float(m->h) * float(desktop->w)) - 1.f) < 0.01f) w = m->w, h = m->h;
    const float fps = float(config.fps);
    const SDL_DisplayMode *highest{}, *exact{}, *multiple{}, *below{}, *lowest{};
    for (const SDL_DisplayMode* m : modes) {
        if (m->w != w || m->h != h || SDL_BITSPERPIXEL(m->format) != SDL_BITSPERPIXEL(desktop->format)) continue;
        const float n = std::round(m->refresh_rate / fps);
        const bool whole = n >= 1.f && std::abs(m->refresh_rate - n * fps) <= m->refresh_rate * 0.005f;
        if (!highest) highest = m;
        if (!exact && whole && n == 1.f) exact = m;
        if (!multiple && whole) multiple = m;
        if (!below && m->refresh_rate <= fps + 0.5f) below = m;
        lowest = m;
    }
    const SDL_DisplayMode* chosen = config.vrr ? highest : exact ? exact : multiple ? multiple : below ? below : lowest;
    if (!chosen) chosen = desktop;
    const char* name = config.display_mode == DisplayMode::Fullscreen ? "Exclusive" : "Borderless";
    if (!SDL_SetWindowFullscreenMode(window, chosen)) return Log(std::format("Display: {} mode not set: {}", name, SDL_GetError()));
    Log(std::format("Display: {}{} at {}x{}, {:.2f} Hz (desktop {}x{}, {:.2f} Hz)", name, config.vrr ? " with VRR" : "", chosen->w,
                    chosen->h, chosen->refresh_rate, desktop->w, desktop->h, desktop->refresh_rate));
}
} // namespace

void Open(const Config& config, bool splash) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    // The input and ImGui code use SDL_EVENT_USER through SDL_EVENT_USER + 15 directly.
    if (!(restart_event = SDL_RegisterEvents(18))) throw std::runtime_error(std::string("SDL_RegisterEvents: ") + SDL_GetError());
    restart_event += 16;
    main_menu_event = restart_event + 1;
    // The window starts at the game resolution, scaled down to fit the desktop work area when windowed.
    const auto& resolution = Resolutions[config.resolution];
    int width = resolution.width, height = resolution.height;
    if (SDL_Rect area{}; config.display_mode == DisplayMode::Windowed && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area)) {
        const float scale = std::min({1.f, float(area.w) / float(width), float(area.h) / float(height)});
        width = int(float(width) * scale);
        height = int(float(height) * scale);
    }
    // Exclusive, and Borderless with VRR, set a display mode: hidden until then, so the display switches once. Borderless stays on
    // screen (not minimized, the mode kept) when it loses the focus.
    const bool sets_mode = config.display_mode == DisplayMode::Fullscreen || (config.display_mode == DisplayMode::Borderless && config.vrr);
    if (config.display_mode == DisplayMode::Borderless) SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    window = SDL_CreateWindow("God of War III Remastered", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                              (config.display_mode == DisplayMode::Windowed ? 0 : SDL_WINDOW_FULLSCREEN) | (sets_mode ? SDL_WINDOW_HIDDEN : 0));
    if (!window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    if (sets_mode) SetDisplayMode(config), SDL_ShowWindow(window);
    SetGameIcon();
    Vulkan::Initialize(window);
    ImGui::Core::Initialize(window);
    Presenter::Initialize(window, config);
    Splash::Show(!config.skip_intro, splash);
}

namespace {
// The message screen on a window of its own, until it is answered; closing the window ends the process.
Prompt::Answer Ask(const Prompt::Content& content) {
    Open(Config{}, false);
    Prompt::Show(content);
    Prompt::Answer answer;
    for (SDL_Event event; (answer = Prompt::Result()) == Prompt::Answer::Pending;) {
        if (SDL_WaitEventTimeout(&event, 16)) {
            if (event.type == SDL_EVENT_QUIT) {
#ifdef _WIN32
                TerminateProcess(GetCurrentProcess(), 0);
#else
                ExitImmediately(0);
#endif
            }
            ImGui::Core::ProcessEvent(event);
        }
        ImGui::Core::NewFrame();
    }
    return answer;
}
} // namespace

void OfferUpdate(const std::string& tag) {
    const auto answer = Ask({"UPDATE AVAILABLE", "INSTALL " + tag + "?",
                             {"Yes downloads the update and restarts the game.", "No skips this release."}, true});
    const char reply = answer == Prompt::Answer::Yes ? 'Y' : 'N';
#ifdef _WIN32
    DWORD written{};
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), &reply, 1, &written, nullptr) || reply == 'N')
        ExitImmediately(0);
#else
    if (write(STDOUT_FILENO, &reply, 1) != 1) ExitImmediately(1);
#endif
    if (reply == 'Y')
        Prompt::Show({"UPDATING", "INSTALLING " + tag, {"The game will reopen automatically."}, false, {}, true});
    // The updater closes its pipe when installation finishes or fails.
#ifdef _WIN32
    while (PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, nullptr, nullptr)) {
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    while (poll(&input, 1, 0) == 0) {
#endif
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 16)) ImGui::Core::ProcessEvent(event);
        ImGui::Core::NewFrame();
    }
#ifndef _WIN32
    char error[1024]{};
    if (read(STDIN_FILENO, error, sizeof(error) - 1) > 0)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GOW3rehooked update", error, window);
#endif
    ExitImmediately(0);
}

void ReportStartupError(StartupError error) {
    static constexpr const char* text[][3]{
#ifdef _WIN32
        {"EBOOT.BIN NOT FOUND", "Place gow3.exe next to eboot.bin.", ""},
#else
        {"EBOOT.BIN NOT FOUND", "Place gow3.AppImage or gow3 next to eboot.bin.", ""},
#endif
        {"YOUR GAME IS COMPRESSED", "Extract it before playing.", ""},
        {"YOUR GAME IS NOT VERSION 1.02", "Update it to version 1.02 before playing.", "CUSA01715 (Europe) and CUSA01623 (America) are supported."}};
    const auto& t = text[size_t(error)];
    Ask({"ERROR STARTING THE GAME", t[0], {t[1], t[2]}, false});
    ExitImmediately(1);
}

void AskAfterCrash(const std::filesystem::path& game_directory) {
    // Closing the window leaves the interrupted session, so the question is asked again.
    const auto answer = Ask({"THE GAME CRASHED ON ITS LAST RUN", "DO YOU WANT TO RESET THE SETTINGS?",
                             {"Yes deletes config.cfg and restarts with the default settings.", "No keeps your settings and starts the game normally."}, true});
    std::error_code error;
    if (answer == Prompt::Answer::Yes)
        std::filesystem::remove(game_directory / PortDir::Root / PortDir::Settings / "config.cfg", error);
    EndLaunch();
#ifdef _WIN32
    const auto executable = ExecutableDirectory() / L"gow3.exe";
    std::wstring command = QuotePath(executable) + L" --game-dir " + QuotePath(game_directory);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "God of War III", HostError("Start the game").c_str(), window);
    }
    ExitImmediately(0);
#else
    Restart(game_directory, false);
#endif
}

namespace {
bool RequestRestart(const Config& config, bool jit_once) {
    auto snapshot = std::make_unique<Config>(config);
    SDL_Event event{};
    event.type = restart_event;
    event.user.data1 = snapshot.get();
    event.user.data2 = jit_once ? snapshot.get() : nullptr;  // only whether it is set matters
    if (!SDL_PushEvent(&event)) return false;
    snapshot.release();
    return true;
}
} // namespace

bool RequestSettingsRestart(const Config& config) { return RequestRestart(config, false); }
bool RequestJitRestart() { return RequestRestart(Config{}, true); }

void NotifyMainMenuReady() {
    static std::atomic_flag queued;
    if (queued.test_and_set()) return;
    SDL_Event event{};
    event.type = main_menu_event;
    if (!SDL_PushEvent(&event)) queued.clear();
}

bool TakeJitOnce() {
#ifdef _WIN32
    const bool set = GetEnvironmentVariableW(JitOnceVariable, nullptr, 0) != 0;
    if (set) SetEnvironmentVariableW(JitOnceVariable, nullptr);  // the next restart is AOT again
#else
    const bool set = std::getenv(JitOnceVariable) != nullptr;
    if (set) unsetenv(JitOnceVariable);
#endif
    return set;
}

void RunEventLoop(const std::filesystem::path& log_path, const std::filesystem::path& game_directory) {
#ifndef _WIN32
    bool restart = false, jit_once = false;
#endif
    auto& controllers = *Common::Singleton<Input::GameControllers>::Instance();
    Input::ControllerOutput::LinkJoystickAxes();
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
    if (EmulatorSettings.IsBackgroundControllerInput()) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    UpdateWindowSize();
    g_window = &input_window;
    for (int i = 0; i < 4; ++i) SDL_AddTimer(4, PollController, controllers[i]);
    SDL_AddTimer(33, Input::MousePolling, controllers[0]);
    Uint64 mouse_time = SDL_GetTicks();
    bool mouse_hidden = false;
    Splash::Priority priority;  // the hold-to-skip input and the splash's frames keep up with the shader build
    for (SDL_Event event;;) {
        priority.Update();
        if (SaveImportUi::Update()) {
#ifdef _WIN32
            try {
                Restart(game_directory, nullptr, false);
                break;
            } catch (const std::exception& error) {
                Log(std::string("Save import restart failed: ") + error.what());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "God of War III", error.what(), window);
                SaveImportUi::RestartFailed();
            }
#else
            restart = true;
            break;
#endif
        }
        const bool mouse_inside = SDL_GetMouseFocus() == window;
        const Uint64 elapsed = SDL_GetTicks() - mouse_time;
        if (!SaveImportUi::Active() && mouse_inside && !mouse_hidden && elapsed >= 3000) {
            ImGui::Core::SetMouseCursorHidden(mouse_hidden = true);
        } else if (!mouse_inside && mouse_hidden) {
            ImGui::Core::SetMouseCursorHidden(mouse_hidden = false);
        }
        int timeout = !SaveImportUi::Active() && mouse_inside && !mouse_hidden ? int(3000 - elapsed) : -1;
        if (ImGui::Core::MustKeepDrawing()) timeout = timeout < 0 ? 16 : std::min(timeout, 16);
        if (!SDL_WaitEventTimeout(&event, timeout)) {
            if (ImGui::Core::MustKeepDrawing()) ImGui::Core::NewFrame();
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL ||
            event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
            mouse_time = SDL_GetTicks();
            if (mouse_hidden) ImGui::Core::SetMouseCursorHidden(mouse_hidden = false);
        }
        if (event.type == main_menu_event) {
            SaveImportUi::Offer(window, game_directory);
            if (SaveImportUi::Active() && mouse_hidden) ImGui::Core::SetMouseCursorHidden(mouse_hidden = false);
            continue;
        }
        if (event.type == restart_event) {
            std::unique_ptr<Config> config(static_cast<Config*>(event.user.data1));
            try {
#ifdef _WIN32
                Restart(game_directory, config.get(), event.user.data2 != nullptr);
#else
                jit_once = event.user.data2 != nullptr;
                if (!jit_once) SaveRestartSettings(game_directory, *config);
                restart = true;
#endif
                break;
            } catch (const std::exception& error) {
                Log(std::string("Settings restart failed: ") + error.what());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "God of War III", error.what(), window);
                continue;
            }
        }
        const bool captured = ImGui::Core::ProcessEvent(event);
        if (ImGui::Core::MustKeepDrawing()) ImGui::Core::NewFrame();
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) Presenter::UpdateDisplay();
        if (captured || (SaveImportUi::Active() && (event.type == SDL_EVENT_KEY_DOWN ||
                         event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN))) continue;
        switch (event.type) {
        case SDL_EVENT_QUIT: case SDL_EVENT_QUIT_DIALOG: break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: UpdateWindowSize(); continue;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_MOUSE_WHEEL_OFF: case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            OnInput(event);
            continue;
        case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED: controllers.TryOpenSDLControllers(); continue;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            OnGamepad(controllers, event);
            continue;
        case SDL_EVENT_TOGGLE_FULLSCREEN:
            SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            continue;
        case SDL_EVENT_RELOAD_INPUTS: Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial())); continue;
        case SDL_EVENT_MOUSE_TO_JOYSTICK:
            SDL_SetWindowRelativeMouseMode(window, Input::ToggleMouseModeTo(Input::MouseMode::Joystick));
            continue;
        case SDL_EVENT_MOUSE_TO_GYRO:
            SDL_SetWindowRelativeMouseMode(window, Input::ToggleMouseModeTo(Input::MouseMode::Gyro));
            continue;
        case SDL_EVENT_MOUSE_TO_TOUCHPAD:
            Input::ToggleMouseModeTo(Input::MouseMode::Touchpad);
            SDL_SetWindowRelativeMouseMode(window, false);
            continue;
        default: continue;
        }
        break;
    }
    // Guest threads cannot be unwound; the process ends with the window.
    SaveImportUi::WaitForCopy();
    Libraries::SaveData::Backup::StopThread();
    StopDiagnosticHooks();
    if (S4::enabled) S4::Stop();
    RenderProbe::Write();
    GpuThread::WriteFrames();
    DumpArchive::Flush();
    Gpu::ShaderCache::Instance().Finish();  // device destructors do not run: the cache's dictionary and the recipes' report end here
    Gpu::Recipes::Instance().Report();
    try { FlushLog(log_path); } catch (...) {}
    EndLaunch();
#ifndef _WIN32
    if (restart) Restart(game_directory, jit_once);
#endif
    ExitImmediately(0);
}
} // namespace Gow3::Window
