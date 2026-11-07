// Scoped aim and a mounted turret (docs/spec/weapons.md "Aim on a turret"): the player aims a scoped rifle, gets on an
// MG42 and off again. On the turret the aim state is kept (PM_Weapon returns early, so the view frame does not advance
// the hand weapon), the scope overlay is not drawn and the view fov is 55; after getting off the aimed fov and the
// overlay come back while the aim is still held. Needs the game data (IW2_MAIN=<game main dir>); the pure rules are
// checked without it.
#include "fs/vfs.h"
#include "game/playerweapon.h"
#include "game/savestate.h"
#include "game/viewweapon.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

// The view's choices as iw2view makes them each frame (0x4ae7b0 / 0x4bc8a0 / 0x4bd760).
struct View {
    float fov;
    float overlay;
};
View viewOf(const PlayerWeapon& pw, bool onTurret) {
    const float zoom = pw.number("adsZoomFov") > 0 ? pw.number("adsZoomFov") : 80.0f;
    const float fov = playerViewFov(adsFov(80.0f, zoom, pw.number("adsZoomInFrac"), pw.number("adsZoomOutFrac"), pw.adsFrac(),
                                           pw.adsZoomingIn()), onTurret);
    const bool overlay = !pw.field("adsOverlayShader").empty();
    const float a = scopeOverlayAlpha(overlay, pw.adsFrac(), pw.number(pw.adsZoomingIn() ? "adsZoomInFrac" : "adsZoomOutFrac"));
    return {fov, scopeOverlayDrawAlpha(a, onTurret)};
}
} // namespace

int main() {
    // Pure rules.
    CHECK(playerViewFov(20.0f, true) == 55.0f);
    CHECK(playerViewFov(20.0f, false) == 20.0f);
    CHECK(playerViewFov(80.0f, true) == 55.0f);
    CHECK(scopeOverlayAlpha(true, 1.0f, 0.05f) == 1.0f);
    CHECK(scopeOverlayAlpha(false, 1.0f, 0.05f) == 0.0f);
    CHECK(scopeOverlayAlpha(true, 0.0f, 0.05f) == 0.0f);
    CHECK(scopeOverlayAlpha(true, 0.5f, 0.05f) == 0.0f);
    CHECK(scopeOverlayDrawAlpha(1.0f, true) == 0.0f);
    CHECK(scopeOverlayDrawAlpha(0.7f, false) == 0.7f);

    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "turret ads world test skipped (IW2_MAIN not set)\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 20; ++i) w.frame();

    PlayerWeapon pw;
    pw.lookup = [&](const std::string& n) { return w.weapon(n); };
    const auto* def = w.weapon("kar98k_sniper");
    CHECK(def != nullptr);
    if (!def) return 1;
    pw.equip("kar98k_sniper", *def);
    CHECK(!pw.field("adsOverlayShader").empty());
    PlayerWeapon::Input aim;
    aim.ads = true;
    for (int i = 0; i < 200; ++i) pw.update(0.02f, aim);
    CHECK(pw.adsFrac() == 1.0f);
    const View aimed = viewOf(pw, false);
    std::cout << "aimed fov " << aimed.fov << " overlay " << aimed.overlay << "\n";
    CHECK(aimed.fov < 55.0f);
    CHECK(aimed.overlay > 0.99f);

    Entity* t = nullptr;
    for (const auto& e : w.entities())
        if (e->turret && !e->obj->removed && !t && e->turret->weapon == "mg42_bipod_stand") t = e.get();
    CHECK(t != nullptr);
    if (!t) return 1;
    const Vec3 o = t->obj->get("origin").asVector(), a = t->obj->get("angles").asVector();
    const float yaw = a[1] * float(M_PI) / 180.0f;
    w.player()->obj->set("origin", Value(Vec3{o[0] - std::cos(yaw) * 34.0f, o[1] - std::sin(yaw) * 34.0f, o[2] - 20.0f}));
    World::PlayerCommand c;
    c.viewAngles = {0, a[1], 0};
    auto press = [&](uint32_t b) {
        c.buttons = b;
        w.setPlayerCommand(c);
        w.frame();
    };
    press(0);
    press(8);
    press(0);
    CHECK(w.playerOnTurret());

    // A save taken on the turret (the decoytown checkpoint on an MG42): after the load the player is still on it, with the
    // turret bound (view locked to its arcs, gun fire and getting off work), not in a turret view without a turret.
    {
        SaveRequest req;
        req.name = "turret";
        req.description = "on a turret";
        const auto file = buildSaveFile(w, req);
        LoadedSave ls;
        CHECK(parseSaveFile(file, ls).empty());
        World b(vfs, ls.info.mapName);
        b.prepareStart();
        restoreSave(b, ls);
        CHECK(b.playerOnTurret());
        CHECK(b.playerTurretWeapon() == w.playerTurretWeapon() && !b.playerTurretWeapon().empty());
        float pitch = 77, yaw = a[1] + 170;
        b.clampPlayerView(pitch, yaw);
        CHECK(pitch != 77); // the view is held inside the turret's arcs again
        float tp = 0, ty = 0;
        CHECK(b.playerTurretAnglesFor({0, a[1], 0}, tp, ty));
        b.setPlayerCommand(c);
        for (int i = 0; i < 5; ++i) b.frame();
        CHECK(b.playerOnTurret());
        auto pressB = [&](uint32_t bt) {
            c.buttons = bt;
            b.setPlayerCommand(c);
            b.frame();
        };
        pressB(0);
        pressB(8);
        pressB(0);
        CHECK(!b.playerOnTurret()); // the use key gets off the restored turret
        c.buttons = 0;
    }

    // An engaged target with aim flag 0x40 left on the turret (an AI gunner used it first, decoytown) does not bend the
    // player's shots: 0x51b250 passes aim flag 0 for a client user, so the shot goes along the barrel (docs/spec/turrets.md).
    {
        TurretState& ts = *t->turret;
        Entity* other = nullptr;
        for (const auto& e : w.entities())
            if (!other && e->obj->kind == "actor" && !e->obj->removed) other = e.get();
        ts.engaged = (other ? other : w.player())->obj;
        ts.flags |= 0x40u;
        const float ay = (a[1] + ts.yaw + 10.0f) * float(M_PI) / 180.0f; // 10 degrees off the barrel: inside the 0.95 cone
        ts.aimPoint = {o[0] + std::cos(ay) * 500.0f, o[1] + std::sin(ay) * 500.0f, o[2]};
        w.takeFxEvents();
        int flashes = 0;
        for (int i = 0; i < 6; ++i) {
            c.buttons = 1;
            w.setPlayerCommand(c);
            w.frame();
            for (const auto& fx : w.takeFxEvents()) {
                if (fx.entity != t->number || fx.tag != "tag_flash") continue;
                ++flashes;
                const float p = (a[0] + ts.pitch) * float(M_PI) / 180.0f, y = (a[1] + ts.yaw) * float(M_PI) / 180.0f;
                const float d = fx.forward[0] * std::cos(p) * std::cos(y) + fx.forward[1] * std::cos(p) * std::sin(y) - fx.forward[2] * std::sin(p);
                CHECK(d > 0.9999f);
            }
        }
        c.buttons = 0;
        w.setPlayerCommand(c);
        CHECK(flashes > 0);
        CHECK(w.playerOnTurret());
    }

    // On the turret: no hand-weapon step (iw2view skips pweapon.update, as PM_Weapon returns at 0x4dfc59).
    for (int i = 0; i < 20; ++i) {
        w.frame();
        if (!w.playerOnTurret()) pw.update(0.05f, aim);
    }
    CHECK(w.playerOnTurret());
    CHECK(pw.adsFrac() == 1.0f);
    const View mounted = viewOf(pw, w.playerOnTurret());
    std::cout << "mounted fov " << mounted.fov << " overlay " << mounted.overlay << "\n";
    CHECK(mounted.fov == 55.0f);
    CHECK(mounted.overlay == 0.0f);

    // Off again: the kept aim gives the aimed view back while aim is held.
    press(8);
    press(0);
    CHECK(!w.playerOnTurret());
    pw.update(0.02f, aim);
    const View off = viewOf(pw, w.playerOnTurret());
    std::cout << "dismounted fov " << off.fov << " overlay " << off.overlay << "\n";
    CHECK(off.fov == aimed.fov);
    CHECK(off.overlay > 0.99f);
    return failures ? 1 : 0;
}
