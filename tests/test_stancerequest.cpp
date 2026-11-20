// Stance requests (docs/spec/pmove.md "Stance requests", 0x509d10): the stances a script forbids and their replacements, the walking
// rules (ladder, prone only from the ground, the rise traced with the clip mask without the body bits and the player as pass entity)
// and the rules of a script-linked player (pm_type 1): stand and crouch follow the buttons with no trace, prone is never entered.
// With IW2_MAIN=<game main dir> the level part rides beltot's truck. The player's seat (tag_player) reaches into the clip brush model
// the map script links to the truck bed next to it (and the parked truck stands in a clip brush of the map), so a traced rise is
// refused there for most poses of the truck: a player who crouched on the truck could not stand up again, and the refusals left in
// the event ring raised the "cannot stand" hint after the ride.
#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/pmove.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

namespace {

PlayerState crouched(const Vec& origin, int pmType) {
    PlayerState ps;
    ps.pmType = pmType;
    ps.origin = origin;
    ps.onGround = pmType == 0;
    ps.stance = Stance::Crouch;
    ps.viewHeightTarget = 40;
    ps.viewHeight = 40;
    ps.timeMs = 100000;
    return ps;
}

PlayerInput ask(Stance s, uint32_t disallow = 0) {
    PlayerInput in;
    in.stance = s;
    in.stanceDisallow = disallow;
    return in;
}

// The stance events (0x8c..0x8e) among the events added since `from`, as event * 16 + parm of the last one; 0 for none.
int lastStanceEvent(const PlayerState& ps, int from) {
    int last = 0;
    for (int i = std::max(from, ps.eventSeq - 4); i < ps.eventSeq; ++i)
        if (ps.events[i & 3] >= 0x8c && ps.events[i & 3] <= 0x8e) last = ps.events[i & 3] * 16 + ps.eventParms[i & 3];
    return last;
}

bool same(const StanceRequest& r, Stance s, int event) { return r.stance == s && r.event == event; }

} // namespace

int main() {
    const Stance St = Stance::Stand, Cr = Stance::Crouch, Pr = Stance::Prone;
    {
        // Walking (0x509ebb..0x509f97): an allowed request is kept; a forbidden one is replaced and named by an event.
        CHECK(same(stanceRequestAllowed(St, Pr, 0, false), Pr, 0));
        CHECK(same(stanceRequestAllowed(St, Cr, kNoProne, false), Cr, 0));
        CHECK(same(stanceRequestAllowed(Cr, St, kNoProne | kNoCrouch, false), St, 0));
        // prone forbidden: stand when standing and allowed, else crouch when allowed, else stand
        CHECK(same(stanceRequestAllowed(St, Pr, kNoProne, false), St, 0x8c));
        CHECK(same(stanceRequestAllowed(Cr, Pr, kNoProne, false), Cr, 0x8d));
        CHECK(same(stanceRequestAllowed(St, Pr, kNoProne | kNoStand, false), Cr, 0x8d));
        CHECK(same(stanceRequestAllowed(Cr, Pr, kNoProne | kNoCrouch, false), St, 0x8c));
        // crouch forbidden: prone when standing and prone allowed, else stand when allowed, else prone
        CHECK(same(stanceRequestAllowed(St, Cr, kNoCrouch, false), Pr, 0x8e));
        CHECK(same(stanceRequestAllowed(Pr, Cr, kNoCrouch, false), St, 0x8c));
        CHECK(same(stanceRequestAllowed(St, Cr, kNoCrouch | kNoProne, false), St, 0x8c));
        CHECK(same(stanceRequestAllowed(Pr, Cr, kNoCrouch | kNoStand, false), Pr, 0x8e));
        // stand forbidden: prone when prone and allowed, else crouch when allowed, else prone
        CHECK(same(stanceRequestAllowed(Pr, St, kNoStand, false), Pr, 0x8e));
        CHECK(same(stanceRequestAllowed(Cr, St, kNoStand, false), Cr, 0x8d));
        CHECK(same(stanceRequestAllowed(Cr, St, kNoStand | kNoCrouch, false), Pr, 0x8e));
        // Linked (0x50a2c8..0x50a362): prone is always replaced, whatever allowprone says; the prone flag is not read.
        CHECK(same(stanceRequestAllowed(St, Pr, 0, true), St, 0x8c));
        CHECK(same(stanceRequestAllowed(Cr, Pr, 0, true), Cr, 0x8d));
        CHECK(same(stanceRequestAllowed(Cr, Pr, kNoCrouch, true), St, 0x8c));
        CHECK(same(stanceRequestAllowed(St, Pr, kNoStand, true), Cr, 0x8d));
        CHECK(same(stanceRequestAllowed(St, Cr, kNoProne, true), Cr, 0));
        CHECK(same(stanceRequestAllowed(St, Cr, kNoCrouch, true), St, 0x8c));
        CHECK(same(stanceRequestAllowed(Cr, St, kNoStand, true), Cr, 0x8d));
        CHECK(same(stanceRequestAllowed(Cr, St, kNoProne, true), St, 0));
    }
    {
        const CollisionWorld empty;
        // A linked player: crouch and stand follow the request; a prone request is given back to the client and changes nothing.
        PlayerState ps;
        ps.pmType = 1;
        ps.timeMs = 100000;
        playerMove(empty, ps, ask(Cr), 0.05f);
        CHECK(ps.stance == Cr && ps.eventSeq == 0);
        for (int i = 0; i < 10; ++i) playerMove(empty, ps, ask(Cr), 0.05f);
        int seq = ps.eventSeq;
        playerMove(empty, ps, ask(Pr), 0.05f);
        CHECK(ps.stance == Cr && lastStanceEvent(ps, seq) == 0x8d * 16);
        playerMove(empty, ps, ask(St), 0.05f);
        CHECK(ps.stance == St);
        for (int i = 0; i < 10; ++i) playerMove(empty, ps, ask(St), 0.05f);
        seq = ps.eventSeq;
        playerMove(empty, ps, ask(Pr), 0.05f);
        CHECK(ps.stance == St && lastStanceEvent(ps, seq) == 0x8c * 16);
        // allowcrouch(false) while linked: the crouch request becomes stand
        seq = ps.eventSeq;
        playerMove(empty, ps, ask(Cr, kNoCrouch), 0.05f);
        CHECK(ps.stance == St && lastStanceEvent(ps, seq) == 0x8c * 16);
        // The client handles the events of a linked step in that step. Left in the ring they are taken as new by the first step
        // that looks: this is what replayed the refused rises of a ride after the unlink.
        PlayerState held, lazy;
        held.pmType = lazy.pmType = 1;
        int heldSeen = 0, lazySeen = 0, heldHandled = 0, lazyHandled = 0;
        for (int i = 0; i < 10; ++i) {
            playerMove(empty, held, ask(Pr), 0.05f);
            forEachNewPlayerEvent(held, heldHandled, [&](int ev, int parm) { heldSeen += ev == 0x8c && parm == 0; });
            playerMove(empty, lazy, ask(Pr), 0.05f);
        }
        CHECK(heldSeen == 10);
        held.pmType = lazy.pmType = 0;
        playerMove(empty, held, ask(St), 0.05f);
        playerMove(empty, lazy, ask(St), 0.05f);
        heldSeen = 0;
        forEachNewPlayerEvent(held, heldHandled, [&](int, int) { ++heldSeen; });
        forEachNewPlayerEvent(lazy, lazyHandled, [&](int, int) { ++lazySeen; });
        CHECK(heldSeen == 0 && lazySeen == 4);
    }
    {
        const CollisionWorld empty;
        // Walking, in the air: a prone request does nothing (no ground entity), not even the first stage down to crouch.
        PlayerState air;
        air.timeMs = 100000;
        for (int i = 0; i < 5; ++i) playerMove(empty, air, ask(Pr), 0.02f);
        CHECK(air.stance == St && air.eventSeq == 0);
        // allowprone(false): the request is replaced by stand and the client told so, on every step that still asks.
        for (int i = 0; i < 3; ++i) playerMove(empty, air, ask(Pr, kNoProne), 0.02f);
        CHECK(air.stance == St && air.eventSeq == 3 && lastStanceEvent(air, 0) == 0x8c * 16);
        // On a ladder a crouch request is cleared with the stand event.
        PlayerState lad;
        lad.timeMs = 100000;
        lad.onLadder = true;
        playerMove(empty, lad, ask(Cr), 0.02f);
        CHECK(lad.stance == St && lastStanceEvent(lad, 0) == 0x8c * 16);
    }
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << (failures ? "FAILED\n" : "stance request test: rules OK, level part skipped (IW2_MAIN not set)\n");
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    {
        // The rise trace: mask = clip mask without the body bits (0x281c011 & 0xfdff3fff = 0x810011), pass entity = the player.
        // A roof over a crouched player far outside the map's brushes (entity clips need a loaded map).
        const auto bytes = vfs.read("maps/beltot.d3dbsp");
        CHECK(bytes.has_value());
        if (!bytes) return 1;
        const iw2::assets::D3dBsp bsp(*bytes);
        CollisionWorld cw(bsp);
        const Vec at{60000.0f, 60000.0f, 0.0f};
        auto riseUnder = [&](uint32_t roofContents, int roofId, int pmType, int& event) {
            EntityClip floor, roof;
            floor.id = 1000;
            floor.origin = at;
            floor.mins = {-400, -400, -64};
            floor.maxs = {400, 400, 0};
            floor.contents = Contents::Solid;
            roof.id = roofId;
            roof.origin = at;
            roof.mins = {-400, -400, 55};
            roof.maxs = {400, 400, 120};
            roof.contents = roofContents;
            cw.setEntityClips({floor, roof});
            PlayerState ps = crouched(at, pmType);
            MoveParams mp;
            mp.passEntity = 7;
            for (int i = 0; i < 3; ++i) playerMove(cw, ps, ask(Stance::Stand), 0.02f, mp);
            event = lastStanceEvent(ps, 0);
            return ps.stance == Stance::Stand;
        };
        int ev = 0;
        CHECK(!riseUnder(Contents::Solid, 1001, 0, ev) && ev == 0x8d * 16 + 1);
        CHECK(!riseUnder(Contents::PlayerClip, 1001, 0, ev) && ev == 0x8d * 16 + 1);
        CHECK(!riseUnder(0x800000u, 1001, 0, ev) && ev == 0x8d * 16 + 1);      // a vehicle's collmap contents are in the mask
        CHECK(riseUnder(Contents::PlayerBody, 1001, 0, ev) && ev == 0);         // bodies are not: 0x2000000,
        CHECK(riseUnder(Contents::ActorBody, 1001, 0, ev) && ev == 0);          // 0x4000
        CHECK(riseUnder(0x8000u, 1001, 0, ev) && ev == 0);                      // and 0x8000
        CHECK(riseUnder(Contents::PlayerClip, 7, 0, ev) && ev == 0);            // the pass entity
        CHECK(riseUnder(Contents::Solid, 1001, 1, ev) && ev == 0);              // a linked player is not traced at all
    }
    // beltot: the player climbs on the truck (the mount thread of the map script links it with playerlinktodelta to tag_player and
    // forbids prone), then the truck drives its path.
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_stancerequest_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/stancerequest.gsc")
        << "mount()\n{\n\ttruck = getent(\"truck\", \"targetname\");\n\tlevel.player setorigin(truck.origin + (0, -150, 20));\n\twait 0.2;\n"
           "\tlevel notify(\"activate truck\");\n\twait 0.5;\n\tgetent(\"mount_trigger\", \"targetname\") notify(\"trigger\", level.player);\n"
           "\tlevel waittill(\"player on truck\");\n\tlevel.test_mounted = 1;\n}\n"
           "ride()\n{\n\tlevel thread maps\\_vehicle::gopath(getent(\"truck\", \"targetname\"));\n}\n";
    vfs.mountDirectory(dir.string());
    {
        World w(vfs, "beltot");
        w.start();
        for (int i = 0; i < 20; ++i) w.frame();
        w.vm().start("maps/stancerequest", "mount", {}, iw2::script::Value(w.vm().level()));
        for (int i = 0; i < 20 * 60 && !w.vm().level()->get("test_mounted").truthy(); ++i) w.frame();
        CHECK(w.vm().level()->get("test_mounted").truthy());
        for (int i = 0; i < 5; ++i) w.frame();
        CHECK(w.playerHasLinkParent() && w.playerLinked());
        CHECK(w.playerPmType() == 1);
        CHECK((w.playerPmFlags() & kStanceDisallowMask) == kNoProne); // the mount thread's allowprone(false)
        MoveParams mp;
        mp.passEntity = w.player()->number;
        int samples = 0, solid = 0, stood = 0, refused = 0;
        auto sample = [&] {
            const auto o = w.player()->obj->get("origin").asVector();
            const Vec at{o[0], o[1], o[2]};
            ++samples;
            // The premise: the standing box at the seat starts solid for the mask of a walking rise.
            solid += w.collision().trace(at, at, mp.mins, mp.maxs, mp.traceMask & 0xfdff3fffu, mp.passEntity).startSolid;
            // The walking rules would refuse the rise there, with the event of the "cannot stand" hint ...
            PlayerState walk = crouched(at, 0);
            playerMove(w.collision(), walk, ask(Stance::Stand, w.playerPmFlags() & kStanceDisallowMask), 0.05f, mp);
            refused += walk.stance == Stance::Crouch && lastStanceEvent(walk, 0) == 0x8d * 16 + 1;
            // ... the linked player stands up, with no event.
            PlayerState ps = crouched(at, w.playerPmType());
            playerMove(w.collision(), ps, ask(Stance::Stand, w.playerPmFlags() & kStanceDisallowMask), 0.05f, mp);
            stood += ps.stance == Stance::Stand && ps.eventSeq == 0;
            // and a prone key gives the stance back to the client instead of lying down
            PlayerState pr = crouched(at, w.playerPmType());
            playerMove(w.collision(), pr, ask(Stance::Prone, w.playerPmFlags() & kStanceDisallowMask), 0.05f, mp);
            CHECK(pr.stance == Stance::Crouch && lastStanceEvent(pr, 0) == 0x8d * 16);
        };
        sample(); // parked
        CHECK(solid == 1 && refused == 1); // the premise: the walking rules refuse the rise at the parked seat
        w.vm().start("maps/stancerequest", "ride", {}, iw2::script::Value(w.vm().level()));
        const auto parked = w.player()->obj->get("origin").asVector();
        for (int i = 0; i < 20 * 20; ++i) {
            w.frame();
            w.player()->obj->set("$godmode", iw2::script::Value(1));
            if (i % 20 == 19 && w.playerHasLinkParent()) sample();
        }
        const auto now = w.player()->obj->get("origin").asVector();
        std::cout << "beltot truck: " << samples << " samples, standing box solid at " << solid << ", walking rise refused at " << refused
                  << ", linked rise done at " << stood << ", rode " << std::hypot(now[0] - parked[0], now[1] - parked[1]) << " units\n";
        CHECK(samples >= 15);
        CHECK(std::hypot(now[0] - parked[0], now[1] - parked[1]) > 1000.0f); // the truck did drive
        // The premise holds for most of the ride (how far the seat reaches into the clip model depends on the truck's pose) ...
        CHECK(solid * 2 > samples && refused * 2 > samples);
        CHECK(stood == samples); // ... and the linked player stands up at every sample
    }
    std::filesystem::remove_all(dir);
    std::cout << (failures ? "FAILED\n" : "stance request test OK\n");
    return failures ? 1 : 0;
}
