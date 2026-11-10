// Offline multiplayer DM (docs/spec/mp.md): game init order, client connect / begin / menu responses, spawn on an
// mp_dm_spawn, damage through CodeCallback_PlayerDamage, death, score / deaths, respawn, score limit, disconnect.
// Needs the game files (IW2_MAIN).

#include "game/world.h"

#include <cmath>
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

using iw2::script::Value;
using iw2::game::Entity;
using iw2::game::World;

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
int ifield(Entity* e, const char* f) {
    const Value v = e ? e->obj->get(f) : Value();
    return v.isNumber() ? v.asInt() : -9999;
}

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

bool onDmSpawn(World& w, Entity* p) {
    const auto o = p->obj->get("origin").asVector();
    for (const auto& e : w.entities())
        if (e->classname == "mp_dm_spawn") {
            const auto s = e->obj->get("origin").asVector();
            if (std::fabs(s[0] - o[0]) < 0.5f && std::fabs(s[1] - o[1]) < 0.5f && std::fabs(s[2] - o[2]) < 0.5f) return true;
        }
    return false;
}

bool logHas(World& w, const std::string& prefix) {
    for (const auto& l : w.mpState().log)
        if (l.rfind(prefix, 0) == 0) return true;
    return false;
}

void printErrors(World& w) {
    for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);

    // A missing gametype script is a load error.
    {
        World w(vfs, "mp_toujane", true);
        bool threw = false;
        try {
            w.mpStart("nosuchgametype");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Time limit (scr_dm_timelimit in minutes): 0.05 = 3 s ends the map (dm.gsc:686-705).
    {
        World t(vfs, "mp_toujane", true);
        t.cvars["scr_dm_timelimit"] = "0.05";
        t.mpStart("dm");
        run(t, 40);
        CHECK(!t.vm().level()->get("mapended").truthy());
        run(t, 40);
        CHECK(t.vm().level()->get("mapended").truthy());
    }

    World w(vfs, "mp_toujane", true);
    w.cvars["scr_dm_scorelimit"] = "2";
    w.mpStart("dm");
    printErrors(w);
    if (std::getenv("MPDBG")) for (const auto& [k, v] : w.vm().level()->fields) std::cerr << "level." << k << " = " << v.toString() << "\n";
    // Init: mapname is the bare name, the gametype ran its start (level.gametypestarted, dm.gsc spawn placement),
    // the level script ran (game["allies"] set by maps/mp/mp_toujane.gsc to british).
    CHECK(w.cvars["mapname"] == "mp_toujane");
    CHECK(w.cvars["g_gametype"] == "dm");
    CHECK(w.vm().level()->get("gametypestarted").truthy());
    CHECK(w.vm().level()->get("scorelimit") == Value(int32_t(2)));
    CHECK(w.vm().game()->get("allies").toString() == "british");
    CHECK(w.vm().game()->get("menu_team").toString() == "team_britishgerman");

    // Connect: spectator until "begin"; begin -> the gametype joins it as spectator and opens the server info menu.
    const int c0 = w.mpConnect("local");
    CHECK(c0 == 0);
    Entity* p0 = w.mpClientEntity(c0);
    CHECK(p0 == w.player());
    CHECK(field(p0, "statusicon") == "hud_status_connecting");
    w.mpBegin(c0);
    run(w, 2);
    CHECK(field(p0, "sessionstate") == "spectator");
    CHECK(w.mpState().clients[0].openMenu == "serverinfo_dm");
    CHECK(logHas(w, "J;0;"));

    // Team menu -> weapon menu; weapon response -> spawned on an mp_dm_spawn, playing, with the weapon.
    w.mpMenuResponse(c0, "team_britishgerman", "autoassign");
    run(w, 1);
    const std::string wmenu = w.mpState().clients[0].openMenu;
    CHECK(wmenu == "weapon_british" || wmenu == "weapon_german");
    const std::string weapon = wmenu == "weapon_british" ? "thompson_mp" : "mp40_mp";
    w.mpMenuResponse(c0, wmenu, weapon);
    run(w, 2);
    CHECK(field(p0, "sessionstate") == "playing");
    CHECK(ifield(p0, "health") == 100);
    CHECK(onDmSpawn(w, p0));
    CHECK(field(p0, "$weapon") == weapon);

    // A second local client (the slot a bot or a network client would use).
    const int c1 = w.mpConnect("second");
    CHECK(c1 == 1);
    Entity* p1 = w.mpClientEntity(c1);
    CHECK(p1 && p1 != p0 && p1->classname == "player");
    w.mpBegin(c1);
    run(w, 1);
    w.mpMenuResponse(c1, "team_britishgerman", "axis");
    run(w, 1);
    CHECK(w.mpState().clients[1].openMenu == "weapon_german");
    w.mpMenuResponse(c1, "weapon_german", "mp40_mp");
    run(w, 2);
    CHECK(field(p1, "sessionstate") == "playing");
    CHECK(onDmSpawn(w, p1));

    // Damage goes through CodeCallback_PlayerDamage -> finishPlayerDamage; a spectator takes none.
    w.mpDebugDamage(c1, c0, 30, "MOD_RIFLE_BULLET");
    run(w, 1);
    CHECK(ifield(p1, "health") < 100 && ifield(p1, "health") > 0);
    CHECK(logHas(w, "D;0;" + std::to_string(p1->number) + ";"));

    // Hits in quick succession: there is no damage debounce, every hit takes its full amount, one per frame and two in
    // the same frame, for the second client and for the local player (docs/spec/mp.md "No damage debounce").
    for (Entity* t : {p1, p0}) {
        const int victim = t == p1 ? c1 : c0, from = t == p1 ? c0 : c1;
        int h = ifield(t, "health");
        for (int i = 0; i < 3; ++i) {
            w.mpDebugDamage(victim, from, 10, "MOD_RIFLE_BULLET");
            run(w, 1);
            CHECK(ifield(t, "health") == h - 10);
            h -= 10;
        }
        w.mpDebugDamage(victim, from, 5, "MOD_RIFLE_BULLET");
        w.mpDebugDamage(victim, from, 5, "MOD_RIFLE_BULLET");
        run(w, 1);
        CHECK(ifield(t, "health") == h - 10);
    }
    // A health set from outside the game (not through damage or regeneration) is not seen by the regeneration loop of
    // _healthoverlay.gsc: it remembers the last health it saw, so a later hit down to that same value looks like no
    // change and, once the 5 s regeneration delay has passed since the last drop it saw, the next tick restores it.
    // Tests must therefore not reset health by hand between hits (the antilag test lets regeneration restore it).
    run(w, 120);
    CHECK(ifield(p1, "health") == 100);  // regenerated

    // Kill: attacker score +1, victim deaths +1, sessionstate dead; the killcam, then the use button skips it and respawns.
    w.mpDebugDamage(c1, c0, 500, "MOD_RIFLE_BULLET");
    run(w, 1);
    CHECK(field(p1, "sessionstate") == "dead");
    CHECK(ifield(p0, "score") == 1);
    CHECK(ifield(p1, "deaths") == 1);
    CHECK(ifield(p1, "score") == 0);
    CHECK(logHas(w, "K;0;" + std::to_string(p1->number) + ";"));
    run(w, 60); // the 2 s death delay, then the killcam of the killer (docs/spec/mp-hud.md "Killcam")
    CHECK(field(p1, "sessionstate") == "spectator");
    CHECK(p1->obj->get("spectatorclient") == Value(int32_t(p0->number)));
    w.mpSetButtons(c1, 0x8);
    run(w, 3);
    w.mpSetButtons(c1, 0);
    CHECK(field(p1, "sessionstate") == "playing");
    CHECK(ifield(p1, "health") == 100);

    // Suicide (a team change while playing): score -1 for the player.
    w.mpMenuResponse(c1, "team_britishgerman", "allies");
    run(w, 1);
    CHECK(ifield(p1, "score") == 0); // switching_teams: no score or death change
    CHECK(ifield(p1, "deaths") == 1);
    CHECK(field(p1, "sessionstate") == "dead");

    // Second kill reaches scr_dm_scorelimit 2: the map ends (intermission for everyone, then exitLevel after 10 s).
    w.mpMenuResponse(c1, "weapon_british", "thompson_mp");
    run(w, 2);
    CHECK(field(p1, "sessionstate") == "playing");
    w.mpDebugDamage(c1, c0, 500, "MOD_RIFLE_BULLET");
    run(w, 2);
    CHECK(ifield(p0, "score") == 2);
    CHECK(w.vm().level()->get("mapended").truthy());
    CHECK(field(p0, "sessionstate") == "intermission");
    CHECK(logHas(w, "W;;0;local"));
    run(w, 210);
    CHECK(w.mpState().exitRequested);

    // sessionstate / sessionteam are validated client fields; other objects are not checked.
    {
        bool threw = false;
        try {
            w.vm().fieldStoreHook(p0->obj, "sessionstate", Value("bogus"));
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            w.vm().fieldStoreHook(p0->obj, "sessionteam", Value("none"));
            w.vm().fieldStoreHook(w.vm().level(), "sessionstate", Value("bogus"));
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(!threw);
    }

    // Disconnect: the gametype logs the quit; the slot is free again.
    w.mpDisconnect(c1);
    CHECK(logHas(w, "Q;0;"));
    CHECK(w.mpState().clients[1].state == iw2::game::MpClientState::Free);
    CHECK(p1->obj->removed);

    if (!w.vm().errors().empty()) {
        printErrors(w);
        CHECK(w.vm().errors().empty());
    }
    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
