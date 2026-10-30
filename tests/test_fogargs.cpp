// Argument rules of setCullFog / setExpFog (0x4f53f0, 0x4f5630) and setshadowhint (0x4ef700), src/game/fog.h.
#include "game/fog.h"
#include <cstdio>

using namespace iw2::game;

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    chk(fogArgError("setCullFog", 0, 100, 0, 0.5f, 1, 0).empty(), __LINE__);
    chk(fogArgError("setCullFog", -1, 100, 0, 0, 0, 0) == "setCullFog: near distance must be >= 0", __LINE__);
    chk(fogArgError("setCullFog", 5, 5, 0, 0, 0, 0) == "setCullFog: near distance must be less than far distance", __LINE__);
    chk(fogArgError("setCullFog", 0, 1, 0, 1.01f, 0, 0).find("color components") != std::string::npos, __LINE__);
    chk(fogArgError("setCullFog", 0, 1, -0.1f, 0, 0, 0).find("color components") != std::string::npos, __LINE__);
    chk(fogArgError("setExpFog", 0, 1, 0, 0, 0, -1) == "setExpFog: transition time must be >= 0 seconds", __LINE__);
    chk(fogDensityValid(0.5f) && !fogDensityValid(0) && !fogDensityValid(1), __LINE__);
    uint32_t b = 99;
    chk(shadowHintBits("normal", b) && b == 0, __LINE__);
    chk(shadowHintBits("never", b) && b == 0x2000, __LINE__);
    chk(shadowHintBits("low_priority", b) && b == 0x4000, __LINE__);
    chk(shadowHintBits("high_priority", b) && b == 0x6000, __LINE__);
    chk(shadowHintBits("always", b) && b == 0x8000, __LINE__);
    chk(shadowHintBits("receiver", b) && b == 0xa000, __LINE__);
    chk(!shadowHintBits("Never", b), __LINE__);
    {   // Exponential fog state (0x100244d0 lerps +0x10 / +0x14 / +0x18 of the fog record, +0x18 being the density): linear in time.
        FogState f;
        f.commandExp(0.00002f, 0.4f, 0.4f, 0.444f, 0, 0);
        FogConsts c = f.consts(0);
        chk(c.active && c.exponential && c.exponent == -0.00002f, __LINE__); // the first command applies at once, exponent = -density
        f.commandExp(0.00075f, 0.4f, 0.4f, 0.444f, 1000, 5000);
        chk(std::fabs(f.consts(5000).exponent + 0.00002f) < 1e-9f, __LINE__);
        chk(std::fabs(f.consts(5500).exponent + (0.00002f + 0.00075f) * 0.5f) < 1e-8f, __LINE__); // midpoint density
        chk(f.consts(6000).exponent == -0.00075f && f.consts(9000).exponent == -0.00075f, __LINE__);
        // exp fog distance factor exp(dist * exponent) (lib/fogcalc.hlsl): 50% at ln2 / density
        chk(std::fabs(std::exp(-0.00075f * (0.693147f / 0.00075f)) - 0.5f) < 1e-4f, __LINE__);
        FogState g; // linear then exponential: the type change takes the target at once
        g.command(500, 10000, 0.7f, 0.5f, 0.3f, 0, 0);
        g.commandExp(0.001f, 0.1f, 0.1f, 0.1f, 5000, 100);
        chk(g.consts(100).exponential && g.consts(100).exponent == -0.001f, __LINE__);
        FogState h; // a 1 s linear transition from an active fog: near / far / colour halfway at 500 ms
        h.command(0, 1000, 0, 0, 0, 0, 0);
        h.command(1000, 3000, 1, 1, 1, 1000, 10);
        const FogParams p = h.current(510);
        chk(std::fabs(p.nearDist - 500) < 1 && std::fabs(p.farDist - 2000) < 1 && std::abs(int(p.color[0]) - 128) <= 1, __LINE__);
    }
    return bad;
}
