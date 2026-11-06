// trainyard (only with IW2_MAIN): the pipe the player crawls through after the bunker (trigger_inpipe). Its rings
// (misc_model railpipe_connector at y -3995 / -3275 / -2684) are static models: bullets (point traces) hit their collision
// surfaces, the player's movement box does not see them, as in the original (docs/spec/collision.md, "Static models").
// The steel towers that carry the pipe block the player through the map's clip brushes.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/clientinput.h"
#include "game/pmove.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "pipe collision test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "trainyard");
    const CollisionWorld& c = w.collision();
    const uint32_t playerMask = 0x281c011;
    int fails = 0;
    for (float ringY : {-3995.0f, -3275.0f, -2684.0f}) {
        bool found = false;
        for (size_t i = 0; i < c.staticModelCount(); ++i) {
            const StaticModel& m = c.staticModel(i);
            if (std::fabs(m.origin[0] + 610) < 1 && std::fabs(m.origin[1] - ringY) < 1 && std::fabs(m.origin[2] - 384) < 1) found = true;
        }
        // A bullet from the pipe axis out to the wall below the axis meets the ring (radius 30) before the pipe (32).
        const TraceResult b = c.trace({-610, ringY + 4, 384}, {-610 + 80 * 0.866f, ringY + 4, 384 - 80 * 0.5f}, {0, 0, 0}, {0, 0, 0},
                                      Contents::BulletMask, -1, false);
        const bool bulletHitsRing = b.fraction < 1 && b.material == -1 && b.entity == -1 && (b.contents & 1);
        // A crouching player box sliding along the pipe floor through the ring: only world brushes / patches stop it.
        const TraceResult p = c.trace({-610, ringY - 60, 362}, {-610, ringY + 60, 362}, {-15, -15, 0}, {15, 15, 50}, playerMask, -1, false);
        const bool boxIgnoresRing = !(p.fraction < 1 && p.material == -1);
        std::cout << "ring y " << ringY << ": static model " << found << ", bullet hit " << bulletHitsRing << ", box fraction "
                  << p.fraction << " material " << p.material << "\n";
        if (!found || !bulletHitsRing || !boxIgnoresRing) ++fails;
    }
    // Crouched in the pipe with a stand wish (a released +movedown): one blocked-rise event (0x8d parm 1), which turns the client
    // stance to crouch, and no more while walking on.
    {
        ClientInput input;
        PlayerState ps;
        ps.origin = {-610, -3000, 362};
        ps.stance = Stance::Crouch;
        ps.viewHeightTarget = 40;
        int blocked = 0, seen = 0;
        for (int i = 0; i < 240; ++i) {
            PlayerInput in;
            in.forward = 1;
            in.yaw = 90;
            in.stance = input.stance() == 1 ? Stance::Crouch : input.stance() == 2 ? Stance::Prone : Stance::Stand;
            playerMove(c, ps, in, 1.0f / 60.0f);
            for (; seen < ps.eventSeq; ++seen) {
                const int ev = ps.events[seen & 3];
                if (ev == 0x8d && ps.eventParms[seen & 3] == 1) ++blocked;
                input.stanceEvent(ev);
            }
        }
        std::cout << "blocked rise events " << blocked << ", client stance " << input.stance() << "\n";
        if (blocked != 1 || input.stance() != 1 || ps.stance != Stance::Crouch) ++fails;
    }
    // Tower legs (prop_pipetower_angleiron, no collision surfaces): a player walking into them is stopped by a clip brush.
    for (float legY : {-4023.0f, -3303.0f}) {
        const TraceResult t = c.trace({-610, legY - 80, 60}, {-610, legY + 20, 60}, {-15, -15, 0}, {15, 15, 70}, playerMask, -1, false);
        const bool clipped = t.fraction < 1 && t.material >= 0 && (t.contents & 0x10000);
        std::cout << "tower y " << legY << ": fraction " << t.fraction << " contents 0x" << std::hex << t.contents << std::dec << "\n";
        if (!clipped) ++fails;
    }
    if (fails) {
        std::cerr << "unexpected trainyard pipe collision\n";
        return 1;
    }
    return 0;
}
