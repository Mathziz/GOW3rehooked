// SPDX-License-Identifier: GPL-2.0-or-later
#include "exe_icon.h"
#include "main/api.h"
#include "main/host.h"
#include <stdexcept>
#include <cstdlib>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool inspect{};
    bool validate_runtime{};
    HANDLE parent{}, cancel{}, ready{};
    try {
        const auto app = Gow3::ExecutableDirectory();
        auto game = app;
        int count{};
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) throw std::runtime_error(Gow3::WindowsError("Read arguments"));
        bool valid = true;
        for (int i = 1; i < count; ++i) {
            const std::wstring_view arg(args[i]);
            if (arg == L"--inspect") inspect = true;
            else if (arg == L"--validate-runtime") validate_runtime = true;
            else if (arg == L"--game-dir" && i + 1 < count) game = args[++i];
            else if (arg == L"--restart-after" && i + 3 < count) {
                HANDLE* handles[]{&parent, &cancel, &ready};
                for (auto* handle : handles) {
                    wchar_t* end{};
                    const auto value = std::wcstoull(args[++i], &end, 10);
                    if (!value || *end) valid = false;
                    *handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
                }
            }
            else valid = false;
        }
        LocalFree(args);
        if (!valid || int(inspect) + int(validate_runtime) > 1)
            throw std::runtime_error("Usage: gow3.exe [--inspect | --validate-runtime] [--game-dir <directory>]");
        if (parent) {
            if (!SetEvent(ready)) throw std::runtime_error(Gow3::WindowsError("Prepare restart"));
            const HANDLE waits[]{cancel, parent};
            const auto result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            CloseHandle(ready);
            CloseHandle(cancel);
            CloseHandle(parent);
            if (result == WAIT_OBJECT_0) return 0;
            if (result != WAIT_OBJECT_0 + 1) throw std::runtime_error(Gow3::WindowsError("Wait for previous game"));
        }
        if (!parent && !inspect && !validate_runtime) Gow3::EmbedGameIcon(game / L"sce_sys" / L"icon0.png");
        const auto result = Gow3Run(app.c_str(), game.c_str(), inspect ? Gow3Inspect :
                                    validate_runtime ? Gow3ValidateRuntime : 0);
        if (result && !inspect && !validate_runtime)
            MessageBoxW(nullptr, Gow3LastError(), L"God of War III", MB_OK | MB_ICONERROR);
        return result;
    } catch (const std::exception& error) {
        if (!inspect && !validate_runtime) MessageBoxA(nullptr, error.what(), "God of War III", MB_OK | MB_ICONERROR);
        return 1;
    }
}
