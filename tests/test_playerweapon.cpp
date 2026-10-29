// Tests for the player weapon state machine with hand-written weapon definitions.

#include "game/playerweapon.h"

#include <iostream>
#include <map>
#include <optional>
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

// Bolt-action brass (0x4b79d1, 0x4dd700, 0x4dd8c0 / 0x4ddfa0): no casing on the shot; event 0xa2 rechamberBoltTime into the
// rechamber, or min(rechamberBoltTime, add delay) into the reload after the last round. Other weapons never send it.
int brassAt(PlayerWeapon& w, float seconds, bool fire, std::vector<int>& at, int& tick) {
    int shots = 0;
    for (int i = 0; i < int(seconds * 100 + 0.5f); ++i, ++tick) {
        shots += w.update(0.01f, {fire, false});
        for (const auto& e : w.takeEvents())
            if (e.kind == PlayerWeapon::Event::EjectBrass) at.push_back(tick);
    }
    return shots;
}

void boltBrass() {
    {
        PlayerWeapon w;
        w.equip("kar", {{"semiauto", "1"}, {"boltaction", "1"}, {"firetime", "0.33"}, {"rechambertime", "1"},
                        {"rechamberbolttime", "0.65"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0.5"},
                        {"reloadtime", "2"}, {"fireanim", "fire"}, {"rechamberanim", "rechamber"}});
        std::vector<int> at;
        int tick = 0;
        brassAt(w, 0.6f, false, at, tick);
        CHECK(brassAt(w, 0.01f, true, at, tick) == 1); // the shot at tick 60
        CHECK(at.empty());
        brassAt(w, 0.30f, false, at, tick);              // firing (0.33 s)
        CHECK(at.empty());
        brassAt(w, 1.2f, false, at, tick);               // rechamber starts ~0.33 s after the shot, brass 0.65 s into it
        CHECK(at.size() == 1);
        if (at.size() == 1) CHECK(at[0] >= 60 + 33 + 64 && at[0] <= 60 + 33 + 67);
        CHECK(w.anim() != "fire");
    }
    {
        PlayerWeapon w; // last round: no rechamber, the casing comes out during the reload
        w.equip("kar", {{"semiauto", "1"}, {"boltaction", "1"}, {"firetime", "0.33"}, {"rechambertime", "1"},
                        {"rechamberbolttime", "0.65"}, {"clipsize", "1"}, {"startammo", "5"}, {"raisetime", "0.5"},
                        {"reloadtime", "2"}, {"reloademptytime", "2"}, {"reloadaddtime", "1.5"}, {"weapontype", "bullet"}});
        std::vector<int> at;
        int tick = 0;
        brassAt(w, 0.6f, false, at, tick);
        CHECK(brassAt(w, 0.01f, true, at, tick) == 1);
        brassAt(w, 0.33f, false, at, tick);
        CHECK(at.empty());
        brassAt(w, 1.0f, false, at, tick);
        CHECK(at.size() == 1);
        if (at.size() == 1) CHECK(at[0] >= 60 + 33 + 63 && at[0] <= 60 + 33 + 68);
    }
    {
        PlayerWeapon w; // automatic: brass with the shot (cgame), never event 0xa2
        w.equip("smg", {{"firetime", "0.1"}, {"clipsize", "30"}, {"startammo", "60"}, {"raisetime", "0.1"}, {"reloadtime", "1"}});
        std::vector<int> at;
        int tick = 0;
        brassAt(w, 0.2f, false, at, tick);
        CHECK(brassAt(w, 1.0f, true, at, tick) == 10);
        brassAt(w, 1.0f, false, at, tick);
        CHECK(at.empty());
    }
    {
        PlayerWeapon w; // the pending casing is a per-weapon bit (ps+0x530): it survives a switch away and back
        const PlayerWeapon::Def kar{{"semiauto", "1"}, {"boltaction", "1"}, {"firetime", "0.33"}, {"rechambertime", "1"},
                                    {"rechamberbolttime", "0.65"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0.1"},
                                    {"reloadtime", "2"}, {"fireanim", "fire"}, {"rechamberanim", "rechamber"}};
        w.equip("kar", kar);
        std::vector<int> at;
        int tick = 0;
        brassAt(w, 0.3f, false, at, tick);
        brassAt(w, 0.01f, true, at, tick);
        w.equip("other", {{"firetime", "0.1"}, {"clipsize", "30"}, {"startammo", "60"}, {"raisetime", "0.1"}});
        w.equip("kar", kar);
        CHECK(w.hasBrassPending());
    }
}

// An empty projectile weapon reloads with reloadAnim / reloadTime: the empty variant is only for weaponType bullet
// (0x4dd997..0x4dda14). The panzerschreck has no reloadEmptyAnim; before, its reload showed the idle pose.
void projectileReload() {
    PlayerWeapon w;
    w.equip("schreck", {{"weapontype", "projectile"}, {"firetime", "0.8"}, {"clipsize", "1"}, {"startammo", "4"},
                        {"raisetime", "0.6"}, {"reloadtime", "4"}, {"reloademptytime", "1"}, {"reloadaddtime", "2.3"},
                        {"idleanim", "idle"}, {"fireanim", "idle"}, {"reloadanim", "reload"}});
    run(w, 0.7f, false);
    CHECK(run(w, 0.01f, true) == 1);
    CHECK(w.clip() == 0);
    run(w, 0.85f, false); // fire time, then the automatic reload
    CHECK(w.stateNumber() == 5 && w.anim() == "reload");
    run(w, 2.0f, false);
    CHECK(w.clip() == 0);                  // reloadTime 4 (not reloadEmptyTime 1); the add comes at 2.3 s
    run(w, 0.5f, false);
    CHECK(w.clip() == 1 && w.stock() == 2);
}

// The clip emptied by the last shot reloads by itself once the shot is over (PM_Weapon 0x4de370), without a new press.
void autoReload() {
    PlayerWeapon w;
    w.equip("rifle", {{"semiauto", "1"}, {"firetime", "0.1"}, {"clipsize", "1"}, {"startammo", "5"},
                      {"raisetime", "0"}, {"reloadtime", "1"}, {"reloademptytime", "1"}});
    CHECK(run(w, 0.05f, true) == 1);
    CHECK(w.clip() == 0);
    run(w, 1.5f, false);
    CHECK(w.clip() == 1 && w.stock() == 3);
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

using Def = PlayerWeapon::Def;

// Weapon change (0x4ddb40 / PM_Weapon state 2): dropTime, then the new weapon's raiseTime.
void weaponChange() {
    const Def a{{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "10"}, {"droptime", "0.3"}, {"raisetime", "0.4"},
                {"dropanim", "a_drop"}, {"raiseanim", "a_raise"}, {"meleedamage", "50"}};
    const Def b{{"firetime", "0.1"}, {"clipsize", "8"}, {"startammo", "8"}, {"droptime", "0.2"}, {"raisetime", "0.5"},
                {"raiseanim", "b_raise"}};
    PlayerWeapon w;
    w.lookup = [&](const std::string& n) -> const Def* { return n == "a" ? &a : n == "b" ? &b : nullptr; };
    w.equip("a", a);
    PlayerWeapon::Input in;
    in.weapon = "a";
    for (int i = 0; i < 50; ++i) w.update(0.01f, in); // raised
    in.fire = true;
    w.update(0.01f, in);
    CHECK(w.clip() == 4);
    in.fire = false;
    for (int i = 0; i < 20; ++i) w.update(0.01f, in);
    in.weapon = "b";
    w.update(0.01f, in);
    CHECK(w.anim() == "a_drop" && w.stateNumber() == 2);
    for (int i = 0; i < 29; ++i) w.update(0.01f, in);
    CHECK(w.name() == "a");
    w.update(0.01f, in);
    w.update(0.01f, in);
    CHECK(w.name() == "b" && w.anim() == "b_raise" && w.stateNumber() == 1 && w.clip() == 8);
    bool raised = false;
    for (const auto& e : w.takeEvents()) raised |= e.kind == PlayerWeapon::Event::WeaponRaised && e.weapon == "b";
    CHECK(raised);
    in.weapon = "a";
    for (int i = 0; i < 200; ++i) w.update(0.01f, in);
    CHECK(w.name() == "a" && w.clip() == 4); // ammo kept while put away
}

// Melee (0x4def20): meleeDelay until the blow (MeleeHit), meleeTime in all.
void melee() {
    const Def a{{"meleedamage", "50"}, {"meleetime", "0.6"}, {"meleedelay", "0.2"}, {"meleeanim", "melee"}, {"raisetime", "0"}};
    PlayerWeapon w;
    w.lookup = [&](const std::string&) { return &a; };
    w.equip("a", a);
    PlayerWeapon::Input in;
    in.weapon = "a";
    w.update(0.01f, in);
    in.melee = true;
    w.update(0.01f, in);
    CHECK(w.anim() == "melee" && w.stateNumber() == 10);
    int hits = 0;
    for (int i = 0; i < 100; ++i) {
        w.update(0.01f, in); // held: no second swing
        for (const auto& e : w.takeEvents()) hits += e.kind == PlayerWeapon::Event::MeleeHit;
        if (i == 15) CHECK(hits == 0);
        if (i == 25) CHECK(hits == 1);
    }
    CHECK(hits == 1 && w.stateNumber() == 0);
}

// Offhand (0x4df470 and states 12–16): quick drop, pull the pin (holdFireTime), hold, throw on release, quick raise.
void offhand() {
    const Def a{{"quickdroptime", "0.1"}, {"quickraisetime", "0.1"}, {"raisetime", "0"}, {"quickdropanim", "qd"}};
    const Def g{{"weapontype", "grenade"}, {"holdfiretime", "0.3"}, {"firetime", "0.2"}, {"fusetime", "3"},
                {"holdfireanim", "pullpin"}, {"fireanim", "throw"}, {"gunmodel", "grenade"}, {"pullbacksound", "pin"},
                {"firesound", "toss"}};
    PlayerWeapon w;
    w.lookup = [&](const std::string& n) -> const Def* { return n == "frag" ? &g : &a; };
    w.findOffhand = [](const std::string& cls) { return cls == "Frag Grenade" ? std::string("frag") : std::string(); };
    w.equip("a", a);
    PlayerWeapon::Input in;
    in.weapon = "a";
    w.update(0.01f, in);
    in.frag = true;
    w.update(0.01f, in);
    CHECK(w.stateNumber() == 12 && w.anim() == "qd");
    for (int i = 0; i < 11; ++i) w.update(0.01f, in);
    CHECK(w.stateNumber() == 13 && w.anim() == "pullpin" && w.gunModel() == "xmodel/grenade");
    CHECK(w.takeSounds() == std::vector<std::string>{"pin"}); // event 0xa5 on entering state 13
    for (int i = 0; i < 60; ++i) w.update(0.01f, in);
    CHECK(w.stateNumber() == 14); // held
    int throws = 0;
    in.frag = false;
    for (int i = 0; i < 2; ++i) {
        w.update(0.01f, in);
        for (const auto& e : w.takeEvents()) throws += e.kind == PlayerWeapon::Event::OffhandThrow && e.weapon == "frag";
    }
    CHECK(throws == 1 && w.stateNumber() == 15 && w.anim() == "throw");
    CHECK(w.takeSounds() == std::vector<std::string>{"toss"}); // event 0xa6 on entering state 15
    for (int i = 0; i < 40; ++i) w.update(0.01f, in);
    CHECK(w.stateNumber() == 0 && w.name() == "a");
}

} // namespace

// Weapon sounds follow the cgame priority chains (0x4ba8d0): player variants first, one sound per event.
void weaponSounds() {
    PlayerWeapon w;
    PlayerWeapon::Def a = {{"clipsize", "2"}, {"startammo", "4"}, {"raisetime", "0.1"}, {"reloadtime", "0.5"}, {"reloademptytime", "0.5"},
                           {"weaponType", "bullet"}, {"raisesound", "up"}, {"putawaysound", "down"}, {"reloadsound", "rel"},
                           {"reloademptysoundplayer", "relempty_p"}, {"meleedamage", "50"}, {"meleetime", "0.5"},
                           {"meleeswipesound", "swipe"}, {"firetime", "0.1"}};
    w.lookup = [&](const std::string&) -> const Def* { return &a; };
    w.equip("a", a);
    CHECK(w.takeSounds() == std::vector<std::string>{"up"});
    // Reload with an empty clip (event 0x97): reloadEmptySoundPlayer wins over reloadSound.
    PlayerWeapon r;
    r.equip("a", a);
    run(r, 0.2f, false);
    r.takeSounds();
    for (int i = 0; i < 40 && r.clip() > 0; ++i) r.update(0.01f, {true, false});
    while (r.clip() > 0) run(r, 0.2f, true);
    run(r, 0.3f, false);
    const auto snd = r.takeSounds();
    CHECK(!snd.empty() && snd.back() == "relempty_p");
    // Out of ammo: the empty click plays and the next action waits 500 ms.
    PlayerWeapon e;
    PlayerWeapon::Def b = a;
    b["startammo"] = "2";
    e.equip("b", b);
    run(e, 0.2f, false);
    e.takeSounds();
    run(e, 0.25f, true); // fires both rounds (0.1 s apart), then the empty click at ~0.2 s
    CHECK(e.takeSounds() == std::vector<std::string>{"player_out_of_ammo"});
    run(e, 0.4f, true); // 500 ms wait: no second click yet
    CHECK(e.takeSounds().empty());
    run(e, 0.2f, true);
    CHECK(e.takeSounds() == std::vector<std::string>{"player_out_of_ammo"});
    {
        int n = 0; // event 0x92 per dry fire (the cgame's auto switch, 0x4b7090)
        for (const auto& ev : e.takeEvents()) n += ev.kind == PlayerWeapon::Event::OutOfAmmo && ev.weapon == "b";
        CHECK(n == 2);
    }
    // clipOnly: no click on an empty clip, and no putaway sound with an empty clip (0x4ddbd9, 0x4baec1).
    PlayerWeapon::Def c = a;
    c["cliponly"] = "1";
    c["startammo"] = "2";
    PlayerWeapon p;
    p.lookup = [&](const std::string&) -> const Def* { return &b; };
    p.equip("c", c);
    run(p, 0.2f, false);
    p.takeSounds();
    run(p, 0.25f, true);
    CHECK(p.takeSounds().empty());
    run(p, 0.6f, false);
    p.update(0.01f, {false, false, false, false, false, false, "b"});
    for (const auto& snd : p.takeSounds()) CHECK(snd != "down");
}

// Segmented reload (springfield-like values; PM_Weapon states 7, 5, 9: 0x4dda60, 0x4dd980, 0x4de0e0, 0x4de200).
// The reload key held down (a real +reload is held for many frames).
void held(PlayerWeapon& w, float seconds, bool) {
    for (int i = 0; i < int(seconds * 100 + 0.5f); ++i) w.update(0.01f, {false, true});
}

void segmentedReload() {
    const Def a{{"clipsize", "5"}, {"startammo", "10"}, {"raisetime", "0"}, {"firetime", "0.1"}, {"segmentedreload", "1"},
                {"nopartialreload", "1"}, {"reloadammoadd", "1"}, {"reloadstartadd", "1"}, {"reloadtime", "0.6"},
                {"reloademptytime", "0.6"}, {"reloadaddtime", "0.2"}, {"reloadstarttime", "1.8"}, {"reloadstartaddtime", "1.4"},
                {"reloadendtime", "0.77"}, {"weapontype", "bullet"}, {"reloadstartsound", "start"}, {"reloadsound", "loop"},
                {"reloadendsound", "end"}};
    auto fireDown = [](PlayerWeapon& w, int clip) {
        for (int i = 0; i < 2000 && w.clip() > clip; ++i) w.update(0.01f, {true, false});
        for (int i = 0; i < 50; ++i) w.update(0.01f, {false, false});
    };
    {
        PlayerWeapon w;
        w.equip("a", a);
        run(w, 0.1f, false);
        fireDown(w, 2);
        CHECK(w.clip() == 2 && w.stock() == 5);
        w.takeSounds();
        w.takeEvents();
        w.update(0.01f, {false, true});
        {
            const auto ev = w.takeEvents();
            CHECK(ev.size() == 1 && ev[0].kind == PlayerWeapon::Event::ReloadBegin);
        }
        CHECK(w.takeSounds() == std::vector<std::string>{"start"});
        held(w, 1.3f, false); // t = 1.32: the intro adds its round only at 1.4
        CHECK(w.clip() == 2);
        held(w, 0.15f, false); // t = 1.47
        CHECK(w.clip() == 3 && w.stock() == 4);
        held(w, 0.4f, false); // t = 1.87: the first loop round began at 1.8, adds after 0.2
        CHECK(w.clip() == 3);
        CHECK(w.takeSounds() == std::vector<std::string>{"loop"});
        held(w, 0.2f, false); // t = 2.07
        CHECK(w.clip() == 4);
        held(w, 1.0f, false); // second round done at 2.4 + 0.2, a full clip ends the loop, the outro (0.77 s) runs
        CHECK(w.clip() == 5 && w.stock() == 2);
        CHECK((w.takeSounds() == std::vector<std::string>{"loop", "end"}));
        held(w, 0.8f, false);
        CHECK(w.update(0.01f, {true, false}) == 1); // ready again
    }
    {
        // A fresh attack press during the loop ends the reload after the running round (the clip is not empty).
        PlayerWeapon w;
        w.equip("a", a);
        run(w, 0.1f, false);
        fireDown(w, 1);
        run(w, 0.02f, false, true);
        run(w, 1.9f, false); // inside the first loop round (1.8 .. 2.4)
        w.update(0.01f, {true, false});
        w.update(0.01f, {false, false});
        run(w, 0.7f, false); // the round ends, then the outro
        const int c = w.clip();
        CHECK(c == 3 && w.stock() == 3);
        run(w, 1.0f, false);
        CHECK(w.clip() == 3);
    }
    {
        // noPartialReload with reloadAmmoAdd: a clip too full for a whole add does not reload by command.
        Def b = a;
        b["reloadammoadd"] = "5";
        b["clipsize"] = "10";
        b["startammo"] = "20";
        PlayerWeapon w;
        w.equip("b", b);
        run(w, 0.1f, false);
        fireDown(w, 8);
        w.takeSounds();
        run(w, 0.02f, false, true);
        CHECK(w.takeSounds().empty() && w.clip() == 8);
    }
}

// A plain reload adds its rounds reloadAddTime into the round (0x4dd8c0, 0x4dd5b0), not at its end.
void reloadAddTiming() {
    const Def a{{"clipsize", "5"}, {"startammo", "10"}, {"raisetime", "0"}, {"firetime", "0.1"}, {"reloadtime", "1.0"},
                {"reloadaddtime", "0.4"}, {"weapontype", "bullet"}};
    PlayerWeapon w;
    w.equip("a", a);
    run(w, 0.1f, false);
    while (w.clip() > 2) run(w, 0.15f, true);
    run(w, 0.3f, false);
    run(w, 0.02f, false, true);
    run(w, 0.3f, false);
    CHECK(w.clip() == 2);
    run(w, 0.2f, false); // 0.52 s into the round
    CHECK(w.clip() == 5 && w.stock() == 2);
    // A weapon change before the add leaves the clip as it was.
    PlayerWeapon v;
    v.lookup = [&](const std::string&) -> const Def* { return &a; };
    v.equip("a", a);
    run(v, 0.1f, false);
    while (v.clip() > 2) run(v, 0.15f, true);
    run(v, 0.3f, false);
    run(v, 0.02f, false, true);
    v.update(0.01f, {false, false, false, false, false, false, "b"});
    run(v, 0.5f, false);
    v.update(0.01f, {false, false, false, false, false, false, "a"});
    run(v, 1.5f, false);
    CHECK(v.clip() == 2);
}


// Binoculars (0x4df9b0 + PM_Weapon 0x11-0x16): toggle press -> quick drop (0x12) -> up (0x13, event 0xa8, pm_flags 0x20000) ->
// 0x14 (aim forced in); use press / release -> 0xaa / 0xab; press again -> 0x15 (event 0xac) -> 0x16 (event 0xa9, quick raise) -> 0.
void binoculars() {
    PlayerWeapon w;
    const PlayerWeapon::Def binoc{{"weapontype", "binoculars"}, {"aimdownsight", "1"}, {"adstransintime", "0.4"},
                                  {"adstransouttime", "0.4"}, {"adszoomfov", "10"}, {"idleanim", "b_idle"}, {"gunmodel", "viewmodel_binoculars"}};
    w.binocularDef = [&]() { return &binoc; };
    w.equip("rifle", {{"raisetime", "0"}, {"quickdroptime", "0.25"}, {"quickraisetime", "0.25"}, {"idleanim", "idle"},
                      {"quickdropanim", "qdrop"}, {"quickraiseanim", "qraise"}, {"aimdownsight", "1"}, {"adstransouttime", "0.2"}});
    auto step = [&](float t, PlayerWeapon::Input in) {
        std::vector<PlayerWeapon::Event> ev;
        for (int i = 0; i < int(t * 100 + 0.5f); ++i) {
            in.ads = w.stateNumber() == 0x13 || w.stateNumber() == 0x14; // the client forces the aim button (0x409af0)
            w.update(0.01f, in);
            for (auto& e : w.takeEvents()) ev.push_back(e);
        }
        return ev;
    };
    auto has = [](const std::vector<PlayerWeapon::Event>& ev, PlayerWeapon::Event::Kind k) {
        for (auto& e : ev) if (e.kind == k) return true;
        return false;
    };
    PlayerWeapon::Input in;
    in.weapon = "rifle";
    step(0.05f, in);
    CHECK(w.stateNumber() == 0);
    in.binoculars = true;
    step(0.01f, in);
    CHECK(w.stateNumber() == 0x12 && w.anim() == "qdrop");
    in.binoculars = false;
    auto ev = step(0.3f, in);
    CHECK(has(ev, PlayerWeapon::Event::BinocularEnter) && w.binocularsUp());
    CHECK(w.gunModel() == "xmodel/viewmodel_binoculars" && w.number("adsZoomFov") == 10);
    step(0.45f, in);
    CHECK(w.stateNumber() == 0x14 && w.adsFrac() > 0.99f);
    in.use = true;
    CHECK(has(step(0.02f, in), PlayerWeapon::Event::BinocularFire));
    in.use = false;
    CHECK(has(step(0.02f, in), PlayerWeapon::Event::BinocularRelease));
    in.binoculars = true;
    ev = step(0.01f, in);
    CHECK(w.stateNumber() == 0x15 && has(ev, PlayerWeapon::Event::BinocularDrop));
    in.binoculars = false;
    ev = step(0.45f, in);
    CHECK(has(ev, PlayerWeapon::Event::BinocularExit) && !w.binocularsUp() && w.stateNumber() == 0x16 && w.anim() == "qraise");
    step(0.3f, in);
    CHECK(w.stateNumber() == 0 && w.adsFrac() == 0);
    // Melee while up cancels at once (0x4df0a0): exit event, state 0.
    in.binoculars = true;
    step(0.01f, in);
    in.binoculars = false;
    step(0.8f, in);
    CHECK(w.stateNumber() == 0x14);
    in.melee = true;
    ev = step(0.01f, in);
    CHECK(has(ev, PlayerWeapon::Event::BinocularExit) && !w.binocularsUp() && w.adsFrac() == 0);
    // Holstered hand weapon (in a vehicle, 0x4de880 weapon 0): the press brings the binoculars up without raising the rifle.
    {
        PlayerWeapon v;
        v.binocularDef = [&]() { return &binoc; };
        const PlayerWeapon::Def rifle{{"raisetime", "0"}, {"droptime", "0"}, {"quickdroptime", "0.25"}, {"idleanim", "idle"}};
        v.lookup = [&](const std::string&) { return &rifle; };
        v.equip("rifle", rifle);
        PlayerWeapon::Input vi;
        vi.weapon = "rifle";
        vi.holster = vi.inVehicle = true;
        for (int i = 0; i < 20; ++i) v.update(0.01f, vi);
        v.takeEvents();
        CHECK(!v.equipped());
        vi.binoculars = true;
        std::vector<PlayerWeapon::Event> vev;
        for (int i = 0; i < 3; ++i) {
            vi.ads = v.stateNumber() == 0x13 || v.stateNumber() == 0x14;
            v.update(0.01f, vi);
            for (auto& e : v.takeEvents()) vev.push_back(e);
            vi.binoculars = false;
        }
        CHECK(has(vev, PlayerWeapon::Event::BinocularEnter) && !has(vev, PlayerWeapon::Event::WeaponRaised));
        CHECK(v.stateNumber() == 0x13 && v.binocularsUp() && !v.equipped());
    }
}

// The ammunition comes from the player state (inventory) through readAmmo / writeAmmo: a picked-up weapon of the same type brings its
// own clip (the counters are not remembered per weapon name), and firing writes back.
void inventoryAmmo() {
    std::map<std::string, std::pair<int, int>> inv; // owned weapon -> {clip, stock}
    PlayerWeapon w;
    w.readAmmo = [&](const std::string& n) -> std::optional<std::pair<int, int>> {
        auto it = inv.find(n);
        if (it == inv.end()) return std::nullopt;
        return it->second;
    };
    w.writeAmmo = [&](const std::string& n, int c, int s) { if (inv.count(n)) inv[n] = {c, s}; };
    const PlayerWeapon::Def mp40{{"semiauto", "0"}, {"firetime", "0.1"}, {"clipsize", "32"}, {"startammo", "64"}, {"raisetime", "0"},
                                 {"reloadtime", "2"}, {"reloademptytime", "2"}};
    inv["mp40"] = {2, 0};
    w.equip("mp40", mp40);
    CHECK(w.clip() == 2 && w.stock() == 0);
    run(w, 0.3f, true);
    CHECK(w.clip() == 0 && inv["mp40"].first == 0); // fired out, written back
    // Dropped (no longer owned): nothing is written back; a new MP40 from the ground sets the clip slot to its own 32.
    inv.erase("mp40");
    run(w, 0.05f, false);
    inv["mp40"] = {32, 10};
    run(w, 0.05f, false);
    CHECK(w.clip() == 32 && w.stock() == 10);
}

// player_adsExitDelay (0x4dd0d0): at a full aim fraction the aim is held for the delay after the button is released.
void adsExitDelay() {
    PlayerWeapon w;
    w.equip("rifle", {{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0"},
                      {"aimdownsight", "1"}, {"adstransintime", "0.1"}, {"adstransouttime", "0.1"}});
    PlayerWeapon::Input in;
    in.ads = true;
    for (int i = 0; i < 30; ++i) w.update(0.01f, in);
    CHECK(w.adsFrac() == 1.0f);
    in.ads = false;
    in.adsExitDelay = 0.2f;
    for (int i = 0; i < 15; ++i) w.update(0.01f, in);
    CHECK(w.adsFrac() == 1.0f); // still held
    for (int i = 0; i < 30; ++i) w.update(0.01f, in);
    CHECK(w.adsFrac() == 0.0f); // then released
}

// player_scopeExitOnDamage (0x4dd0fe): a damaged frame drops the aim of a scope weapon at once (event 0x95), only with the dvar.
void scopeExitOnDamage() {
    PlayerWeapon w;
    w.equip("sniper", {{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0"}, {"aimdownsight", "1"},
                       {"adstransintime", "0.1"}, {"adstransouttime", "0.5"}, {"adsoverlayreticle", "default"}});
    PlayerWeapon::Input in;
    in.ads = true;
    for (int i = 0; i < 30; ++i) w.update(0.01f, in);
    CHECK(w.adsFrac() == 1.0f);
    w.takeEvents();
    in.damaged = true; // dvar off: the aim stays
    w.update(0.01f, in);
    CHECK(w.adsFrac() == 1.0f);
    in.scopeExitOnDamage = true;
    w.update(0.01f, in);
    CHECK(w.adsFrac() == 0.0f);
    bool ev = false;
    for (const auto& e : w.takeEvents()) ev = ev || e.kind == PlayerWeapon::Event::ReloadBegin;
    CHECK(ev);
    // A weapon without a scope overlay keeps its aim.
    PlayerWeapon p;
    p.equip("pistol", {{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0"}, {"aimdownsight", "1"},
                       {"adstransintime", "0.1"}, {"adstransouttime", "0.5"}});
    in.damaged = false;
    for (int i = 0; i < 30; ++i) p.update(0.01f, in);
    in.damaged = true;
    p.update(0.01f, in);
    CHECK(p.adsFrac() == 1.0f);
}

// adsFire (+0x348, 0x4dea00 / 0x4dd1b2): a hip press starts the fire state with weaponDelay = the remaining aim time; the aim
// is held and the shot comes when it is complete. A non-segmented reload releases the aim only in its last adsReloadTransTime.
void adsFire() {
    PlayerWeapon w;
    w.equip("launcher", {{"weapontype", "projectile"}, {"firetime", "0.8"}, {"clipsize", "1"}, {"startammo", "4"}, {"raisetime", "0"},
                         {"reloadtime", "4"}, {"aimdownsight", "1"}, {"adsfire", "1"}, {"adstransintime", "0.5"},
                         {"adstransouttime", "0.2"}, {"adsreloadtranstime", "0.5"}});
    run(w, 0.05f, false);
    int shots = run(w, 0.01f, true);
    CHECK(shots == 0 && w.clip() == 1); // the press alone does not shoot
    shots = run(w, 0.3f, false);
    CHECK(shots == 0 && w.adsFrac() > 0.5f && w.adsFrac() < 1.0f);
    shots = 0;
    float aimAtShot = 0;
    for (int i = 0; i < 30; ++i)
        if (const int n = w.update(0.01f, {}); n) shots += n, aimAtShot = w.adsFrac();
    CHECK(shots == 1 && w.clip() == 0 && aimAtShot > 0.999f); // the aim finished, then the shot
    PlayerWeapon r;
    r.equip("rifle", {{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0"}, {"reloadtime", "4"},
                      {"reloademptytime", "4"}, {"aimdownsight", "1"}, {"adstransintime", "0.1"}, {"adstransouttime", "0.1"},
                      {"adsreloadtranstime", "0.5"}});
    PlayerWeapon::Input in;
    in.ads = true;
    for (int i = 0; i < 20; ++i) r.update(0.01f, in);
    in.fire = true;
    r.update(0.01f, in);
    in.fire = false;
    for (int i = 0; i < 20; ++i) r.update(0.01f, in);
    in.reload = true;
    r.update(0.01f, in);
    in.reload = false;
    for (int i = 0; i < 100; ++i) r.update(0.01f, in);
    CHECK(r.adsFrac() == 0.0f); // aim released while reloading
    for (int i = 0; i < 270; ++i) r.update(0.01f, in);
    CHECK(r.adsFrac() > 0.9f);  // back in the last 0.5 s
}

int main() {
    scopeExitOnDamage();
    adsFire();
    adsExitDelay();
    reloadAddTiming();
    segmentedReload();
    weaponSounds();
    boltAction();
    boltBrass();
    projectileReload();
    automatic();
    autoReload();
    weaponChange();
    melee();
    offhand();
    binoculars();
    inventoryAmmo();
    if (failures) return 1;
    std::cout << "playerweapon tests passed\n";
    return 0;
}
