// Server settings of the create-server screen (docs/spec/mp-ui.md "Server cvars"): an empty scr_killcam / scr_drawfriend /
// scr_spectate* is set by the gametype to its default, and that value comes back to the menu cvars.
// Team assignment in sd with bots (docs/spec/mp-bots.md "Teams"): bots join through the gametype's autoassign, which
// puts each newcomer on the smaller team (sd.gsc:2086-2125), so five bots split 3 / 2. A player who then picks the
// bigger team by hand is let in while that team is below half of sv_maxclients (_teams.gsc:318-335), giving 4 / 2;
// only scr_teambalance 1 evens the teams, at a later round restart (_teams.gsc:43-62). Needs the game files (IW2_MAIN).

#include "fs/vfs.h"
#include "game/world.h"
#include "ui/mpserver.h"

#include <map>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::World;
namespace script = iw2::script;

struct Count {
    int allies = 0, axis = 0;
};

Count teams(World& w) {
    Count n;
    for (int c = 0; c < 6; ++c) {
        auto* e = w.mpClientEntity(c);
        if (!e) continue;
        const script::Value p = e->obj->get("pers");
        if (!p.isArray()) continue;
        const script::Value* v = std::get<script::ArrayPtr>(p.v)->get(script::ArrayKey{true, 0, "team"});
        const std::string t = v && v->isDefined() ? v->toString() : std::string();
        n.allies += t == "allies";
        n.axis += t == "axis";
    }
    return n;
}

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

// Five bots join, then the local player picks the team that already has three.
Count scenario(iw2::fs::Vfs& vfs, const char* teambalance, int minutesAfter, Count* bots, Count* joined) {
    World w(vfs, "mp_toujane", true);
    w.cvars["scr_teambalance"] = teambalance;
    w.mpStart("sd");
    const int c0 = w.mpConnect("player");
    w.mpBegin(c0);
    w.frame();
    for (int i = 0; i < 5; ++i) {
        w.mpAddBot();
        w.frame();
    }
    run(w, 20 * 20);
    *bots = teams(w);
    w.mpAutoJoin(c0, "", bots->allies > bots->axis ? "allies" : "axis");
    *joined = teams(w);
    Count end = *joined;
    for (int m = 0; m < minutesAfter; ++m) {
        run(w, 20 * 60);
        end = teams(w);
        if (end.allies == end.axis) break;
    }
    CHECK(w.vm().errors().empty());
    return end;
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set; skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    {
        std::map<std::string, std::string> ui{{"scr_teambalance", "1"}, {"scr_friendlyfire", "2"}};
        World w(vfs, "mp_toujane", true);
        for (const auto& [k, v] : ui) w.cvars[k] = v; // iw2view hands the menu cvars to the world
        w.mpStart("sd");
        run(w, 10);
        CHECK(w.cvars["scr_killcam"] == "1" && w.cvars["scr_drawfriend"] == "1" && w.cvars["scr_spectatefree"] == "1");
        CHECK(w.cvars["scr_teambalance"] == "1" && w.cvars["scr_friendlyfire"] == "2"); // the menu's values reach the game
        iw2::ui::carryWorldCvars(w.cvars, ui);
        CHECK(ui["scr_killcam"] == "1" && ui["scr_spectateenemy"] == "1" && ui["scr_friendlyfire"] == "2");
    }
    Count bots, joined;
    // scr_teambalance 0 (the gametype's default): the 4 / 2 split stays.
    Count end = scenario(vfs, "0", 2, &bots, &joined);
    std::cout << "teambalance 0: bots " << bots.allies << "/" << bots.axis << ", player joined " << joined.allies << "/" << joined.axis
              << ", end " << end.allies << "/" << end.axis << "\n";
    CHECK(bots.allies + bots.axis == 5 && std::abs(bots.allies - bots.axis) == 1);
    CHECK(std::abs(joined.allies - joined.axis) == 2);
    CHECK(std::abs(end.allies - end.axis) == 2);
    // scr_teambalance 1: evened at a later round restart.
    end = scenario(vfs, "1", 10, &bots, &joined);
    std::cout << "teambalance 1: bots " << bots.allies << "/" << bots.axis << ", player joined " << joined.allies << "/" << joined.axis
              << ", end " << end.allies << "/" << end.axis << "\n";
    CHECK(std::abs(joined.allies - joined.axis) == 2);
    CHECK(end.allies == 3 && end.axis == 3);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
