// Renderer draw-list sort key (DLL 0x100247d0).
#include "game/spritesortkey.h"

#include <iostream>

int main() {
    using namespace iw2::game;
    int failures = 0;
    auto check = [&](bool c, int line) {
        if (!c) {
            std::cerr << "line " << line << " failed\n";
            ++failures;
        }
    };
#define CHECK(c) check((c), __LINE__)
    // Material-major: material dominates, then layer, then entity.
    CHECK(drawSortKey(3, 0, 0, 4, false) == ((3u << 21) | 4u));
    CHECK(drawSortKey(1, 31, 7, 2, false) == ((1u << 21) | (31u << 16) | (7u << 4) | 2u));
    CHECK(drawSortKey(1, 0, 4095, 4, false) < drawSortKey(2, 0, 0, 4, false));
    CHECK(drawSortKey(1, 5, 4095, 4, false) < drawSortKey(1, 6, 0, 4, false));
    // fx_sort applies to type 2 only.
    CHECK(drawSortKey(5, 3, 9, 4, true) == drawSortKey(5, 3, 9, 4, false));
    // Entity-major for sprites: effect entities (>= 0x1000) sort first by entity, then material; low entities wrap and sort last.
    CHECK(drawSortKey(900, 31, 0x1000, 2, true) < drawSortKey(0, 0, 0x1001, 2, true));
    CHECK(drawSortKey(1, 0, 0x1000, 2, true) < drawSortKey(2, 0, 0x1000, 2, true));
    CHECK(drawSortKey(0, 0, 0x1fff, 2, true) < drawSortKey(0, 0, 5, 2, true));
    CHECK((drawSortKey(0, 0, 5, 2, true) & 0x80000000u) != 0);
    // Round trip through the walker's decoding.
    const auto a = drawKeyFields(drawSortKey(321, 17, 1234, 2, false));
    CHECK(a.material == 321 && a.layer == 17 && a.entity == 1234);
    const auto b = drawKeyFields(drawSortKey(321, 17, 5, 2, true));
    CHECK(b.material == 321 && b.layer == 17 && b.entity == ((5u - 0x1000u) & 0xfff));
    return failures ? 1 : 0;
}
