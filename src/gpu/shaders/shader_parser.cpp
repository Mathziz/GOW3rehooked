// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/piece_hash.h"
#include "gpu/shaders/shader_parser.h"
#include "main/cpu.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <xxhash.h>
#include <windows.h>

namespace Gpu::Parser {
namespace {
constexpr u32 kMagic = 0x49573347, kFormat = 1;  // "G3WI"; kFormat: the layout of the index file
static_assert(sizeof(Program) == 56);
template<class T> T Read(const u8* p) { T value; std::memcpy(&value, p, sizeof(value)); return value; }

// The programs of one file. A record is the OrbShdr footer; the code starts before it, behind the input-usage tables and
// padding (its length field is not the distance), at the 0xBEEB03FF header whose size field lands on the footer.
void Scan(const Mapping& file, u32 index, std::vector<Program>& out) {
    std::unordered_set<u64> seen;
    size_t cursor = 0;
    while (cursor + 28 <= file.size) {
        const auto* found = static_cast<const u8*>(std::memchr(file.data + cursor, 'O', file.size - cursor - 27));
        if (!found) break;
        const size_t footer = size_t(found - file.data);
        cursor = footer + 1;
        if (std::memcmp(found, "OrbShdr\x07", 8)) continue;
        const u32 flags = Read<u32>(found + 8), length = flags >> 8, type = (flags >> 2) & 15;
        if (!length || length % 4 || length > footer || (type != 0 && type != 1 && type != 4)) continue;
        const size_t latest = footer - length, earliest = latest > 4096 ? latest - 4096 : 0;
        size_t start = latest;
        for (;;) {
            if (start + 8 <= footer && Read<u32>(file.data + start) == 0xbeeb03ff && u64(start) + (u64(Read<u32>(file.data + start + 4)) + 1) * 8 == footer) break;
            if (start == earliest) { start = file.size; break; }
            --start;
        }
        if (start == file.size) continue;
        Program p{};
        p.code_hash = ::Gpu::PieceHash::ShaderCode(file.data + start, footer - start), p.start = start, p.size = footer - start, p.file = index, p.type = u8(type);
        if (type == 1) {  // the fetch shader follows the footer, up to its s_setpc_b64 (as FetchProgram finds it in memory)
            const size_t at = footer + 28;
            for (size_t i = 0; i < 0x100 && at + (i + 1) * 4 <= file.size; ++i)
                if ((Read<u32>(file.data + at + i * 4) & 0xFF80FF00) == 0xBE802000) {
                    p.fetch_start = at, p.fetch_size = u32((i + 1) * 4), p.fetch_hash = ::Gpu::PieceHash::ShaderCode(file.data + at, p.fetch_size);
                    break;
                }
        }
        if (seen.insert(p.code_hash * 0x9E3779B97F4A7C15ull ^ p.fetch_hash).second) out.push_back(p);
    }
}

std::vector<std::filesystem::path> Files(const std::filesystem::path& game) {
    std::vector<std::filesystem::path> files{"eboot.bin"};
    const auto directory = game / "EXEC" / "WAD";
    if (!std::filesystem::is_directory(directory)) throw std::runtime_error("AOT: EXEC/WAD directory missing");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        if (entry.is_regular_file() && (entry.path().extension() == ".WAD" || entry.path().extension() == ".wad")) files.push_back(std::filesystem::relative(entry.path(), game));
    std::sort(files.begin() + 1, files.end());
    return files;
}

// What the index was made from: the files' names, sizes and times.
u64 Stamp(const std::filesystem::path& game, const std::vector<std::filesystem::path>& files) {
    std::vector<u8> bytes;
    bytes.reserve(files.size() * 64);
    for (const auto& f : files) {
        std::error_code ec;
        const std::string name = f.generic_string();
        const u64 meta[2]{std::filesystem::file_size(game / f, ec), u64(std::filesystem::last_write_time(game / f, ec).time_since_epoch().count())};
        bytes.insert(bytes.end(), name.begin(), name.end());
        const auto* data = reinterpret_cast<const u8*>(meta);
        bytes.insert(bytes.end(), data, data + sizeof(meta));
    }
    return ::Gpu::PieceHash::Stable64(bytes.data(), bytes.size());
}
} // namespace

Mapping::~Mapping() {
    if (data) UnmapViewOfFile(data);
    if (mapping) CloseHandle(mapping);
    if (file && file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
bool Mapping::Open(const std::filesystem::path& path, bool sequential) {
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, sequential ? FILE_FLAG_SEQUENTIAL_SCAN : 0, nullptr);
    LARGE_INTEGER length{};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length) || length.QuadPart <= 0) return false;
    size = size_t(length.QuadPart);
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping) data = static_cast<const u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    return data != nullptr;
}

Index Load(const std::filesystem::path& game, const std::filesystem::path& index_file, const std::function<void(u32)>& files_total,
           const std::function<void()>& done) {
    const auto files = Files(game);
    const u64 stamp = Stamp(game, files);
    if (std::ifstream in(index_file, std::ios::binary); in) {
        u32 head[4]{};
        u64 saved = 0;
        in.read((char*)head, sizeof(head)), in.read((char*)&saved, 8);
        if (in && head[0] == kMagic && head[1] == kFormat && saved == stamp && head[2] == files.size()) {
            Index index;
            index.stamp = stamp;
            for (u32 k = 0; k < head[2]; ++k) index.files.push_back(files[k].generic_string());
            index.programs.resize(head[3]);
            if (in.read((char*)index.programs.data(), std::streamsize(index.programs.size() * sizeof(Program)))) return index;
        }
    }
    Index index;
    index.stamp = stamp;
    for (const auto& f : files) index.files.push_back(f.generic_string());
    std::vector<std::vector<Program>> found(files.size());
    std::atomic<size_t> next = 0;
    std::atomic<bool> complete = true;  // every file was read
    if (files_total) files_total(u32(files.size()));
    {
        std::vector<std::jthread> workers;
        for (unsigned t = 0; t < Gow3::Cpu::plan.shader_threads; ++t)
            workers.emplace_back([&] {
                for (size_t k; (k = next++) < files.size();) {
                    Mapping m;
                    try {
                        if (m.Open(game / files[k], true)) Scan(m, u32(k), found[k]);
                        else complete = false;
                    } catch (const std::exception&) {  // a file that cannot be scanned contributes what it had
                        complete = false;
                    }
                    if (done) done();
                }
            });
    }
    for (auto& f : found) index.programs.insert(index.programs.end(), f.begin(), f.end());
    if (index.programs.empty()) throw std::runtime_error("No shaders found in WADs/EBOOT");
    if (!complete) return index;  // used for this run, but only a complete reading is kept for the next
    std::ofstream out(index_file, std::ios::binary | std::ios::trunc);
    const u32 head[4]{kMagic, kFormat, u32(files.size()), u32(index.programs.size())};
    out.write((const char*)head, sizeof(head)), out.write((const char*)&stamp, 8);
    out.write((const char*)index.programs.data(), std::streamsize(index.programs.size() * sizeof(Program)));
    return index;
}
} // namespace Gpu::Parser
