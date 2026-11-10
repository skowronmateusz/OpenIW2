// Multiplayer mounted MG: a client on a map turret (misc_turret, mg42_bipod_*_mp) that fires kills a client in front of the
// barrel, and the obituary names the turret weapon, with every weapon class configuration (docs/spec/turrets.md, docs/spec/mp.md
// "Player damage"). Needs the game files (IW2_MAIN).

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

using iw2::game::Entity;
using iw2::game::World;
using iw2::script::Value;
using V3 = iw2::script::Vec3;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

// The local client mounts the first turret of mp_carentan and fires at a second client placed in front of the barrel.
void localKill(iw2::fs::Vfs& vfs, const std::string& classes) {
    World w(vfs, "mp_carentan", true);
    w.cvars["iw2_weaponclasses"] = classes;
    w.mpStart("dm");
    const int c0 = w.mpConnect("gunner");
    w.mpBegin(c0);
    run(w, 1);
    w.mpAutoJoin(c0);
    const int c1 = w.mpConnect("target");
    w.mpBegin(c1);
    run(w, 1);
    w.mpAutoJoin(c1);
    run(w, 5);
    Entity* p0 = w.mpClientEntity(c0);
    Entity* p1 = w.mpClientEntity(c1);
    CHECK(p0 && p1 && p0 == w.player());
    if (!p0 || !p1) return;
    Entity* t = nullptr;
    for (const auto& e : w.entities())
        if (e->turret && !e->obj->removed && !t) t = e.get();
    CHECK(t != nullptr);
    if (!t) return;
    const std::string tw = t->turret->weapon;
    const V3 o = t->obj->get("origin").asVector(), a = t->obj->get("angles").asVector();
    const float yaw = a[1] * float(M_PI) / 180.0f;
    const V3 fwd{std::cos(yaw), std::sin(yaw), 0};
    p0->obj->set("origin", Value(V3{o[0] - fwd[0] * 34.0f, o[1] - fwd[1] * 34.0f, o[2] - 20.0f}));
    World::PlayerCommand c;
    c.viewAngles = {0, a[1], 0};
    w.setPlayerCommand(c);
    w.frame();
    c.buttons = 8;
    w.setPlayerCommand(c);
    w.frame();
    c.buttons = 0;
    w.setPlayerCommand(c);
    w.frame();
    CHECK(w.playerOnTurret());
    c.buttons = 1;
    w.setPlayerCommand(c);
    const size_t obits = w.mpState().obituaryLog.size();
    bool killed = false;
    for (int f = 0; f < 20 * 5 && !killed; ++f) {
        if (p1->obj->get("sessionstate").toString() == "playing")
            p1->obj->set("origin", Value(V3{o[0] + fwd[0] * 200.0f, o[1] + fwd[1] * 200.0f, o[2] - 40.0f}));
        w.frame();
        const auto& log = w.mpState().obituaryLog;
        for (size_t i = obits; i < log.size(); ++i)
            if (log[i].victim == c1) {
                std::cout << classes << ": turret " << tw << " kill weapon " << log[i].weapon << " attacker " << log[i].attacker << "\n";
                CHECK(log[i].attacker == c0);
                CHECK(log[i].weapon == tw || (log[i].modByte && log[i].mod == 8)); // a head shot shows the head shot icon
                killed = true;
            }
    }
    if (!killed) std::cout << classes << ": no kill, target health " << p1->obj->get("health").toString() << "\n";
    CHECK(killed);
    CHECK(w.vm().errors().empty());
}


// A non-local (network) client: its usercmd buttons and view reach the world (mpSetButtons / mpSetView, as the network
// server's client think does); a use press behind the gun mounts it on the server, attack fires the turret, the kill names
// the turret weapon; a second press gets it off and frees the turret.
void remoteKill(iw2::fs::Vfs& vfs, const std::string& classes) {
    World w(vfs, "mp_carentan", true);
    w.cvars["iw2_weaponclasses"] = classes;
    w.mpStart("dm");
    const int c0 = w.mpConnect("local");
    w.mpBegin(c0);
    run(w, 1);
    w.mpAutoJoin(c0);
    const int c1 = w.mpConnect("gunner");
    w.mpBegin(c1);
    run(w, 1);
    w.mpAutoJoin(c1);
    const int c2 = w.mpConnect("target");
    w.mpBegin(c2);
    run(w, 1);
    w.mpAutoJoin(c2);
    run(w, 5);
    Entity* p0 = w.mpClientEntity(c0);
    Entity* p1 = w.mpClientEntity(c1);
    Entity* p2 = w.mpClientEntity(c2);
    CHECK(p0 && p1 && p2 && p1 != w.player());
    if (!p0 || !p1 || !p2) return;
    Entity* t = nullptr;
    for (const auto& e : w.entities())
        if (e->turret && !e->obj->removed && !t) t = e.get();
    CHECK(t != nullptr);
    if (!t) return;
    const std::string tw = t->turret->weapon;
    const V3 o = t->obj->get("origin").asVector(), a = t->obj->get("angles").asVector();
    const float yaw = a[1] * float(M_PI) / 180.0f;
    const V3 fwd{std::cos(yaw), std::sin(yaw), 0};
    const V3 behind{o[0] - fwd[0] * 34.0f, o[1] - fwd[1] * 34.0f, o[2] - 20.0f};
    p0->obj->set("origin", Value(V3{o[0] - fwd[0] * 600.0f, o[1] - fwd[1] * 600.0f, o[2]}));
    p1->obj->set("origin", Value(behind));
    w.mpSetView(c1, V3{0, a[1], 0}, 60.0f);
    w.mpSetButtons(c1, 0);
    w.frame();
    CHECK(!w.mpClientOnTurret(c1));
    w.mpSetButtons(c1, 8);
    w.frame();
    w.mpSetButtons(c1, 0);
    w.frame();
    CHECK(w.mpClientOnTurret(c1));
    CHECK(t->turret->inUse && t->turret->owner == p1->obj);
    CHECK(!w.playerOnTurret()); // the local player's state is untouched
    w.mpSetButtons(c1, 1);
    const size_t obits = w.mpState().obituaryLog.size();
    bool killed = false;
    for (int f = 0; f < 20 * 5 && !killed; ++f) {
        if (p2->obj->get("sessionstate").toString() == "playing")
            p2->obj->set("origin", Value(V3{o[0] + fwd[0] * 200.0f, o[1] + fwd[1] * 200.0f, o[2] - 55.0f}));
        w.frame();
        const auto& log = w.mpState().obituaryLog;
        for (size_t i = obits; i < log.size(); ++i)
            if (log[i].victim == c2) {
                std::cout << classes << ": remote turret " << tw << " kill weapon " << log[i].weapon << " mod " << log[i].mod << "\n";
                CHECK(log[i].attacker == c1);
                CHECK(log[i].weapon == tw || (log[i].modByte && log[i].mod == 8)); // a head shot shows the head shot icon
                killed = true;
            }
    }
    if (!killed) std::cout << classes << ": remote no kill, target health " << p2->obj->get("health").toString() << "\n";
    CHECK(killed);
    // Held behind the gun while on it.
    const V3 po = p1->obj->get("origin").asVector();
    CHECK(std::fabs(po[0] - (o[0] - fwd[0] * 34.0f)) < 1.0f && std::fabs(po[1] - (o[1] - fwd[1] * 34.0f)) < 1.0f);
    // Off again: use press, the turret is free.
    w.mpSetButtons(c1, 0);
    w.frame();
    w.mpSetButtons(c1, 8);
    w.frame();
    w.mpSetButtons(c1, 0);
    w.frame();
    CHECK(!w.mpClientOnTurret(c1));
    CHECK(!t->turret->inUse);
    // Mount again, then disconnect: the turret is freed.
    p1->obj->set("origin", Value(behind));
    w.mpSetButtons(c1, 8);
    w.frame();
    w.mpSetButtons(c1, 0);
    w.frame();
    CHECK(w.mpClientOnTurret(c1));
    w.mpDisconnect(c1);
    w.frame();
    CHECK(!t->turret->inUse);
    CHECK(w.vm().errors().empty());
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
    for (const char* classes : {"rifle", "smg", "mg", "sniper", "shotgun", "rifle,smg,mg,sniper,shotgun"}) {
        localKill(vfs, classes);
        remoteKill(vfs, classes);
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
