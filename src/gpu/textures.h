// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Textures, render targets and tiling. Merges the files copied from shadPS4 c1d7c3aa (pixel_format.h, tile.h, tiling.h) with
// a stand-in for its common/types.h and common/assert.h, so they build without its logging stack; the Gpu helpers that map
// guest formats to Vulkan ones follow.
#pragma once
#include <vulkan/vulkan.h>
#include <fmt/format.h>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <tuple>
#include <utility>
#include "gpu/shaders/gcn_translator.h"

using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;
using VAddr = std::uintptr_t;

#define ASSERT(_a_) ((_a_) ? void() : std::abort())
#define ASSERT_MSG(_a_, ...) ASSERT(_a_)
#define DEBUG_ASSERT(_a_) ((void)0)
#define UNREACHABLE() std::abort()
#define UNREACHABLE_MSG(...) std::abort()

namespace AmdGpu {

// Table 8.13 Data and Image Formats [Sea Islands Series Instruction Set Architecture]
enum class DataFormat : u32 {
    FormatInvalid = 0,
    Format8 = 1,
    Format16 = 2,
    Format8_8 = 3,
    Format32 = 4,
    Format16_16 = 5,
    Format10_11_11 = 6,
    Format11_11_10 = 7,
    Format10_10_10_2 = 8,
    Format2_10_10_10 = 9,
    Format8_8_8_8 = 10,
    Format32_32 = 11,
    Format16_16_16_16 = 12,
    Format32_32_32 = 13,
    Format32_32_32_32 = 14,
    Format5_6_5 = 16,
    Format1_5_5_5 = 17,
    Format5_5_5_1 = 18,
    Format4_4_4_4 = 19,
    Format8_24 = 20,
    Format24_8 = 21,
    FormatX24_8_32 = 22,
    FormatGB_GR = 32,
    FormatBG_RG = 33,
    Format5_9_9_9 = 34,
    FormatBc1 = 35,
    FormatBc2 = 36,
    FormatBc3 = 37,
    FormatBc4 = 38,
    FormatBc5 = 39,
    FormatBc6 = 40,
    FormatBc7 = 41,
    FormatFmask8_S2_F1 = 44,
    FormatFmask8_S4_F1 = 45,
    FormatFmask8_S8_F1 = 46,
    FormatFmask8_S2_F2 = 47,
    FormatFmask8_S4_F2 = 48,
    FormatFmask8_S4_F4 = 49,
    FormatFmask16_S16_F1 = 50,
    FormatFmask16_S8_F2 = 51,
    FormatFmask32_S16_F2 = 52,
    FormatFmask32_S8_F4 = 53,
    FormatFmask32_S8_F8 = 54,
    FormatFmask64_S16_F4 = 55,
    FormatFmask64_S16_F8 = 56,
    Format4_4 = 57,
    Format6_5_5 = 58,
    Format1 = 59,
    Format1_Reversed = 60,
    Format32_As_8 = 61,
    Format32_As_8_8 = 62,
    Format32_As_32_32_32_32 = 63,
};

enum class NumberFormat : u32 {
    Unorm = 0,
    Snorm = 1,
    Uscaled = 2,
    Sscaled = 3,
    Uint = 4,
    Sint = 5,
    SnormNz = 6,
    Float = 7,
    Srgb = 9,
    Ubnorm = 10,
    UbnormNz = 11,
    Ubint = 12,
    Ubscaled = 13,
};

enum class NumberClass : u8 {
    Float = 0,
    Sint = 1,
    Uint = 2,
};

enum class CompSwizzle : u8 {
    Zero = 0,
    One = 1,
    Red = 4,
    Green = 5,
    Blue = 6,
    Alpha = 7,
};

enum class NumberConversion : u32 {
    None = 0,
    UintToUscaled = 1,
    SintToSscaled = 2,
    UnormToUbnorm = 3,
    Sint8ToSnormNz = 4,
    Sint16ToSnormNz = 5,
    Uint32ToUnorm = 6,
    SrgbToNorm = 7,
    Sint32ToSnormNz = 8,
};

union CompMapping {
    struct {
        CompSwizzle r;
        CompSwizzle g;
        CompSwizzle b;
        CompSwizzle a;
    };
    std::array<CompSwizzle, 4> array;

    bool operator==(const CompMapping& other) const {
        return std::bit_cast<u32>(array) == std::bit_cast<u32>(other.array);
    }

    template <typename T>
    [[nodiscard]] std::array<T, 4> Apply(const std::array<T, 4>& data) const {
        return {
            ApplySingle(data, r),
            ApplySingle(data, g),
            ApplySingle(data, b),
            ApplySingle(data, a),
        };
    }

    [[nodiscard]] u32 ApplyMask(u32 mask) const {
        u32 swizzled_mask{};
        for (u32 i = 0; i < 4; ++i) {
            swizzled_mask |= ((mask >> i) & 1) << Map(i);
        }
        return swizzled_mask;
    }

    [[nodiscard]] CompMapping Inverse() const {
        CompMapping result{};
        InverseSingle(result.r, CompSwizzle::Red);
        InverseSingle(result.g, CompSwizzle::Green);
        InverseSingle(result.b, CompSwizzle::Blue);
        InverseSingle(result.a, CompSwizzle::Alpha);
        return result;
    }

    [[nodiscard]] u32 Map(u32 comp) const {
        const u32 swizzled_comp = u32(array[comp]);
        constexpr u32 min_comp = u32(AmdGpu::CompSwizzle::Red);
        return swizzled_comp >= min_comp ? swizzled_comp - min_comp : comp;
    }

private:
    template <typename T>
    T ApplySingle(const std::array<T, 4>& data, const CompSwizzle swizzle) const {
        switch (swizzle) {
        case CompSwizzle::Zero:
            return T(0);
        case CompSwizzle::One:
            return T(1);
        case CompSwizzle::Red:
            return data[0];
        case CompSwizzle::Green:
            return data[1];
        case CompSwizzle::Blue:
            return data[2];
        case CompSwizzle::Alpha:
            return data[3];
        default:
            UNREACHABLE();
        }
    }

    void InverseSingle(CompSwizzle& dst, const CompSwizzle target) const {
        if (r == target) {
            dst = CompSwizzle::Red;
        } else if (g == target) {
            dst = CompSwizzle::Green;
        } else if (b == target) {
            dst = CompSwizzle::Blue;
        } else if (a == target) {
            dst = CompSwizzle::Alpha;
        } else {
            dst = CompSwizzle::Zero;
        }
    }
};

static constexpr CompMapping IdentityMapping = {
    .r = CompSwizzle::Red,
    .g = CompSwizzle::Green,
    .b = CompSwizzle::Blue,
    .a = CompSwizzle::Alpha,
};

constexpr DataFormat RemapDataFormat(const DataFormat format) {
    switch (format) {
    case DataFormat::Format11_11_10:
        return DataFormat::Format10_11_11;
    case DataFormat::Format10_10_10_2:
        return DataFormat::Format2_10_10_10;
    case DataFormat::Format5_5_5_1:
        return DataFormat::Format1_5_5_5;
    default:
        return format;
    }
}

constexpr NumberFormat RemapNumberFormat(const NumberFormat format, const DataFormat data_format) {
    switch (format) {
    case NumberFormat::Unorm: {
        switch (data_format) {
        case DataFormat::Format32:
        case DataFormat::Format32_32:
        case DataFormat::Format32_32_32:
        case DataFormat::Format32_32_32_32:
            return NumberFormat::Uint;
        default:
            return format;
        }
    }
    case NumberFormat::Srgb:
        switch (data_format) {
        case DataFormat::FormatBc4:
        case DataFormat::FormatBc5:
        case DataFormat::FormatBc6:
            // BC4/BC5 store non-color data (single/two-channel, used for normal maps),
            // and BC6 is HDR float — none have sRGB Vulkan equivalents.
            return NumberFormat::Unorm;
        default:
            return format;
        }
    case NumberFormat::Uscaled:
        return NumberFormat::Uint;
    case NumberFormat::Sscaled:
    case NumberFormat::SnormNz:
        return NumberFormat::Sint;
    case NumberFormat::Ubnorm:
        return NumberFormat::Unorm;
    case NumberFormat::Float:
        if (data_format == DataFormat::Format8) {
            // Games may ask for 8-bit float when they want to access the stencil component
            // of a depth-stencil image. Change to unsigned int to match the stencil format.
            // This is also the closest approximation to pass the bits through unconverted.
            return NumberFormat::Uint;
        }
        [[fallthrough]];
    default:
        return format;
    }
}

constexpr CompMapping RemapSwizzle(const DataFormat format, const CompMapping swizzle) {
    switch (format) {
    case DataFormat::Format1_5_5_5:
    case DataFormat::Format11_11_10: {
        CompMapping result;
        result.r = swizzle.b;
        result.g = swizzle.g;
        result.b = swizzle.r;
        result.a = swizzle.a;
        return result;
    }
    case DataFormat::Format10_10_10_2: {
        CompMapping result;
        result.r = swizzle.a;
        result.g = swizzle.b;
        result.b = swizzle.g;
        result.a = swizzle.r;
        return result;
    }
    case DataFormat::Format4_4_4_4: {
        // Remap to a more supported component order.
        CompMapping result;
        result.r = swizzle.g;
        result.g = swizzle.b;
        result.b = swizzle.a;
        result.a = swizzle.r;
        return result;
    }
    case DataFormat::Format5_6_5: {
        // Remap to a more supported component order.
        CompMapping result;
        result.r = swizzle.b;
        result.g = swizzle.g;
        result.b = swizzle.r;
        result.a = swizzle.a;
        return result;
    }
    case DataFormat::Format8:
    case DataFormat::Format16:
    case DataFormat::Format32: {
        // PS4 single-channel formats expose the texel as either R or A depending on dst_sel,
        // but Vulkan single-channel formats expose it only via R. Redirect any selector
        // that points at Alpha to Red so the texel is read correctly.
        CompMapping result = swizzle;
        const auto remap_alpha_to_red = [](CompSwizzle& c) {
            if (c == CompSwizzle::Alpha) {
                c = CompSwizzle::Red;
            }
        };
        remap_alpha_to_red(result.r);
        remap_alpha_to_red(result.g);
        remap_alpha_to_red(result.b);
        remap_alpha_to_red(result.a);
        return result;
    }
    default:
        return swizzle;
    }
}

constexpr NumberConversion MapNumberConversion(const NumberFormat num_fmt,
                                               const DataFormat data_fmt) {
    switch (num_fmt) {
    case NumberFormat::Unorm: {
        switch (data_fmt) {
        case DataFormat::Format32:
        case DataFormat::Format32_32:
        case DataFormat::Format32_32_32:
        case DataFormat::Format32_32_32_32:
            return NumberConversion::Uint32ToUnorm;
        default:
            return NumberConversion::None;
        }
    }
    case NumberFormat::Srgb:
        switch (data_fmt) {
        case DataFormat::FormatBc4:
        case DataFormat::FormatBc5:
            // BC4/BC5 have no sRGB variant; no conversion needed (treated as Unorm).
            return NumberConversion::None;
        case DataFormat::FormatBc6:
            return NumberConversion::SrgbToNorm;
        default:
            return NumberConversion::None;
        }
    case NumberFormat::Uscaled:
        return NumberConversion::UintToUscaled;
    case NumberFormat::Sscaled:
        return NumberConversion::SintToSscaled;
    case NumberFormat::Ubnorm:
        return NumberConversion::UnormToUbnorm;
    case NumberFormat::SnormNz: {
        switch (data_fmt) {
        case DataFormat::Format8:
        case DataFormat::Format8_8:
        case DataFormat::Format8_8_8_8:
            return NumberConversion::Sint8ToSnormNz;
        case DataFormat::Format16:
        case DataFormat::Format16_16:
        case DataFormat::Format16_16_16_16:
            return NumberConversion::Sint16ToSnormNz;
        case DataFormat::Format32:
        case DataFormat::Format32_32:
        case DataFormat::Format32_32_32:
        case DataFormat::Format32_32_32_32:
            return NumberConversion::Sint32ToSnormNz;
        default:
            UNREACHABLE_MSG("data_fmt = {}", u32(data_fmt));
        }
    }
    default:
        return NumberConversion::None;
    }
}

constexpr NumberClass GetNumberClass(const NumberFormat nfmt) {
    switch (nfmt) {
    case NumberFormat::Sint:
        return NumberClass::Sint;
    case NumberFormat::Uint:
        return NumberClass::Uint;
    default:
        return NumberClass::Float;
    }
}

constexpr bool IsRgb(CompSwizzle swizzle) {
    return swizzle == CompSwizzle::Red || swizzle == CompSwizzle::Green ||
           swizzle == CompSwizzle::Blue;
}

constexpr bool IsInteger(const NumberFormat nfmt) {
    return nfmt == NumberFormat::Sint || nfmt == NumberFormat::Uint;
}

constexpr bool IsBlockCoded(DataFormat format) {
    return format >= DataFormat::FormatBc1 && format <= DataFormat::FormatBc7;
}

constexpr bool IsFmask(DataFormat format) {
    return format >= DataFormat::FormatFmask8_S2_F1 && format <= DataFormat::FormatFmask64_S16_F8;
}

std::string_view NameOf(DataFormat fmt);
std::string_view NameOf(NumberFormat fmt);

u32 NumComponents(DataFormat format);
// Color buffer export swizzle for CB_COLOR*_INFO COMP_SWAP (shadPS4 ColorBuffer::Swizzle): output channel i takes the
// shader component Map(i), so the image holds the guest memory order.
CompMapping MrtSwizzle(u32 comp_swap, DataFormat format);
u32 NumBitsPerBlock(DataFormat format);
u32 NumBitsPerElement(DataFormat format);

} // namespace AmdGpu

namespace AmdGpu {
enum class TileMode : u32;
}

namespace VideoCore {

// clang-format off
// The table of macro tiles parameters for given tiling index (row) and bpp (column)
/* Calculation:
 * - Inputs:
 *     TileMode, BytesPerPixel, NumFragments
 * - Constants:
 *     MicroTileWidth = 8, MicroTileHeight = 8,
 *     Tile Mode LUTs: IsDepth(), IsPrt(), TileThickness(), TileSplit(), SampleSplit(), NumPipes()
 *     Macro Tile Mode LUTs: BankWidth(), BankHeight(), NumBanks(), MacroTileAspect()
 * - Determine the macro tile mode:
 *     TileBytes = MicroTileWidth * MicroTileHeight * TileThickness(TileMode) * BytesPerPixel
 *     TileSplit = min(IsDepth(TileMode) ? TileSplit(TileMode) : max(TileBytes * SampleSplit(TileMode), 256), NumFragments * TileBytes, 1024)
 *     MacroTileModeIndex = log2(TileSplit / 64)
 *     MacroTileMode = IsPrt(TileMode) ? MacroTileModeIndex + 8 : MacroTileModeIndex
 * - Calculate macro tile width and height:
 *     Width = NumPipes(TileMode) * BankWidth(MacroTileMode) * MicroTileWidth * MacroTileAspect(MacroTileMode, AltTileMode)
 *     Height = NumBanks(MacroTileMode, AltTileMode) * BankHeight(MacroTileMode, AltTileMode) * MicroTileHeight / MacroTileAspect(MacroTileMode, AltTileMode)
 */

constexpr std::array macro_tile_extents_x1{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 0A
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 0B
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 0E
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 0F
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 10
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 11
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_x2{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0A
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0B
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0E
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0F
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 10
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 11
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_x4{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0E
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_x8{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0E
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_alt_x1{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 0A
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 0B
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 0E
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 0F
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 10
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 11
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents_alt_x2{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0A
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0B
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0E
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0F
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 10
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 11
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents_alt_x4{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0E
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents_alt_x8{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0E
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents{
    macro_tile_extents_x1,
    macro_tile_extents_x2,
    macro_tile_extents_x4,
    macro_tile_extents_x8,
};

constexpr std::array macro_tile_extents_alt{
    macro_tile_extents_alt_x1,
    macro_tile_extents_alt_x2,
    macro_tile_extents_alt_x4,
    macro_tile_extents_alt_x8,
};
// clang-format on

constexpr std::pair micro_tile_extent{8u, 8u};
constexpr auto hw_pipe_interleave = 256u;

constexpr std::pair<u32, u32> GetMacroTileExtents(AmdGpu::TileMode tile_mode, u32 bpp,
                                                  u32 num_samples, bool alt) {
    ASSERT(num_samples <= 8);
    const auto samples_log = static_cast<u32>(std::bit_width(num_samples) - 1);
    const auto row = u32(tile_mode) * 5;
    const auto column = std::bit_width(bpp) - 4; // bpps are 8, 16, 32, 64, 128
    return (alt ? macro_tile_extents_alt : macro_tile_extents)[samples_log][row + column];
}

constexpr std::tuple<u32, u32, size_t> ImageSizeLinearAligned(u32 pitch, u32 height, u32 bpp,
                                                              u32 num_samples) {
    const auto pitch_align = std::max(8u, 64u / ((bpp + 7) / 8));
    auto pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const auto height_aligned = height;
    size_t log_sz = pitch_aligned * height_aligned * num_samples;
    const auto slice_align = std::max(64u, 256u / ((bpp + 7) / 8));
    while (log_sz % slice_align) {
        pitch_aligned += pitch_align;
        log_sz = pitch_aligned * height_aligned * num_samples;
    }
    return {pitch_aligned, height_aligned, (log_sz * bpp + 7) / 8};
}

constexpr std::tuple<u32, u32, size_t> ImageSizeMicroTiled(u32 pitch, u32 height, u32 thickness,
                                                           u32 bpp, u32 num_samples) {
    constexpr auto pitch_align = micro_tile_extent.first;
    constexpr auto height_align = micro_tile_extent.second;
    auto pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const auto height_aligned = (height + height_align - 1) & ~(height_align - 1);
    size_t log_sz = (pitch_aligned * height_aligned * bpp * num_samples + 7) / 8;
    while ((log_sz * thickness) % 256) {
        pitch_aligned += pitch_align;
        log_sz = (pitch_aligned * height_aligned * bpp * num_samples + 7) / 8;
    }
    return {pitch_aligned, height_aligned, log_sz};
}

constexpr std::tuple<u32, u32, size_t> ImageSizeMacroTiled(u32 pitch, u32 height, u32 thickness,
                                                           u32 bpp, u32 num_samples,
                                                           AmdGpu::TileMode tile_mode, u32 mip_n,
                                                           bool alt) {
    const auto [pitch_align, height_align] = GetMacroTileExtents(tile_mode, bpp, num_samples, alt);
    ASSERT(pitch_align != 0 && height_align != 0);
    bool downgrade_to_micro = false;
    if (mip_n > 0) {
        const bool is_less_than_tile = pitch < pitch_align || height < height_align;
        // TODO: threshold check
        downgrade_to_micro = is_less_than_tile;
    }

    if (downgrade_to_micro) {
        return ImageSizeMicroTiled(pitch, height, thickness, bpp, num_samples);
    }

    const auto pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const auto height_aligned = (height + height_align - 1) & ~(height_align - 1);
    const auto log_sz = pitch_aligned * height_aligned * num_samples;
    return {pitch_aligned, height_aligned, (log_sz * bpp + 7) / 8};
}

} // namespace VideoCore

namespace AmdGpu {

struct Image;

static constexpr size_t NUM_TILE_MODES = 32;

enum class PipeConfig : u32 {
    P2 = 0,
    P4_8x16 = 4,
    P4_16x16 = 5,
    P4_16x32 = 6,
    P4_32x32 = 7,
    P8_16x16_8x16 = 8,
    P8_16x32_8x16 = 9,
    P8_32x32_8x16 = 10,
    P8_16x32_16x16 = 11,
    P8_32x32_16x16 = 12,
    P8_32x32_16x32 = 13,
    P8_32x64_32x32 = 14,
    P16_32x32_8x16 = 16,
    P16_32x32_16x16 = 17,
    P16 = 18,
};

enum class MicroTileMode : u32 {
    Display = 0,
    Thin = 1,
    Depth = 2,
    Rotated = 3,
    Thick = 4,
};

enum class MacroTileMode : u32 {
    Mode_1x4_16 = 0,
    Mode_1x2_16 = 1,
    Mode_1x1_16 = 2,
    Mode_1x1_16_Dup = 3,
    Mode_1x1_8 = 4,
    Mode_1x1_4 = 5,
    Mode_1x1_2 = 6,
    Mode_1x1_2_Dup = 7,
    Mode_1x8_16 = 8,
    Mode_1x4_16_Dup = 9,
    Mode_1x2_16_Dup = 10,
    Mode_1x1_16_Dup2 = 11,
    Mode_1x1_8_Dup = 12,
    Mode_1x1_4_Dup = 13,
    Mode_1x1_2_Dup2 = 14,
    Mode_1x1_2_Dup3 = 15,
};

enum class ArrayMode : u32 {
    ArrayLinearGeneral = 0,
    ArrayLinearAligned = 1,
    Array1DTiledThin1 = 2,
    Array1DTiledThick = 3,
    Array2DTiledThin1 = 4,
    ArrayPrtTiledThin1 = 5,
    ArrayPrt2DTiledThin1 = 6,
    Array2DTiledThick = 7,
    Array2DTiledXThick = 8,
    ArrayPrtTiledThick = 9,
    ArrayPrt2DTiledThick = 10,
    ArrayPrt3DTiledThin1 = 11,
    Array3DTiledThin1 = 12,
    Array3DTiledThick = 13,
    Array3DTiledXThick = 14,
    ArrayPrt3DTiledThick = 15,
};

enum class TileMode : u32 {
    Depth2DThin64 = 0,
    Depth2DThin128 = 1,
    Depth2DThin256 = 2,
    Depth2DThin512 = 3,
    Depth2DThin1K = 4,
    Depth1DThin = 5,
    Depth2DThinPrt256 = 6,
    Depth2DThinPrt1K = 7,
    DisplayLinearAligned = 8,
    Display1DThin = 9,
    Display2DThin = 10,
    DisplayThinPrt = 11,
    Display2DThinPrt = 12,
    Thin1DThin = 13,
    Thin2DThin = 14,
    Thin3DThin = 15,
    ThinThinPrt = 16,
    Thin2DThinPrt = 17,
    Thin3DThinPrt = 18,
    Thick1DThick = 19,
    Thick2DThick = 20,
    Thick3DThick = 21,
    ThickThickPrt = 22,
    Thick2DThickPrt = 23,
    Thick3DThickPrt = 24,
    Thick2DXThick = 25,
    Thick3DXThick = 26,
    DisplayLinearGeneral = 31,
};

std::string_view NameOf(TileMode tile_mode);

ArrayMode GetArrayMode(TileMode tile_mode);

MicroTileMode GetMicroTileMode(TileMode tile_mode);

PipeConfig GetPipeConfig(TileMode tile_mode);

PipeConfig GetAltPipeConfig(TileMode tile_mode);

u32 GetSampleSplit(TileMode tile_mode);

u32 GetTileSplitHw(TileMode tile_mode);

u32 GetBankWidth(MacroTileMode mode);

u32 GetBankHeight(MacroTileMode mode);

u32 GetNumBanks(MacroTileMode mode);

u32 GetMacrotileAspect(MacroTileMode mode);

u32 GetAltBankHeight(MacroTileMode mode);

u32 GetAltNumBanks(MacroTileMode mode);

u32 GetAltMacrotileAspect(MacroTileMode mode);

bool IsMacroTiled(ArrayMode array_mode);

bool IsPrt(ArrayMode array_mode);

u32 GetMicroTileThickness(ArrayMode array_mode);

u32 GetPipeCount(PipeConfig pipe_cfg);

u32 CalculateTileSplit(TileMode tile_mode, ArrayMode array_mode, MicroTileMode micro_tile_mode,
                       u32 bpp);

MacroTileMode CalculateMacrotileMode(TileMode tile_mode, u32 bpp, u32 num_samples) noexcept;

} // namespace AmdGpu

namespace Gpu {
VkFormat ColorFormat(u32 info);                         // CB_COLOR*_INFO FORMAT and NUMBER_TYPE -> attachment format
VkFormat SurfaceFormat(u32 dfmt, u32 nfmt);             // T# data/number format -> image format
u32 TexelBytes(VkFormat f);                             // bytes per texel of the uncompressed formats render targets use
AmdGpu::CompMapping DstSel(const u32* t);               // T# DST_SEL_X/Y/Z/W for the image's view format
VkComponentMapping DstSelect(const u32* t);
int DimKind(Gcn::Dim d);                                // bindless array of a texture slot
std::string_view TilingSource();                        // the detiling compute shader (GLSL)
} // namespace Gpu
