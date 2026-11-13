// Third-person multiplayer players (docs/spec/mp-players.md): the per-client input the animation driver reads and the
// driver's state. Whoever moves a client (the bot controller, later the network server) fills MpPlayerInput every frame
// it moves the client; World::mpPlayersFrame (src/game/mpplayers.cpp) turns it into animations.
#pragma once

#include "game/playeranimscript.h"

#include <cstdint>
#include <memory>
#include <string>

namespace iw2::game {

struct MpPlayerInput {
    bool valid = false;   // filled this life (else the driver derives the movement from the entity velocity)
    PaMovement move;      // stance, speed, command moves, slow flag
    bool ads = false;     // aiming down the sights
    int onGround = -1;    // a network client's movement: 1 on the ground, 0 in the air, -1 unknown (isOnGround)
    // A puppet of a network client's world: the server's climb / mantle animation (the multiplayer tree node of the
    // entity's legsAnim, 0 none; docs/spec/mp-players.md "Ladder and mantle move types").
    int serverLegsNode = 0;
    // One-frame events, cleared by the driver after it plays them.
    bool fired = false;
    bool reloaded = false;
    bool jumped = false;
    bool shocked = false; // the shellshock method on a living client (the script event "shellshock", 0x81163c2..0x81163fc)
};

struct MpPlayerAnim {
    const PaItem* legsItem = nullptr; // the move-type item playing on the legs
    std::string moveType;
    int legsNode = 0;                 // the multiplayer tree node the move-type item plays on the legs (0 none)
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
};

} // namespace iw2::game
