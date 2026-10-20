// Tests for the player weapon state machine with hand-written weapon definitions.

#include "game/playerweapon.h"

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

using iw2::game::PlayerWeapon;

int run(PlayerWeapon& w, float seconds, bool fire, bool reload = false) {
    int shots = 0;
    for (int i = 0; i < int(seconds * 100 + 0.5f); ++i) shots += w.update(0.01f, {fire, reload && i == 0});
    return shots;
}

void boltAction() {
    PlayerWeapon w;
    w.equip("rifle", {{"semiauto", "1"}, {"boltaction", "1"}, {"firetime", "0.3"}, {"rechambertime", "1"},
                      {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0.5"}, {"reloadtime", "2"},
                      {"idleanim", "idle"}, {"fireanim", "fire"}, {"rechamberanim", "rechamber"}});
    CHECK(w.clip() == 5 && w.stock() == 15);
    CHECK(run(w, 0.4f, true) == 0);        // still raising
    CHECK(run(w, 0.2f, true) == 1);        // first shot once ready
    CHECK(w.anim() == "fire");
    CHECK(run(w, 0.5f, true) == 0);        // trigger held: semi-auto waits for release
    CHECK(w.anim() == "rechamber");
    CHECK(run(w, 0.9f, false) == 0);       // rechambering
    CHECK(run(w, 0.1f, true) == 1);
    CHECK(w.clip() == 3);
}

void automatic() {
    PlayerWeapon w;
    w.equip("smg", {{"semiauto", "0"}, {"firetime", "0.1"}, {"clipsize", "20"}, {"startammo", "40"},
                    {"raisetime", "0"}, {"reloadtime", "2"}, {"reloademptytime", "2.5"}});
    CHECK(run(w, 1.005f, true) == 10);
    CHECK(w.clip() == 10);
    CHECK(run(w, 1.5f, true) == 10);      // empties the clip, then starts the empty reload
    CHECK(w.clip() == 0);
    CHECK(run(w, 2.6f, false) == 0);
    CHECK(w.clip() == 20 && w.stock() == 0);
    run(w, 0.3f, true);
    CHECK(run(w, 0.1f, false, true) == 0); // manual reload with no stock does nothing
    CHECK(w.stock() == 0);
}

} // namespace

int main() {
    boltAction();
    automatic();
    if (failures) return 1;
    std::cout << "playerweapon tests passed\n";
    return 0;
}
