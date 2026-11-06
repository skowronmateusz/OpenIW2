// Bullet passes through the World (docs/spec/weapons.md, "Bullet passes") and the clip-only ammo rule, only with IW2_MAIN=<game main dir>.
#include "fs/vfs.h"
#include "game/world.h"
#include "script/vm.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "bullet pass test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();
    std::vector<Entity*> actors;
    Entity* player = nullptr;
    for (const auto& e : w.entities()) {
        if (e->isPlayer) player = e.get();
        if (e->isActor && !e->isSpawner) actors.push_back(e.get());
    }
    CHECK(player && actors.size() >= 2);
    if (!player || actors.size() < 2) return 1;
    Entity &a = *actors[0], &b = *actors[1];

    // A rifle bullet through two actors in line: both are hit, the second with half the damage.
    const Vec3 ao = a.obj->get("origin").asVector();
    const Vec3 bo{ao[0] + 40.0f, ao[1], ao[2]};
    b.obj->set("origin", Value(bo));
    w.frame();
    a.obj->set("health", Value(int32_t(1000)));
    b.obj->set("health", Value(int32_t(1000)));
    player->obj->set("$weapon", a.obj->get("weapon"));
    // Hit boxes follow the pose, so a grid of parallel shots looks for one line through both.
    int through = 0, ordered = 0;
    for (float dz = 10.0f; dz <= 60.0f; dz += 5.0f)
        for (float dy = -8.0f; dy <= 8.0f; dy += 2.0f) {
            const int ha = a.obj->get("health").asInt(), hb = b.obj->get("health").asInt();
            const Vec3 ac = a.obj->get("origin").asVector(), bc = b.obj->get("origin").asVector();
            const Vec3 along{(bc[0] - ac[0]) / 40.0f, (bc[1] - ac[1]) / 40.0f, 0.0f};
            const Vec3 from{ac[0] - along[0] * 60.0f - along[1] * dy, ac[1] - along[1] * 60.0f + along[0] * dy, ac[2] + dz};
            const Vec3 to{from[0] + along[0] * 400.0f, from[1] + along[1] * 400.0f, from[2]};
            Entity* first = w.debugFireBullet(from, to);
            const int da = ha - a.obj->get("health").asInt(), db = hb - b.obj->get("health").asInt();
            if (first == &a && db > 0) {
                ++through;
                ordered += db < da * 3 / 4 + 1;
            }
            CHECK(first == &a || da == 0);
        }
    // A killing shot goes on too: the dying actor keeps its sentient.
    int lethal = 0;
    for (float dz = 10.0f; dz <= 60.0f && !lethal; dz += 5.0f)
        for (float dy = -8.0f; dy <= 8.0f && !lethal; dy += 2.0f) {
            const Vec3 ac = a.obj->get("origin").asVector(), bc = b.obj->get("origin").asVector();
            const Vec3 along{(bc[0] - ac[0]) / 40.0f, (bc[1] - ac[1]) / 40.0f, 0.0f};
            const Vec3 from{ac[0] - along[0] * 60.0f - along[1] * dy, ac[1] - along[1] * 60.0f + along[0] * dy, ac[2] + dz};
            const Vec3 to{from[0] + along[0] * 400.0f, from[1] + along[1] * 400.0f, from[2]};
            a.obj->set("health", Value(int32_t(1)));
            const int hb = b.obj->get("health").asInt();
            if (w.debugFireBullet(from, to) == &a && b.obj->get("health").asInt() < hb) lethal = 1;
            else a.obj->set("health", Value(int32_t(1)));
        }
    CHECK(lethal == 1);
    std::cout << "rifle shots through both actors " << through << ", second one weaker " << ordered << "\n";
    CHECK(through > 0 && ordered > 0);

    // A shot at the player: near the axis it can hit, 12 units beside it never does.
    const Vec3 po = player->obj->get("origin").asVector();
    int onAxis = 0, beside = 0;
    Entity* gun = nullptr; // a shooter that is no actor, so team rules stay out
    for (const auto& e : w.entities())
        if (!e->isActor && !e->isPlayer && !e->isSpawner && !gun) gun = e.get();
    CHECK(gun != nullptr);
    if (!gun) return 1;
    for (float dz = 10.0f; dz <= 60.0f; dz += 5.0f) {
        for (float dy : {-4.0f, 0.0f, 4.0f})
            onAxis += w.debugFireBulletFrom(*gun, {po[0] - 100.0f, po[1] + dy, po[2] + dz}, {po[0] + 100.0f, po[1] + dy, po[2] + dz}) == player;
        for (float dy : {-12.0f, 12.0f})
            beside += w.debugFireBulletFrom(*gun, {po[0] - 100.0f, po[1] + dy, po[2] + dz}, {po[0] + 100.0f, po[1] + dy, po[2] + dz}) == player;
    }
    std::cout << "player hits on the axis " << onAxis << ", beside it " << beside << "\n";
    CHECK(onAxis > 0 && beside == 0);
    // A clip-only weapon keeps its rounds in the clip: added ammo fills it, no stock remains.
    Inventory& inv = w.inventory(*player);
    const auto* def = w.weapon("mk1britishfrag");
    CHECK(def != nullptr);
    if (!def) return 1;
    const std::string frag = "mk1britishfrag", fragAmmo = def->at("ammoname"), fragClip = def->at("clipname");
    inv.ammo[fragAmmo] = 0;
    inv.clip[fragClip] = 0;
    w.debugAddAmmo(*player, frag, 3);
    CHECK(inv.clip[fragClip] == 3 && inv.ammo[fragAmmo] == 0);
    w.debugAddAmmo(*player, frag, 9);
    CHECK(inv.clip[fragClip] == std::atoi(def->at("clipsize").c_str()) && inv.ammo[fragAmmo] == 0);
    if (failures == 0) std::cout << "bullet pass ok\n";
    return failures ? 1 : 0;
}
