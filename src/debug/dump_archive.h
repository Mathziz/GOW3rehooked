// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include "orbis/common/types.h"

// Single-file dump archive (<port>/gow3-<YYYYMMDD-HHMMSS>.dmp): POSIX ustar TAR compressed with Zstandard (level 3, no dictionary).
// All diagnostic dumps (F9 frame dumps, GOW3_TEMP_TRACE_EVERY traces, exit frame/cpu/render/video probes, and
// streaming traces like shadow-*.bin, draws-*.bin, s4-gpu-written.txt) stage or adopt their outputs here so
// bug reporters only have to attach gow3.log and the session's .dmp file.
namespace Gow3::DumpArchive {
inline constexpr const char* ArchivePrefix = "gow3";
inline constexpr const char* ArchiveExtension = ".dmp";

std::filesystem::path Path();
bool Contains(std::string_view name);
void Add(std::string_view name, const void* data, size_t size);
inline void Add(std::string_view name, std::string_view text) { Add(name, text.data(), text.size()); }
inline void Add(std::string_view name, std::span<const u8> bytes) { Add(name, bytes.data(), bytes.size()); }
// Reads an existing loose dump file into the archive and deletes the original if read succeeds.
bool AdoptFile(std::string_view name, const std::filesystem::path& file);
// Compresses all pending entries into <port>/gow3-<YYYYMMDD-HHMMSS>.dmp (appending to earlier flushes from the same session).
bool Flush();
} // namespace Gow3::DumpArchive
