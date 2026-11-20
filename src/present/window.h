// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include "main/config.h"

namespace Gow3::Window {
// Opens the SDL window that receives keyboard, mouse and gamepad input and shows the system dialogs, with the splash screen
// unless `splash` is false.
void Open(const Config& config, bool splash = true);
[[noreturn]] void OfferUpdate(const std::string& tag);
// The game folder cannot be started: no eboot.bin there (NoGame), only a packaged .fpkg (Packaged), or an eboot.bin of another
// version than 1.02 (WrongVersion). Shows the message screen and ends the process once it is dismissed.
enum class StartupError { NoGame, Packaged, WrongVersion };
[[noreturn]] void ReportStartupError(StartupError error);
// The previous run died before the start menu: asks whether to reset the settings, then starts the game again (with the default
// settings if so). Never returns; closing the window leaves the question for the next launch.
[[noreturn]] void AskAfterCrash(const std::filesystem::path& game_directory);
// Pumps window events on the calling (main) thread; closing the window flushes the log and exits.
bool RequestSettingsRestart(const Config& config);
// Restarts the game with JIT shaders for that one run, the saved settings left as they are (the AOT build was skipped).
bool RequestJitRestart();
void NotifyMainMenuReady();
// True when this process is such a restart; the request is consumed, so later restarts are AOT again.
bool TakeJitOnce();
[[noreturn]] void RunEventLoop(const std::filesystem::path& log_path, const std::filesystem::path& game_directory);
} // namespace Gow3::Window
