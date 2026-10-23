// Tests for the animtree parser: block comments and the sync flags of synced subtrees (0x479260 / 0x479120).

#include "game/animtree.h"

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
    const iw2::game::AnimTree t("test", R"(
body
{
    loops : loopsync
    {
        walk
        run
        {
            run_f
            run_b
        }
    }
    starts : nonloopsync
    {
        start_a
    }
/*  hidden : complete loopsync
    {
        hidden_leaf
    }
*/
    plain
    {
        idle // : loopsync
    }
}
)");
    auto flags = [&](const char* n) { const int i = t.find(n); return i < 0 ? -1 : t.nodes()[size_t(i)].syncFlags; };
    CHECK(flags("loops") == 5);
    CHECK(flags("run") == 1);
    CHECK(flags("run_f") == 0); // leaves carry an anim, not flags
    CHECK(flags("starts") == 6);
    CHECK(flags("plain") == 0);
    CHECK(t.find("hidden") < 0);
    CHECK(t.find("hidden_leaf") < 0);
    CHECK(t.nodes()[size_t(t.find("idle"))].parent == t.find("plain"));
    if (failures) return 1;
    std::cout << "animtree tests passed\n";
    return 0;
}
