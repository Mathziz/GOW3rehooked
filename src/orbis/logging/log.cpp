// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "orbis/logging/log.h"
#include "main/host.h"
#include <cstdlib>
#include <fmt/format.h>

namespace Common::Log {
// Normal play: errors only (the crash log ring). Debug runs: Info and up, so the trace and debug lines of the calls the
// game makes every frame (audio, AJM jobs, pad, flips, events) are dropped before any formatting; GOW3_LOG_LEVEL=trace,
// debug, info, warning, error, critical or off sets the debug level (debug.ps1 -LogLevel).
std::array<Level, NUM_LOG_CLASSES> g_class_levels = [] {
    Level level = Gow3::DebugEnabled() ? Level::Info : Level::Error;
    if (const char* e = Gow3::DebugOption("GOW3_LOG_LEVEL"))
        for (const std::string_view names[]{"trace", "debug", "info", "warning", "error", "critical", "off"}; const auto& n : names)
            if (n == e) level = Level(&n - names);
    std::array<Level, NUM_LOG_CLASSES> levels;
    levels.fill(level);
    return levels;
}();
void VLog(Class log_class, Level level, const char*, int, const char*,
          fmt::string_view format, fmt::format_args args) {
    Gow3::Log(fmt::format("Orbis {} [{}]: {}", NameOf(log_class),
                         static_cast<int>(level), fmt::vformat(format, args)));
}
}
