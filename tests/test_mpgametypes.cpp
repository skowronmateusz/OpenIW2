// Multiplayer team gametypes (docs/spec/mp.md "Team gametypes", "Fast restart"): team scores, the team builtins'
// errors, placeSpawnPoint, tdm kills and the score limit, sd rounds through map_restart (game array and pers kept),
// ctf / hq starts. With the argument "matrix": every stock map in tdm, sd, ctf and hq with bots for a few seconds and no
// script errors. Needs the game files (IW2_MAIN); the bots' graphs are cached in IW2_NAV_CACHE.

#include "game/mpbots.h"
#include "game/mpgame.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::Entity;
using iw2::game::World;
using iw2::script::Value;

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
int ifield(Entity* e, const char* f) {
    const Value v = e ? e->obj->get(f) : Value();
    return v.isNumber() ? v.asInt() : -9999;
}
std::string pers(Entity* e, const char* key) {
    const Value p = e ? e->obj->get("pers") : Value();
    if (!p.isArray()) return "<none>";
    for (const auto& [k, v] : std::get<iw2::script::ArrayPtr>(p.v)->items)
        if (k.isString && k.s == key) return v.toString();
    return "<undefined>";
}
void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}
void printErrors(World& w) {
    for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
}
bool throws(World& w, const std::string& file, const std::string& fn) {
    const size_t before = w.vm().errors().size();
    w.vm().start(file, fn, {}, Value(w.vm().level()));
    w.vm().drainReady();
    return w.vm().errors().size() > before;
}

// Joins `client` to `team` through the gametype's menus with the first weapon of its weapon menu.
void join(World& w, int client, const std::string& team) {
    w.mpMenuResponse(client, w.vm().game()->get("menu_team").toString(), team);
    w.frame();
    if (std::getenv("MPDBG")) std::cerr << "join " << client << " menu " << w.mpState().clients[size_t(client)].openMenu << "\n";
    const std::string menu = w.mpState().clients[size_t(client)].openMenu;
    const std::string weapon = menu == "weapon_german" ? "mp40_mp" : menu == "weapon_russian" ? "ppsh_mp" : "thompson_mp";
    w.mpMenuResponse(client, menu, weapon);
    w.frame();
}


bool logHas(World& w, const std::string& part) {
    for (const auto& l : w.mpState().log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

// A game of 6 bots (and the idle local player) on mp_toujane for up to `seconds`, until `done` holds.
template <class Done>
bool botGame(iw2::fs::Vfs& vfs, const char* gt, float seconds, Done done, const char* what) {
    World w(vfs, "mp_toujane", true);
    w.vm().setLoopGuardIterations(200000u);
    w.mpSetBotSkill(2);
    w.mpStart(gt);
    w.mpConnect("player");
    w.mpBegin(0);
    w.frame();
    w.mpAutoJoin(0);
    for (int b = 0; b < 6; ++b) {
        w.mpAddBot();
        w.frame();
    }
    bool ok = false;
    for (int f = 0; f < int(seconds * 20) && !ok; ++f) {
        w.frame();
        if (f % 20 == 0) ok = done(w);
    }
    if (!ok) std::cerr << gt << ": not reached: " << what << "\n";
    if (std::getenv("MPDBG")) std::cerr << gt << ": " << what << " after " << w.timeMs() / 1000 << " s\n";
    if (!w.vm().errors().empty()) printErrors(w);
    return ok && w.vm().errors().empty();
}

int matrix(iw2::fs::Vfs& vfs) {
    const char* maps[] = {"mp_farmhouse", "mp_brecourt", "mp_burgundy", "mp_trainstation", "mp_carentan",
                          "mp_decoy",     "mp_leningrad", "mp_matmata", "mp_downtown",     "mp_harbor",
                          "mp_dawnville", "mp_railyard",  "mp_toujane", "mp_breakout",     "mp_rhine"};
    for (const char* m : maps)
        for (const char* gt : {"tdm", "sd", "ctf", "hq"}) {
            World w(vfs, m, true);
            w.vm().setLoopGuardIterations(200000u);
            w.mpStart(gt);
            w.mpConnect("player");
            w.mpBegin(0);
            w.frame();
            w.mpAutoJoin(0);
            for (int b = 0; b < 4; ++b) {
                w.mpAddBot();
                w.frame();
            }
            run(w, 160);
            int playing = 0;
            for (const auto& cl : w.mpState().clients)
                if (cl.ent && field(cl.ent, "sessionstate") == "playing") ++playing;
            const bool ok = w.vm().errors().empty() && playing >= 2;
            if (!ok) {
                std::cerr << m << " " << gt << ": " << playing << " playing\n";
                printErrors(w);
            }
            CHECK(ok);
        }
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    if (argc > 1 && std::string(argv[1]) == "matrix") return matrix(vfs);
    // A small script for the builtin checks (written to a per-process temporary directory).
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("iw2test_mpgametypes_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "maps/mp");
    std::ofstream(dir / "maps/mp/iw2test_gametypes.gsc")
        << "badteam() { getTeamScore(\"spectator\"); }\n"
           "badobjective() { objective_team(16, \"axis\"); }\n"
           "badtrigger() { level setTeamForTrigger(\"axis\"); }\n"
           "setaxis7() { setTeamScore(\"axis\", 7); }\n"
           "telefrag() { level.iw2test_telefrag = positionWouldTelefrag((0, 0, -100000)); }\n"
           "restarttwice() { map_restart(true); map_restart(true); }\n"
           "botgoal() { level.iw2test_isbot = level.iw2test_bot isBot(); level.iw2test_bot botSetRole(\"hunt\"); "
           "level.iw2test_bot botSetGoal(level.iw2test_goal, 64); }\n"
           "badrole() { level.iw2test_bot botSetRole(\"camp\"); }\n"
           "botclear() { level.iw2test_bot botClearGoal(); }\n";
    vfs.mountDirectory(dir);

    // tdm: team scores, kills between teams, placeSpawnPoint, the score limit.
    {
        World w(vfs, "mp_toujane", true);
        w.cvars["scr_tdm_scorelimit"] = "2";
        w.mpStart("tdm");
        CHECK(w.mpTeamScore("allies") == 0 && w.mpTeamScore("axis") == 0);
        // Spawn points stand on the floor: the standing box at the origin is free, 1 unit lower it is not.
        int spawns = 0, onFloor = 0;
        for (const auto& e : w.entities())
            if (e->classname == "mp_tdm_spawn") {
                ++spawns;
                const auto o = e->obj->get("origin").asVector();
                const auto in = w.collision().trace(o, o, {-15, -15, 0}, {15, 15, 70}, 0x2810011u);
                const auto below = w.collision().trace(o, {o[0], o[1], o[2] - 1.0f}, {-15, -15, 0}, {15, 15, 70}, 0x2810011u);
                if (!in.startSolid && below.fraction < 0.5f) ++onFloor;
            }
        CHECK(spawns > 0 && onFloor == spawns);

        w.mpConnect("a");
        w.mpBegin(0);
        w.frame();
        w.mpConnect("b");
        w.mpBegin(1);
        w.frame();
        join(w, 0, "allies");
        join(w, 1, "axis");
        Entity* a = w.mpClientEntity(0);
        Entity* b = w.mpClientEntity(1);
        CHECK(field(a, "sessionteam") == "allies" && field(b, "sessionteam") == "axis");
        CHECK(field(a, "sessionstate") == "playing" && field(b, "sessionstate") == "playing");
        w.mpDebugDamage(1, 0, 500, "MOD_RIFLE_BULLET");
        run(w, 2);
        CHECK(w.mpTeamScore("allies") == 1);
        CHECK(ifield(a, "score") == 1);
        CHECK(!w.vm().level()->get("mapended").truthy());
        run(w, 100); // respawn on use
        w.mpSetButtons(1, 0x8);
        run(w, 3);
        w.mpSetButtons(1, 0);
        CHECK(field(b, "sessionstate") == "playing");
        w.mpDebugDamage(1, 0, 500, "MOD_RIFLE_BULLET");
        run(w, 3);
        CHECK(w.mpTeamScore("allies") == 2);
        CHECK(w.vm().level()->get("mapended").truthy()); // scr_tdm_scorelimit 2 reached
        if (!w.vm().errors().empty()) printErrors(w);
        CHECK(w.vm().errors().empty());
    }

    // The builtins' argument checks are script errors; map_restart is one-shot and runs at the end of the frame.
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("tdm");
        const std::string f = "maps/mp/iw2test_gametypes.gsc";
        auto lastError = [&]() { return w.vm().errors().empty() ? std::string() : w.vm().errors().back(); };
        CHECK(throws(w, f, "badteam") && lastError().find("Illegal team string 'spectator'") != std::string::npos);
        CHECK(throws(w, f, "badobjective") && lastError().find("illegal objective index") != std::string::npos);
        CHECK(throws(w, f, "badtrigger") && lastError().find("setteamfortrigger") != std::string::npos);
        CHECK(!throws(w, f, "setaxis7"));
        CHECK(w.mpTeamScore("axis") == 7);
        CHECK(!throws(w, f, "telefrag"));
        CHECK(w.vm().level()->get("iw2test_telefrag") == Value(int32_t(0)));
        CHECK(throws(w, f, "restarttwice") && lastError().find("map_restart already called") != std::string::npos);
        CHECK(w.mpRestarts() == 0);
        w.frame();
        CHECK(w.mpRestarts() == 1);
        CHECK(w.mpTeamScore("axis") == 0); // game init clears the team scores (tdm does not set them again)
    }

    // sd: the first round is a "reset" round once both teams have a player; its end restarts the map with persist on.
    // The clients come back with their pers (team, weapon), the game array keeps matchstarted and the round counter.
    {
        World w(vfs, "mp_toujane", true);
        w.cvars["scr_sd_roundlength"] = "1";
        w.mpStart("sd");
        w.mpConnect("a");
        w.mpBegin(0);
        w.frame();
        w.mpConnect("b");
        w.mpBegin(1);
        w.frame();
        join(w, 0, "allies");
        join(w, 1, "axis");
        CHECK(field(w.mpClientEntity(1), "sessionstate") == "playing");
        for (int i = 0; i < 400 && w.mpRestarts() == 0; ++i) w.frame();
        CHECK(w.mpRestarts() == 1);
        CHECK(w.vm().game()->get("matchstarted").truthy());
        CHECK(w.vm().game()->get("roundsplayed") == Value(int32_t(0)));
        run(w, 5);
        Entity* a = w.mpClientEntity(0);
        Entity* b = w.mpClientEntity(1);
        CHECK(a == w.player());
        CHECK(pers(a, "team") == "allies" && pers(b, "team") == "axis");
        CHECK(field(a, "sessionstate") == "playing" && field(b, "sessionstate") == "playing");
        CHECK(w.mpState().clients[0].state == iw2::game::MpClientState::Active);
        // A kill of the last defender ends the round: the attackers' team score, roundsplayed 1, another restart.
        const std::string attackers = w.vm().game()->get("attackers").toString();
        const int killer = pers(a, "team") == attackers ? 0 : 1;
        w.mpDebugDamage(1 - killer, killer, 500, "MOD_RIFLE_BULLET");
        for (int i = 0; i < 400 && w.mpRestarts() == 1; ++i) w.frame();
        CHECK(w.mpRestarts() == 2);
        // The restart reuses the script function names: the same stubs as a world that parsed all scripts.
        CHECK(w.stubbedBuiltins() == World(vfs, "mp_toujane", true).stubbedBuiltins());
        CHECK(w.mpTeamScore(attackers) == 1);
        CHECK(w.vm().game()->get("roundsplayed") == Value(int32_t(1)));
        if (!w.vm().errors().empty()) printErrors(w);
        CHECK(w.vm().errors().empty());
    }

    // ctf and hq start and seat two players of different teams without script errors.
    for (const char* gt : {"ctf", "hq"}) {
        World w(vfs, "mp_toujane", true);
        w.mpStart(gt);
        w.mpConnect("a");
        w.mpBegin(0);
        w.frame();
        w.mpConnect("b");
        w.mpBegin(1);
        w.frame();
        join(w, 0, "allies");
        join(w, 1, "axis");
        run(w, 100);
        CHECK(field(w.mpClientEntity(0), "sessionstate") == "playing");
        CHECK(field(w.mpClientEntity(1), "sessionstate") == "playing");
        if (!w.vm().errors().empty()) printErrors(w);
        CHECK(w.vm().errors().empty());
    }


    // Weapon classes from the weapon files (docs/spec/mp-bots.md, "Weapon classes").
    {
        World w(vfs, "mp_toujane", true);
        const std::pair<const char*, const char*> expect[] = {
            {"thompson_mp", "smg"},       {"greasegun_mp", "smg"},    {"sten_mp", "smg"},        {"mp40_mp", "smg"},
            {"ppsh_mp", "smg"},           {"PPS42_mp", "smg"},        {"m1garand_mp", "rifle"},  {"m1carbine_mp", "rifle"},
            {"enfield_mp", "bolt"},       {"kar98k_mp", "bolt"},      {"g43_mp", "rifle"},       {"mosin_nagant_mp", "bolt"},
            {"SVT40_mp", "rifle"},        {"bar_mp", "mg"},           {"bren_mp", "mg"},         {"mp44_mp", "mg"},
            {"springfield_mp", "sniper"}, {"enfield_scope_mp", "sniper"}, {"kar98k_sniper_mp", "sniper"},
            {"mosin_nagant_sniper_mp", "sniper"}, {"shotgun_mp", "shotgun"}};
        for (const auto& [wpn, cls] : expect) {
            const std::string got = w.mpWeaponClass(wpn);
            if (got != cls) std::cerr << wpn << ": " << got << " (expected " << cls << ")\n";
            CHECK(got == cls);
            CHECK(!iw2::game::mpWeaponAllowCvar(wpn).empty());
            CHECK(iw2::game::mpMenuWeaponClass(wpn) == cls); // the table the --weapons validation uses agrees with the weapon file
        }
        w.cvars["iw2_weaponclasses"] = "sniper, mg";
        w.mpStart("tdm");
        CHECK(w.cvars["scr_allow_kar98ksniper"] == "1" && w.cvars["scr_allow_bren"] == "1");
        CHECK(w.cvars["scr_allow_mp40"] == "0" && w.cvars["scr_allow_shotgun"] == "0" && w.cvars["scr_allow_kar98k"] == "0");
        const auto german = w.mpAllowedWeapons("weapon_german");
        CHECK(german.size() == 2); // kar98k_sniper_mp, mp44_mp
        // Bots answer the weapon menu with an allowed weapon only; bot_weaponclass narrows it to a class.
        w.cvars["bot_weaponclass"] = "sniper";
        for (int b = 0; b < 6; ++b) {
            w.mpAddBot();
            w.frame();
        }
        run(w, 40);
        int bots = 0;
        for (size_t c = 0; c < w.mpState().clients.size(); ++c) {
            if (!w.mpIsBot(int(c))) continue;
            const std::string wpn = pers(w.mpClientEntity(int(c)), "weapon");
            ++bots;
            if (w.mpWeaponClass(wpn) != "sniper") std::cerr << "bot " << c << " weapon " << wpn << "\n";
            CHECK(w.mpWeaponClass(wpn) == "sniper");
            CHECK(field(w.mpClientEntity(int(c)), "sessionstate") == "playing");
        }
        CHECK(bots == 6);
        CHECK(w.vm().errors().empty());
    }

    // Bots play each gametype's objective (docs/spec/mp-bots.md, "Objectives"): tdm and hq team scores, a ctf capture,
    // an sd bomb plant and rounds; the bot goal API from GSC.
    CHECK(botGame(vfs, "tdm", 120, [](World& w) { return w.mpTeamScore("allies") + w.mpTeamScore("axis") >= 5; }, "5 kills"));
    CHECK(botGame(vfs, "hq", 240, [](World& w) { return w.mpTeamScore("allies") + w.mpTeamScore("axis") > 0; }, "radio points"));
    CHECK(botGame(vfs, "ctf", 400, [](World& w) { return w.mpTeamScore("allies") + w.mpTeamScore("axis") > 0; }, "a capture"));
    CHECK(botGame(vfs, "sd", 400, [](World& w) { return logHas(w, ";bomb_plant") && w.mpRestarts() >= 2; }, "plant, 2 rounds"));
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("tdm");
        const int b = w.mpAddBot();
        w.frame();
        run(w, 20);
        Entity* be = w.mpClientEntity(b);
        CHECK(field(be, "sessionstate") == "playing");
        w.mpBotSetCombat(b, false);
        // GSC: isBot, botSetRole, botSetGoal(origin, radius), botClearGoal.
        const auto o = be->obj->get("origin").asVector();
        w.vm().level()->set("iw2test_bot", Value(be->obj));
        w.vm().level()->set("iw2test_goal", Value(iw2::script::Vec3{o[0], o[1], o[2]}));
        CHECK(!throws(w, "maps/mp/iw2test_gametypes.gsc", "botgoal"));
        CHECK(w.vm().level()->get("iw2test_isbot") == Value(int32_t(1)));
        run(w, 10);
        CHECK(w.mpBotObjectiveName(b) == "goal");
        CHECK(w.mpBotDescribe(b).find("goal reached") != std::string::npos);
        CHECK(throws(w, "maps/mp/iw2test_gametypes.gsc", "badrole"));
        CHECK(!throws(w, "maps/mp/iw2test_gametypes.gsc", "botclear"));
        run(w, 2);
        CHECK(w.mpBotObjectiveName(b) == "");
    }

    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
