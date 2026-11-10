// Killcam (docs/spec/mp-hud.md "Killcam"): the archive lookup and trimming rule, setarchive, the DM killcam from the
// killer's view (spectator of the killer, archivetime 9 s, back to dead), trimming to a short history, skipping with
// the use button, and the archived frame the front end draws. The DM part needs the game files (IW2_MAIN).

#include "game/world.h"

#include <cmath>
#include <utility>
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

using iw2::game::ArchivedFrame;
using iw2::game::Entity;
using iw2::game::MpArchive;
using iw2::game::World;
using iw2::script::Value;

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
int ifield(Entity* e, const char* f) {
    const Value v = e ? e->obj->get(f) : Value();
    return v.isNumber() ? v.asInt() : -9999;
}
float ffield(Entity* e, const char* f) {
    const Value v = e ? e->obj->get(f) : Value();
    return v.isNumber() ? v.asFloat() : -9999.0f;
}
// The local client's use button (+activate 8) comes from its usercmd.
void use(World& w, bool down) {
    World::PlayerCommand c;
    c.buttons = down ? 0x8u : 0u;
    w.setPlayerCommand(c);
    w.setPlayerCommand(c); // usebuttonpressed also sees the previous command's buttons
}
void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

MpArchive archiveOf(int frames) {
    MpArchive a;
    a.enabled = true;
    for (int i = 0; i < frames; ++i) {
        a.frames.push_back(ArchivedFrame{});
        a.frames.back().timeMs = i * 50;
        ++a.count;
        while (a.frames.size() > size_t(MpArchive::kFrames)) a.frames.pop_front();
    }
    return a;
}

void lookupRule() {
    using iw2::game::archiveLookup;
    // Off, or a time of 0: no frame and the time untouched.
    MpArchive off = archiveOf(100);
    off.enabled = false;
    int ms = 9000;
    CHECK(archiveLookup(off, ms, 20) == -1 && ms == 9000);
    MpArchive a = archiveOf(400);
    ms = 0;
    CHECK(archiveLookup(a, ms, 20) == -1 && ms == 0);
    // 9 s at 20 frames per second: 180 frames back, the time kept.
    ms = 9000;
    CHECK(archiveLookup(a, ms, 20) == 400 - 180 && ms == 9000);
    // Longer than the history: from frame 0, the time cut to 400 frames = 20 s.
    ms = 30000;
    CHECK(archiveLookup(a, ms, 20) == 0 && ms == 20000);
    // More than 1200 frames back: clamped to 1200 frames (60 s).
    MpArchive big = archiveOf(2000);
    ms = 90000;
    CHECK(archiveLookup(big, ms, 20) == 2000 - 1200 && ms == 60000);
    // An empty archive: no frame, the time becomes 0.
    MpArchive empty = archiveOf(0);
    ms = 5000;
    CHECK(archiveLookup(empty, ms, 20) == -1 && ms == 0);
}

// Joins a client to DM with a weapon.
void join(World& w, int c) {
    w.mpBegin(c);
    run(w, 1);
    w.mpMenuResponse(c, "team_britishgerman", "autoassign");
    run(w, 1);
    const std::string menu = w.mpState().clients[size_t(c)].openMenu;
    w.mpMenuResponse(c, menu, menu == "weapon_german" ? "mp40_mp" : "thompson_mp");
    run(w, 2);
}

} // namespace

int main() {
    { // the replay range of the archived events and the fire age (src/game/mpkillcam.cpp)
        using iw2::game::killcamReplayRange;
        CHECK(killcamReplayRange(-1, 10) == std::make_pair(int64_t(10), int64_t(10))); // a new killcam: only the shown frame
        CHECK(killcamReplayRange(7, 10) == std::make_pair(int64_t(8), int64_t(10)));  // frames since the last replay
        CHECK(killcamReplayRange(10, 10).first > killcamReplayRange(10, 10).second);  // nothing new
        CHECK(killcamReplayRange(20, 10) == std::make_pair(int64_t(10), int64_t(10))); // jumped back
        CHECK(killcamReplayRange(3, -1).first > killcamReplayRange(3, -1).second);    // not in a killcam
        iw2::game::MpArchive a;
        a.enabled = true;
        for (int n = 0; n < 10; ++n) {
            iw2::game::ArchivedFrame f;
            f.timeMs = n * 50;
            f.clients.resize(2);
            f.clients[1].active = true;
            f.clients[1].fireCount = n < 4 ? 0 : n < 7 ? 1 : 2; // shots in frames 4 and 7
            a.frames.push_back(f);
            ++a.count;
        }
        CHECK(iw2::game::killcamFireAge(a, 9, 1) == 100);
        CHECK(iw2::game::killcamFireAge(a, 7, 1) == 0);
        CHECK(iw2::game::killcamFireAge(a, 6, 1) == 100);
        CHECK(iw2::game::killcamFireAge(a, 3, 1) == -1);
        CHECK(iw2::game::killcamFireAge(a, 9, 1, 2) == -1); // beyond the search window
        CHECK(iw2::game::killcamFireAge(a, 9, 0) == -1);    // an inactive client
    }

    lookupRule();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, DM part skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);

    World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    // _killcam.gsc:14-15: scr_killcam defaults to 1 and turns the archive on.
    CHECK(w.mpArchive() && w.mpArchive()->enabled);
    const int c0 = w.mpConnect("victim"); // the local client
    join(w, c0);
    const int c1 = w.mpConnect("killer");
    join(w, c1);
    Entity* p0 = w.mpClientEntity(c0);
    Entity* p1 = w.mpClientEntity(c1);
    CHECK(field(p0, "sessionstate") == "playing" && field(p1, "sessionstate") == "playing");

    // 11 s of history; the killcam starts 2 s after the kill and shows 9 s back, i.e. 7 s before the kill: the killer
    // stands at a known spot from 8 s to 5 s before the kill.
    run(w, 60);
    const iw2::script::Vec3 early{100.0f, 200.0f, 300.0f};
    p1->obj->set("origin", Value(early));
    run(w, 60);
    p1->obj->set("origin", Value(iw2::script::Vec3{-500.0f, -600.0f, 50.0f}));
    run(w, 100);
    w.mpDebugDamage(c0, c1, 500, "MOD_RIFLE_BULLET");
    run(w, 1);
    CHECK(field(p0, "sessionstate") == "dead");
    run(w, 41); // dm.gsc:379-380: the 2 s delay, then the killcam's one-frame wait
    CHECK(field(p0, "sessionstate") == "spectator");
    CHECK(ifield(p0, "spectatorclient") == p1->number);
    CHECK(std::fabs(ffield(p0, "archivetime") - 9.0f) < 0.001f); // enough history: not trimmed
    CHECK(p0->obj->get("killcam").truthy());
    // The killcam HUD (_killcam.gsc:71-159): bars, title, skip text and timer of this client.
    int own = 0;
    for (const auto& h : w.hudElems())
        if (h->get("$owner").isNumber() && h->get("$owner").asInt() == c0) ++own;
    CHECK(own >= 5);
    // The front end's view: the killer's archived eye 9 s back (the spot it stood at then), not its live position.
    {
        const World::MpKillcamView v = w.mpKillcamView(1.0f);
        CHECK(v.active);
        CHECK(v.killer == p1->number);
        CHECK(v.weapon == field(p1, "$weapon"));
        CHECK(std::fabs(v.eye[0] - early[0]) < 0.5f && std::fabs(v.eye[1] - early[1]) < 0.5f && v.eye[2] > early[2]);
        // The archived frame swapped in for drawing: the killer hidden, the local body drawn where it stood.
        const auto liveOrigin = p0->obj->get("origin").asVector();
        CHECK(w.mpKillcamApply(1.0f));
        CHECK(p1->obj->get("$hidden").truthy());
        CHECK(w.mpPlayerDrawn(*p0));
        w.mpKillcamRestore();
        CHECK(!p1->obj->get("$hidden").truthy());
        CHECK(!w.mpPlayerDrawn(*p0));
        CHECK(p0->obj->get("origin").asVector() == liveOrigin);
        CHECK(p1->obj->get("origin").asVector()[0] == -500.0f);
    }
    // waitKillcamTime (_killcam.gsc:183): over after archivetime - 0.05 s, back to dead.
    run(w, 185);
    CHECK(field(p0, "sessionstate") == "dead");
    CHECK(ifield(p0, "spectatorclient") == -1);
    CHECK(!w.mpKillcamView(1.0f).active);
    CHECK(!p0->obj->get("killcam").isDefined());

    // Respawn and a second kill: the use button skips the killcam (a new press, _killcam.gsc:192-198).
    use(w, true);
    run(w, 3);
    use(w, false);
    run(w, 3);
    CHECK(field(p0, "sessionstate") == "playing");
    run(w, 20);
    w.mpDebugDamage(c0, c1, 500, "MOD_RIFLE_BULLET");
    run(w, 42);
    CHECK(field(p0, "sessionstate") == "spectator");
    use(w, true);
    run(w, 2);
    use(w, false);
    run(w, 2);
    // dm.gsc:383 passes the respawn flag: the same press ends the killcam and respawns.
    CHECK(field(p0, "sessionstate") == "playing");
    CHECK(ifield(p0, "spectatorclient") == -1);

    // A short history: a fresh world killed 3 s after the start has archivetime trimmed to the archived frames, a
    // world without the archive (scr_killcam 0) skips the killcam.
    for (const bool killcam : {true, false}) {
        World s(vfs, "mp_toujane", true);
        if (!killcam) s.cvars["scr_killcam"] = "0";
        s.mpStart("dm");
        const int v = s.mpConnect("victim");
        join(s, v);
        const int k = s.mpConnect("killer");
        join(s, k);
        Entity* pv = s.mpClientEntity(v);
        s.mpDebugDamage(v, k, 500, "MOD_RIFLE_BULLET");
        run(s, 42);
        if (killcam) {
            const float at = ffield(pv, "archivetime");
            const float archived = float(s.mpArchive()->count) * 0.05f;
            CHECK(field(pv, "sessionstate") == "spectator");
            CHECK(at > 2.0f && at < 9.0f && at <= archived); // the history at the trimming frame
        } else {
            CHECK(!s.mpArchive()->enabled);
            CHECK(field(pv, "sessionstate") == "dead");
            CHECK(ffield(pv, "archivetime") == 0.0f);
        }
        for (const auto& e : s.vm().errors()) std::cerr << "  script error: " << e << "\n";
    }

    for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
    CHECK(w.vm().errors().empty());
    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
