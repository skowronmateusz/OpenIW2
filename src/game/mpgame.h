// Multiplayer game layer of the World (offline listen server): client slots and the gametype code callbacks.
// Implemented from the spec in docs/spec/mp.md. No networking: clients are local, the slot table is the boundary a
// network layer (or bots) would feed through connect / begin / menu response / command / disconnect.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/mphud.h"
#include "game/mpkillcam.h"
#include "game/mpplayers.h"
#include "game/pmove.h"
#include "script/value.h"

namespace iw2::game {

struct Entity;

// Client connection states (docs/spec/mp.md, "Client lifecycle"): free, connected (between connect and begin),
// active (after begin).
enum class MpClientState { Free = 0, Connected = 1, Active = 2 };

struct MpClient {
    MpClientState state = MpClientState::Free;
    Entity* ent = nullptr;
    std::string name;
    std::map<std::string, std::string> cvars; // setClientCvar values sent to this client
    std::string openMenu;                     // the script menu the client has open ("" none)
    uint32_t buttons = 0;                     // this frame's usercmd buttons of a non-local client
    MpPlayerInput animInput;                  // movement for the third-person animation (docs/spec/mp-players.md)
    MpPlayerAnim anim;                        // the animation driver's state
    uint32_t buttonsSinceFrame = 0;           // every button any usercmd held since the last server frame (a press between two frames)
    int weaponState = 0;                      // PM_Weapon state of the last command (the use search stops in the binocular states)
    bool mantling = false;                    // the last command was a mantle (use search)
    uint32_t oldButtons = 0;                  // the previous frame's buttons (use is a press)
    bool weaponDisabled = false;              // disableWeapon / enableWeapon
    int fireCount = 0;                        // shots whose fire event was played (the network game sends them)
    bool lastShot = false;                    // the newest of them emptied the clip
    MpViewState view;                         // the recorded first-person view state (docs/spec/mp-hud.md "The recorded view state")
    MpEventRing events;                       // the client's entity event ring (fire, reload, melee, offhand)
    // Spectating (docs/spec/mp-spectate.md): the allowSpectateTeam mask (bits none 1, axis 2, allies 4, freelook 0x10
    // set = not allowed), the followed client (-1 none), whether the view is the target's (pm_flags 0x400000) and the
    // buttons of the previous spectator think.
    int spectateMask = 0;
    int followClient = -1;
    bool following = false;
    uint32_t spectateOldButtons = 0;
    // The client's last weapon fire (the event that pings the compass): the bots hear it (docs/spec/mp-bots.md).
    int64_t lastShotMs = -100000;
    std::array<float, 3> lastShotPos{};
    // Shell shock in the client's player state (ps+0x5d8 index, +0x5dc start, +0x5e0 duration ms; docs/spec/mp.md
    // "Per-client player state"): every client has its own, the local view shows only the local client's.
    int shockIndex = 0;
    int64_t shockStartMs = 0;
    int shockDurationMs = 0;
    // Client spawns so far: ClientSpawn clears the whole player state, so the movement of the client restarts.
    int spawnCount = 0;
    std::vector<KnockbackHit> knockbacks; // finishPlayerDamage's pushes not yet applied to the client's movement
    // The damage timer of finishPlayerDamage (ps+0x90 / +0x94 / +0x98; the third-person stumbles read it).
    int damageTimerMs = 0, damageDurationMs = 0;
    float flinchYaw = 0;
    int ping = 0; // the network client's round trip in ms, for the scores command (local clients and bots have none)
    bool controlsFrozen = false; // freezeControls of this client (client field, not the local front end's)
    // A non-local client's view (the newest usercmd angles and view height) and its turret use state: the per-client
    // copy of what the local player keeps in the world (ps eFlags 0x300, ps+0x564 view lock, ps+0x568 turret, entity
    // +0x112 use state, the view clamp; docs/spec/turrets.md "Multiplayer").
    std::array<float, 3> viewAngles{};
    float viewHeight = 60.0f;
    struct TurretUse {
        Entity* turret = nullptr;
        int useState = 0;
        uint32_t eFlags = 0;
        int viewLocked = 0;
        float clampBase[2] = {0, 0}, clampRange[2] = {0, 0};
    } turretUse;
    // The use search (mpClientUseSearch) also needs the ground and the grenade fuse (mpSetClientGround).
    bool onGround = true;
    int grenadeFuseMs = 0;
    // The use search of the client's frame end (docs/spec/mp.md "Use search of every client", 0x8121bc6): the entity a
    // press of use uses (-1 none, player state +0x5a0) and the cursor hint (+0x598 hint, weapon hints as the weapon's
    // name; +0x59c the hint string text, "" none).
    int useEntity = -1;
    int useHint = 0;
    std::string useHintWeapon;
    std::string useHintText;
};

// The teleport bit of eFlags: every client spawn toggles it (docs/spec/mp.md "Client spawn"), so a client can tell a respawn
// from a move.
constexpr int mpTeleportBit(int spawnCount) { return (spawnCount & 1) ? 2 : 0; }

struct MpState {
    std::string gametype;
    std::vector<MpClient> clients;   // sv_maxclients slots
    std::vector<std::string> log;    // logPrint lines (games_mp.log)
    std::vector<std::string> prints; // iprintln / obituary lines, for tests and the console
    // Chat server commands ("h <text>" / "i <text>") sent to each client (docs/spec/mp-chat.md), for tests.
    std::vector<std::pair<int, std::string>> chat;
    // Other reliable server commands to one client, as sent (setClientCvar: v <name> "<value>", docs/spec/net.md
    // section 10).
    std::vector<std::pair<int, std::string>> clientCommands;
    // Cvars made server info by makeCvarServerInfo (docs/spec/net.md section 11), in call order.
    std::vector<std::string> serverInfoCvars;
    int bodyQueueNext = 0;  // cloneplayer's body queue counter (slot = counter % 8)
    bool exitRequested = false;      // exitLevel was called
    MpAnimData animData;             // mp/playeranim.script (docs/spec/mp-players.md)
    int localClient = 0;             // the client this process shows (docs/spec/mp.md "Local client view")
    int teamScore[2] = {0, 0};       // setTeamScore: [0] allies, [1] axis (reset at game init)
    int restartState = 0;            // 1 map_restart called, 2 exitLevel called (one-shot per level)
    bool restartPersist = false;     // map_restart(persist): keep the game array and the clients' pers
    int restarts = 0;                // fast restarts done in this world
    std::vector<std::string> statusIcons; // precacheStatusIcon names; a client's icon index is its position + 1
    MpArchive archive;               // the frame archive of the killcam (src/game/mpkillcam.cpp)
    std::vector<MpObituary> obituaries; // obituary events not yet taken by the front end (docs/spec/mp-hud.md)
    std::vector<MpObituary> obituaryLog; // every obituary event of this world (the network server forwards them)
    // Every missile explosion of this world (the network server sends them as explosion events, docs/spec/net.md
    // section 13): grenade (rocket = false) or rocket explosion, the weapon, point, direction and surface type.
    struct Explosion {
        bool rocket = false;
        std::string weapon;
        std::array<float, 3> at{}, dir{};
        int surface = 0;
    };
    std::vector<Explosion> explosionLog;
};

} // namespace iw2::game
