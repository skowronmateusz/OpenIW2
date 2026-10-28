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
    return bad;
}
