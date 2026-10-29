#include "assets/material.h"

#include <cstring>
#include <stdexcept>

namespace iw2::assets {
namespace {

uint32_t u32(std::span<const uint8_t> d, size_t o) {
    if (o + 4 > d.size()) throw std::runtime_error("material: truncated");
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}

uint16_t u16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size()) throw std::runtime_error("material: truncated");
    return uint16_t(d[o] | (d[o + 1] << 8));
}

std::string str(std::span<const uint8_t> d, uint32_t o) {
    if (o >= d.size()) throw std::runtime_error("material: string offset out of range");
    const char* p = reinterpret_cast<const char*>(d.data() + o);
    return std::string(p, strnlen(p, d.size() - o));
}

} // namespace

const std::string& Material::colorImage() const {
    static const std::string empty;
    for (const auto& t : textures)
        if (t.samplerName == "colorMap") return t.imageName;
    for (const auto& t : textures)
        if (t.semantic == TextureSemantic::Color) return t.imageName;
    return empty;
}

Material parseMaterial(std::span<const uint8_t> d) {
    Material m;
    m.name = str(d, u32(d, 0x00));
    m.referenceImage = str(d, u32(d, 0x04));
    const uint16_t textureCount = u16(d, 0x34);
    const uint16_t constantCount = u16(d, 0x36);
    m.sortByte = d[0x0d];
    m.techniqueSet = str(d, u32(d, 0x38));

    const uint32_t texTable = u32(d, 0x3c);
    for (uint16_t i = 0; i < textureCount; ++i) {
        const size_t e = size_t(texTable) + size_t(i) * 12;
        MaterialTexture t;
        t.samplerName = str(d, u32(d, e));
        if (e + 6 > d.size()) throw std::runtime_error("material: truncated texture table");
        t.samplerState = d[e + 4];
        t.semantic = d[e + 5];
        if (t.semantic != TextureSemantic::Water) {
            t.imageName = str(d, u32(d, e + 8));
        } else {
            const size_t o = u32(d, e + 8);
            if (o + 0x1c > d.size()) throw std::runtime_error("material: truncated water description");
            auto f = [&](size_t k) { const uint32_t b = u32(d, o + k); float v; std::memcpy(&v, &b, 4); return v; };
            t.water = {int(u16(d, o)), f(4), f(8), f(0xc), f(0x10), f(0x14), f(0x18)};
        }
        m.textures.push_back(std::move(t));
    }

    const uint32_t constTable = u32(d, 0x40);
    for (uint16_t i = 0; i < constantCount; ++i) {
        const size_t e = size_t(constTable) + size_t(i) * 20;
        MaterialConstant c;
        c.name = str(d, u32(d, e));
        for (int k = 0; k < 4; ++k) {
            const uint32_t bits = u32(d, e + 4 + size_t(k) * 4);
            std::memcpy(&c.value[size_t(k)], &bits, 4);
        }
        m.constants.push_back(std::move(c));
    }
    return m;
}

LightmapParts splitLightmap(std::span<const uint8_t> block) {
    constexpr size_t coeff = size_t(LightmapParts::kCoeffSize) * LightmapParts::kCoeffSize * 4;
    constexpr size_t sun = size_t(LightmapParts::kSunSize) * LightmapParts::kSunSize;
    if (block.size() != 3 * coeff + sun) throw std::runtime_error("lightmap: block must be 4 MiB");
    return {block.subspan(0, coeff), block.subspan(coeff, coeff), block.subspan(2 * coeff, coeff),
            block.subspan(3 * coeff, sun)};
}

} // namespace iw2::assets
