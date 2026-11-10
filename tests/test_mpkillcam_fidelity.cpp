// Killcam fidelity with bots (docs/spec/mp-hud.md "Killcam", docs/spec/mp-bots.md): what the killcam shows at an
// archived server time must be what the killer did at that time, and the victim must be in every replayed frame.
// Needs the game files (IW2_MAIN).

#include "game/mpbots.h"
#include "game/mpkillcam.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
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
using iw2::script::Vec3;

struct Sample { // what a bot really did in a server frame (its controller state after the frame)
    Vec3 origin{};
    float pitch = 0, yaw = 0;
    int fireCount = 0, spawnCount = 0;
    bool ads = false, playing = false;
    Vec3 victim{};
    bool victimPlaying = false;
    iw2::game::MpViewState view; // the recorded view state (docs/spec/mp-hud.md "The recorded view state")
};

float angleDiff(float a, float b) { return std::fabs(std::remainder(a - b, 360.0f)); }
// A yaw difference scaled to its effect on the view direction at the views' pitch (cos of the steeper pitch).
float yawTurn(float yaw1, float pitch1, float yaw2, float pitch2) {
    return angleDiff(yaw1, yaw2) * std::cos(std::max(std::fabs(pitch1), std::fabs(pitch2)) * 3.14159265f / 180.0f);
}

void join(World& w, int me) {
    w.mpBegin(me);
    w.frame();
    w.mpMenuResponse(me, "team_britishgerman", "autoassign");
    w.frame();
    const std::string menu = w.mpState().clients[size_t(me)].openMenu;
    w.mpMenuResponse(me, menu, menu == "weapon_german" ? "mp40_mp" : "thompson_mp");
    w.frame();
    w.frame();
}

// Real fights: every killcam of the local client is compared frame by frame with the recorded bot state.
int gBodyFrames = 0, gBodyDrawn = 0; // frames after the victim's death with its body, and those that draw it
struct Totals { int killcams = 0, frames = 0, prevFrames = 0, victimFrames = 0, bodyFrames = 0, shots = 0, lateShots = 0; float maxTurn = 0; };

void fights(iw2::fs::Vfs& vfs, const char* map, int nbots, int seconds, Totals& tot) {
    World w(vfs, map, true);
    w.mpStart("dm");
    const int me = w.mpConnect("victim");
    join(w, me);
    w.mpSetBotSkill(3); // veterans: the standing victim dies often
    for (int i = 0; i < nbots; ++i) {
        w.mpAddBot();
        w.frame();
    }
    Entity* p = w.mpClientEntity(me);
    std::map<int64_t, std::map<int, Sample>> rec; // server time -> client -> sample
    int killcams = 0, frames = 0, prevFrames = 0, victimFrames = 0, bodyFrames = 0, shots = 0, lateShots = 0;
    int64_t prevShown = -1;
    float maxTurn = 0;
    for (int i = 0; i < 20 * seconds; ++i) {
        w.frame();
        if (p->obj->get("sessionstate").toString() == "dead") { // after the killcam: respawn (a use press)
            World::PlayerCommand c;
            c.buttons = (i & 1) ? 0x8u : 0u;
            w.setPlayerCommand(c);
        }
        auto& fr = rec[w.timeMs()];
        for (size_t c = 0; c < w.mpState().clients.size(); ++c) {
            const iw2::game::MpBot* bp = w.mpBot(int(c));
            Entity* e = bp ? w.mpClientEntity(int(c)) : nullptr;
            if (!bp || !e) continue;
            const iw2::game::MpBot& b = *bp;
            Sample s;
            s.origin = e->obj->get("origin").asVector();
            s.pitch = b.pitch;
            s.yaw = b.yaw;
            s.fireCount = w.mpState().clients[size_t(b.client)].fireCount;
            s.spawnCount = w.mpState().clients[size_t(b.client)].spawnCount;
            s.playing = e->obj->get("sessionstate").toString() == "playing";
            s.ads = s.playing && b.weapon.adsHeld(); // the recorded view state's aim flag (docs/spec/mp-hud.md "The recorded view state")
            if (s.playing) s.view = iw2::game::mpViewStateOf(b.weapon, b.ps); // a dead bot records nothing
            s.victim = p->obj->get("origin").asVector();
            s.victimPlaying = p->obj->get("sessionstate").toString() == "playing";
            fr[b.client] = s;
        }
        if (rec.size() > 400) rec.erase(rec.begin());
        for (const float lerp : {0.0f, 0.5f, 1.0f}) {
            const World::MpKillcamView v = w.mpKillcamView(lerp);
            if (!v.active) {
                prevShown = -1;
                continue;
            }
            if (lerp == 1.0f) {
                if (prevShown < 0) ++killcams;
                ++frames;
            }
            const auto ft = rec.find(v.frameTimeMs);
            const auto pt = rec.find(v.frameTimeMs - 50);
            const auto pt2 = rec.find(v.frameTimeMs - 100); // the spawn script moves the entity one frame before the controller restarts
            if (ft == rec.end() || !ft->second.count(v.client)) continue; // older than the record kept here
            const Sample& s = ft->second.at(v.client);
            const Sample* ps = pt != rec.end() && pt->second.count(v.client) ? &pt->second.at(v.client) : nullptr;
            if (lerp == 1.0f) {
                // The camera is the bot's eye and view of that frame, its HUD its fire count and aim.
                CHECK(std::fabs(v.eye[0] - s.origin[0]) < 0.01f && std::fabs(v.eye[1] - s.origin[1]) < 0.01f);
                CHECK(v.eye[2] > s.origin[2] + 10.0f && v.eye[2] <= s.origin[2] + 60.01f);
                CHECK(angleDiff(v.angles[0], s.pitch) < 0.01f && angleDiff(v.angles[1], s.yaw) < 0.01f);
                CHECK(v.fireCount == s.fireCount && v.ads == s.ads);
                // The killcam view carries the bot's recorded view state of that frame, whole (weapon state, animation,
                // aim, bob, velocity, heights).
                CHECK(v.view == s.view);
                if (!(v.view == s.view))
                    std::cout << "view state differs at " << v.frameTimeMs << ":\n  killcam " << iw2::game::mpViewStateText(v.view) << "\n  bot     "
                              << iw2::game::mpViewStateText(s.view) << "\n";
                if (ps) {
                    ++prevFrames;
                    const float moved = std::hypot(s.origin[0] - ps->origin[0], s.origin[1] - ps->origin[1]);
                    const bool spawned = s.spawnCount != ps->spawnCount ||
                                         (pt2 != rec.end() && pt2->second.count(v.client) && pt2->second.at(v.client).spawnCount != s.spawnCount);
                    if (s.playing && ps->playing && !spawned && moved < 64.0f) { // a new life (or a moved bot) restarts with the spawn's angles
                        // The yaw and the pitch are limited separately; the yaw step counts by its effect on the view direction
                        // (a bot standing over its target swings its yaw by tens of degrees for a few degrees of view).
                        const float turn = std::max(yawTurn(s.yaw, s.pitch, ps->yaw, ps->pitch), angleDiff(s.pitch, ps->pitch));
                        if (turn > 30.51f)
                            std::cout << map << ": client " << v.client << " turned " << turn << " deg in the frame at " << v.frameTimeMs << " (yaw " << ps->yaw << " -> " << s.yaw
                                      << ", pitch " << ps->pitch << " -> " << s.pitch << ", shots " << ps->fireCount << " -> " << s.fireCount << ")\n";
                        maxTurn = std::max(maxTurn, turn);
                    }
                    // A shot of this frame: the fire age counts from the start of the interval the view draws toward it,
                    // when its events are replayed (0 at lerp 0, 50 at lerp 1).
                    if (s.fireCount != ps->fireCount) {
                        ++shots;
                        CHECK(v.fireAgeMs == 50);
                        if (w.mpKillcamView(0.0f).fireAgeMs != 0) ++lateShots;
                    }
                }
                // The victim is in every frame of its life, drawn in third person at its archived place.
                if (s.victimPlaying && v.frameTimeMs < w.timeMs()) {
                    ++victimFrames;
                    w.mpKillcamApply(1.0f);
                    const bool drawn = w.mpPlayerDrawn(*p) && !p->obj->get("$hidden").truthy();
                    const Vec3 o = p->obj->get("origin").asVector();
                    if (drawn && std::fabs(o[0] - s.victim[0]) < 0.01f && std::fabs(o[1] - s.victim[1]) < 0.01f) ++bodyFrames;
                    w.mpKillcamRestore();
                }
                // After its death the victim is its body, drawn from the archived frame even when the queue reused the slot since.
                if (!s.victimPlaying)
                    if (const iw2::game::ArchivedFrame* af = w.mpArchive()->frame(v.frame))
                        for (const iw2::game::ArchivedEntity& ae : af->entities) {
                            if (ae.bodySlot < 0 || ae.bodyClient != w.mpLocalClient()) continue;
                            ++gBodyFrames;
                            w.mpKillcamApply(1.0f);
                            for (const auto& e : w.entities())
                                if (e->number == ae.number && !e->obj->removed && !e->obj->get("$hidden").truthy() && e->anim) ++gBodyDrawn;
                            w.mpKillcamRestore();
                        }
            } else if (ps && lerp == 0.5f) { // the midpoint between the two frames
                CHECK(std::fabs(v.eye[0] - 0.5f * (s.origin[0] + ps->origin[0])) < 0.05f);
                CHECK(angleDiff(v.angles[1], ps->yaw + std::remainder(s.yaw - ps->yaw, 360.0f) * 0.5f) < 0.05f);
            }
            if (lerp == 1.0f) prevShown = v.frameTimeMs;
        }
    }
    std::cout << map << ": " << killcams << " killcams, " << frames << " frames, " << prevFrames << " with the frame before, victim in "
              << bodyFrames << "/" << victimFrames << " frames, " << shots << " shots (" << lateShots << " late), largest view turn per frame "
              << maxTurn << " deg\n";
    CHECK(bodyFrames == victimFrames);
    CHECK(lateShots == 0);
    // No view snap: a bot turns at most its turn rate per frame (the veteran's 450 deg/s is 22.5 per frame) plus the
    // shot's correction of up to 8 degrees in the frame it fires, on ladders too.
    CHECK(maxTurn <= 30.51f);
    tot.killcams += killcams, tot.frames += frames, tot.prevFrames += prevFrames, tot.victimFrames += victimFrames;
    tot.bodyFrames += bodyFrames, tot.shots += shots, tot.lateShots += lateShots, tot.maxTurn = std::max(tot.maxTurn, maxTurn);
    for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
}

// Bots that climb and mantle without fighting: the view (the killcam camera) turns to a ladder or mantle link at the
// turn rate, the move takes the link at once.
void ladders(iw2::fs::Vfs& vfs) {
    World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    const int me = w.mpConnect("victim");
    join(w, me);
    int bots[3];
    for (int& b : bots) {
        b = w.mpAddBot();
        w.mpBotSetCombat(b, false);
        w.frame();
    }
    std::map<int, std::pair<float, float>> prevView;
    float maxStep = 0;
    int frames = 0, traverses = 0;
    for (int i = 0; i < 20 * 400; ++i) {
        w.frame();
        for (const int bot : bots) {
            const iw2::game::MpBot* b = w.mpBot(bot);
            if (!b || !b->psValid) continue;
            if (const auto pv = prevView.find(bot); pv != prevView.end()) {
                ++frames;
                maxStep = std::max(maxStep, std::max(angleDiff(b->yaw, pv->second.first), std::fabs(b->pitch - pv->second.second)));
            }
            if (b->traverseSinceMs >= 0) ++traverses;
            prevView[bot] = {b->yaw, b->pitch};
        }
    }
    std::cout << "ladders: largest view step " << maxStep << " deg, " << traverses << " traversing frames\n";
    CHECK(frames > 1000 && traverses > 0);
    CHECK(maxStep <= 30.51f);
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
    ladders(vfs);
    Totals tot;
    fights(vfs, "mp_toujane", 7, 400, tot);
    // TODO: a second map; the standing victim is rarely found on the larger maps (carentan, dawnville) within 10 minutes.
    CHECK(tot.killcams >= 3 && tot.frames > 300 && tot.prevFrames > 300);
    std::cout << "own body after death: " << gBodyFrames << " frames, drawn " << gBodyDrawn << "\n";
    CHECK(gBodyFrames > 0 && gBodyDrawn == gBodyFrames);
    CHECK(tot.victimFrames > 100 && tot.shots > 10);
    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
