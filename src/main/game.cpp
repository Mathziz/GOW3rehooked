// SPDX-License-Identifier: GPL-2.0-or-later
#include "main/api.h"
#include "main/host.h"
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#ifdef _WIN32
#include "exe_icon.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#else
#include <SDL3/SDL.h>

int main(int count, char** args) {
    // The legacy AMD Vulkan layer can hide RADV unless asked for it before Vulkan starts.
    setenv("AMD_VULKAN_ICD", "RADV", 0);
#endif
    bool inspect{};
    bool validate_runtime{};
#ifdef _WIN32
    HANDLE parent{}, cancel{}, ready{};
    constexpr auto inspect_option = L"--inspect", validate_option = L"--validate-runtime", game_dir_option = L"--game-dir";
    constexpr auto usage = "Usage: gow3.exe [--inspect | --validate-runtime] [--game-dir <directory>]";
#else
    constexpr auto inspect_option = "--inspect", validate_option = "--validate-runtime", game_dir_option = "--game-dir";
    constexpr auto usage = "Usage: gow3 [--inspect | --validate-runtime] [--game-dir <directory>]";
#endif
    try {
        const auto app = Gow3::ExecutableDirectory();
        auto game = app;
#ifdef _WIN32
        int count{};
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) throw std::runtime_error(Gow3::HostError("Read arguments"));
#else
        if (const char* image = std::getenv("APPIMAGE"); image && *image)
            game = std::filesystem::path(image).parent_path();
#endif
        bool valid = true;
        for (int i = 1; i < count; ++i) {
            const std::basic_string_view<Gow3PathChar> arg(args[i]);
            if (arg == inspect_option) inspect = true;
            else if (arg == validate_option) validate_runtime = true;
            else if (arg == game_dir_option && i + 1 < count) game = args[++i];
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
        if (!parent && !inspect && !validate_runtime) Gow3::EmbedGameIcon(game / L"sce_sys" / L"icon0.png");
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
