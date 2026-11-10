// Multiplayer HUD feedback (docs/spec/mp-hud.md): obituary events and lines, team colours, head icon / crosshair name
// rules, playLocalSound only for the local client, positional sounds of other clients, compass pings of other clients,
// the MP objective built-ins and setWayPoint. The World part needs the game files (IW2_MAIN).

#include "game/mphud.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
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

using namespace iw2::game;
using iw2::script::Value;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

Value call(World& w, const std::string& fn, std::vector<Value> args, Value self = Value()) {
    iw2::script::Thread t;
    iw2::script::CallContext c{w.vm(), t, self, std::span<Value>(args)};
    return w.vm().builtin(fn)(c);
}

bool throws(World& w, const std::string& fn, std::vector<Value> args, Value self = Value()) {
    try {
        call(w, fn, std::move(args), self);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void pureRules() {
    // Server event byte (0x8113128): melee, head shot, crush, falling, suicide by MOD; everything else by weapon.
    CHECK(mpMakeObituary(1, 0, "thompson_mp", "MOD_MELEE").modByte);
    CHECK(mpMakeObituary(1, 0, "thompson_mp", "MOD_HEAD_SHOT").mod == 8);
    CHECK(mpMakeObituary(1, 1, "none", "MOD_SUICIDE").modByte);
    CHECK(mpMakeObituary(1, -1, "none", "MOD_FALLING").mod == 11);
    CHECK(mpMakeObituary(1, -1, "none", "MOD_CRUSH").modByte);
    CHECK(!mpMakeObituary(1, 0, "thompson_mp", "MOD_PISTOL_BULLET").modByte);
    CHECK(!mpMakeObituary(1, 0, "none", "MOD_TRIGGER_HURT").modByte); // 13 is not in the server's set
    CHECK(mpMakeObituary(1, 0, "thompson_mp", "MOD_RIFLE_BULLET").weapon == "thompson_mp");

    // Client line (0x4e03f0).
    KillIconInfo wide{"killiconkar98", true, true};
    ObituaryLine l = obituaryLine(mpMakeObituary(1, 0, "kar98k_mp", "MOD_RIFLE_BULLET"), wide, "Bob", 1, "Ann", 2, 0);
    CHECK(l.icon == "killiconkar98");
    CHECK(std::fabs(l.iconW - 2.8f) < 1e-6f && std::fabs(l.iconH - 1.4f) < 1e-6f && l.flip);
    CHECK(l.attacker == "Ann^7" && l.victim == "Bob^7");
    CHECK(l.attackerTeam == 2 && l.victimTeam == 1);
    CHECK(l.centerPrint == "CGAME_YOUKILLED" && l.centerName == "Bob^7" && !l.teammate);
    CHECK(l.centerText == "CGAME_YOUKILLED\x15" "Bob^7");
    // The local client as the victim; same team = teammate form.
    l = obituaryLine(mpMakeObituary(0, 3, "mp40_mp", "MOD_PISTOL_BULLET"), KillIconInfo{"killiconmp40", false, false}, "Me", 2, "Them", 2, 0);
    CHECK(l.centerPrint == "CGAME_YOUWEREKILLED" && l.centerName == "Them" && l.teammate);
    CHECK(l.centerText == std::string("CGAME_YOUWEREKILLED\x15^1&&2^7 Them\x14") + "CGAME_TEAMMATE");
    CHECK(std::fabs(l.iconW - 1.4f) < 1e-6f && !l.flip);
    // Empty killIcon -> killicondied; MOD icons are 1.4 wide and not flipped.
    CHECK(obituaryLine(mpMakeObituary(1, 0, "x", "MOD_GRENADE"), KillIconInfo{}, "a", 0, "b", 0, 5).icon == "killicondied");
    l = obituaryLine(mpMakeObituary(1, 0, "kar98k_mp", "MOD_HEAD_SHOT"), wide, "a", 0, "b", 0, 5);
    CHECK(l.icon == "killiconheadshot" && std::fabs(l.iconW - 1.4f) < 1e-6f && !l.flip && l.centerPrint.empty());
    CHECK(obituaryLine(mpMakeObituary(1, 0, "x", "MOD_MELEE"), wide, "a", 0, "b", 0, 5).icon == "killiconmelee");
    CHECK(obituaryLine(mpMakeObituary(1, -1, "x", "MOD_FALLING"), wide, "a", 0, "", 0, 5).icon == "killiconfalling");
    CHECK(obituaryLine(mpMakeObituary(1, 0, "x", "MOD_CRUSH"), wide, "a", 0, "b", 0, 5).icon == "killiconcrush");
    // Suicide: no attacker text, no centre print even for the local client.
    l = obituaryLine(mpMakeObituary(0, 0, "none", "MOD_SUICIDE"), KillIconInfo{}, "Me", 0, "Me", 0, 0);
    CHECK(l.icon == "killiconsuicide" && l.attacker.empty() && l.centerPrint.empty());
    // No attacker (world).
    l = obituaryLine(mpMakeObituary(1, -1, "none", "MOD_FALLING"), KillIconInfo{}, "a", 0, "", 0, 1);
    CHECK(l.attacker.empty() && l.centerPrint.empty());
    // Names are cut to 31 characters before the ^7.
    CHECK(obituaryLine(mpMakeObituary(1, -1, "", "MOD_FALLING"), KillIconInfo{}, std::string(40, 'x'), 0, "", 0, 0).victim ==
          std::string(31, 'x') + "^7");

    // Team colours (0x4d9e50, defaults from the server registration).
    CHECK((mpTeamColor(1) == std::array<float, 4>{1.0f, 0.5f, 0.5f, 1.0f}));
    CHECK((mpTeamColor(2) == std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f}));
    CHECK((mpTeamColor(0) == std::array<float, 4>{1, 1, 1, 1}));
    CHECK((mpTeamColor(3) == std::array<float, 4>{1, 1, 1, 1}));
    CHECK((mpTeamColor(2, "", "0 1 0 1") == std::array<float, 4>{0, 1, 0, 1}));

    // Head icons (0x4dbcc0) and crosshair names (0x4c97c0).
    CHECK(mpHeadIconVisible(0, 1) && mpHeadIconVisible(2, 2) && mpHeadIconVisible(1, 3) && !mpHeadIconVisible(1, 2));
    CHECK(!mpCrosshairNameVisible(0, 0)); // DM: never
    CHECK(mpCrosshairNameVisible(2, 2) && !mpCrosshairNameVisible(2, 1) && mpCrosshairNameVisible(3, 1));
    CHECK(mpCrosshairNameAlpha(0) == 1.0f && mpCrosshairNameAlpha(50) == 1.0f);
    CHECK(std::fabs(mpCrosshairNameAlpha(100) - 0.5f) < 1e-6f);
    CHECK(mpCrosshairNameAlpha(150) < 0 && mpCrosshairNameAlpha(-1) < 0);
}

void join(World& w, int c, const std::string& team, const std::string& weapon) {
    w.mpBegin(c);
    run(w, 1);
    w.mpMenuResponse(c, w.vm().game()->get("menu_team").toString(), team);
    run(w, 1);
    w.mpMenuResponse(c, w.mpState().clients[size_t(c)].openMenu, weapon);
    run(w, 2);
}

void worldDm(const iw2::fs::Vfs& vfs) {
    World w(vfs, "mp_toujane", true);
    std::vector<std::pair<std::string, bool>> sounds; // alias, positional
    w.playSound = [&](const std::string& a, const Vec3* o, float*, int) {
        sounds.push_back({a, o != nullptr});
        return 0;
    };
    w.mpStart("dm");
    const int c0 = w.mpConnect("local");
    join(w, c0, "autoassign", "thompson_mp");
    const int c1 = w.mpConnect("other");
    run(w, 1);
    join(w, c1, "axis", "mp40_mp");
    Entity* p0 = w.mpClientEntity(c0);
    Entity* p1 = w.mpClientEntity(c1);
    CHECK(p0 && p1 && p1->obj->get("sessionstate").toString() == "playing");
    w.takeMpObituaries();

    // The local client hits the other: _damagefeedback.gsc plays MP_hit_alert with playLocalSound on the attacker (local).
    sounds.clear();
    w.mpDebugDamage(c1, c0, 30, "MOD_PISTOL_BULLET");
    run(w, 1);
    bool hitAlert = false;
    for (const auto& [a, pos] : sounds)
        if (a == "MP_hit_alert" && !pos) hitAlert = true;
    CHECK(hitAlert);
    // The other client hits the local one: the alert goes to client 1 and is not heard here.
    sounds.clear();
    w.mpDebugDamage(c0, c1, 10, "MOD_PISTOL_BULLET");
    run(w, 1);
    for (const auto& [a, pos] : sounds) CHECK(a != "MP_hit_alert");
    // The hit direction of the local player's damage points from the attacker (P_DamageFeedback, not the world flag).
    CHECK(w.playerDamageFeedback().pitch != 0xff || w.playerDamageFeedback().yaw != 0xff);

    // A kill: one obituary event with the victim / attacker clients and the weapon; the line uses the weapon's killIcon.
    w.mpDebugDamage(c1, c0, 500, "MOD_PISTOL_BULLET");
    run(w, 1);
    const auto obs = w.takeMpObituaries();
    CHECK(obs.size() == 1);
    if (!obs.empty()) {
        CHECK(obs[0].victim == c1 && obs[0].attacker == c0 && !obs[0].modByte);
        const ObituaryLine l = w.mpObituaryLine(obs[0]);
        CHECK(l.victim == "other^7" && l.attacker == "local^7");
        CHECK(!l.icon.empty() && l.icon != "killicondied"); // thompson_mp has a killIcon
        CHECK(l.centerPrint == "CGAME_YOUKILLED");
        CHECK(l.attackerTeam == 0 && l.victimTeam == 0); // DM: sessionteam none
    }
    CHECK(w.takeMpObituaries().empty());

    // The local player is killed: the fatal damage is not reported while dead, and the client spawn clears the client
    // (ClientSpawn, docs/spec/mp.md), so no hit direction icon from the previous life appears after the respawn.
    {
        const int before = w.playerDamageFeedback().event;
        w.mpDebugDamage(c0, c1, 1000, "MOD_PISTOL_BULLET");
        run(w, 2);
        CHECK(p0->obj->get("sessionstate").toString() != "playing");
        CHECK(w.playerDamageFeedback().event == before);
        call(w, "spawn", {Value(p0->obj->get("origin").asVector()), Value(Vec3{0, 0, 0})}, Value(p0->obj));
        p0->obj->set("sessionstate", Value("playing"));
        p0->obj->set("health", Value(int32_t(100)));
        run(w, 2);
        CHECK(w.playerDamageFeedback().event == before);
        w.takeMpObituaries();
    }

    // playLocalSound needs a client and plays only for the local one.
    sounds.clear();
    call(w, "playlocalsound", {Value("MP_announcer_round_draw")}, Value(p1->obj));
    CHECK(sounds.empty());
    call(w, "playlocalsound", {Value("MP_announcer_round_draw")}, Value(p0->obj));
    CHECK(sounds.size() == 1 && !sounds[0].second);
    CHECK(throws(w, "playlocalsound", {Value("x")}, Value(w.vm().level())));
    // playSound on another client is positional; on the local client it is not.
    sounds.clear();
    call(w, "playsound", {Value("MP_hit_alert")}, Value(p1->obj));
    call(w, "playsound", {Value("MP_hit_alert")}, Value(p0->obj));
    CHECK(sounds.size() == 2 && sounds[0].second && !sounds[1].second);

    // Compass in DM: no friendlies; another client's shot is an enemy ping, the local client's shot none.
    CHECK(w.compassFriendlies().empty());
    w.compassPing(*p0, {1, 2, 3}, 500);
    CHECK(w.compassPings().activeEnemyPings(w.timeMs(), 2.0f).empty());
    w.compassPing(*p1, {10, 20, 30}, 500);
    const auto pings = w.compassPings().activeEnemyPings(w.timeMs(), 2.0f);
    CHECK(pings.size() == 1 && pings[0].pos[0] == 10.0f && pings[0].pos[1] == 20.0f);

    // Head icons: none in DM by default; a script icon with team none shows over the other client at origin z + 82.
    CHECK(w.mpHeadIcons().empty());
    w.mpSetButtons(c1, 0x28); // the use button respawns the dead client (dm.gsc)
    run(w, 80);
    w.mpSetButtons(c1, 0); // in the killcam a held button does not count: release and press again (_killcam.gsc:192-198)
    run(w, 2);
    w.mpSetButtons(c1, 0x28);
    run(w, 3);
    w.mpSetButtons(c1, 0);
    CHECK(p1->obj->get("sessionstate").toString() == "playing");
    p1->obj->set("headicon", Value("talkingicon"));
    p1->obj->set("headiconteam", Value("none"));
    auto icons = w.mpHeadIcons();
    if (p1->obj->get("sessionstate").toString() == "playing") {
        CHECK(icons.size() == 1 && icons[0].material == "talkingicon" && std::fabs(icons[0].size - 10.0f) < 1e-6f);
        if (!icons.empty()) CHECK(std::fabs(icons[0].pos[2] - (p1->obj->get("origin").asVector()[2] + 82.0f)) < 1e-3f);
        p1->obj->set("headiconteam", Value("axis")); // local team 0 is not axis
        CHECK(w.mpHeadIcons().empty());
        p0->obj->set("headicon", Value("talkingicon")); // the local client's own icon is not drawn
        p0->obj->set("headiconteam", Value("none"));
        p1->obj->set("headicon", Value(""));
        CHECK(w.mpHeadIcons().empty());
    }

    // Crosshair trace (0x4c9700): from the local eye along +X hits the other client 300 units ahead; aimed away, nothing.
    {
        const Vec3 o0 = p0->obj->get("origin").asVector();
        const Vec3 eye{o0[0], o0[1], o0[2] + 1060.0f}; // high above the map: nothing else on the line
        p1->obj->set("origin", Value(Vec3{o0[0] + 300.0f, o0[1], o0[2] + 1000.0f}));
        w.frame();
        CHECK(w.mpCrosshairClient(eye, {1, 0, 0}) == c1);
        CHECK(w.mpCrosshairClient(eye, {-1, 0, 0}) != c1);
    }

    // MP objectives: (index, state [, origin [, icon]]), states empty / invisible / current, onEntity, team.
    call(w, "objective_add", {Value(int32_t(2)), Value("current"), Value(Vec3{5, 6, 7}), Value("objective")});
    const Objective& o = w.objectives()[2];
    CHECK(o.state == 4 && o.positions[0][0] == 5.0f && o.icon == "objective");
    CHECK(throws(w, "objective_add", {Value(int32_t(2)), Value("active")}));
    CHECK(throws(w, "objective_add", {Value(int32_t(16)), Value("current")}));
    CHECK(throws(w, "objective_add", {Value(int32_t(1))}));
    call(w, "objective_onentity", {Value(int32_t(2)), Value(p1->obj)});
    p1->obj->set("origin", Value(Vec3{100, 200, 300}));
    run(w, 1);
    CHECK(w.objectives()[2].positions[0][0] == p1->obj->get("origin").asVector()[0]);
    CHECK(w.mpObjectiveVisible(o));
    call(w, "objective_team", {Value(int32_t(2)), Value("axis")});
    CHECK(!w.mpObjectiveVisible(w.objectives()[2])); // local team none
    call(w, "objective_team", {Value(int32_t(2)), Value("none")});
    CHECK(w.mpObjectiveVisible(w.objectives()[2]));
    CHECK(throws(w, "objective_team", {Value(int32_t(2)), Value("blue")}));
    CHECK(throws(w, "objective_team", {Value(int32_t(16)), Value("axis")}));
    // Every MP built-in has a single implementation in the MP layer (no file overrides another one).
    for (const std::string& n : w.mpDuplicateBuiltins()) std::cerr << "duplicate MP builtin: " << n << "\n";
    CHECK(w.mpDuplicateBuiltins().empty());

    // setWayPoint makes a world-positioned element.
    const Value h = call(w, "newhudelem", {});
    call(w, "setwaypoint", {Value(int32_t(1))}, h);
    CHECK(h.asObject()->get("$waypoint").truthy() && h.asObject()->get("$waypointconst").asInt() == 1);

    for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
    CHECK(w.vm().errors().empty());
}

// Rejoining: the elements a client's connect made die with its disconnect (0x8103414, called by 0x80f94ce before the
// disconnect callback), so each connect of a reused slot leaves one set. docs/spec/mp.md "Client HUD elements".
std::vector<std::string> ownHud(World& w, int client) {
    std::vector<std::string> keys;
    for (const auto& h : w.hudElems()) {
        if (h->removed || !h->get("$owner").isNumber() || h->get("$owner").asInt() != client) continue;
        keys.push_back(h->get("horzalign").toString() + "/" + h->get("vertalign").toString() + " " + h->get("x").toString() + "," +
                       h->get("y").toString());
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void rejoin(iw2::fs::Vfs& vfs, const char* gametype, int firstSlot) {
    World w(vfs, "mp_carentan", true);
    w.mpStart(gametype);
    for (int i = 0; i < firstSlot; ++i) w.mpBegin(w.mpConnect("other"));
    const size_t threads0 = w.vm().threadCount();
    int c = w.mpConnect("rejoiner");
    CHECK(c == firstSlot);
    w.mpBegin(c);
    run(w, 5);
    const std::vector<std::string> first = ownHud(w, c);
    CHECK(!first.empty());
    const size_t threads1 = w.vm().threadCount();
    for (int round = 0; round < 6; ++round) {
        // A played life: team, weapon, score, a script field of the old client.
        w.mpAutoJoin(c, "", "autoassign");
        run(w, 3);
        Entity* e = w.mpClientEntity(c);
        CHECK(e->obj->get("sessionstate").toString() == "playing");
        CHECK(!w.inventory(*e).owned.empty());
        e->obj->set("score", Value(int32_t(7)));
        e->obj->set("stale_field", Value(int32_t(1)));
        const auto elems = w.hudElems();
        w.mpDisconnect(c);
        CHECK(ownHud(w, c).empty());
        for (const auto& h : elems)
            if (h->get("$owner").isNumber() && h->get("$owner").asInt() == c) CHECK(h->removed);
        run(w, 2);
        // Nothing queued for the old connection reaches the next client of the slot.
        for (const auto& [target, text] : w.mpState().clientCommands) CHECK(target != c);
        c = w.mpConnect("rejoiner");
        CHECK(c == firstSlot);
        e = w.mpClientEntity(c);
        CHECK(!e->obj->get("stale_field").isDefined());
        CHECK(e->obj->get("score").asInt() == 0);
        CHECK(e->obj->get("sessionteam").toString() == "spectator");
        CHECK(e->obj->get("sessionstate").toString() == "spectator");
        CHECK(e->obj->get("spectatorclient").asInt() == -1);
        CHECK(w.inventory(*e).owned.empty());
        w.mpBegin(c);
        run(w, 5);
        CHECK(ownHud(w, c) == first);
    }
    std::cout << gametype << " slot " << firstSlot << ": " << first.size() << " own elements, threads " << threads0 << " -> " << threads1
              << " -> " << w.vm().threadCount() << "\n";
    CHECK(w.vm().threadCount() <= threads1 + 2);
}

} // namespace

int main() {
    pureRules();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, world part skipped\n";
    } else {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        worldDm(vfs);
        for (const char* gt : {"tdm", "dm", "sd", "ctf", "hq"})
            for (int slot : {0, 1}) rejoin(vfs, gt, slot);
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
