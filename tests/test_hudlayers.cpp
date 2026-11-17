// Scope / binocular overlay draw order (VA 0x4bfa70 2D pass: crosshair and overlay before hud elements, menus and subtitles).
#include "game/hudlayers.h"

#include <iostream>
#include <vector>

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
    // list: 0 = something drawn before the hud elements, 10/11 = hud element quads, 20 = subtitle; overlay 1, 2 goes after "0".
    std::vector<int> list{0, 10, 11, 20};
    iw2::game::insertBelow(list, 1, std::vector<int>{1, 2});
    CHECK((list == std::vector<int>{0, 1, 2, 10, 11, 20}));
    std::vector<int> e;
    iw2::game::insertBelow(e, 5, std::vector<int>{7}); // position past the end is clamped
    CHECK((e == std::vector<int>{7}));
    iw2::game::insertBelow(list, 0, std::vector<int>{});
    CHECK(list.size() == 6);
    // MP intermission (0x4cbca0 -> 0x4cb810): the cgame part becomes the scoreboard alone; foreground HUD elements and the scope
    // overlay go (the caller adds the UI menus after it, 0x414a37 -> 0x532540).
    std::vector<int> cgame{0, 10, 11}, fg{12}, scope{1}, board{30, 31};
    iw2::game::intermission2D(cgame, fg, scope, board);
    CHECK((cgame == std::vector<int>{30, 31}));
    CHECK(fg.empty() && scope.empty());
    return failures ? 1 : 0;
}
