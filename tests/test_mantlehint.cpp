// The mantle hint flag (docs/spec/pmove.md "Mantle hint"): every move step drops it whatever the move type, and a
// prediction of a client held by a turret (eFlags 0x300) drops it too.
#include "game/collision.h"
#include "game/pmove.h"
#include "netgame/netmove.h"

#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

int main() {
    using namespace iw2;
    game::CollisionWorld w;
    // Every move type (walking, linked 1, noclip 2, ufo 3, dead 4, intermission 5) drops the hint bit; the other bits stay.
    for (int type : {0, 1, 2, 3, 4, 5}) {
        game::PlayerState ps;
        ps.pmType = type;
        ps.mantleFlags = 8 | 2;
        game::PlayerInput in;
        in.dead = type == 4;
        game::playerMove(w, ps, in, 0.05f);
        CHECK((ps.mantleFlags & 8) == 0);
        CHECK((ps.mantleFlags & 2) == 2);
    }
    {
        game::PlayerState ps;
        ps.mantleFlags = 8;
        game::MoveParams mp;
        mp.mpSpectator = true;
        game::playerMove(w, ps, game::PlayerInput{}, 0.05f, mp);
        CHECK((ps.mantleFlags & 8) == 0);
    }
    // A client on a turret: the carried prediction state loses the hint bit (the move is not run for it).
    {
        game::PlayerState carry;
        carry.mantleFlags = 8;
        netgame::PsFields server;
        server.eFlags = 0x300;
        const game::PlayerState ps = netgame::predictPlayer(w, server, {}, &carry, nullptr, -1);
        CHECK((ps.mantleFlags & 8) == 0);
    }
    {
        game::PlayerState carry;
        carry.mantleFlags = 8;
        netgame::PsFields server; // not on a turret: no commands, nothing runs, the state is carried as it is
        const game::PlayerState ps = netgame::predictPlayer(w, server, {}, &carry, nullptr, -1);
        CHECK((ps.mantleFlags & 8) != 0);
    }
    if (failures) return 1;
    std::cout << "mantlehint: ok\n";
    return 0;
}
