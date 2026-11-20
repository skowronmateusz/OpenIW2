// Multiplayer mounted MG: the fire loop and the stop sound of a gunner that is not the local client (a bot, a network
// client) are the turret's own loopFireSound / stopFireSound, played at the turret on a positional channel, so they
// fade with the distance. Only the gunner this process shows plays the player variants (channel "local", no position)
// (docs/spec/turrets.md "Multiplayer"). Needs the game files (IW2_MAIN).

#include "audio/sound.h"
#include "fs/vfs.h"
#include "game/world.h"

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

using iw2::game::Entity;
using iw2::game::World;
using iw2::script::Value;
using V3 = iw2::script::Vec3;

struct Played {
    std::string alias;
    bool positioned = false;
    V3 at{0, 0, 0};
};

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

float dist(const V3& a, const V3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

void gunner(iw2::fs::Vfs& vfs, const iw2::audio::SoundAliases& al, bool local) {
    World w(vfs, "mp_carentan", true);
    w.mpStart("dm");
    const int c0 = w.mpConnect("local");
    w.mpBegin(c0);
    run(w, 1);
    w.mpAutoJoin(c0);
    const int c1 = w.mpConnect("gunner");
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
    const V3 o = t->obj->get("origin").asVector(), a = t->obj->get("angles").asVector();
    const float yaw = a[1] * float(M_PI) / 180.0f;
    const V3 fwd{std::cos(yaw), std::sin(yaw), 0};
    const V3 behind{o[0] - fwd[0] * 34.0f, o[1] - fwd[1] * 34.0f, o[2] - 20.0f};
    std::vector<Played> plays;
    w.playSound = [&plays](const std::string& alias, const iw2::game::Vec3* at, float*, int) {
        plays.push_back({alias, at != nullptr, at ? *at : V3{0, 0, 0}});
        return int(plays.size());
    };
    if (local) {
        p0->obj->set("origin", Value(behind));
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
        run(w, 10);
        c.buttons = 0;
        w.setPlayerCommand(c);
        run(w, 20);
    } else {
        p0->obj->set("origin", Value(V3{o[0] - fwd[0] * 3000.0f, o[1] - fwd[1] * 3000.0f, o[2]}));
        p1->obj->set("origin", Value(behind));
        w.mpSetView(c1, V3{0, a[1], 0}, 60.0f);
        w.mpSetButtons(c1, 0);
        w.frame();
        w.mpSetButtons(c1, 8);
        w.frame();
        w.mpSetButtons(c1, 0);
        w.frame();
        CHECK(w.mpClientOnTurret(c1));
        w.mpSetButtons(c1, 1);
        run(w, 10);
        w.mpSetButtons(c1, 0);
        run(w, 20);
    }
    bool loop = false, stop = false;
    for (const Played& p : plays) {
        const bool isLoop = p.alias.rfind("weap_mg42_loop", 0) == 0, isStop = p.alias.rfind("weap_mg42_cooldown", 0) == 0;
        if (!isLoop && !isStop) continue;
        (isLoop ? loop : stop) = true;
        const bool plrVariant = p.alias.size() > 4 && p.alias.compare(p.alias.size() - 4, 4, "_plr") == 0;
        std::cout << (local ? "local" : "other") << " gunner: " << p.alias << (p.positioned ? " at the turret" : " 2d") << "\n";
        CHECK(plrVariant == local);
        if (!local) {
            // At the turret, on a channel that attenuates: a listener farther than dist_max hears nothing.
            CHECK(p.positioned && dist(p.at, o) < 1.0f);
            std::mt19937 rng(1);
            const iw2::audio::SoundAlias* sa = al.pick(p.alias, rng);
            CHECK(sa && sa->channel != "local" && sa->distMax > sa->distMin && sa->distMax < 20000.0f);
            CHECK(dist(o, p0->obj->get("origin").asVector()) > (sa ? sa->distMax : 1e9f) * 0.2f);
        }
    }
    CHECK(loop && stop);
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
    iw2::audio::SoundAliases al;
    al.loadLevel(vfs, "mp_carentan", "all_mp");
    gunner(vfs, al, false);
    gunner(vfs, al, true);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
