// SPDX-License-Identifier: GPL-2.0-or-later
#include "main/api.h"
#include "main/host.h"
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#ifdef _WIN32
#ifdef GOW3_EXE_ICON
#include "exe_icon.h"
#endif
#ifdef GOW3_RELEASE_TAG
#include "main/update.h"
#include "present/window.h"
#endif
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#else
#include <SDL3/SDL.h>
#include "present/window.h"

int main(int count, char** args) {
    // The legacy AMD Vulkan layer can hide RADV unless asked for it before Vulkan starts.
    setenv("AMD_VULKAN_ICD", "RADV", 0);
#endif
    bool inspect{};
    bool validate_runtime{};
#ifdef _WIN32
    HANDLE parent{}, cancel{}, ready{};
    constexpr auto inspect_option = L"--inspect", validate_option = L"--validate-runtime", game_dir_option = L"--game-dir",
                   debug_option = L"--debug", deep_debug_option = L"--debug=2";
    constexpr auto usage = "Usage: gow3.exe [--inspect | --validate-runtime] [--debug | --debug=2] [--game-dir <directory>]";
#else
    constexpr auto inspect_option = "--inspect", validate_option = "--validate-runtime", game_dir_option = "--game-dir",
                   debug_option = "--debug", deep_debug_option = "--debug=2";
    constexpr auto usage = "Usage: gow3 [--inspect | --validate-runtime] [--debug | --debug=2] [--game-dir <directory>]";
#endif
    try {
        const auto app = Gow3::ExecutableDirectory();
        auto game = app;
#ifdef _WIN32
        int count{};
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) throw std::runtime_error(Gow3::HostError("Read arguments"));
#else
        if (count == 3 && std::string_view(args[1]) == "--update-offer")
            Gow3::Window::OfferUpdate(args[2]);
        if (const char* image = std::getenv("APPIMAGE"); image && *image)
            game = std::filesystem::path(image).parent_path();
#endif
#if defined(_WIN32) && defined(GOW3_RELEASE_TAG)
        if (count == 3 && std::wstring_view(args[1]) == L"--update-offer") {
            std::wstring tag(args[2]);
            LocalFree(args);
            Gow3::Window::OfferUpdate(std::string(tag.begin(), tag.end()));
        }
#endif
        bool valid = true;
        for (int i = 1; i < count; ++i) {
            const std::basic_string_view<Gow3PathChar> arg(args[i]);
            if (arg == inspect_option) inspect = true;
            else if (arg == validate_option) validate_runtime = true;
            else if (arg == game_dir_option && i + 1 < count) game = args[++i];
            else if (arg == debug_option || arg == deep_debug_option) {} // read before main (DebugMode)
#ifdef _WIN32
            else if (arg == L"--restart-after" && i + 3 < count) {
                HANDLE* handles[]{&parent, &cancel, &ready};
                for (auto* handle : handles) {
                    wchar_t* end{};
                    const auto value = std::wcstoull(args[++i], &end, 10);
                    if (!value || *end) valid = false;
                    *handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
                }
            }
#endif
            else valid = false;
        }
#ifdef _WIN32
        LocalFree(args);
#endif
        if (!valid || int(inspect) + int(validate_runtime) > 1)
            throw std::runtime_error(usage);
#ifdef _WIN32
        if (parent) {
            if (!SetEvent(ready)) throw std::runtime_error(Gow3::HostError("Prepare restart"));
            const HANDLE waits[]{cancel, parent};
            const auto result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            CloseHandle(ready);
            CloseHandle(cancel);
            CloseHandle(parent);
            if (result == WAIT_OBJECT_0) return 0;
            if (result != WAIT_OBJECT_0 + 1) throw std::runtime_error(Gow3::HostError("Wait for previous game"));
        }
        if (!parent && !inspect && !validate_runtime) {
            // The runtime DLL, launchers and update stand-in of v0.1.5 to v0.2.1, which antiviruses flagged.
            for (const auto* name : {L"gow3.dll", L"gow3_debug.exe", L"gow3_update.exe", L"gow3_update.update.old"})
                DeleteFileW((app / name).c_str());
#ifdef GOW3_RELEASE_TAG
            if (Gow3::Update::OfferAndInstall(app, game, GOW3_RELEASE_TAG)) return 0;
#elif defined(GOW3_EXE_ICON)
            Gow3::EmbedGameIcon(game / L"sce_sys" / L"icon0.png");
#endif
        }
#endif
        const auto result = Gow3Run(app.c_str(), game.c_str(), inspect ? Gow3Inspect :
                                    validate_runtime ? Gow3ValidateRuntime : 0);
#ifdef _WIN32
        if (result && !inspect && !validate_runtime)
            MessageBoxW(nullptr, Gow3LastError(), L"God of War III", MB_OK | MB_ICONERROR);
#else
        if (result) {
            std::fprintf(stderr, "%s\n", Gow3LastError());
            if (!inspect && !validate_runtime)
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "God of War III", Gow3LastError(), nullptr);
        }
#endif
        return result;
    } catch (const std::exception& error) {
#ifdef _WIN32
        if (!inspect && !validate_runtime) MessageBoxA(nullptr, error.what(), "God of War III", MB_OK | MB_ICONERROR);
#else
        std::fprintf(stderr, "%s\n", error.what());
#endif
        return 1;
    }
}
