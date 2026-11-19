// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/dump_archive.h"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "main/host.h"

namespace Gow3::DumpArchive {
namespace {
constexpr int kZstdLevel = 3; // matches the project's zstd compression level (shader_cache.cpp)
constexpr size_t kTarBlock = 512;

struct Entry {
    std::string name;
    std::vector<u8> data;
    std::uint64_t mtime = 0;
};

std::mutex archive_mutex;
std::mutex flush_mutex;
std::vector<Entry> pending;
std::unordered_set<std::string> committed_names;
std::filesystem::path session_path;
bool session_written = false;

const std::filesystem::path& EnsureSessionPathLocked() {
    if (!session_path.empty()) return session_path;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    Gow3::LocalTime(tm, now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const std::filesystem::path dir = PortDirectory();
    std::error_code ec;
    std::filesystem::path candidate = dir / std::format("{}-{}{}", ArchivePrefix, stamp, ArchiveExtension);
    for (u32 suffix = 2; std::filesystem::exists(candidate, ec); ++suffix)
        candidate = dir / std::format("{}-{}-{}{}", ArchivePrefix, stamp, suffix, ArchiveExtension);
    session_path = std::move(candidate);
    return session_path;
}

std::string NormalizeName(std::string_view raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back(c == '\\' ? '/' : c);
    while (s.starts_with("./")) s.erase(0, 2);
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
}

size_t AlignBlock(size_t n) { return (n + kTarBlock - 1) & ~(kTarBlock - 1); }

void WriteOctal(char* dst, size_t width, std::uint64_t value) {
    // Standard ustar NUL-terminated octal field (width - 1 digits + trailing NUL).
    std::memset(dst, '0', width - 1);
    dst[width - 1] = '\0';
    for (size_t i = width - 1; i > 0; --i) {
        dst[i - 1] = char('0' + (value & 7));
        value >>= 3;
    }
}

bool SplitUstarPath(const std::string& path, std::string& prefix, std::string& name) {
    if (path.size() <= 100) {
        prefix.clear();
        name = path;
        return true;
    }
    if (path.size() > 256) return false;
    // Find a '/' split where prefix <= 155 and suffix <= 100.
    const size_t min_split = path.size() > 100 ? path.size() - 101 : 0;
    size_t pos = path.rfind('/', 155);
    while (pos != std::string::npos && pos >= min_split) {
        if (pos <= 155 && (path.size() - pos - 1) <= 100 && (path.size() - pos - 1) > 0) {
            prefix = path.substr(0, pos);
            name = path.substr(pos + 1);
            return true;
        }
        if (pos == 0) break;
        pos = path.rfind('/', pos - 1);
    }
    return false;
}

void AppendTarHeader(std::vector<u8>& tar, const std::string& name, const std::string& prefix, char typeflag,
                     std::uint64_t size, std::uint64_t mtime) {
    std::array<u8, kTarBlock> hdr{};
    std::memcpy(hdr.data() + 0, name.data(), std::min<size_t>(name.size(), 100));
    WriteOctal(reinterpret_cast<char*>(hdr.data() + 100), 8, 0644);
    WriteOctal(reinterpret_cast<char*>(hdr.data() + 108), 8, 0);
    WriteOctal(reinterpret_cast<char*>(hdr.data() + 116), 8, 0);
    WriteOctal(reinterpret_cast<char*>(hdr.data() + 124), 12, size);
    WriteOctal(reinterpret_cast<char*>(hdr.data() + 136), 12, mtime);
    std::memset(hdr.data() + 148, ' ', 8); // checksum placeholder during summation
    hdr[156] = static_cast<u8>(typeflag);
    std::memcpy(hdr.data() + 257, "ustar", 6);
    hdr[263] = '0';
    hdr[264] = '0';
    if (!prefix.empty()) std::memcpy(hdr.data() + 345, prefix.data(), std::min<size_t>(prefix.size(), 155));
    unsigned sum = 0;
    for (u8 b : hdr) sum += b;
    WriteOctal(reinterpret_cast<char*>(hdr.data() + 148), 7, sum);
    hdr[155] = ' ';
    tar.insert(tar.end(), hdr.begin(), hdr.end());
}

void AppendTarEntry(std::vector<u8>& tar, const Entry& e) {
    std::string prefix, name;
    if (!SplitUstarPath(e.name, prefix, name)) {
        // PAX extended header ('x') for paths that exceed ustar limits or have no slash split.
        std::string rec = std::format(" path={}\n", e.name);
        size_t total = rec.size() + 3;
        while (true) {
            std::string line = std::to_string(total) + rec;
            if (line.size() == total) {
                rec = std::move(line);
                break;
            }
            total = rec.size() + std::to_string(total).size();
        }
        AppendTarHeader(tar, "PaxHeader", {}, 'x', rec.size(), e.mtime);
        tar.insert(tar.end(), rec.begin(), rec.end());
        tar.resize(AlignBlock(tar.size()), 0);
        name = e.name.substr(0, 100);
        prefix.clear();
    }
    AppendTarHeader(tar, name, prefix, '0', e.data.size(), e.mtime);
    if (!e.data.empty()) {
        tar.insert(tar.end(), e.data.begin(), e.data.end());
        tar.resize(AlignBlock(tar.size()), 0);
    }
}

// Walks existing TAR headers from offset 0 so the end-of-archive zero blocks can be stripped before appending.
size_t FindTarEndOffset(const std::vector<u8>& tar) {
    size_t off = 0;
    while (off + kTarBlock <= tar.size()) {
        bool zero = true;
        for (size_t i = 0; i < kTarBlock; ++i) {
            if (tar[off + i] != 0) {
                zero = false;
                break;
            }
        }
        if (zero) return off;
        std::uint64_t size = 0;
        const char* p = reinterpret_cast<const char*>(tar.data() + off + 124);
        for (size_t i = 0; i < 12; ++i) {
            if (p[i] == '\0' || p[i] == ' ') continue;
            if (p[i] < '0' || p[i] > '7') return off;
            size = (size << 3) | static_cast<unsigned>(p[i] - '0');
        }
        const size_t next = off + kTarBlock + AlignBlock(static_cast<size_t>(size));
        if (next > tar.size() || next <= off) return off;
        off = next;
    }
    return off;
}

bool DecompressZstd(const std::vector<u8>& compressed, std::vector<u8>& out) {
    struct DStreamGuard {
        ZSTD_DStream* s = ZSTD_createDStream();
        ~DStreamGuard() { ZSTD_freeDStream(s); }
    } ds;
    if (!ds.s || ZSTD_isError(ZSTD_initDStream(ds.s))) return false;
    out.clear();
    std::vector<u8> scratch(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in{compressed.data(), compressed.size(), 0};
    while (in.pos < in.size) {
        ZSTD_outBuffer ob{scratch.data(), scratch.size(), 0};
        const size_t ret = ZSTD_decompressStream(ds.s, &ob, &in);
        if (ZSTD_isError(ret)) return false;
        if (ob.pos > 0) out.insert(out.end(), scratch.data(), scratch.data() + ob.pos);
    }
    return true;
}

bool CompressZstdToFile(const std::vector<u8>& tar, const std::filesystem::path& archive_path) {
    struct CCtxGuard {
        ZSTD_CCtx* c = ZSTD_createCCtx();
        ~CCtxGuard() { ZSTD_freeCCtx(c); }
    } ctx;
    if (!ctx.c) return false;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx.c, ZSTD_c_compressionLevel, kZstdLevel))) return false;
    if (ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(ctx.c, tar.size()))) return false;

    std::error_code ec;
    std::filesystem::create_directories(archive_path.parent_path(), ec);
#ifdef _WIN32
    const std::filesystem::path temp = archive_path.wstring() + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"wb");
#else
    auto temp = archive_path;
    temp += ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
#endif
    if (!f) return false;

    std::vector<u8> out_buf(ZSTD_CStreamOutSize());
    ZSTD_inBuffer in{tar.data(), tar.size(), 0};
    bool ok = true;
    while (true) {
        ZSTD_outBuffer ob{out_buf.data(), out_buf.size(), 0};
        const size_t remaining = ZSTD_compressStream2(ctx.c, &ob, &in, ZSTD_e_end);
        if (ZSTD_isError(remaining)) {
            ok = false;
            break;
        }
        if (ob.pos > 0 && std::fwrite(out_buf.data(), 1, ob.pos, f) != ob.pos) {
            ok = false;
            break;
        }
        if (remaining == 0 && in.pos == in.size) break;
    }
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    if (!Gow3::ReplaceFile(temp, archive_path)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<u8>& out) {
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
#ifdef _WIN32
    constexpr auto seek = _fseeki64;
    constexpr auto tell = _ftelli64;
#else
    constexpr auto seek = fseeko;
    constexpr auto tell = ftello;
#endif
    if (seek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long long size = tell(f);
    if (size < 0 || seek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t read = size > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return read == out.size();
}
} // namespace

std::filesystem::path Path() {
    std::scoped_lock lock(archive_mutex);
    return EnsureSessionPathLocked();
}

bool Contains(std::string_view name) {
    const std::string normalized = NormalizeName(name);
    std::scoped_lock lock(archive_mutex);
    if (committed_names.contains(normalized)) return true;
    for (const Entry& e : pending)
        if (e.name == normalized) return true;
    return false;
}

void Add(std::string_view name, const void* data, size_t size) {
    std::string normalized = NormalizeName(name);
    if (normalized.empty()) return;
    std::vector<u8> copy;
    if (data && size) {
        const auto* bytes = static_cast<const u8*>(data);
        copy.assign(bytes, bytes + size);
    }
    const std::uint64_t mtime = static_cast<std::uint64_t>(std::max<std::time_t>(0, std::time(nullptr)));
    std::scoped_lock lock(archive_mutex);
    pending.push_back({std::move(normalized), std::move(copy), mtime});
}

bool AdoptFile(std::string_view name, const std::filesystem::path& file) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::is_regular_file(file, ec)) return false;
    std::vector<u8> bytes;
    if (!ReadWholeFile(file, bytes)) return false;
    Add(name, bytes.data(), bytes.size());
    std::filesystem::remove(file, ec);
    return true;
}

bool Flush() {
    std::scoped_lock io_lock(flush_mutex);
    std::vector<Entry> batch;
    bool append_existing = false;
    std::filesystem::path archive_path;
    {
        std::scoped_lock lock(archive_mutex);
        if (pending.empty()) return true;
        batch.swap(pending);
        append_existing = session_written;
        archive_path = EnsureSessionPathLocked();
    }

    std::vector<u8> tar;
    if (append_existing) {
        std::vector<u8> existing_zst;
        if (ReadWholeFile(archive_path, existing_zst) && DecompressZstd(existing_zst, tar)) {
            tar.resize(FindTarEndOffset(tar));
        } else {
            tar.clear();
        }
    }

    size_t extra_bytes = kTarBlock * 2;
    for (const Entry& e : batch) extra_bytes += kTarBlock + AlignBlock(e.data.size());
    tar.reserve(tar.size() + extra_bytes);
    for (const Entry& e : batch) AppendTarEntry(tar, e);
    tar.resize(tar.size() + kTarBlock * 2, 0); // standard TAR end-of-archive (two 512-byte zero blocks)

    if (!CompressZstdToFile(tar, archive_path)) {
        // Restore pending entries on failure so a subsequent flush can retry.
        std::scoped_lock lock(archive_mutex);
        pending.insert(pending.begin(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        return false;
    }

    std::scoped_lock lock(archive_mutex);
    session_written = true;
    for (const Entry& e : batch) committed_names.insert(e.name);
    return true;
}
} // namespace Gow3::DumpArchive
