// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shaders/shader_catalog.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "main/mapped_file.h"
#endif
#include <algorithm>
#include <atomic>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <thread>

namespace Gpu::Catalog {
using s32 = std::int32_t;
using s64 = std::int64_t;

#ifdef _WIN32
Mapping::Mapping(const std::filesystem::path& path_) : path(path_) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    LARGE_INTEGER length{};
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open " + path.string());
    file = handle;
    if (!GetFileSizeEx(handle, &length) || length.QuadPart <= 0) { CloseHandle(handle); throw std::runtime_error("Empty " + path.string()); }
    size = size_t(length.QuadPart);
    mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping) data = static_cast<const u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data) { if (mapping) CloseHandle(mapping); CloseHandle(handle); throw std::runtime_error("Cannot map " + path.string()); }
}
Mapping::~Mapping() { UnmapViewOfFile(data); CloseHandle(mapping); CloseHandle(file); }
#else
Mapping::Mapping(const std::filesystem::path& path_) : path(path_) {
    if (!Gow3::MapReadOnly(path, data, size, Gow3::FileAccess::Random))
        throw std::runtime_error("Cannot map " + path.string());
}
Mapping::~Mapping() { Gow3::UnmapReadOnly(data, size); }
#endif
void Mapping::Range(size_t p, size_t n, size_t end) const {
    if (end > size || p > end || n > end - p) throw std::runtime_error("Asset range outside record");
}
size_t Mapping::Relative(size_t p, size_t begin, size_t end) const {
    const s64 target = s64(p) + Read<s32>(p);
    if (target < s64(begin) || target >= s64(end)) throw std::runtime_error("Asset relative reference outside record");
    return size_t(target);
}
std::string Mapping::Text(size_t p, size_t n) const {
    Range(p, n, size);
    const auto* text = reinterpret_cast<const char*>(data + p);
    return {text, strnlen(text, n)};
}

Image ApiFormat(u32 api) {
    Image image;
    image.data_format = u8(api & 63); image.num_format = u8(api >> 8 & 15);
    image.dst_select = {u8(api >> 12 & 7), u8(api >> 15 & 7), u8(api >> 18 & 7), u8(api >> 21 & 7)};
    return image;
}

namespace {
size_t Rva(const Mapping& f, u64 address, size_t size = 1) {
    const size_t elf = f.Read<u32>(0) == 0x1d3d154f ? 32 + size_t(f.Read<u16>(24)) * 32 : 0;
    if (f.Read<u32>(elf) != 0x464c457f) throw std::runtime_error("Unknown EBOOT format");
    const auto ph = elf + f.Read<u64>(elf + 32);
    for (u32 i = 0; i < f.Read<u16>(elf + 56); ++i) {
        const auto p = ph + i * 56, va = f.Read<u64>(p + 16), bytes = f.Read<u64>(p + 32);
        if (f.Read<u32>(p) != 1 || va > address || address - va > bytes || size > bytes - (address - va)) continue;
        size_t offset = f.Read<u64>(p + 8);
        if (elf) for (u32 n = 0; n < f.Read<u16>(24); ++n) {
            const auto entry = 32 + n * 32; const auto flags = f.Read<u64>(entry);
            if ((flags & 0x800) && (flags >> 20 & 0xfff) == i) {
                if (flags & 10 || f.Read<u64>(entry + 16) < bytes) throw std::runtime_error("Compressed/encrypted EBOOT segment");
                offset = f.Read<u64>(entry + 8); break;
            }
        }
        f.Range(offset + address - va, size, f.size);
        return offset + address - va;
    }
    throw std::runtime_error("EBOOT address not mapped");
}

Shader Header(const Mapping& f, size_t p, u32 stage, size_t end, size_t external_code = 0) {
    const size_t fixed = stage == 0 ? 60 : 40;
    f.Range(p, fixed, end);
    const size_t code = external_code ? external_code : p + f.Read<u32>(p + 8), count = f.data[p + 3];
    const size_t n = fixed + count * 4 + (stage == 0 ? size_t(f.data[p + 56]) * 2 : stage == 1 ? size_t(f.data[p + 36]) * 4 + size_t(f.data[p + 37]) * 2 : 0);
    f.Range(p, n, end);
    if (f.Read<u32>(p + 12) != 0xffffffff || (!external_code && ((n + 3) & ~size_t(3)) != code - p))
        throw std::runtime_error("Invalid linked shader header");
    f.Range(code, 8, end);
    const size_t footer = code + (u64(f.Read<u32>(code + 4)) + 1) * 8;
    f.Range(footer, 28, end);
    if (std::memcmp(f.data + footer, "OrbShdr\x07", 8) || ((f.Read<u32>(footer + 8) >> 2) & 15) != stage)
        throw std::runtime_error("Linked shader stage mismatch");
    const size_t usage_end = footer - size_t(f.data[footer + 12]) * 4, usage_bytes = size_t(f.data[footer + 13]) * 4;
    f.Range(usage_end >= usage_bytes ? usage_end - usage_bytes : f.size, usage_bytes, end);
    if (usage_bytes != count * 4 || usage_end < usage_bytes || std::memcmp(f.data + p + fixed, f.data + usage_end - usage_bytes, usage_bytes))
        throw std::runtime_error("Linked shader usage mismatch");
    f.Range(code, f.Read<u32>(footer + 8) >> 8, end);
    Shader shader{f.Read<u64>(footer + 16), stage, p, code, footer, {}};
    for (size_t i = 0; i < count; ++i) {
        const auto* u = f.data + p + fixed + i * 4;
        shader.usage.push_back({u[0], u[1], u[2], u[3]});
    }
    return shader;
}

std::vector<EbootShader> Embedded(const Mapping& f) {
    std::vector<EbootShader> shaders;
    for (const auto [table, code_base] : {std::pair{0x3c2b84u, 0x3fe600u}, {0x3c2c98u, 0x3fe800u}, {0x3c30e8u, 0x3ff100u}, {0x3c3284u, 0x3ff500u},
            {0x3c3338u, 0x3ff700u}, {0x3c3424u, 0x3ff900u}, {0x3c3508u, 0x3ffb00u}, {0x3c3b14u, 0x408a00u}}) {
        const auto root = Rva(f, table);
        const auto count = f.Read<u32>(root);
        if (count < 1 || count > 6 || f.Read<u32>(root + 4) != 4 + count * 4) throw std::runtime_error("Unknown embedded shader table");
        for (u32 g = 0; g < count; ++g) {
            const auto group = root + f.Read<u32>(root + 4 + g * 4);
            for (u32 n = 0; n < f.Read<u32>(group); ++n) {
                const auto links = root + f.Read<u32>(group + 4 + n * 4);
                for (u32 s = 0; s < 2; ++s) {
                    const auto relative = f.Read<u32>(links + s * 4);
                    if (!relative) continue;
                    const auto p = root + relative + 4;
                    if (f.Read<u32>(p + 12) != 0xffffffff) { if (s) break; throw std::runtime_error("Embedded shader relocation mismatch"); }
                    const auto code = Rva(f, code_base + u32(f.Read<u32>(p + 8) << 8));
                    const auto footer = code + (u64(f.Read<u32>(code + 4)) + 1) * 8;
                    const auto stage = (f.Read<u32>(footer + 8) >> 2) & 15;
                    if (s && stage != 1) break;
                    shaders.push_back({Header(f, p, stage, f.size, code), table, g});
                    if (stage == 4) break;
                }
            }
        }
    }
    return shaders;
}

u32 BufferFormat(u32 api) { return api ? ((api & 15) << 15) | ((api >> 8 & 7) << 12) | (api >> 12 & 0xfff) : 0x77200; }

u32 TextureFormat(const Mapping& f, size_t dds) {
    const auto flags = f.Read<u32>(dds + 0x4c), fourcc = f.Read<u32>(dds + 0x50);
    const auto bits = f.Read<u32>(dds + 0x54), alpha = f.Read<u32>(dds + 0x64);
    u32 kind{};
    if (flags & 4) {
        if (fourcc == 0x31545844) kind = 1;
        if (fourcc == 0x32545844 || fourcc == 0x33545844) kind = 2;
        if (fourcc == 0x34545844 || fourcc == 0x35545844) kind = 3;
    } else if (bits == 8) kind = (flags >> 6 & 1) + 5;
    else if (bits == 32) kind = 4;
    else if (bits == 16) {
        if (flags & 0x40) kind = alpha == 0 ? 10 : alpha == 0xf000 ? 9 : alpha == 0x8000 ? 8 : 0;
        else if (flags & 1) kind = alpha == 0xff || alpha == 0xff00 ? 7 : 11;
        else if (flags == 0x20000) kind = 11;
    }
    constexpr std::array<u32, 12> formats{0, 0xfac023, 0xfac024, 0xfac025, 0xf2e00a, 0x800001, 0x324001, 0xb24003, 0x9f5012, 0x9f5013, 0x3ac010, 0x924002};
    if (!kind) throw std::runtime_error("DDS format not handled by original 01213c30/011c7970");
    return formats[kind];
}

// Technique record (0x770001 group entry): binding lists, vertex declaration and linked shaders.
Recipe Technique(const Mapping& f, size_t base, size_t begin, size_t end, const std::array<u32, 24>& formats) {
    f.Range(base, 0x8c, end);
    Recipe recipe{.shader_key = f.Read<u32>(base + 0x14), .permutation = f.Read<u64>(base + 0x20)};
    for (const auto& [kind, offset, count_offset] : {std::tuple{0, 0x64, 0x3a}, {1, 0x68, 0x3c}, {1, 0x6c, 0x3e}, {1, 0x70, 0x40}, {1, 0x74, 0x42}, {2, 0x78, 0x44}}) {
        const auto count = f.Read<u16>(base + count_offset);
        const size_t list = count ? f.Relative(base + offset, begin, end) : 0;
        f.Range(list, size_t(count) * 4, count ? end : f.size);
        for (u32 i = 0; i < count; ++i) {
            const auto word = f.Read<u32>(list + i * 4);
            const u32 slot = (word >> 9) & 511, source = (word >> 20) & 2047;
            if (kind == 0) {
                const u32 format = (word >> 2) & 127, components = (word & 3) + 1;
                if (format < 1 || format > 6 || slot >= 16) throw std::runtime_error("Unknown vertex format or slot");
                recipe.vertex.push_back({slot, source, BufferFormat(formats[(format - 1) * 4 + components - 1])});
            } else if (kind == 2) recipe.textures.push_back({slot, source});
        }
    }
    if (f.Read<s32>(base + 0x84)) recipe.vs = Header(f, f.Relative(base + 0x84, begin, end), 1, end);
    if (f.Read<s32>(base + 0x88)) recipe.ps = Header(f, f.Relative(base + 0x88, begin, end), 0, end);
    return recipe;
}

std::vector<std::pair<u32, std::vector<Stream>>> Geometry(const Mapping& f, size_t base, size_t end) {
    std::vector<std::pair<u32, std::vector<Stream>>> meshes;  // material index, streams
    f.Range(base, 0x18, end);
    const auto count = f.Read<u16>(base + 8);
    f.Range(base + 0x18, size_t(count) * 4, end);
    for (u32 g = 0; g < count; ++g) {
        const auto group = base + f.Read<u32>(base + 0x18 + g * 4);
        f.Range(group, 4, end);
        f.Range(group + 4, size_t(f.data[group + 2]) * 4, end);
        for (u32 l = 0; l < f.data[group + 2]; ++l) {
            const auto lod = group + f.Read<u32>(group + 4 + l * 4);
            f.Range(lod, 0x14, end);
            const auto parts = f.Read<u16>(lod + 4);
            const auto table = f.Relative(lod + 0x10, base, end);
            f.Range(table, size_t(parts) * 4, end);
            for (u32 p = 0; p < parts; ++p) {
                const auto part = f.Relative(table + p * 4, base, end);
                if ((f.Read<u32>(part) & 0xfff00ff) != 0x17) continue;
                f.Range(part, 0x20, end);
                const auto meshes_in_part = f.Read<u32>(part + 0x10);
                const auto start = part + f.Read<u64>(part + 0x18);
                f.Range(start, size_t(meshes_in_part) * 0x24, end);
                for (u32 m = 0; m < meshes_in_part; ++m) {
                    const auto mesh = start + m * 0x24;
                    const auto streams = f.Read<u16>(mesh);
                    const auto first = streams ? f.Relative(mesh + 0x14, base, end) : 0;
                    f.Range(first, size_t(streams) * 12, streams ? end : f.size);
                    auto& entry = meshes.emplace_back(f.Read<u16>(part + 8), std::vector<Stream>{});
                    for (u32 s = 0; s < streams; ++s) entry.second.push_back({f.data[first + s * 12 + 5], u32(f.Read<u16>(first + s * 12 + 6) >> 6)});
                }
            }
        }
    }
    return meshes;
}

File Scan(const std::filesystem::path& path, const std::array<u32, 24>& formats) {
    File result;
    result.map = std::make_unique<Mapping>(path);
    const Mapping& f = *result.map;
    struct Group { std::vector<std::string> references; bool model{}; };
    std::vector<Group> groups, models;
    std::vector<std::pair<std::string, std::vector<std::pair<u32, std::vector<Stream>>>>> geometry;
    std::vector<std::string> material_names;
    for (size_t record = 0; record < f.size;) {
        f.Range(record, 64, f.size);
        const auto type = f.Read<u32>(record), length = f.Read<u32>(record + 4);
        const size_t base = record + 64, bytes = type == 0 ? 0 : length, end = base + bytes;
        f.Range(base, bytes, f.size);
        ++result.records;
        const auto name = f.Text(record + 8, 56);
        if (type == 2) groups.emplace_back();
        if (type == 1 && !bytes && !groups.empty()) groups.back().references.push_back(name);
        if (type == 3) {
            if (groups.empty()) throw std::runtime_error("Unbalanced WAD group end");
            if (groups.back().model) models.push_back(std::move(groups.back()));
            groups.pop_back();
        }
        try {
            if (type == 0x770001 && !name.starts_with("dbg_")) {
                f.Range(base, 12, end);
                const auto count = f.Read<u32>(base + 8);
                f.Range(base + 12, size_t(count) * 4, end);
                std::vector<Recipe> recipes;
                for (u32 g = 0; g < count; ++g) {
                    const size_t group = base + f.Read<u32>(base + 12 + g * 4);
                    f.Range(group, 16, end);
                    const auto entries = f.Read<u32>(group);
                    const auto list = group + f.Read<u64>(group + 8);
                    f.Range(list, size_t(entries) * 8, end);
                    for (u32 n = 0; n < entries; ++n) recipes.push_back(Technique(f, group + f.Read<u64>(list + n * 8), base, end, formats));
                }
                std::ranges::move(recipes, std::back_inserter(result.recipes));
            } else if (type == 0x560001) {
                f.Range(base, 0x58, end);
                Material material{.flags = f.Read<u32>(base + 0x4c)};
                material.pass_supplies_shader = material.flags & 0x1000;
                size_t layer = base + 0x58;
                for (u32 n = 0; n < f.Read<u16>(base + 0x50); ++n) {
                    f.Range(layer, 0x80, end);
                    const u32 count = f.data[layer + 0x18];
                    f.Range(layer + 0x80, size_t(count) * 0x68, end);
                    auto& item = material.layers.emplace_back(Material::Layer{f.Read<u32>(layer + 0x24), {}});
                    for (u32 t = 0; t < count; ++t) {
                        const auto p = layer + 0x80 + t * 0x68;
                        item.textures.emplace_back(f.data[p], f.Text(p + 0x30, 56));
                    }
                    layer += 0x80 + size_t(count) * 0x68;
                }
                // The render-state block: its entry 1 (32 bytes) holds the blend at bytes 16-21; its values do not reach the shaders.
                if (f.Read<u16>(base + 0x52)) {
                    f.Range(layer, 8, end);
                    const auto count = f.Read<u16>(layer + 2);
                    const auto table = layer + f.Read<u16>(layer + 4);
                    f.Range(table, size_t(count) * 4, end);
                    for (u32 n = 0; n < count; ++n)
                        if (f.Read<u16>(table + n * 4) == 1) {
                            const size_t state = layer + f.Read<u16>(table + n * 4 + 2);
                            f.Range(state, 32, end);
                            std::memcpy(material.blend.data(), f.data + state + 16, material.blend.size());
                            break;
                        }
                }
                result.materials.push_back(std::move(material)); material_names.push_back(name);
            } else if (type == 0x520001) {
                const auto version = (f.Read<u32>(base) >> 16) & 0xfff;
                if (version == 1) geometry.emplace_back(name, Geometry(f, base, end));
                else if (version == 2 && !groups.empty()) groups.back().model = true;
                else throw std::runtime_error("Unsupported model/geometry source version");
            } else if (type == 0xd0001 && bytes >= 4 && (f.Read<u32>(base) & 255) == 7) {
                f.Range(base, 0xcc, end);
                if (f.Read<u32>(base + 0x4c) != 0x20534444) throw std::runtime_error("Texture has no DDS header");
                const size_t dds = base + 0x50;
                auto image = ApiFormat(TextureFormat(f, dds));
                const u32 depth = f.Read<u32>(dds + 20);
                image.width = u16(f.Read<u32>(dds + 12)); image.height = u16(f.Read<u32>(dds + 8)); image.depth = u16(std::max(1u, depth));
                image.degamma = f.Read<u32>(dds + 4) & 0x10000000;
                image.type = (f.data[dds + 0x6d] & 0xfe) ? 11 : (f.data[dds + 0x6e] & 0x20) && depth ? 10 : 9;
                result.textures[name] = image;
            }
        } catch (const std::exception&) { ++result.failed; }
        record = (end + 15) & ~size_t(15);
    }
    if (!groups.empty()) throw std::runtime_error("Unbalanced WAD group start");
    std::unordered_map<std::string, u32> materials, geometries;
    for (u32 i = 0; i < material_names.size(); ++i) materials.emplace(material_names[i], i);
    for (u32 i = 0; i < geometry.size(); ++i) geometries.emplace(geometry[i].first, i);
    for (const auto& model : models) {
        std::vector<u32> order;
        for (const auto& reference : model.references) if (const auto it = materials.find(reference); it != materials.end()) order.push_back(it->second);
        for (const auto& reference : model.references) {
            const auto found = geometries.find(reference);
            if (found == geometries.end()) continue;
            const auto& meshes = geometry[found->second].second;
            for (u32 i = 0; i < meshes.size(); ++i) {
                if (meshes[i].first >= order.size()) throw std::runtime_error("Submesh material index outside model list");
                result.mesh_materials.push_back({found->second, i, order[meshes[i].first]});
            }
        }
    }
    for (auto& [_, meshes] : geometry) {
        auto& streams = result.geometry.emplace_back();
        for (auto& [__, mesh] : meshes) streams.push_back(std::move(mesh));
    }
    return result;
}
} // namespace

Data Read(const std::filesystem::path& game, u32 threads, u32 sample_log2) {
    Data data{.game = game, .eboot = std::make_unique<Mapping>(game / "eboot.bin")};
    const Mapping& eboot = *data.eboot;
    if (eboot.Read<u32>(Rva(eboot, 0x3f7db0)) != 1 || eboot.Read<u64>(Rva(eboot, 0x3f7db8, 8)) != 2 ||
        eboot.Read<u32>(Rva(eboot, 0x3f84d0)) != 2 || eboot.Read<u64>(Rva(eboot, 0x3f84d8, 8)) != 4)
        throw std::runtime_error("Unsupported EBOOT material permutation selectors");
    const auto format_table = Rva(eboot, 0x3cdac0, sizeof(data.vertex_formats));
    for (u32 n = 0; n < data.vertex_formats.size(); ++n) data.vertex_formats[n] = eboot.Read<u32>(format_table + n * 4);
    data.embedded = Embedded(eboot);
    if (eboot.Read<u32>(Rva(eboot, 0x3cdcd0)) != 1 || sample_log2 > 2)
        throw std::runtime_error("Unsupported MSAA configuration");
    data.sample_log2 = sample_log2;
    // Render resources (0x3c42a0, 22 x 0x34) with their aliases; three of them take the configured sample count.
    struct Resource { u32 alias, target, sampled, sample_log2; bool configured, degamma; };
    std::array<Resource, 22> resources{};
    const auto table = Rva(eboot, 0x3c42a0, 22 * 0x34), aliases = Rva(eboot, 0x2ebc90, 22 * 4), multisampled = Rva(eboot, 0x2ebc80, 3 * 4);
    for (u32 i = 0; i < resources.size(); ++i) {
        const auto p = table + i * 0x34;
        bool configured{};
        for (u32 n = 0; n < 3; ++n) configured |= eboot.Read<u32>(multisampled + n * 4) == i;
        resources[i] = {eboot.Read<u32>(aliases + i * 4), eboot.Read<u32>(p + 12), eboot.Read<u32>(p + 16), eboot.Read<u32>(p + 20), configured, bool(eboot.data[p + 0x23])};
    }
    data.scene_color = ApiFormat(resources[resources[6].alias].target);
    // Texture semantics (0x3cc880, 49 x 0x4e) name the pass views.
    std::unordered_map<std::string, u32> semantics;
    const auto names = Rva(eboot, 0x3cc880, 49 * 0x4e);
    for (u32 i = 0; i < 49; ++i) {
        const auto name = eboot.Text(names + i * 0x4e + 9, 0x4e - 9);
        if (name.empty() || !semantics.emplace(name, i).second) throw std::runtime_error("Invalid EBOOT texture semantic table");
        const u8 dimension = eboot.data[names + i * 0x4e + 2];  // 2: 2D, 3: volume, 4: cube
        data.semantic_types[i] = dimension == 3 ? 10 : dimension == 4 ? 11 : 9;
    }
    const auto add_view = [&](const char* name, u32 api, std::optional<u32> type, std::array<u16, 3> extent, bool degamma, bool unnormalized, std::string condition) {
        PassView view{semantics.at(name), ApiFormat(api), type.has_value(), std::move(condition)};
        view.image.type = u8(type.value_or(data.sample_log2 ? 14 : 9));
        if (view.image.type == 14 || view.image.type == 15) view.image.sample_log2 = u8(data.sample_log2);
        view.image.width = extent[0]; view.image.height = extent[1]; view.image.depth = extent[2];
        view.image.degamma = degamma; view.image.unnormalized = unnormalized;
        data.views.push_back(std::move(view));
    };
    for (const auto [name, id] : {std::pair{"WhiteBuffer", 8u}, {"DepthBuffer", 20u}, {"RefractionMapSampler", 6u}, {"ReflectionMapSampler", 13u}}) {
        const auto& resource = resources[resources[id].alias];
        const bool depth = id == 20, refraction = id == 6;
        const std::optional<u32> type = refraction ? std::optional<u32>(9) : resource.configured ? std::nullopt : std::optional<u32>(resource.sample_log2 ? 14 : 9);
        add_view(name, depth ? 0x204704u : refraction ? 0xf2e00au : resource.sampled, type, {8, 8, 1}, !depth && (refraction || resource.degamma),
                 id == 8, id == 8 ? "packet_flags & 0x100" : "context semantic enabled");
        if (type == 14) data.views.back().image.sample_log2 = u8(resource.sample_log2);
    }
    data.depth_view = data.views[1].image;
    add_view("FogTextureSampler", 0x97770c, 10, {64, 32, 16}, false, false, "fog_mode == 1");
    add_view("SpatialLightingSampler", 0xfac00a, 11, {8, 8, 1}, false, false, "PS3_RF_PixelLightProbes != 0");

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(game / "EXEC" / "WAD"))
        if (entry.is_regular_file() && (entry.path().extension() == ".WAD" || entry.path().extension() == ".wad") && entry.path().parent_path().filename() != "bak") paths.push_back(entry.path());
    std::ranges::sort(paths);
    data.files.resize(paths.size());
    std::atomic<size_t> next{};
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (u32 i = 0; i < threads; ++i) workers.emplace_back([&] {
        for (size_t n; (n = next.fetch_add(1, std::memory_order_relaxed)) < paths.size();) {
            try { data.files[n] = Scan(paths[n], data.vertex_formats); }
            catch (const std::exception& error) { data.files[n] = {}; data.files[n].fatal = paths[n].string() + ": " + error.what(); }
        }
    });
    workers.clear();
    return data;
}
} // namespace Gpu::Catalog
