// Alpha test and sampler state decoded from a material (assets/materialstate.h, docs/spec/material.md).
#include "assets/materialstate.h"
#include <cstdio>

using namespace iw2::assets;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    // Alpha function from state word 0: disable bit, then function bits.
    CHECK(decodeAlphaFunc(0x0802) == AlphaFunc::None);
    CHECK(decodeAlphaFunc(0x3802) == AlphaFunc::None);
    CHECK(decodeAlphaFunc(0x2812b012) == AlphaFunc::GreaterEqual128); // egypt_rug3
    CHECK(decodeAlphaFunc(0x28125012) == AlphaFunc::GreaterThan0);    // toujane_decal_rug1
    CHECK(decodeAlphaFunc(0x2000) == AlphaFunc::Less128);
    CHECK(decodeAlphaFunc(0x0000) == AlphaFunc::GreaterEqual128); // the fall-through case

    CHECK(!alphaRejected(AlphaFunc::GreaterEqual128, 128));
    CHECK(alphaRejected(AlphaFunc::GreaterEqual128, 127));
    CHECK(alphaRejected(AlphaFunc::GreaterThan0, 0));
    CHECK(!alphaRejected(AlphaFunc::GreaterThan0, 1));
    CHECK(alphaRejected(AlphaFunc::Less128, 128));
    CHECK(!alphaRejected(AlphaFunc::Less128, 127));
    CHECK(!alphaRejected(AlphaFunc::None, 0));

    // Defaults: a state with no filter bits takes r_textureMode; bilinear is mip point, no anisotropy.
    CHECK(parseTextureMode("bilinear") == TextureMode::Bilinear);
    CHECK(parseTextureMode("4") == TextureMode::Anisotropic);
    CHECK(parseTextureMode("bogus") == TextureMode::Bilinear);
    SamplerDesc d = decodeSamplerState(0x00, TextureMode::Bilinear);
    CHECK(d.linear && !d.anisotropic && d.mip == MipFilter::Nearest && !d.clampU && !d.clampV);
    d = decodeSamplerState(0x00, TextureMode::Trilinear);
    CHECK(d.linear && !d.anisotropic && d.mip == MipFilter::Linear);
    d = decodeSamplerState(0x00, TextureMode::Anisotropic);
    CHECK(d.linear && d.anisotropic && d.mip == MipFilter::Linear);
    d = decodeSamplerState(0x00, TextureMode::Nearest);
    CHECK(!d.linear && !d.anisotropic && d.mip == MipFilter::None);

    // The clamp bits do not count as filter bits: egypt_rug1 (0x30) still follows r_textureMode.
    d = decodeSamplerState(0x30, TextureMode::Bilinear);
    CHECK(d.clampU && d.clampV && !d.clampW && d.linear && d.mip == MipFilter::Nearest);

    // A material's own filter bits win over r_textureMode.
    d = decodeSamplerState(0x02, TextureMode::Anisotropic); // font atlas: level 0 only
    CHECK(d.linear && !d.anisotropic && d.mip == MipFilter::None);
    d = decodeSamplerState(0x37, TextureMode::Bilinear); // anisotropic, point mip, clamped
    CHECK(d.linear && d.anisotropic && d.mip == MipFilter::Nearest && d.clampU && d.clampV);
    d = decodeSamplerState(0x31, TextureMode::Bilinear); // floatZ code sampler
    CHECK(!d.linear && !d.anisotropic && d.mip == MipFilter::None && d.clampU && d.clampV);

    CHECK(clampAnisotropy(1) == kMinAnisotropy);
    CHECK(clampAnisotropy(99) == kMaxAnisotropy);
    CHECK(clampAnisotropy(kDefaultAnisotropy) == 8);

    CHECK(parseAaSamples("") == 4); // not set: the openiw2 default
    CHECK(parseAaSamples("1") == 1);
    CHECK(parseAaSamples("2") == 2);
    CHECK(parseAaSamples("4") == 4);
    CHECK(parseAaSamples("3") == 2);
    CHECK(parseAaSamples("16") == 8);
    CHECK(alphaShaderParam(AlphaFunc::GreaterEqual128, false, false) == 1.0f);
    CHECK(alphaShaderParam(AlphaFunc::GreaterEqual128, false, true) == 9.0f);
    CHECK(alphaShaderParam(AlphaFunc::Less128, false, true) == 11.0f);
    CHECK(alphaShaderParam(AlphaFunc::None, true, false) == -1.0f);
    CHECK(alphaShaderParam(AlphaFunc::None, true, true) == 7.0f);

    if (failures) return 1;
    std::printf("materialstate tests passed\n");
    return 0;
}
