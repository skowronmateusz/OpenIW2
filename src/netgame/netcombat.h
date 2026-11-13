// The weapon side of a network player on the server (docs/spec/net.md section 13): the user command's buttons and
// weapon drive the same weapon state machine as the local player (game::PlayerWeapon), and its shots, melee blows,
// grenade throws and cooked grenades go through the same World functions (World::clientFire and friends), so damage,
// death, obituaries and script callbacks are the local player's. The entity events of the shots, reloads and throws
// go to the other clients in the player's entity event ring.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
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
    kEvSoundAlias = 0xb3,       // a sound at the entity (parm: the sound index; 0x811f32e), e.g. a turret's stop fire sound
    kEvGrenadeBounce = 0xbb,    // a grenade's bounce, on the missile (parm: the surface type; 0x810d5ba)
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

// The weapon step of a command of `msec` ms (docs/spec/net.md section 10 "Command steps"): pieces of at most 66 ms, the
// reload press an edge in the first piece only. Returns the shots. The server and the client's prediction both use it.
int weaponStep(game::PlayerWeapon& weapon, int msec, game::PlayerWeapon::Input in);

// The client's predicted weapon (docs/spec/net.md section 10 "Predicted weapon"). The front end steps its weapon once
// per command through command(); the weapon after every command is kept with the command's input and shots. A snapshot
// whose weapon state and time differ from the kept weapon of its commandTime (or whose commandTime is no command's
// time, the server's clamp) rebuilds the weapon: the kept one with the server's state, time and counters, and the kept
// commands after it run again. Shots are counted as fire events: the server's (the snapshot ring) plus those of the
// commands it has not run, played when that number passes the highest one played. A played number the prediction has
// fallen below (a shot predicted in a stall that the server did not fire) is given back after kAheadMs.
class PredictedWeapon {
public:
    using Counters = std::map<std::string, int>;
    // How long a played shot the server has not fired stands for the server's next shot: the longest time one command
    // runs (usercmdMsec), in command time. After it the next shot is played again.
    static constexpr int kAheadMs = 1000;
    PredictedAmmo ammo;  // the drawn counters (RemoteView::apply adjusts the snapshot's with it)
    // Once per new snapshot of the client's own player state, after RemoteView::apply wrote its counters into the
    // inventory (clip by clipName, stock by ammoName: the maps the weapon's readAmmo / writeAmmo use). serverWeapon: the
    // name of the player state's weapon. Returns true when the weapon was rebuilt.
    bool snapshot(const PsFields& server, const std::string& serverWeapon, game::PlayerWeapon& weapon, Counters& clip,
                  Counters& stock);
    // The weapon step of the frame's command. Returns the fire events to play now.
    int command(int commandTime, const game::PlayerWeapon::Input& in, game::PlayerWeapon& weapon, const Counters& clip,
                const Counters& stock);
    // A command the weapon does not run in (on a map turret the server skips the weapon as well): its time is passed, so
    // the next weapon step starts from it. Returns the fire events to play now.
    int idle(int commandTime);
    // The shots of the last command() call's own weapon step (before the event count), for traces.
    int lastStepShots() const { return lastStepShots_; }
    int rebuilds() const { return rebuilds_; }
    // The predicted number of fire events: the server's plus those of the commands it has not run (tests, traces).
    int predictedFires() const { return serverFires_ + pendingShots(); }
    void clear();

private:
    struct Record {
        game::PlayerWeapon weapon;  // after the command
        game::PlayerWeapon::Input in;
        int shots = 0;
    };
    int pendingShots() const;
    int takeFires();
    std::map<int, Record> records_;  // by command time
    int stepTime_ = 0;       // the command of the last weapon step
    int ackTime_ = 0;        // commandTime of the last snapshot
    bool haveRing_ = false;
    int ringSequence_ = 0;   // the snapshot ring's sequence at the last snapshot
    int serverFires_ = 0;    // fire events seen in the snapshots' ring
    int ackClip_ = -1;       // the loaded rounds of the last snapshot's weapon (-1 unknown)
    std::string ackWeapon_;  // that weapon
    int played_ = 0;         // the highest predicted number of fire events played
    int aheadSince_ = 0;     // the command time since which the played number is ahead of the predicted one (0: it is not)
    int lastStepShots_ = 0;
    int rebuilds_ = 0;
};

class NetCombat {
public:
    // The weapon part of one user command for MP client `client` (its entity resolved through the world every call, so a
    // round restart that rebuilds the world's entities does not leave stale pointers). ps: the movement state after the
    // command; cmdWeapon: the command's weapon byte as a weapon name ("" = none / unknown). msec: the command's time
    // (usercmdMsec).
    void think(game::World& w, int client, const net::UserCmd& cmd, game::PlayerState& ps, int msec,
               const std::string& cmdWeapon);
    // The movement inputs from this weapon (aim allowed, aim fraction, state, move speed scale and hip spread).
    WeaponMove move() const { return weaponMoveOf(weapon_); }
    const game::PlayerWeapon& weapon() const { return weapon_; }
    const EventRing& ring() const { return ring_; }
    int shots() const { return shots_; }  // total shots fired (tests)
    // The weapon a script selected for this playing client since the last call (switchtoweapon, a pickup, the spawn
    // weapon: a `$weapon` the weapon step did not write), empty when none. The server tells the client with the
    // command `a <index>` (docs/spec/net.md section 10 "Weapon selection").
    std::string takeScriptedSelect() { return std::exchange(scriptedSelect_, {}); }
    // weapon index of an event parameter (set by the owner: the weapon configstring index of a name).
    std::function<int(const std::string&)> weaponIndex;
    // The client's antilag time for its shots (the owner sets it before think; -1: the current positions).
    int antilagTime = -1;
    bool antilagFine = false; // the shooter draws the snapshots sent between the frames (World::fireAntilagFine)

private:
    void wire(game::World& w, int client);
    void reset();
    game::PlayerWeapon weapon_;
    bool wired_ = false;
    std::string select_, written_, lastCmdWeapon_;
    std::string scriptedSelect_; // a script's weapon change of this think, for the client (takeScriptedSelect)
    uint32_t oldButtons_ = 0;
    EventRing ring_;
    int shots_ = 0;
    int statePrev_ = 0; // the weapon state after the previous think (the offhand prepare event is the entry into 13)
};

} // namespace iw2::netgame
