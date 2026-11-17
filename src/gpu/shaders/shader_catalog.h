// SPDX-License-Identifier: GPL-2.0-or-later
// The shader programs of the EBOOT and the WADs with the draw contracts the engine derives from them: technique records
// (shaders, vertex and texture bindings), materials, model meshes and textures, and the render resources pass code samples.
// The AOT builds its variants from this (shader_contract.cpp), so a program is translated whether or not a recipe has met it.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gpu::Catalog {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Read-only mapping of a game file. Generation reads shader headers and code from it, so it stays open.
struct Mapping {
    explicit Mapping(const std::filesystem::path& path);
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    void Range(size_t p, size_t n, size_t end) const;
    template<class T> T Read(size_t p) const { Range(p, sizeof(T), size); T value; std::memcpy(&value, data + p, sizeof(T)); return value; }
    size_t Relative(size_t p, size_t begin, size_t end) const;
    std::string Text(size_t p, size_t n) const;
    std::filesystem::path path;
    void* file{};
    void* mapping{};
    const u8* data{};
    size_t size{};
};
// A linked shader: stage header, ISA and OrbShdr footer.
struct Shader {
    u64 identity{};
    u32 stage{};  // 0 pixel, 1 vertex, 4 compute
    size_t header{}, code{}, footer{};
    std::vector<std::array<u8, 4>> usage;  // input usage: kind, slot, user register, flags
};
struct Image {
    u8 type{9}, data_format{14}, num_format{7}, sample_log2{};
    std::array<u8, 4> dst_select{};
    u16 width{64}, height{64}, depth{1};
    bool degamma{}, unnormalized{};
    auto operator<=>(const Image&) const = default;
};
Image ApiFormat(u32 api);
struct Recipe {
    struct Vertex { u32 slot{}, source{}, word{}; };
    struct Texture { u32 slot{}, source{}; };
    u32 shader_key{};
    u64 permutation{};
    std::vector<Vertex> vertex;
    std::vector<Texture> textures;
    std::optional<Shader> vs, ps;
};
struct Material {
    struct Layer { u32 shader_key{}; std::vector<std::pair<u32, std::string>> textures; };
    u32 flags{};
    bool pass_supplies_shader{};
    std::vector<Layer> layers;
};
struct Stream {
    u32 source{}, stride{};
    auto operator<=>(const Stream&) const = default;
};
struct MeshMaterial { u32 geometry{}, mesh{}, material{}; };
struct File {
    std::unique_ptr<Mapping> map;
    std::vector<Recipe> recipes;
    std::vector<Material> materials;
    std::unordered_map<std::string, Image> textures;
    std::vector<std::vector<std::vector<Stream>>> geometry;  // geometry -> mesh -> streams
    std::vector<MeshMaterial> mesh_materials;
    std::string fatal;
    u32 records{}, failed{};
};
struct EbootShader : Shader { u32 table{}, group{}; };
// A render resource sampled by pass code under a semantic. Without a fixed type the view follows the scene sample count.
struct PassView {
    u32 semantic{};
    Image image;
    bool fixed_type{};
    std::string condition;
};
struct Data {
    std::filesystem::path game;
    std::unique_ptr<Mapping> eboot;
    std::vector<File> files;
    std::vector<EbootShader> embedded;
    std::array<u32, 24> vertex_formats{};
    u32 sample_log2{};
    std::array<u8, 49> semantic_types{};  // image type a texture semantic declares
    Image scene_color, depth_view;
    std::vector<PassView> views;
};
// Reads the EBOOT's tables and every WAD (a thread each). Throws when the EBOOT is not the supported one.
Data Read(const std::filesystem::path& game, u32 threads, u32 sample_log2 = 1);
} // namespace Gpu::Catalog
