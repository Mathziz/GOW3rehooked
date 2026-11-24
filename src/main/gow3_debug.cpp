// SPDX-License-Identifier: GPL-2.0-or-later
// gow3_debug (Linux): ships beside gow3 and starts it with GOW3_DEBUG=1, which turns on every diagnostic that keeps the game
// playable (debug/debug.h), so a user produces the log (_rehooked_/gow3.log), the frame recording, the long-frame samples,
// the light log and the exploded-vertex records without setting environment variables. Command-line arguments are passed on.
// Windows uses gow3_debug.cmd.
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
