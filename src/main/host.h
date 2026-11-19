// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "debug/debug.h" // the GOW3_DEBUG gate, for everything that includes the host header

namespace Gow3 {
#ifdef _WIN32
inline constexpr auto ExecutableName = L"gow3.exe";
[[gnu::always_inline]] inline auto ThreadId() { return GetCurrentThreadId(); }
inline void LocalTime(std::tm& tm, std::time_t now) { localtime_s(&tm, &now); }
#else
inline constexpr auto ExecutableName = "gow3";
inline auto ThreadId() { return static_cast<std::uint32_t>(gettid()); }
inline void LocalTime(std::tm& tm, std::time_t now) { localtime_r(&now, &tm); }
#endif
bool ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination);
[[noreturn]] void ExitImmediately(int status);
// Port directory beside eboot.bin and its folders, each created by its first writer. The logs and diagnostic recordings
// (gow3.log, frames-*.bin, video traces) are files in the Root itself; everything a debug run dumps goes under Dumps.
namespace PortDir {
inline constexpr const char *Root = "_rehooked_", *Settings = "settings", *ShaderCache = "shader_cache", *Saves = "saves",
                            *Screenshots = "screenshots", *Dumps = "dumps", *Home = "guest_home";
}
std::filesystem::path ExecutableDirectory();
// Port directory of the game, with its settings and logs folders.
std::filesystem::path PreparePortDirectory(const std::filesystem::path& game);
const std::filesystem::path& PortDirectory();
// Persistent session phase and last AOT activity. True: the previous run died before the menu; acknowledge with EndLaunch().
bool BeginLaunchSession(const std::filesystem::path& port);
void MarkLaunchReady();
void EndLaunch(); // orderly exit or acknowledged startup error, safe from any thread
// AOT circuit breaker: 10-minute cooldown between builds, measured from its end (or last activity if interrupted).
bool AotBuildAllowed(); // false: skip every startup compilation category, keeping the existing cache
void BeginAotBuild();
void EndAotBuild();
void ResetAotCooldown();
std::vector<unsigned char> ReadFile(const std::filesystem::path& path);
std::string HostError(std::string_view action);
// Cold: paths that log are laid out away from the hot code that branches to them.
[[gnu::cold]] void Log(std::string_view message);
[[gnu::cold]] void FlushLog(const std::filesystem::path& path);
}
