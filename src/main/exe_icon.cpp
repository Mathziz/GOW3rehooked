// SPDX-License-Identifier: GPL-2.0-or-later
#include "exe_icon.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include <stb_image.h> // implemented in imgui/ui_bridge.cpp
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

namespace Gow3 {
namespace {
constexpr int kSizes[]{256, 48, 32, 16};
constexpr wchar_t kStampName[] = L"GAMEICON";

// Identifies the icon file an executable was patched from (size and write time).
struct Stamp { uint64_t size; int64_t time; bool operator==(const Stamp&) const = default; };

#pragma pack(push, 2)
struct GroupHeader { WORD reserved, type, count; };
struct GroupEntry { BYTE width, height, colors, reserved; WORD planes, bits; DWORD bytes; WORD id; };
#pragma pack(pop)

// Box filter over premultiplied alpha, so transparent pixels do not bleed their colour into the edges.
std::vector<uint8_t> Downscale(const stbi_uc* src, int sw, int sh, int n) {
    std::vector<uint8_t> out(size_t(n) * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int x0 = x * sw / n, x1 = std::max(x0 + 1, (x + 1) * sw / n);
            const int y0 = y * sh / n, y1 = std::max(y0 + 1, (y + 1) * sh / n);
            uint64_t r{}, g{}, b{}, a{};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const stbi_uc* p = src + (size_t(sy) * sw + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3];
                }
            uint8_t* o = &out[(size_t(y) * n + x) * 4];
            if (a) { o[0] = uint8_t(r / a); o[1] = uint8_t(g / a); o[2] = uint8_t(b / a); }
            o[3] = uint8_t(a / (uint64_t(x1 - x0) * (y1 - y0)));
        }
    return out;
}

// RT_ICON payload: the 256 px entry is a PNG, the smaller ones a 32-bit DIB (bottom-up BGRA plus an empty AND mask).
std::vector<uint8_t> IconImage(const std::vector<uint8_t>& rgba, int n) {
    std::vector<uint8_t> data;
    if (n >= 256) {
        int length{};
        const std::unique_ptr<uint8_t, decltype(&free)> png(stbi_write_png_to_mem(rgba.data(), n * 4, n, n, 4, &length), free);
        if (png) data.assign(png.get(), png.get() + length);
        return data;
    }
    const size_t mask_row = ((size_t(n) + 31) / 32) * 4;
    data.resize(sizeof(BITMAPINFOHEADER) + size_t(n) * n * 4 + mask_row * n);
    auto& header = *reinterpret_cast<BITMAPINFOHEADER*>(data.data());
    header = {sizeof(BITMAPINFOHEADER), n, 2 * n, 1, 32, BI_RGB};
    uint8_t* row = data.data() + sizeof(header);
    for (int y = n - 1; y >= 0; --y)
        for (int x = 0; x < n; ++x, row += 4) {
            const uint8_t* p = &rgba[(size_t(y) * n + x) * 4];
            row[0] = p[2]; row[1] = p[1]; row[2] = p[0]; row[3] = p[3];
        }
    return data;
}

std::wstring ExecutablePath() {
    std::wstring path(1024, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
    return path;
}

bool Matches(const Stamp& stamp) {
    const HRSRC resource = FindResourceW(nullptr, kStampName, RT_RCDATA);
    if (!resource || SizeofResource(nullptr, resource) != sizeof(Stamp)) return false;
    const void* data = LockResource(LoadResource(nullptr, resource));
    return data && *static_cast<const Stamp*>(data) == stamp;
}

bool WriteResources(const std::wstring& file, const std::vector<std::vector<uint8_t>>& images, const std::vector<int>& sizes,
                    const Stamp& stamp) {
    const HANDLE update = BeginUpdateResourceW(file.c_str(), FALSE);
    if (!update) return false;
    std::vector<uint8_t> group(sizeof(GroupHeader) + images.size() * sizeof(GroupEntry));
    *reinterpret_cast<GroupHeader*>(group.data()) = {0, 1, WORD(images.size())};
    bool ok = true;
    for (size_t i = 0; i < images.size(); ++i) {
        const BYTE side = sizes[i] >= 256 ? 0 : BYTE(sizes[i]);
        reinterpret_cast<GroupEntry*>(group.data() + sizeof(GroupHeader))[i] = {side, side, 0, 0, 1, 32, DWORD(images[i].size()), WORD(i + 1)};
        ok &= UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(i + 1), 0, const_cast<uint8_t*>(images[i].data()), DWORD(images[i].size())) != FALSE;
    }
    ok &= UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), 0, group.data(), DWORD(group.size())) != FALSE;
    ok &= UpdateResourceW(update, RT_RCDATA, kStampName, 0, const_cast<Stamp*>(&stamp), sizeof(stamp)) != FALSE;
    return EndUpdateResourceW(update, !ok) && ok;
}
} // namespace

void EmbedGameIcon(const std::filesystem::path& icon_png) {
    try {
        const std::wstring exe = ExecutablePath(), copy = exe + L".icon", old = exe + L".old";
        DeleteFileW(old.c_str()); // left by the previous patch, which could not delete the image it was running from
        std::error_code error;
        const auto size = std::filesystem::file_size(icon_png, error);
        const auto time = std::filesystem::last_write_time(icon_png, error).time_since_epoch().count();
        if (error) return;
        const Stamp stamp{size, time};
        if (Matches(stamp)) return;

        std::ifstream file(icon_png, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        int width{}, height{}, channels{};
        const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
            stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &width, &height, &channels, 4), stbi_image_free);
        if (!pixels) return;
        std::vector<std::vector<uint8_t>> images;
        std::vector<int> sizes;
        for (const int n : kSizes) {
            if (n > std::min(width, height)) continue;
            images.push_back(IconImage(Downscale(pixels.get(), width, height, n), n));
            sizes.push_back(n);
            if (images.back().empty()) return;
        }
        if (images.empty()) return;

        // A running executable can be renamed but not written: patch a copy, move the original aside, put the copy in place.
        if (!CopyFileW(exe.c_str(), copy.c_str(), FALSE)) return;
        if (!WriteResources(copy, images, sizes, stamp) || !MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(copy.c_str());
            return;
        }
        if (!MoveFileExW(copy.c_str(), exe.c_str(), 0)) {
            MoveFileExW(old.c_str(), exe.c_str(), 0);
            DeleteFileW(copy.c_str());
        }
    } catch (...) {}
}
} // namespace Gow3
