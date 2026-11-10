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
    // File byte 0x0d: the draw sort byte. The 1.3 renderer copies the 0x44-byte header verbatim (0x100171a0 / 0x10017203, rep movsl x17)
    // and the material table comparator 0x10016ae0 sorts by it ascending first (values in the shipped data: 3 opaque default, 4 sky,
    // 5 sun, 7-16 decals, 25-26 effect blends, 27 additive; ties are broken by the techset load order).
    uint8_t sortByte = 0;
    std::vector<MaterialTexture> textures;
    std::vector<MaterialConstant> constants;

    // Image used for the base color, or empty.
    const std::string& colorImage() const;
};

// Flag techniques (technique sets "*_flag*", shaders/lib/flag.hlsl): the vertex colour alpha is not a colour but the
// weight of the cloth wave (0 on the rigid pole and base, up to 1 at the free edge); the shader outputs the vertex
// colour with its green and blue channels swapped and alpha 1. Returns the colour a vertex is drawn with.
// TODO(spec): the wave offset itself (three sine waves along the normal, scaled by that weight) is not modelled.
bool isFlagTechniqueSet(const std::string& techniqueSet);
std::array<uint8_t, 4> drawnVertexColor(bool flagTechnique, const std::array<uint8_t, 4>& color);

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
