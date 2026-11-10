// The weapon side of a network player on the server (docs/spec/net.md section 13): the user command's buttons and
// weapon drive the same weapon state machine as the local player (game::PlayerWeapon), and its shots, melee blows,
// grenade throws and cooked grenades go through the same World functions (World::clientFire and friends), so damage,
// death, obituaries and script callbacks are the local player's. The entity events of the shots, reloads and throws
// go to the other clients in the player's entity event ring.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/playerweapon.h"
#include "game/pmove.h"
#include "net/gameclient.h"
#include "netgame/netmove.h"

namespace iw2::game {
class World;
struct Entity;
}

namespace iw2::netgame {

// Entity event numbers (the event-name table of the 1.3 Linux dedicated server at 0x8183da0, index = event number).
enum NetEvent : int {
    kEvNoAmmo = 0x92,
    kEvReload = 0x96,
    kEvReloadFromEmpty = 0x97,
    kEvRaiseWeapon = 0x9a,
    kEvPutawayWeapon = 0x9b,
    kEvFireWeapon = 0x9e,
    kEvFireWeaponLastShot = 0xa0,
    kEvFireWeaponMg42 = 0xaf,   // a turret's shot, on the turret entity (parm: the gunner's entity number)
    kEvRechamberWeapon = 0xa1,
    kEvMeleeSwipe = 0xa3,
    kEvFireMelee = 0xa4,
    kEvPrepOffhand = 0xa5,
    kEvUseOffhand = 0xa6,
    kEvGrenadeSuicide = 0xc5,
};

// The entity event ring of a player (entity state eventSequence +164, events[4] +168, eventParms[4] +184).
struct EventRing {
    int sequence = 0;
    std::array<int, 4> events{};
    std::array<int, 4> parms{};
    void push(int event, int parm) {
        events[size_t(sequence & 3)] = event;
        parms[size_t(sequence & 3)] = parm & 0xff;
        sequence = (sequence + 1) & 0xff;
    }
};

// The movement inputs of a weapon state (aim allowed, aim fraction, state, move speed scale and hip spread on top of the
// default movement parameters): the server's for its clients and the client's prediction use the same function.
WeaponMove weaponMoveOf(const game::PlayerWeapon& w);

class NetCombat {
public:
    // The weapon part of one user command for MP client `client` (its entity resolved through the world every call, so a
    // round restart that rebuilds the world's entities does not leave stale pointers). ps: the movement state after the
    // command; cmdWeapon: the command's weapon byte as a weapon name ("" = none / unknown). dt in seconds.
    void think(game::World& w, int client, const net::UserCmd& cmd, game::PlayerState& ps, float dt,
               const std::string& cmdWeapon);
    // The movement inputs from this weapon (aim allowed, aim fraction, state, move speed scale and hip spread).
    WeaponMove move() const { return weaponMoveOf(weapon_); }
    const game::PlayerWeapon& weapon() const { return weapon_; }
    const EventRing& ring() const { return ring_; }
    int shots() const { return shots_; }  // total shots fired (tests)
    // weapon index of an event parameter (set by the owner: the weapon configstring index of a name).
    std::function<int(const std::string&)> weaponIndex;
    // The client's antilag time for its shots (the owner sets it before think; -1: the current positions).
    int antilagTime = -1;

private:
    void wire(game::World& w, int client);
    void reset();
    game::PlayerWeapon weapon_;
    bool wired_ = false;
    std::string select_, written_, lastCmdWeapon_;
    uint32_t oldButtons_ = 0;
    EventRing ring_;
    int shots_ = 0;
    int statePrev_ = 0; // the weapon state after the previous think (the offhand prepare event is the entry into 13)
};

} // namespace iw2::netgame
