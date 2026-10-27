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

// Water description a semantic-5 texture entry points to (loader 0x1001bf30 builds the renderer's 0x44-byte struct from it:
// width = height = u16 at +0, +4 = Lx, +8 = Ly, +0xc = A, +0x10 = V, +0x14 / +0x18 = wind x / y; g is the constant 800.0).
struct WaterDesc {
    int size = 0;
    float lx = 0, ly = 0, amplitude = 0, v = 0, windX = 0, windY = 0;
};

struct MaterialTexture {
    std::string samplerName; // e.g. "colorMap"
    uint8_t samplerState = 0;
    uint8_t semantic = 0;
    std::string imageName;   // empty for water entries
    WaterDesc water;         // semantic 5 only
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
