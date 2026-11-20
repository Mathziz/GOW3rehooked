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
constexpr size_t kPartBytes = 24'000'000;  // archive bytes per .zip: GitHub takes attachments up to 25 MB

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

// Part `n` (from 1) of an archive too big for one .zip: gow3-<stamp>.part<n>.zip.
std::filesystem::path PartPath(std::filesystem::path archive, u32 n) { return archive.replace_extension(std::format(".part{}.zip", n)); }

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
    for (u32 suffix = 2; std::filesystem::exists(candidate, ec) || std::filesystem::exists(PartPath(candidate, 1), ec); ++suffix)
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

bool CompressZstd(const std::vector<u8>& tar, std::vector<u8>& out) {
    struct CCtxGuard {
        ZSTD_CCtx* c = ZSTD_createCCtx();
        ~CCtxGuard() { ZSTD_freeCCtx(c); }
    } ctx;
    if (!ctx.c) return false;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx.c, ZSTD_c_compressionLevel, kZstdLevel))) return false;
    if (ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(ctx.c, tar.size()))) return false;
    out.clear();
    std::vector<u8> out_buf(ZSTD_CStreamOutSize());
    ZSTD_inBuffer in{tar.data(), tar.size(), 0};
    while (true) {
        ZSTD_outBuffer ob{out_buf.data(), out_buf.size(), 0};
        const size_t remaining = ZSTD_compressStream2(ctx.c, &ob, &in, ZSTD_e_end);
        if (ZSTD_isError(remaining)) return false;
        out.insert(out.end(), out_buf.data(), out_buf.data() + ob.pos);
        if (remaining == 0 && in.pos == in.size) return true;
    }
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    const std::filesystem::path temp = path.wstring() + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"wb");
#else
    auto temp = path;
    temp += ".tmp";
    FILE* f = std::fopen(temp.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0) ok = false;
    if (!ok || !Gow3::ReplaceFile(temp, path)) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

u32 Crc32(std::span<const u8> bytes) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    u32 c = ~0u;
    for (u8 b : bytes) c = table[(c ^ b) & 0xFF] ^ c >> 8;
    return ~c;
}

// A .zip holding `data` as its one entry `name`, stored (the bytes are compressed already).
std::vector<u8> StoredZip(const std::string& name, std::span<const u8> data) {
    std::tm tm{};
    Gow3::LocalTime(tm, std::time(nullptr));
    const u32 time = u32(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2), date = u32((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    const u32 crc = Crc32(data), size = u32(data.size()), n = u32(name.size());
    std::vector<u8> z;
    z.reserve(30 + 46 + 22 + 2 * n + size);
    const auto put = [&](u32 v, int bytes) {
        for (int i = 0; i < bytes; ++i) z.push_back(u8(v >> 8 * i));
    };
    const auto header = [&](u32 signature, bool central) {  // local file header, or central directory record of the entry at 0
        put(signature, 4);
        if (central) put(10, 2);  // made by: version 1.0
        put(10, 2), put(0, 2), put(0, 2), put(time, 2), put(date, 2), put(crc, 4), put(size, 4), put(size, 4), put(n, 2), put(0, 2);
        if (central) put(0, 2), put(0, 2), put(0, 2), put(0, 4), put(0, 4);
        z.insert(z.end(), name.begin(), name.end());
    };
    header(0x04034b50, false);
    z.insert(z.end(), data.begin(), data.end());
    const u32 central = u32(z.size());
    header(0x02014b50, true);
    put(0x06054b50, 4), put(0, 4), put(1, 2), put(1, 2), put(u32(z.size()) - central, 4), put(central, 4), put(0, 2);
    return z;
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

// The compressed archive: gow3-<stamp>.dmp stored in gow3-<stamp>.zip or, past kPartBytes, its pieces gow3-<stamp>.dmp.001,
// .002... stored one per part (joined back in order, they make the .dmp).
bool WriteArchive(const std::vector<u8>& zst, const std::filesystem::path& archive) {
    const size_t parts = std::max<size_t>(1, (zst.size() + kPartBytes - 1) / kPartBytes);
    const std::string dmp = std::filesystem::path(archive).replace_extension(".dmp").filename().string();
    for (size_t i = 0; i < parts; ++i) {
        const std::span<const u8> piece(zst.data() + i * kPartBytes, std::min(kPartBytes, zst.size() - i * kPartBytes));
        if (parts == 1) return WriteFileAtomic(archive, StoredZip(dmp, piece));
        if (!WriteFileAtomic(PartPath(archive, u32(i + 1)), StoredZip(std::format("{}.{:03}", dmp, i + 1), piece))) return false;
    }
    std::error_code ec;
    std::filesystem::remove(archive, ec);  // the single .zip of an earlier flush
    return true;
}

bool ReadArchive(const std::filesystem::path& archive, std::vector<u8>& zst) {
    zst.clear();
    std::vector<u8> z;
    const auto take = [&] {  // the stored entry of a .zip written by StoredZip
        const auto at = [&](size_t o, int bytes) {
            u32 v = 0;
            for (int i = 0; i < bytes; ++i) v |= u32(z[o + i]) << 8 * i;
            return v;
        };
        if (z.size() < 30 || at(0, 4) != 0x04034b50) return false;
        const size_t begin = 30 + at(26, 2) + at(28, 2), size = at(18, 4);
        if (begin + size > z.size()) return false;
        zst.insert(zst.end(), z.begin() + begin, z.begin() + begin + size);
        return true;
    };
    if (ReadWholeFile(archive, z)) return take();
    for (u32 n = 1; ReadWholeFile(PartPath(archive, n), z); ++n)
        if (!take()) return false;
    return !zst.empty();
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
        if (ReadArchive(archive_path, existing_zst) && DecompressZstd(existing_zst, tar)) {
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

    std::vector<u8> zst;
    if (!CompressZstd(tar, zst) || !WriteArchive(zst, archive_path)) {
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
