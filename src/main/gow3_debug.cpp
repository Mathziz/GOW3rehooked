// SPDX-License-Identifier: GPL-2.0-or-later
// gow3_debug.exe: ships beside gow3.exe and starts it with GOW3_DEBUG=1, which turns on every diagnostic that keeps the game
// playable (debug/debug.h), so a user produces the log (_rehooked_\gow3.log), the frame recording, the long-frame samples,
// the light log and the exploded-vertex records without setting environment variables. Command-line arguments are passed on.
#ifdef _WIN32
#include <windows.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t self[MAX_PATH];
    std::wstring directory(self, GetModuleFileNameW(nullptr, self, MAX_PATH));
    directory.resize(directory.find_last_of(L'\\') + 1);
    const std::wstring game = directory + L"gow3.exe";
    // The arguments after this program's own name.
    const wchar_t* arguments = GetCommandLineW();
    if (*arguments == L'"') {
        for (++arguments; *arguments && *arguments != L'"';) ++arguments;
        if (*arguments) ++arguments;
    } else {
        while (*arguments && *arguments != L' ') ++arguments;
    }
    std::wstring command = L"\"" + game + L"\"" + arguments;
    wchar_t existing[4]{};
    if (GetEnvironmentVariableW(L"GOW3_DEBUG", existing, 4) != 1 || (existing[0] != L'1' && existing[0] != L'2'))
        SetEnvironmentVariableW(L"GOW3_DEBUG", L"1");
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process)) {
        MessageBoxW(nullptr, (L"Cannot start " + game + L".\nPut gow3_debug.exe in the same folder as gow3.exe.").c_str(), L"God of War III",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
#else
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <unistd.h>

int main(int, char** args) {
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        std::fprintf(stderr, "Cannot locate gow3_debug: %s\n", error.message().c_str());
        return 1;
    }
    const char* image = std::getenv("APPIMAGE");
    auto game = image && *image ? (std::filesystem::path(image).parent_path() / "gow3.AppImage").string()
                               : (self.parent_path() / "gow3").string();
    const auto* existing = std::getenv("GOW3_DEBUG");
    if ((!existing || (std::string_view(existing) != "1" && std::string_view(existing) != "2")) && setenv("GOW3_DEBUG", "1", 1)) {
        std::perror("GOW3_DEBUG");
        return 1;
    }
    args[0] = game.data();
    execv(game.c_str(), args);
    std::perror(game.c_str());
    return 1;
}
#endif
