// Killcam with bots (docs/spec/mp-hud.md "Killcam"): a known bot kills the local client at time T; the killcam must
// follow that bot (its client, found by entity number) and its replay must reach the kill (the archived frame time
// passes T before the killcam ends). Needs the game files (IW2_MAIN).

#include "game/mpbots.h"
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

using iw2::game::Entity;
using iw2::game::World;

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

} // namespace

// Real combat: the bots fight; every death of the local client by a bot is checked the same way.
void combat(iw2::fs::Vfs& vfs, const char* map, int nbots, int seconds) {
    World w(vfs, map, true);
    w.mpStart("dm");
    const int me = w.mpConnect("victim");
    w.mpBegin(me);
    run(w, 1);
    w.mpMenuResponse(me, "team_britishgerman", "autoassign");
    run(w, 1);
    const std::string menu = w.mpState().clients[size_t(me)].openMenu;
    w.mpMenuResponse(me, menu, menu == "weapon_german" ? "mp40_mp" : "thompson_mp");
    run(w, 2);
    for (int i = 0; i < nbots; ++i) {
        w.mpAddBot();
        run(w, 1);
    }
    Entity* p = w.mpClientEntity(me);
    std::string prev = field(p, "sessionstate");
    int64_t killMs = -1, lastFrameMs = -1;
    int killer = -1, deaths = 0, killFrames = 0, lastMod = -1, killMod = -1;
    iw2::script::Vec3 victimEye{};
    for (int i = 0; i < 20 * seconds; ++i) {
        run(w, 1);
        for (const auto& ob : w.takeMpObituaries())
            if (ob.victim == me) lastMod = ob.mod;
        const std::string s = field(p, "sessionstate");
        if (prev == "playing" && s == "dead") {
            killMs = w.timeMs() - 50;
            victimEye = p->obj->get("origin").asVector();
            killer = p->lastAttacker ? p->lastAttacker->number : -1;
            killMod = lastMod;
            lastFrameMs = -1;
            ++deaths;
        }
        const World::MpKillcamView v = w.mpKillcamView(1.0f);
        if (v.active) {
            if (v.killer != killer) std::cout << "combat: killcam follows " << v.killer << ", last attacker " << killer << "\n";
            CHECK(v.killer == killer);
            lastFrameMs = v.frameTimeMs;
            // The killer's player state is the HUD's: its weapon and rounds; a shot shows as a fire age.
            CHECK(!v.weapon.empty() && v.client >= 0 && v.maxHealth > 0);
            // A bullet kill (MOD_PISTOL_BULLET, MOD_RIFLE_BULLET, MOD_HEAD_SHOT): a grenade has no shot to replay.
            if (v.frameTimeMs == killMs && (killMod == 1 || killMod == 2 || killMod == 8)) {
                // The killing shot is in the replayed frames: a recent fire age and its fire sound archived with the frame.
                CHECK(v.fireAgeMs >= 0 && v.fireAgeMs < 1000);
                bool sound = false;
                for (const auto& f : w.mpArchive()->frames)
                    if (f.timeMs > killMs - 1000 && f.timeMs <= killMs)
                        for (const auto& ev : f.events) sound |= ev.kind == iw2::game::ArchivedEvent::Sound && ev.owner == killer;
                CHECK(sound);
                // HUD elements: an archived one of the killer is shown, the victim's own archived one is not, the
                // victim's non-archived one (the killcam HUD) is.
                auto elem = [&](int owner, bool archived) {
                    auto h = w.vm().newObject("hudelem");
                    h->set("$owner", iw2::script::Value(int32_t(owner)));
                    if (!archived) h->set("archived", iw2::script::Value(int32_t(0)));
                    return h;
                };
                CHECK(w.mpHudElemVisible(elem(v.client, true)));
                CHECK(!w.mpHudElemVisible(elem(me, true)));
                CHECK(w.mpHudElemVisible(elem(me, false)));
                CHECK(!w.mpHudElemVisible(elem(v.client, false)));
            }
            if (v.frameTimeMs == killMs) { // the kill frame: the killer looks at the victim
                ++killFrames;
                const float yaw = v.angles[1] * 3.14159265f / 180.0f;
                const auto to = victimEye;
                const float dx = to[0] - v.eye[0], dy = to[1] - v.eye[1];
                float d = std::atan2(dy, dx) * 180.0f / 3.14159265f - v.angles[1];
                d = std::remainder(d, 360.0f);
                std::cout << "combat: kill frame eye " << v.eye[0] << " " << v.eye[1] << " " << v.eye[2] << " angles "
                          << v.angles[0] << " " << v.angles[1] << " yaw off by " << d << " deg\n";
                (void)yaw;
                CHECK(std::fabs(d) < 20.0f);
                // The victim is in the archived frame, drawn (playing, not hidden), with a model and an animation.
                const auto* ar = w.mpArchive();
                const iw2::game::ArchivedEntity* ve = nullptr;
                for (const auto& f : ar->frames)
                    if (f.timeMs == killMs) ve = f.find(p->number);
                std::cout << "combat: victim archived " << (ve != nullptr) << " model '" << (ve ? ve->model : "") << "' state "
                          << (ve ? ve->sessionstate : "") << " anim nodes " << (ve ? ve->anim.size() : 0) << " hidden " << (ve && ve->hidden) << "\n";
                CHECK(ve && !ve->hidden && !ve->model.empty() && ve->sessionstate == "playing" && !ve->anim.empty());
            }
        }
        if (prev == "spectator" && s != "spectator") {
            std::cout << "combat: kill at " << killMs << " by " << killer << ", last killcam frame " << lastFrameMs << "\n";
            CHECK(lastFrameMs >= killMs);
        }
        prev = s;
        if (s == "dead") { // respawn at once (use press)
            World::PlayerCommand c;
            c.buttons = (i & 1) ? 0x8u : 0u;
            w.setPlayerCommand(c);
        }
    }
    std::cout << "combat: " << deaths << " deaths, " << killFrames << " kill frames replayed\n";
    CHECK(deaths >= 1);
    CHECK(killFrames >= 1);
}

// Every client (bots too) killed by another: when its killcam starts, spectatorclient is the attacker's entity number
// and, in the archived frame of the kill, the attacker's eye looks at the victim.
void allVictims(iw2::fs::Vfs& vfs, const char* map, int nbots, int seconds) {
    World w(vfs, map, true);
    w.mpStart("dm");
    const int me = w.mpConnect("player");
    w.mpBegin(me);
    run(w, 1);
    w.mpMenuResponse(me, "team_britishgerman", "autoassign");
    run(w, 1);
    const std::string menu = w.mpState().clients[size_t(me)].openMenu;
    w.mpMenuResponse(me, menu, menu == "weapon_german" ? "mp40_mp" : "thompson_mp");
    run(w, 2);
    for (int i = 0; i < nbots; ++i) {
        w.mpAddBot();
        run(w, 1);
    }
    struct Death { int attacker = -1; int64_t ms = 0; iw2::script::Vec3 at{}; bool checked = true; bool bullet = false; };
    std::vector<std::string> prev(w.mpState().clients.size());
    std::vector<Death> d(prev.size());
    int killcams = 0, bad = 0, aimBad = 0;
    std::vector<std::string> how(prev.size());
    std::vector<int> shots(prev.size(), 0);
    std::vector<int64_t> shotMs(prev.size(), -1);
    for (int f = 0; f < 20 * seconds; ++f) {
        run(w, 1);
        for (size_t c = 0; c < prev.size(); ++c)
            if (const auto* st = w.mpBotStats(int(c)); st && st->shots != shots[c]) shots[c] = st->shots, shotMs[c] = w.timeMs();
        for (const auto& ob : w.takeMpObituaries())
            if (ob.victim >= 0 && size_t(ob.victim) < how.size()) how[size_t(ob.victim)] = ob.modByte ? "mod " + std::to_string(ob.mod) : ob.weapon;
        for (size_t c = 0; c < prev.size(); ++c) {
            Entity* e = w.mpState().clients[c].ent;
            if (!e || w.mpState().clients[c].state == iw2::game::MpClientState::Free) continue;
            const std::string s = field(e, "sessionstate");
            if (prev[c] == "playing" && s == "dead") {
                d[c] = Death{e->lastAttacker && e->lastAttacker != e && e->lastAttacker->isPlayer ? e->lastAttacker->number : -1,
                             w.timeMs() - 50, e->obj->get("origin").asVector(), false};
                // A bullet kill: the killer fired in this frame (a grenade kill shows the thrower's view, turned away).
                if (const int kc = w.mpClientOfNumber(d[c].attacker); kc >= 0) d[c].bullet = shotMs[size_t(kc)] == w.timeMs();
            }
            const iw2::script::Value sc = e->obj->get("spectatorclient");
            if (s == "spectator" && !d[c].checked && sc.isNumber() && sc.asInt() >= 0) {
                d[c].checked = true;
                ++killcams;
                if (sc.asInt() != d[c].attacker) {
                    ++bad;
                    std::cout << map << ": client " << c << " killcam of " << sc.asInt() << ", killed by " << d[c].attacker << "\n";
                }
                // The killer looked at the victim (within 25 degrees) in an archived frame of the last second before the
                // death: the bot fires only with its view on the target, so its killcam shows the victim.
                const int kc = w.mpClientOfNumber(sc.asInt());
                float best = 360.0f, dist = 0;
                for (const auto& fr : w.mpArchive()->frames)
                    if (fr.timeMs <= d[c].ms && fr.timeMs >= d[c].ms - 1000 && kc >= 0 && size_t(kc) < fr.clients.size() &&
                        fr.clients[size_t(kc)].active) {
                        const auto& k = fr.clients[size_t(kc)];
                        const iw2::game::ArchivedEntity* ve = fr.find(e->number);
                        if (!ve) continue;
                        const float off = std::remainder(
                            std::atan2(ve->origin[1] - k.eye[1], ve->origin[0] - k.eye[0]) * 180.0f / 3.14159265f - k.viewAngles[1], 360.0f);
                        if (std::fabs(off) < best) best = std::fabs(off), dist = std::hypot(ve->origin[0] - k.eye[0], ve->origin[1] - k.eye[1]);
                    }
                // A grenade kill in the frame the killer also fired at someone else is not a bullet kill (the obituary's weapon).
                if (how[c].find("grenade") != std::string::npos) d[c].bullet = false;
                if (d[c].bullet && best > 25.0f) {
                    ++aimBad;
                    std::cout << map << ": client " << c << " killer " << sc.asInt() << " (" << how[c] << ") at best " << best << " deg off at " << dist << " units, death " << d[c].ms << " victim entity " << e->number << "\n";
                }
            }
            prev[c] = s;
        }
    }
    std::cout << map << ": " << killcams << " killcams, " << bad << " wrong killer, " << aimBad << " not facing the victim\n";
    CHECK(killcams > 0);
    CHECK(bad == 0);
    CHECK(aimBad == 0);
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    combat(vfs, "mp_toujane", 3, 1800); // a standing victim is met by chance: long enough for it to be found
    allVictims(vfs, "mp_carentan", 7, 600);

    for (int pick = 0; pick < 3; ++pick) {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int me = w.mpConnect("victim");
        w.mpBegin(me);
        run(w, 1);
        w.mpMenuResponse(me, "team_britishgerman", "autoassign");
        run(w, 1);
        const std::string menu = w.mpState().clients[size_t(me)].openMenu;
        w.mpMenuResponse(me, menu, menu == "weapon_german" ? "mp40_mp" : "thompson_mp");
        run(w, 2);
        int bots[3];
        for (int& b : bots) {
            b = w.mpAddBot();
            w.mpBotSetCombat(b, false); // only the debug kill below kills
            run(w, 1);
        }
        run(w, 300); // 15 s of history, the bots playing and moving
        Entity* p = w.mpClientEntity(me);
        const int killer = bots[pick];
        Entity* k = w.mpClientEntity(killer);
        CHECK(field(p, "sessionstate") == "playing" && field(k, "sessionstate") == "playing");
        const int killerNumber = k ? k->number : -1;
        const int64_t killMs = w.timeMs();
        const auto po = p->obj->get("origin").asVector(), ko = k->obj->get("origin").asVector();
        w.mpDebugDamage(me, killer, 1000, "MOD_RIFLE_BULLET");
        // LookAtKiller (0x810160a): the dead yaw faces the attacker, truncated to whole degrees in [0, 360).
        {
            float want = std::atan2(ko[1] - po[1], ko[0] - po[0]) * 180.0f / 3.14159265f;
            if (want < 0) want += 360.0f;
            CHECK(w.playerDeadYaw() && *w.playerDeadYaw() == std::trunc(*w.playerDeadYaw()));
            CHECK(w.playerDeadYaw() && std::fabs(*w.playerDeadYaw() - want) <= 1.0f);
        }
        bool seen = false, wrong = false;
        int64_t lastFrameMs = -1;
        for (int i = 0; i < 400 && field(p, "sessionstate") != "playing"; ++i) {
            run(w, 1);
            const World::MpKillcamView v = w.mpKillcamView(1.0f);
            if (!v.active) continue;
            seen = true;
            if (v.killer != killerNumber) wrong = true;
            lastFrameMs = v.frameTimeMs;
            if (field(p, "sessionstate") == "dead") break;
        }
        CHECK(seen);
        CHECK(!wrong);
        // The replay ends 2 s after the kill (_killcam.gsc: archivetime 9 s, waits archivetime - 0.05 s from 2 s after
        // the death), so its last frame is after the kill.
        std::cout << "pick " << pick << ": kill at " << killMs << " ms, last killcam frame " << lastFrameMs << " ms\n";
        CHECK(lastFrameMs >= killMs);
        for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
    }
    { // a suicide: the dead yaw is the victim's own yaw
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int me = w.mpConnect("victim");
        w.mpBegin(me);
        run(w, 1);
        w.mpMenuResponse(me, "team_britishgerman", "autoassign");
        run(w, 1);
        const std::string menu = w.mpState().clients[size_t(me)].openMenu;
        w.mpMenuResponse(me, menu, menu == "weapon_german" ? "mp40_mp" : "thompson_mp");
        run(w, 2);
        Entity* p = w.mpClientEntity(me);
        p->obj->set("angles", iw2::script::Value(iw2::script::Vec3{0, 123.7f, 0}));
        w.mpDebugDamage(me, me, 1000, "MOD_SUICIDE");
        CHECK(w.playerDeadYaw() && *w.playerDeadYaw() == 123.0f);
    }
    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
