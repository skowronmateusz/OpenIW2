// Binary material files (docs/spec/material.md).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

namespace TextureSemantic {
constexpr uint8_t Color = 2, Normal = 3, Specular = 4, Water = 5;
}

struct MaterialTexture {
    std::string samplerName; // e.g. "colorMap"
    uint8_t samplerState = 0;
    uint8_t semantic = 0;
    std::string imageName;   // empty for water entries
};

struct MaterialConstant {
    std::string name;
    std::array<float, 4> value{};
};

struct Material {
    std::string name;
    std::string referenceImage;
    std::string techniqueSet;
    std::vector<MaterialTexture> textures;
    std::vector<MaterialConstant> constants;

    // Image used for the base color, or empty.
    const std::string& colorImage() const;
};

// Throws std::runtime_error on malformed input.
Material parseMaterial(std::span<const uint8_t> data);

// Splits one 4 MiB lightmap block into its four images.
struct LightmapParts {
    std::span<const uint8_t> coeffR, coeffG, coeffB; // 512x512 BGRA each
    std::span<const uint8_t> sun;                    // 1024x1024 8-bit
    static constexpr int kCoeffSize = 512, kSunSize = 1024;
};
LightmapParts splitLightmap(std::span<const uint8_t> block);

} // namespace iw2::assets
