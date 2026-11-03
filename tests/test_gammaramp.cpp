// The r_gamma ramp (src/game/gammaramp.h, gfx 0x10011fd0).
#include "game/gammaramp.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace iw2::game;

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    const auto id = gammaRamp(1.0f);
    for (int i = 0; i < 256; ++i) chk(id[size_t(i)] == i * 257, __LINE__); // r_gamma 1 is the identity ramp
    chk(id[255] == 65535 && id[0] == 0, __LINE__);
    const auto g2 = gammaRamp(2.0f); // exponent 0.5: mid grey 128 -> 65535 * sqrt(128/255) = 46431
    chk(g2[0] == 0 && g2[255] == 65535, __LINE__);
    chk(std::abs(g2[128] - 46431) <= 1, __LINE__);
    chk(std::abs(g2[64] - 32832) <= 1, __LINE__); // 65535 * sqrt(64/255)
    const auto g05 = gammaRamp(0.5f); // exponent 2: darker; 128 -> 65535 * (128/255)^2 = 16513
    chk(std::abs(g05[128] - 16513) <= 1, __LINE__);
    for (int i = 1; i < 256; ++i) chk(g2[size_t(i)] >= g2[size_t(i - 1)] && g05[size_t(i)] >= g05[size_t(i - 1)], __LINE__); // monotonic
    chk(gammaRamp(3.0f)[128] > g2[128], __LINE__); // larger r_gamma is brighter
    chk(g05[128] < id[128], __LINE__);
    chk(clampGamma(0.1f) == 0.5f && clampGamma(9.0f) == 3.0f && clampGamma(1.3f) == 1.3f, __LINE__);
    chk(std::fabs(gammaApply(0.5f, 2.0f) - 46431.0f / 65535.0f) < 1e-4f, __LINE__); // 0.5 -> index 128
    chk(gammaApply(1.0f, 2.2f) == 1.0f && gammaApply(0.0f, 2.2f) == 0.0f, __LINE__);
    return bad ? 1 : 0;
}
