// Fixed-function state stored in a material file, decoded the way the 1.3 renderer applies it (docs/spec/material.md
// "Alpha test" and "Texture sampler state"). Header-only and free of GPU types so the rules can be tested without a device.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace iw2::assets {

// mtlAlphaTest. Values match the shader uniform: 0 is no test.
enum class AlphaFunc : uint8_t { None = 0, GreaterEqual128 = 1, GreaterThan0 = 2, Less128 = 3 };

// State word 0 (material file offset 0x2c): bit 0x800 disables the test, bits 0x3000 pick the function.
constexpr AlphaFunc decodeAlphaFunc(uint32_t stateWord0) {
    if (stateWord0 & 0x800) return AlphaFunc::None;
    switch (stateWord0 & 0x3000) {
        case 0x1000: return AlphaFunc::GreaterThan0;
        case 0x2000: return AlphaFunc::Less128;
        default: return AlphaFunc::GreaterEqual128;
    }
}

// Whether a fragment with this 8-bit output alpha is rejected.
constexpr bool alphaRejected(AlphaFunc f, uint8_t alpha) {
    switch (f) {
        case AlphaFunc::None: return false;
        case AlphaFunc::GreaterEqual128: return alpha < 128;
        case AlphaFunc::GreaterThan0: return alpha == 0;
        case AlphaFunc::Less128: return alpha >= 128;
    }
    return false;
}

// r_aaSamples for the scene passes: 1 (off), 2, 4 or 8. openiw2 defaults to 4, the original to 1 (docs/spec/material.md "Multisampling").
constexpr int kDefaultAaSamples = 4;
inline int parseAaSamples(const std::string& v) {
    if (v.empty()) return kDefaultAaSamples;
    const int n = std::atoi(v.c_str());
    return n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
}

// Store operations of the first part of the scene pass (docs/spec/material.md "Multisampling"). The pass is interrupted before the
// blended passes for the post-sun copy and resumed with LOAD; when it resumes, the colour samples and the depth must be kept, since
// a store of the resolve alone, or DONT_CARE for the depth, leaves the resumed pass reading undefined memory.
enum class StoreOp : uint8_t { DontCare, Store, Resolve, ResolveAndStore };
struct ScenePassStores {
    StoreOp color, depth;
};
constexpr ScenePassStores scenePassStores(int samples, bool resumed) {
    const bool multisampled = samples > 1;
    return {multisampled ? (resumed ? StoreOp::ResolveAndStore : StoreOp::Resolve) : StoreOp::Store,
            resumed ? StoreOp::Store : StoreOp::DontCare};
}

// The alpha the fixed-function test sees from a USE_ALPHA technique: the colour map's alpha times the vertex alpha (shade library,
// the textured diffuse multiplied by the vertex colour, whose alpha the lightmap and light-probe shaders write out), as 8 bits.
constexpr uint8_t shaderOutputAlpha(uint8_t textureAlpha, uint8_t vertexAlpha) {
    return uint8_t((unsigned(textureAlpha) * unsigned(vertexAlpha) + 127u) / 255u);
}

// params.x of the world and model fragment shaders: -1 for a USE_ALPHA 0 technique, else the function. A pass with alpha to
// coverage adds 8, so 9 selects the GE128 coverage ramp and the other values keep their test with a coverage of 1.
constexpr float alphaShaderParam(AlphaFunc f, bool alphaOne, bool coverage) {
    return (alphaOne ? -1.0f : float(int(f))) + (coverage ? 8.0f : 0.0f);
}

// The blend of state word 0 (material file offset 0x2c): source factor bits 0-3, destination factor bits 4-7, operation bits 8-10
// (0 Disable, 1 Add). Factors: 1 Zero, 2 One, 3 SrcColor, 5 SrcAlpha, 6 InvSrcAlpha (docs/spec/material.md "Draw order").
enum class MaterialBlend : uint8_t { None, Alpha, Additive, Multiply };
constexpr MaterialBlend decodeMaterialBlend(uint32_t stateWord0) {
    if (((stateWord0 >> 8) & 7) == 0) return MaterialBlend::None;
    const uint32_t src = stateWord0 & 0xf, dst = (stateWord0 >> 4) & 0xf;
    if (src == 2 && dst == 2) return MaterialBlend::Additive;
    if (src == 1 && dst == 3) return MaterialBlend::Multiply;
    return MaterialBlend::Alpha;
}

// Key of the scene draw list, compared field by field: material rank (sort byte, then the order of first use of the technique set
// and of the material), layer (lightmap index, 31 for none), entity (dynamic slot < static models < world).
struct SurfaceKey {
    int sort = 0, materialOrder = 0, layer = 31, entity = 0;
};
inline constexpr int kStaticModelEntity = 0x7ff, kWorldEntity = 0x800;
constexpr bool surfaceDrawsBefore(const SurfaceKey& a, const SurfaceKey& b) {
    if (a.sort != b.sort) return a.sort < b.sort;
    if (a.materialOrder != b.materialOrder) return a.materialOrder < b.materialOrder;
    if (a.layer != b.layer) return a.layer < b.layer;
    return a.entity < b.entity;
}
// Material order from the first-use indices of its technique set and of the material itself.
constexpr int materialOrder(int techsetFirstUse, int materialFirstUse) { return techsetFirstUse << 16 | materialFirstUse; }

// Clip depth added to a world surface that does not write depth, toward the camera. It makes
// the LessEqual tie with a coplanar surface drawn earlier hold for differently triangulated faces (deliberate deviation, see
// docs/spec/material.md "Draw order"). An eighth of one polygon offset level.
inline constexpr float kLayerTieBias = -1.0f / 524288.0f;
constexpr float layerTieBias(bool writesDepth) { return writesDepth ? 0.0f : kLayerTieBias; }

// Whether the dynamic light walk draws a surface: its technique set has a "pointlight" technique, for world and model surfaces alike. The
// sprite-like batches iw2view builds for effects are fullbright and never lit.
constexpr bool receivesDynamicLight(bool pointLit, bool fullbright) { return pointLit && !fullbright; }

// r_textureMode, in the order of its value list; the registered default is Bilinear.
enum class TextureMode : uint8_t { Nearest, Linear, Bilinear, Trilinear, Anisotropic };
constexpr int kDefaultAnisotropy = 8, kMinAnisotropy = 2, kMaxAnisotropy = 16;

// openiw2 forces the filter (deliberate deviation, docs/spec/material.md "Texture sampler state"): anisotropic at the highest
// supported level, whatever the profile, the config or a --cvar says. The graphics page's filter row is hidden for it
// (src/ui/menuhide.cpp).
constexpr TextureMode kForcedTextureMode = TextureMode::Anisotropic;
constexpr int kForcedAnisotropy = kMaxAnisotropy;

// The sampler byte a texture with no filter bits of its own takes from r_textureMode.
constexpr uint8_t textureModeState(TextureMode m) {
    constexpr uint8_t table[5] = {0x01, 0x02, 0x06, 0x0a, 0x0b};
    return table[size_t(m)];
}

// Accepts the mode name or its index; anything else is the default.
inline TextureMode parseTextureMode(const std::string& v) {
    static const char* const names[5] = {"nearest", "linear", "bilinear", "trilinear", "anisotropic"};
    for (int i = 0; i < 5; ++i)
        if (v == names[i] || v == std::to_string(i)) return TextureMode(i);
    return TextureMode::Bilinear;
}

enum class MipFilter : uint8_t { None, Nearest, Linear };

struct SamplerDesc {
    bool linear = true;      // minification and magnification; false = point
    bool anisotropic = false; // uses r_anisotropy
    MipFilter mip = MipFilter::None;
    bool clampU = false, clampV = false, clampW = false; // false = wrap
};

// `state` is MaterialTexture::samplerState. Filter bits 0-1 and mip bits 2-3 index {none, point, linear, anisotropic}.
constexpr SamplerDesc decodeSamplerState(uint8_t state, TextureMode mode) {
    if ((state & 0xf) == 0) state |= textureModeState(mode);
    const int filter = state & 3, mip = (state >> 2) & 3;
    SamplerDesc d;
    d.linear = filter >= 2;
    d.anisotropic = filter == 3;
    d.mip = mip == 0 ? MipFilter::None : mip == 1 ? MipFilter::Nearest : MipFilter::Linear;
    d.clampU = (state & 0x10) != 0, d.clampV = (state & 0x20) != 0, d.clampW = (state & 0x40) != 0;
    return d;
}

inline int clampAnisotropy(int a) { return std::clamp(a, kMinAnisotropy, kMaxAnisotropy); }

} // namespace iw2::assets
