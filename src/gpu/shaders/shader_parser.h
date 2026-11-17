// SPDX-License-Identifier: GPL-2.0-or-later
// Ingestion: finds the GCN programs in the game's EBOOT and WADs (OrbShdr records) and indexes them by the hashes the
// runtime computes of a program's code and of the fetch shader a vertex shader calls, so a recipe finds its code. The index
// is a file of its own, valid while the game's files are the ones it was made from.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Gpu::Parser {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Read-only mapping of one file.
struct Mapping {
    void *file = nullptr, *mapping = nullptr;
    const u8* data = nullptr;
    size_t size = 0;
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    ~Mapping();
    bool Open(const std::filesystem::path& path, bool sequential = false);
};

struct Program {
    u64 code_hash, fetch_hash;  // hashes of the code span [start, start + size) and the vertex shader's fetch shader (0: none)
    u64 start, size;            // the code span in file `file`
    u64 fetch_start;            // the fetch shader's span: fetch_size bytes
    u32 file, fetch_size;
    u8 type;                    // OrbShdr type: 0 pixel, 1 vertex, 4 compute
    u8 reserved[7]{};
};
struct Index {
    std::vector<std::string> files;  // relative to the game directory: eboot.bin first, then the WADs
    std::vector<Program> programs;   // one per (code, fetch shader, file)
    u64 stamp = 0;                   // what the game's files were when this was made (names, sizes, times)
};
// The index of the game in `directory`: read from `index_file` when it was made from these files, else built (a thread per
// file, `files` told how many first and `done` after each) and written there.
Index Load(const std::filesystem::path& game, const std::filesystem::path& index_file, const std::function<void(u32)>& files,
           const std::function<void()>& done);
} // namespace Gpu::Parser
