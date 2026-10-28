// The level-start node drop (0x515f80 / 0x515d70, docs/spec/ai.md "Node drop"). Needs the game files: with IW2_MAIN set to
// the game's main directory, moscow's node auto8467 (map origin -2000 -1023 76, the commissar's block idle after
// "go grab a gun") must sit on the trench floor (z 28.2), and the path graph must use the dropped positions.

#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
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
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World w(vfs, "moscow");
    w.prepareStart();
    bool found = false;
    const auto& nodes = w.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto o = nodes[i].obj->get("origin").asVector();
        CHECK(w.paths().position(i) == o);
        const auto tn = nodes[i].obj->get("targetname");
        if (tn.isString() && tn.asString() == "auto8467") {
            found = true;
            CHECK(o[0] == -2000.0f && o[1] == -1023.0f);
            CHECK(o[2] > 27.5f && o[2] < 29.0f); // the floor under the node (the original origin is z 76)
            CHECK(nodes[i].typeNum == 1);
        }
    }
    CHECK(found);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "nodedrop ok\n";
    return failures ? 1 : 0;
}
