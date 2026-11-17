// Third-person multiplayer players (docs/spec/mp-players.md): the per-client input the animation driver reads and the
// driver's state. Whoever moves a client (the bot controller, later the network server) fills MpPlayerInput every frame
// it moves the client; World::mpPlayersFrame (src/game/mpplayers.cpp) turns it into animations.
#pragma once

#include "game/playeranimscript.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace iw2::game {

struct MpPlayerInput {
    bool valid = false;   // filled this life (else the driver derives the movement from the entity velocity)
    PaMovement move;      // stance, speed, command moves, slow flag
    bool ads = false;     // aiming down the sights
    int onGround = -1;    // a network client's movement: 1 on the ground, 0 in the air, -1 unknown (isOnGround)
    // A puppet of a network client's world: the entity's legsAnim, the animation the server plays on the legs that the
    // client cannot derive (a climb, a mantle, a jump, a landing: multiplayer tree node | toggle bit 0x200, 0 none;
    // docs/spec/mp-players.md "Ladder and mantle move types").
    int serverLegsNode = 0;
    // The legs event of the movement (PlayerState::animEvent, docs/spec/mp-players.md "Jumps and landings"): 3 jump,
    // 4 jumpbk, 5 land (0 none) and whether it is forced, kept until the driver plays it; animEventSeq the movement's
    // event count last taken (World::mpSetViewState).
    // A puppet of a network client's world on a map turret (docs/spec/mp-players.md "On a turret"): the turret's position
    // in the map's entity list (0 none) and the attack held, from the entity state.
    int remoteTurretMapIndex = 0;
    bool remoteFiring = false;
    int legsEvent = 0;
    bool legsEventForced = false;
    int animEventSeq = 0;
    // One-frame events, cleared by the driver after it plays them.
    bool fired = false;
    bool reloaded = false;
    bool shocked = false; // the shellshock method on a living client (the script event "shellshock", 0x81163c2..0x81163fc)
};

struct MpPlayerAnim {
    const PaItem* legsItem = nullptr; // the move-type item playing on the legs
    std::string moveType;
    int legsNode = 0;                 // the multiplayer tree node playing on the legs (0 none)
    int torsoNode = 0;
    bool legsTurretAnim = false;      // the legs command carries `turretanim` (posed from the turret, MP 0x4dbe60)                // the multiplayer tree node of the last torso command (0 none)
    // A jump or landing animation on the legs since its event, until a move type is chosen again (sent in legsAnim with
    // the toggle bit legsToggle, docs/spec/mp-players.md "Jumps and landings"); 0 none.
    int eventLegsNode = 0;
    int legsToggle = 0;
    // A puppet: the server's legs animation it plays as it is (legsAnim with the toggle bit), 0 none.
    int serverLegsPlayed = 0;
    // The rate rule's last sample (docs/spec/mp-players.md "Animation rate from the movement").
    bool rateSampled = false;
    int64_t rateSampleMs = 0;
    std::array<float, 3> rateSampleOrigin{};
    int64_t legsHeldUntilMs = 0;      // an event's "both" / "legs" command holds the legs until then
    int64_t torsoHeldUntilMs = 0;     // a torso command holds the torso until then
    bool torsoActive = false;
    std::string weaponModel;          // world model drawn at tag_weapon_right ("" none)
    std::string deathAnim;            // chosen at the death, played by the body
};

// Shared multiplayer animation data (loaded on first use).
struct MpAnimData {
    bool loaded = false;
    std::unique_ptr<PlayerAnimScript> script; // nullptr when mp/playeranim.script is missing or bad
    uint32_t seed = 0x2545f491u;             // random choice among an item's commands
    std::map<int, float> moveSpeed;          // tree node -> move speed (units per second, 0 none), on first use
};

} // namespace iw2::game
