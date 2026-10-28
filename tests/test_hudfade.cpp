// HUD fade helpers (exe 0x4b0900, 0x4a8790).
#include "game/hudfade.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    using namespace iw2::game;
    CHECK(hudFadeAlpha(0, 500, 100, 100) < 0.0f);
    CHECK(hudFadeAlpha(1000, 1000, 100, 100) == 1.0f);
    CHECK(hudFadeAlpha(1000, 1050, 100, 100) == 0.5f);
    CHECK(hudFadeAlpha(1000, 1100, 100, 100) < 0.0f);
    CHECK(hudFadeAlpha(1000, 1500, 2000, 500) == 1.0f);
    CHECK(hudFadeAlpha(1000, 2750, 2000, 500) == 0.5f);
    CHECK(hudHealthFraction(0, 100) == 0.0f);
    CHECK(hudHealthFraction(50, 0) == 0.0f);
    CHECK(hudHealthFraction(50, 100) == 0.5f);
    CHECK(hudHealthFraction(150, 100) == 1.0f);
    return failures ? 1 : 0;
}
