// .d3dbsp map loading (docs/spec/d3dbsp.md).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

namespace BspLump {
constexpr int Materials = 0, Lightmaps = 1, Planes = 4, TriangleSoups = 7, DrawVertices = 8, DrawIndices = 9,
              Models = 35, Entities = 37;
constexpr int Count = 39;
} // namespace BspLump

struct BspMaterial {
    std::string name;
    uint32_t surfaceFlags = 0;
    uint32_t contentFlags = 0;
};

struct BspTriangleSoup {
    uint16_t material = 0;
    uint16_t lightmap = 0; // 31 = none
    uint32_t firstVertex = 0;
    uint16_t vertexCount = 0;
    uint16_t indexCount = 0;
    uint32_t firstIndex = 0;
};

struct BspVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<uint8_t, 4> color{};
    std::array<float, 2> uv{};
    std::array<float, 2> lightmapUv{};
    std::array<float, 3> tangent{};
    std::array<float, 3> binormal{};
};

struct BspLumpInfo {
    uint32_t length = 0;
    uint32_t offset = 0;
};

struct BspEntity {
    std::vector<std::pair<std::string, std::string>> fields;
    const std::string* get(const std::string& key) const;
};

class D3dBsp {
public:
    // Parses and validates the map. `data` must outlive this object. Throws std::runtime_error on bad input.
    explicit D3dBsp(std::span<const uint8_t> data);

    static constexpr uint16_t kNoLightmap = 31;
    static constexpr size_t kLightmapBytes = 4 * 1024 * 1024;

    const std::array<BspLumpInfo, BspLump::Count>& lumps() const { return lumps_; }
    std::span<const uint8_t> lump(int index) const;

    const std::vector<BspMaterial>& materials() const { return materials_; }
    const std::vector<BspTriangleSoup>& soups() const { return soups_; }
    const std::vector<BspVertex>& vertices() const { return vertices_; }
    const std::vector<uint16_t>& indices() const { return indices_; }
    size_t lightmapCount() const { return lump(BspLump::Lightmaps).size() / kLightmapBytes; }

    std::string entityString() const;
    std::vector<BspEntity> entities() const;

private:
    std::span<const uint8_t> data_;
    std::array<BspLumpInfo, BspLump::Count> lumps_{};
    std::vector<BspMaterial> materials_;
    std::vector<BspTriangleSoup> soups_;
    std::vector<BspVertex> vertices_;
    std::vector<uint16_t> indices_;
};

// Parses a `{ "key" "value" ... }` entity string.
std::vector<BspEntity> parseEntities(std::string_view text);

} // namespace iw2::assets
