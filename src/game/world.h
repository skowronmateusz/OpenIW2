// Game world: map entities, path nodes, the player, and the script VM that drives them.
#pragma once

#include "game/bulletdamage.h"
#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/hintstring.h"
#include "game/actorslide.h"
#include "game/actortrail.h"
#include "game/animtree.h"
#include "game/axis.h"
#include "game/inventory.h"
#include "game/movers.h"
#include "game/campaign.h"
#include "game/fx.h"
#include "game/collision.h"
#include "game/compassping.h"
#include "game/friendlychain.h"
#include "game/grenadehint.h"
#include "game/hitboxes.h"
#include "game/pathnodes.h"
#include "game/perception.h"
#include "game/turrets.h"
#include "game/vehicles.h"
#include "script/vm.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::game {

class ClientInput;

struct ShockRecord;

using script::ObjectPtr;
using script::Value;
using script::Vec3;

// A path point (0x1c bytes): its position, the unit 2D direction from the next point (the one nearer the actor) to
// this one and the 2D length of that segment (both 0 for the last point), and its path node (−1 for none).
struct PathPoint {
    Vec3 pos{};
    std::array<float, 2> dir{};
    float len = 0;
    int node = -1;
};

// An actor's path (actor +0x330 … +0x707; cleared by 0x5436a0: budget 32768, avoided sentient none). Points are kept
// goal first: point 0 is the goal, point count − 1 the end the actor started from. `start` is the actor's place on
// the first segment (count − 1 → count − 2), `startLen` what is left of it. The lookahead point is `lookDist` along
// `lookDir` from the actor (its segment `lookIndex`, `lookSegLen` back from that point); `budget` limits how much
// area between the actor and the path the lookahead may cut (0x5447d0).
struct ActorPath {
    std::array<PathPoint, 32> pts{}; // +0x0
    int count = 0;                   // +0x380 (actor +0x6b0)
    int planned = 0;                 // +0x382: points when planned (or re-extended)
    int detour = 0;                  // +0x384: points added by the last detour (−1: blocked at the path end)
    int traversal = 0;               // +0x386: lowest point the follower may use (negotiation pairs, TODO(spec))
    int lookIndex = 0;               // +0x388
    int avoidEnt = 0x7ff;            // +0x38a: entity number of the sentient being avoided, 0x7ff none
    Vec3 goal{};                     // +0x38c
    Vec3 lookDir{};                  // +0x398: unit 2D direction, z = height change per unit
    std::array<float, 2> farDir{};   // +0x3a4: direction to the point `budget` further along (0x544a10)
    float lookDist = 0;              // +0x3ac
    float budget = 32768.0f;         // +0x3b0
    float lookSegLen = 0;            // +0x3b4
    uint32_t flags = 0;              // +0x3b8: 1|0x40 lookahead at the end, 2 / 0x100 the lookahead was clear (the
                                     // budget grows), 4 truncated, 0x10 traversals allowed, 0x20 far off the path
    int64_t planMs = 0;              // +0x3bc
    float planArg = 0;               // +0x3c0
    float startLen = 0;              // +0x3c4
    Vec3 start{};                    // +0x3c8
};

// Script-visible entity. Native state lives in the object's fields so scripts and engine share one view.
struct Attachment {
    std::string model;
    std::string tag;
};

// AI states (state table 0x56f720; docs/spec/ai.md "AI states").
enum AiState { AiNone, AiExposed, AiTurret, AiGrenade, AiBadplace, AiDeath, AiPain, AiScripted, AiCustom, AiNegotiation };

// Prone state of an actor (actor +0x140 .. +0x16c, +0x288; docs/spec/ai.md "Prone"). The 24 bytes +0x158 .. +0x16f
// travel to the client, which pitches tag_origin by them (0x4c1cc0 / 0x4c1b90).
struct ActorProne {
    int nodeDown = 0, nodeStraight = 0, nodeUp = 0; // +0x140 / +0x142 / +0x144 leg blend nodes (0 = not set)
    float invDown = 0, invUp = 0;                   // +0x148 / +0x14c: 1 / down angle, 1 / up angle
    float lastBend = 0;                             // +0x150 bend the leg weights were last set for (360 forces a refresh)
    int fits = 0;                                   // +0x154 last fit result (script field proneok)
    bool corpseBlend = false;                       // +0x158 (set only on corpses, 0x4fdd75)
    bool prone = false;                             // +0x159
    int64_t startMs = 0;                            // +0x15c transition start
    int durMs = 0;                                  // +0x160 transition length; negative while leaving, 0 when settled
    float lift = 0;                                 // +0x164: corpse ground lift (0x4fd890); the prone fit writes int 0 here
    float pitchBody = 0, pitchLegs = 0;             // +0x168 / +0x16c fit pitches
    bool tilt = false;                              // +0x288 set by enterprone, cleared by the think when not prone
    // The think compares the entity's origin and angles with the values of its previous think (0x4ff0b5..0x4ff16e).
    bool haveLast = false;
    Vec3 lastOrigin{}, lastAngles{};
};

// The actor's state machine (actor +0x8 … +0xa4, +0x104, +0x204 … +0x220, +0x25c, +0x704).
struct ActorAi {
    std::array<int, 5> stack{AiExposed};      // +0x8, depth +0x1c
    int depth = 0;
    int bumpedEnt = 0x7ff;                    // +0x318 the entity the last actor slide move hit (0x7ff none, 0x7fe world); slide move only (default on)
    float hitNormal[2] = {0, 0};              // +0x324 / +0x328 (block +0x100 / +0x104): wall normal x / y recorded by the slide move; persists between moves (only +0x318 / +0x32d are reset per move)
    bool deflected = false;                   // +0x32d (block byte +0x109): the last slide move was stopped short by a trace (gethityaw / isdeflected)
    int groundEnt = 0x7fe;                    // block +0x18 (ground entity of the actor slide move; 0x7fe at init, 0x500d27)
    int64_t baseStateMs = 0;                  // +0x20 time the bottom state was last set
    struct Request { int type, state; };      // 1 replace, 2 push, 3 pop
    std::vector<Request> requests;            // +0x30, count +0x88
    std::array<int, 5> predicted{AiExposed};  // +0x8c: the stack once the requests are applied, depth +0xa0
    int predictedDepth = 0;
    int substate = 0;                         // +0x28
    int64_t perceptionFrameMs = -1;           // +0x24
    int64_t painStartMs = 0;                  // +0xa4
    int facing = 0;                           // +0x104 facing mode of the base block (actorfacing.cpp)
    int orient = 0;                           // +0x114 orientmode override (0 = none, else a facing mode 1..6)
    float lookAngles[2] = {0, 0};             // +0xd8 / +0xdc look pitch / yaw (lookforward +0xe0 from them, 0x5049a0)
    int64_t threatCheckMs = 0;                // +0x204c next nearest-threat facing check (0x505790)
    Vec3 blockVel{};                          // +0x230 block velocity of the previous physics step (read by 0x505b80 / 0x505cb0)
    // The slide block's own velocity (block +0xc = actor +0x230; the block lives in the actor at +0x224, 0x501f38 passes
    // actor + 0x224 to 0x5487c0, which updates it in place): it carries over to the next physics step as the move block's
    // velocity (the airborne branch moves on it with gravity). TODO(spec): blockVel above is the frame displacement, which
    // the readers 0x505b80 / 0x505cb0 still take; the exe has only this one field.
    float slideVel[3] = {0, 0, 0};
    bool slideContact = false;       // block +0x20 (actor +0x244): ground contact with normal z >= 0.3
    float slideContactNz = 1.0f;     // block +0x24 (actor +0x248): its normal z
    int64_t walkedMs = -1;                    // level time of the last mode-1 path walk (moveActor)
    bool facingInit = false;
    int floatFrames = 0;                      // IW2_FLOAT_LOG: frames in a row without ground within 16 units
    int64_t meleeFrameMs = -1;                // +0xcc: level time of the last melee blow (one per frame, 0x550c7a)
    float orientTarget = 0;                   // +0x120 override turn target (degrees, 16-bit quantised)
    // Facing look blocks (0x504950 / 0x5055d0): base +0x108 pitch / +0x10c yaw / +0x110 turn target, override +0x118 / +0x11c
    // (its target is orientTarget). All 16-bit quantised degrees. The base target starts at the spawn yaw (0x4fc7dd).
    float basePitch = 0, baseYaw = 0, baseTarget = 0;
    float overridePitch = 0, overrideYaw = 0;
    int64_t faceEnemyUntilMs = 0;             // +0x130: now + 500 when mode 4 sees its enemy while moving (0x505bee). TODO(spec): reader
    int animMode = 0, animModeOverride = 0;   // +0x218 / +0x21c
    int movemode = 0;                         // +0x214: 0 stop, 1 stop_soon, 2 walk, 3 run
    bool animDeltaGoalWeights = false;        // +0x220 anim deltas weigh by goal weights (TODO(spec): the states that set it)
    script::Value customScript;               // +0x208 animcustom function
    std::string negotiationScript;            // +0x208 for state 9: the begin node's animscript key (animscripts/traverse/<key>, 0x543760)
    bool alive = true;                        // +0x25c
    int64_t stopSoonMs = 0;                   // +0x704
    int64_t planTimeMs = 0;                   // +0x720 earliest time of the next path plan without a path
    int64_t nodeBlockMs = 0;
    int trimCount = 0;     // +0x70c: set to path count + 1 by findreacquire*path (0x5492f9); consumed by trimpathtoattack
    int trimState = 0;     // +0x710: set to -2 by findreacquire*path (0x5492e7)                  // sentient +0x40: next node-facing check (0x515390), every 4950 ms
    int moveNoiseMs = 0;                      // actor +0x23c.. (0x224 struct +0x1c): the movement noise accumulator 0x5485a0
    int64_t lastFoliageMs = 0;                // actor +0x290 (0x224 struct +0x6c): time of the last foliage sound event (0x548650)
    int64_t detourMs = 0;                     // +0x724 earliest time of the next detour / blocked wait
    struct Entity* blocker = nullptr;         // +0x728 actor whose blocking this one waits out (itself: blocked)
    struct Entity* blockerEnt = nullptr;      // +0x72c the sentient blocking the way
    struct Entity* followed = nullptr;        // +0x2148 the entity this actor yields to (0x501620 steps it aside)
    int sideBits = 0;                         // gentity FLAGS bits 8 / 0x10 (+0x128, not contents +0xc0): the side the last yield step slid to (read back by the yield step of other actors only)
    ActorTrail trail;                         // +0x214c / +0x2150: the follower trail ring (read by the facing blend 0x505630)
    int64_t yieldMs = -1;                     // level time of the last yield step (one physics step per think)
    int64_t thoughtMs = -1;                   // last think (its next think, gentity +0x138, is 50 ms later)
    float goalSide = 0;                       // +0x73c sideways goal offset (0x502960); only ever 0 in the exe (4fc7ab)
    bool pathToEnemy = false;                 // +0x788 pathing towards the enemy (set by 0x503200, World::stopForEnemy)
    std::vector<int> coverNodes;              // +0x798 findcovernode result, count +0x1738 (best last)
    std::vector<int> reacquireNodes;          // +0x173c findreacquirenode result, count +0x1764 (best last)
    bool corpse = false;                      // turned into a corpse (0x4fdba0)
    uint32_t dyingContents = 0;               // gentity +0xc0 written by the death think 500 ms into the state (0x548bc0: 0x4002000); 0 = unchanged
    bool badPlace = false;                    // +0x2128: noticed a bad place (→ badplace state)
    struct Entity* grenade = nullptr;         // +0x20c4: grenade it reacts to (script field "grenade")
    bool throwPending = false;                // +0x20d0: a solved throw waits for throwGrenade
    bool throwFromTag = false;                // +0x20d4: re-solve the throw from tag_weapon_right when thrown
    std::string throwMethod;                  // +0x20cc
    Vec3 throwStart{}, throwTarget{}, throwVel{}; // +0x20dc / +0x20e8 / +0x20f4
    bool noPush = true;                       // +0x28c bit 0x2000000: pushplayer(0) sets it (TODO(spec): its consumer)
    bool potentialThreat = false;             // +0x20b4: setpotentialthreat; +0x20b8 cos, +0x20bc sin of the angle (TODO(spec): consumer)
    float potentialThreatDir[2] = {0, 0};
    int traverseMode = 0;                     // +0x210: traversemode gravity 1, nogravity 2, noclip 3 (TODO(spec): consumer)
    bool noPhysicsFlag = false;               // gentity flag 0x1000 (+0x128): animmode("nophysics") sets it, every other mode clears it
    ActorProne prone;                         // +0x140 .. +0x16c, +0x288 (src/game/proneworld.cpp)
};

// A rocket missile entity (G_FireRocket 0x527810; src/game/rockets.cpp).
struct RocketState {
    std::string weapon;
    struct Entity* parent = nullptr; // +0x134
    int64_t explodeMs = 0;           // +0x138 think
    int64_t trTime = 0;              // s.pos: linear (2)
    Vec3 trBase{}, trDelta{};
    float destabMs = 0;              // +0x168: ms after trTime until the next destabilization (0x526960)
    bool destabilized = false;
    bool stable = false;             // +0x128 & 0x20000 (setstablemissile on the shooter): never destabilizes       // +0x128 & 0x10000: destabilized at least once
};

// A grenade missile entity (0x5275f0; s.pos trajectory at +0x14, fuse +0x138, AI fields +0x150 … +0x164).
struct GrenadeState {
    std::string weapon;
    struct Entity* parent = nullptr;  // +0x134 thrower
    int64_t explodeMs = 0;            // +0x138 fuse
    int trType = 5;                   // 0 stationary, 5 gravity
    int64_t trTime = 0;
    Vec3 trBase{}, trDelta{};
    bool bounce = true;               // +0x10 & 0x1000000
    bool onGround = false;            // ground entity +0xc
    struct Entity* holder = nullptr;  // +0x150 actor going for it / holding it
    bool inHand = false;              // +0xa2 & 1
    Vec3 landing{};                   // +0x154 predicted rest point, zero = none
    int64_t landingMs = 0;            // +0x160
    int64_t pingMs = -1000;           // +0x164 last grenade_ping
    // Angular trajectory (type 2, linear: base + delta·t; set at 0x5275f0, re-based on a bounce 0x525cb0).
    Vec3 aBase{}, aDelta{};
    int64_t aTime = 0;
};

// The scripted animation block of an entity (+0x1d0, 0x60 bytes from the pool 0x82e980; src/game/animscripted.cpp).
struct ScriptedAnim {
    Axis axis{};              // +0x0: frame of the scripted angles
    Vec3 origin{};            // +0x24: scripted origin
    Vec3 originOffset{};      // +0x30: where the entity was, relative to the start pose; slides to 0
    Vec3 anglesOffset{};      // +0x3c
    int anim = 0;             // +0x48: tree node, 0 when finished
    int root = 0;             // +0x4a
    bool started = false;     // +0x4c: set after the first update
    bool deathplant = false;  // +0x4d
};

// One danger plane slot (actor +0x2050, 4 × 0x18 bytes): fire of an enemy near the actor marks the half plane beyond
// the firing line; the planners (0x5428a0 / 0x545ca0 / 0x517f90) stay on the actor's side of it. Added by 0x54b3b0
// (slot choice 0x54b370), expired by 0x54b4d0, dropped when the source is released (0x54b5a0), collected by 0x54b670.
struct DangerPlane {
    int64_t timeMs = 0;    // +0x0: when it was added (0: the slot is free)
    int sourceEnt = -1;    // +0x4: entity number of the sentient whose fire added it
    float normal[2] = {0, 0}; // +0x8
    float dist = 0;        // +0x10
    bool shortLived = false; // +0x14: set for a same-team source (expires after 300 ms, and 0x54b610 keeps such slots)
};

struct Entity {
    ObjectPtr obj;
    int number = 0;
    uint32_t shadowHint = 0;          // entity state +0x10 bits 0x2000..0xe000 (setshadowhint 0x4ef700; render only)
    std::string classname;
    bool isPlayer = false;
    bool isSpawner = false;
    bool isActor = false;
    std::vector<Attachment> attachments;
    std::string animTree; // set by useanimtree
    std::unique_ptr<AnimState> anim;
    std::unique_ptr<ScriptedAnim> scripted; // animscripted / startscriptedanim (+0x1d0)
    uint64_t animScriptThread = 0;   // thread of the running engine animscript (init, stop, …)
    std::string animScript;
    int64_t animScriptStartMs = 0;
    // Movement towards the script goal (actor +0x330, src/game/actorpath.cpp).
    ActorPath path;
    Vec3 velocity{};                 // actor +0x230 / the player's ps velocity (from the last frame's movement)
    // Sentient node cache (0x515040: +0x38 node, +0x3c cached, +0x3d unreachable).
    int cachedNode = -1;
    bool nodeCached = false, nodeUnreachable = false;
    int64_t nodeCacheMs = -1;
    std::unique_ptr<VehicleState> vehicle; // script_vehicle path state (src/game/vehicles.cpp)
    int usingVehicle = -1;                 // player: the vehicle it rides (entity +0xf4 / client +0x568)
    std::unique_ptr<TurretState> turret;   // misc_turret / misc_mg42 / spawnturret (src/game/turrets.cpp)
    ObjectPtr usingTurret;                 // actor: turret it uses (actor +0x2114)
    int turretAnim = 0;                    // actor +0x2118: setturretanim's node in the actor's tree
    bool turretAnimSet = false;            // actor +0x211a: setturretanim was called
    ActorAi ai;                            // actor state machine (src/game/actorstate.cpp)
    std::array<DangerPlane, 4> dangerPlanes; // actor +0x2050 (src/game/actorpath.cpp)
    struct Entity* lastAttacker = nullptr; // sentient +0x28 (G_Damage 0x5284dd; AI events 5/6 reveal it, pain clears it)
    std::unique_ptr<GrenadeState> grenade; // grenade missile (src/game/grenades.cpp)
    std::unique_ptr<RocketState> rocket;   // rocket missile (src/game/rockets.cpp)
    int claimedNode = -1;                  // sentient +0x2c: claimed path node (script field "node")
    // Friendly chain (src/game/friendlychain.cpp): the sentient's chain node (+0x30, set by setfriendlychain / a chain
    // trigger, advanced to the nearest chain node once a frame while followed), the time of that advance (+0x34), and
    // the actor's own chain node (actor +0x78c, script field "chainnode").
    int friendlyChain = -1;
    int64_t friendlyChainMs = -1;
    int chainNode = -1;
    // The working goal (actor +0x748 pos, +0x754 radius, +0x758 height, +0x760 mode), computed by 0x505f70 each path update
    // (src/game/friendlychainworld.cpp). Until then (and for goals the script set without a path update) the script
    // fields $goalpos / goalradius / goalheight stand in. Modes: 0 fixed, 1 entity, 2 friendly leader, 3 enemy.
    struct WorkGoal {
        bool set = false;
        Vec3 pos{};
        float radius = 0, height = 0;
        int mode = 0;
        bool scriptVolume = true; // +0x75c: the script goal volume (+0x778) in mode 0, none otherwise
    } workGoal;
    // Perception (src/game/perception.cpp): what this actor knows about each sentient, and how many actors have
    // this sentient as their enemy (sentient +0x24).
    std::unordered_map<const script::Object*, SentientInfo> sentientInfo;
    int attackers = 0;
    int64_t eyeFrameMs = -1;               // per-frame cache of the eye (0x500910 caches by frame time)
    Vec3 eyePos{}, eyeForward{};
};

// A half plane the planners stay inside (normal · p ≤ dist), from the actor's danger planes (+0x2050).
struct PathPlane {
    std::array<float, 2> normal{};
    float dist = 0;
};

// A bad place slot (0x1c90e00, 32 × 0x28 bytes).
struct BadPlace {
    int64_t expireMs = 0;   // +0
    int64_t eventMs = 0;    // +4: creation, then the last awareness event
    std::string name;       // +8
    int type = 0;           // +0xa: 0 free, 1 cylinder / arc
    uint32_t teamMask = 0;  // +0xb: bit 1 axis, 2 allies, 3 neutral
    Vec3 origin{};          // +0xc
    float radius = 0, halfHeight = 0, left = 0, right = 360; // +0x18 … +0x24
};

struct PathNode {
    ObjectPtr obj;
    std::string type;               // classname, e.g. node_pathnode
    int typeNum = 0;                // +0x0: 1…18 (spawn table 0x56fea8), 0 unknown
    int spawnflags = 0;             // +0x4: 0x40 priority (setnodepriority), 0x8000 node has an angle (set by the key parse 0x52d2cd), 1 not a cover candidate
    Entity* owner = nullptr;        // +0x40: sentient that claimed it (or just left it)
    int64_t ownerUntilMs = 0;       // +0x44: kNodeClaimed while claimed, else the owner keeps it until then
    std::array<int64_t, 4> teamBlockedMs{}; // +0x44 + 4·team: unusable for that team until then (1 axis, 2 allies)
    std::array<int, 2> link{-1, -1};        // +0x30 / +0x32: the overlapping nodes (0x52d990), -1 = none
    int linkClaims = 0;                     // +0x56: the claims by the nodes linked to this one
    int turretEntity = -1;                  // +0x58 (node_turret only; -1 at spawn 0x516137): setturretnode's entity number
};

class World {
public:
    World(const fs::Vfs& vfs, std::string mapName);

    // Spawns map entities and the player, then runs maps/<map>.gsc::main().
    void start();
    // start() = prepareStart() (cvars, map entities, vehicle nodes, turrets, path disconnects) + runLevelMain() (the level script's
    // main() and the first frame). A savegame load runs prepareStart() and then loadState() instead of runLevelMain().
    void prepareStart();
    // 0x4fc550: the actor drop to floor of every actor spawn and of the level-start spawner drop (0x5325f0); true when the
    // trace is allsolid (the origin is then left 1 higher).
    bool dropActorToFloor(Entity& e);
    void runLevelMain();
    // Savegame state (src/game/savestate.cpp, docs/spec/savegame.md section 5). saveState only between frames.
    void saveState(script::StateWriter& w);
    void loadState(script::StateReader& r);
    // Debug / test: a text digest of the saved state (entities, threads, level fields, time), equal after save -> load.
    std::string stateDigest() const;
    // Save requests (0x44a140 queue, 0x44a230 processing; docs/spec/savegame.md section 3). queueSave returns the save id, or
    // -1 (g_reloading), -2 (second autosave this frame), -3 (3 pending). processSaves runs at the end of each frame: flags & 6
    // produce a file for the front end (takeSaveFiles), flags == 1 (savegamenocommit) keep the file until commitsave.
    struct SaveFile { std::string name; std::vector<uint8_t> bytes; bool manual = false; };
    int queueSave(const std::string& name, const std::string& description, const std::string& image, int type, int flags);
    std::vector<SaveFile> takeSaveFiles() { return std::exchange(saveFiles_, {}); }
    void markSaveLoaded() { saveRecentlyLoaded_ = true; } // issaverecentlyloaded (byte 0x1b15ac0)
    void clearSaveRecentlyLoaded() { saveRecentlyLoaded_ = false; }
    void setSaveFailed(bool f) { saveFailed_ = f; }       // issavesuccessful
    std::vector<script::Value> stateRoots() const; // objects the world holds besides the VM's own roots
    const std::vector<uint8_t>& bspData() const { return bspData_; }
    // Advances game time by one server frame (50 ms).
    void frame(bool advanceTime = true);

    script::VM& vm() { return vm_; }
    const assets::D3dBsp& bsp() const { return *bsp_; }
    int64_t timeMs() const { return timeMs_; }
    // setculldist value (configstring 8; 0 = unset). The renderer folds it into its far plane (farPlaneDistance in fog.h).
    float cullDist() const { return cullDist_; }

    Entity* player() { return player_; }
    const std::vector<std::unique_ptr<Entity>>& entities() const { return entities_; }
    const std::vector<PathNode>& nodes() const { return nodes_; }

    // Debug: spawns one AI from each actor spawner (exercises the aitype/animscripts path).
    // Sets actorPoolUnlimited_ for the rest of the run: spawning every spawner at once would otherwise exhaust the
    // 32-slot actor pool, so later script spawns (turret gunners) would all fail.
    int spawnAllActors();
    // Test hooks for the spawn checks (src/game/spawnchecks.cpp): one spawn as dospawn (force false) or stalingradspawn,
    // the telefrag test, the force-spawn eviction, and the "level loaded" state (0x1c61048 cleared).
    Entity* debugSpawnActor(Entity& spawner, bool force) { return spawnActor(spawner, force); }
    bool debugWouldTelefrag(const Entity& spawner) { return spawnWouldTelefrag(spawner); }
    bool debugEvictForForceSpawn() { return freeActorForForceSpawn(); }
    void debugSetLevelLoaded(bool loaded) { levelInitializing_ = !loaded; }

    // Moves the player entity (set by the movement code each frame).
    void setPlayerOrigin(const Vec3& origin, const Vec3& angles);

    // The player's command (usercmd), set by the front end before each frame: forward / right −127…127, button bits
    // (attack 1, melee 4, +activate 8, reload 0x10, +usereload 0x20, jump 0x400) and the view angles.
    struct PlayerCommand {
        int forward = 0, right = 0;
        uint32_t buttons = 0;
        Vec3 viewAngles{};
        // Player state the use code reads (pmove runs in the front end): stance (pm_flags 1 prone → 2, 2 crouch → 1,
        // else 0), eye height above the origin (ps+0xd4), on the ground (ps+0x60 ≠ none), mantling (pm_flags 4),
        // weapon state (ps+0xb4) and the offhand fuse (ps+0x3c, cooking a grenade).
        int stance = 0;
        float viewHeight = 60.0f;
        bool onGround = true;
        bool mantling = false;
        int weaponState = 0;
        int grenadeFuseMs = 0;
        float lean = 0.0f;     // leanf
        float adsFrac = 0.0f;  // the aim fraction (client +0xb8), read by playerads
    };
    void setPlayerCommand(const PlayerCommand& c) {
        prevButtons_ = playerCmd_.buttons;
        playerCmd_ = c;
    }
    // pm_flags bits of the vehicle code: 0x20000 in a vehicle, 0x40000 the view yaw turns with the vehicle.
    uint32_t playerPmFlags() const { return pmFlags_; }
    // Mirrors a flag the player move owns (pm_flags 0x4000 hold breath) into the flags the HUD reads.
    void setPlayerPmFlag(uint32_t bit, bool on) { pmFlags_ = on ? (pmFlags_ | bit) : (pmFlags_ & ~bit); }
    // On a turret (ps eFlags & 0x300, set by the turret use 0x51db50).
    // Debug (iw2view IW2_FIRE_TURRETS): one shot from every turret entity (number of turrets fired).
    int debugFireTurrets();
    void debugSetEnemy(Entity& a, Entity* b) { setEnemy(a, b, true); } // IW2_COMBAT_SCENE
    // IW2_COMBAT_SCENE: puts b 120 units in front of a (a's yaw), same height.
    void debugPlaceBefore(Entity& b, Entity& a) {
        Vec3 p = a.obj->get("origin").asVector();
        const float yaw = a.obj->get("angles").asVector()[1] * 3.14159265f / 180.0f;
        p[0] += std::cos(yaw) * 120.0f;
        p[1] += std::sin(yaw) * 120.0f;
        b.obj->set("origin", Value(p));
        syncEntityClips(); // the clips are rebuilt once per frame; the scene shoots in the same frame
    }
    // IW2_TRAVERSE_SCENE: the `which`-th negotiation pair (begin node with target == an end node's targetname, both linked in
    // the graph) that has a node linked on each outer side. Puts `a` on the begin side's node and gives it the goal on the end
    // side. Returns the begin node (the end node in `endNode`) or -1.
    int debugTraversalSetup(Entity& a, int which, int& endNode) {
        int seen = 0;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].typeNum != 16) continue;
            const Value tg = nodes_[i].obj->get("target");
            if (!tg.isString()) continue;
            for (size_t j = 0; j < nodes_.size(); ++j) {
                if (nodes_[j].typeNum != 17) continue;
                const Value tn = nodes_[j].obj->get("targetname");
                if (!tn.isString() || tn.asString() != tg.asString()) continue;
                int from = -1, to = -1;
                for (const auto& l : paths_.links(i)) if (l.target != j && from < 0) from = l.target;
                for (const auto& l : paths_.links(j)) if (l.target != i && to < 0) to = l.target;
                if (from < 0 || to < 0) continue;
                const std::vector<int> route = paths_.findPath(from, to); // only pairs the shortest route really uses
                if (std::find(route.begin(), route.end(), int(i)) == route.end() || std::find(route.begin(), route.end(), int(j)) == route.end()) continue;
                if (seen++ != which) continue;
                a.obj->set("origin", nodes_[size_t(from)].obj->get("origin"));
                a.obj->set("$goalpos", nodes_[size_t(to)].obj->get("origin"));
                a.obj->set("$goalfrozen", Value(true));
                a.nodeCached = false;
                clearActorPath(a);
                a.ai.stopSoonMs = 0;
                a.obj->set("keepclaimednode", Value(int32_t(0)));
                a.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
                if (a.claimedNode >= 0) releaseNode(a);
                a.obj->set("team", Value(std::string("neutral"))); // no enemies: the actor stays on the goal path
                endNode = int(j);
                return int(i);
            }
        }
        return -1;
    }
    // IW2_COMBAT_SCENE: one rifle bullet from a's muzzle height to b's chest, credited to a.
    // IW2_COMBAT_SCENE with IW2_SCENE_COVER=1: finds a cover node (types 2..9, not spawnflag 1) and puts b 150 units to its
    // side (in the open, node nearby) and a 300 units in front of the node facing it, with a clear shot a -> b. Returns
    // the node index or -1.
    int debugCoverSetup(Entity& a, Entity& b) {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const PathNode& n = nodes_[i];
            if (n.typeNum < 2 || n.typeNum > 9 || (n.spawnflags & 1)) continue;
            const Vec3 no = n.obj->get("origin").asVector();
            const float yaw = n.obj->get("angles").asVector()[1] * 3.14159265f / 180.0f;
            const float fx = std::cos(yaw), fy = std::sin(yaw);
            for (float side : {150.0f, -150.0f}) {
                Vec3 bp = {no[0] - fy * side, no[1] + fx * side, no[2]};
                Vec3 ap = {no[0] + fx * 300.0f, no[1] + fy * 300.0f, no[2]};
                b.obj->set("origin", Value(bp));
                a.obj->set("origin", Value(ap));
                a.obj->set("angles", Value(Vec3{0, n.obj->get("angles").asVector()[1] + 180.0f, 0}));
                syncEntityClips();
                if (debugClearShot(a, b)) {
                    b.obj->set("$goalpos", Value(bp)); // the cover search stays inside the goal volume around this point
                    b.obj->set("goalradius", Value(512.0f));
                    return int(i);
                }
            }
        }
        return -1;
    }
    // IW2_COMBAT_SCENE: true when a bullet from a reaches b (hit boxes, no damage).
    bool debugClearShot(Entity& a, Entity& b) {
        Vec3 s = a.obj->get("origin").asVector(), t = b.obj->get("origin").asVector();
        s[2] += 56.0f;
        t[2] += 20.0f;
        int loc = 0;
        return locationalTrace(s, t, Contents::BulletMask, &a, kHitPriorityDefault, loc).entity == b.number;
    }
    int debugShoot(Entity& a, Entity& b) {
        Vec3 s = a.obj->get("origin").asVector(), t = b.obj->get("origin").asVector();
        s[2] += 56.0f;
        t[2] += 20.0f; // a scripted-pose actor is low; the hit boxes follow the pose
        Entity* h = fireBullet(a, s, t, 10, &a);
        return h ? h->number : -1;
    }
    bool playerOnTurret() const { return (playerEFlags_ & 0x300u) != 0; }
    // The eye on a turret (0x4e8b70 → 0x4d7730): the turret's tag_player.
    // Cursor hint of what the use key would use (ps+0x578 hint, +0x57c string; 0x534d40). hint: 0 none, 1 HINT_NONE,
    // 2 HINT_ACTIVATE, 3 HINT_HEALTH, 4 HINT_FRIENDLY (setcursorhint 0x4f0c50, table 0x5a965c); a weapon hint
    // (weapon index + 4) is given as `weapon`. text: the localized string key of the hint string, "" for none.
    struct PlayerUseHint {
        int hint = 0;
        std::string weapon;
        std::string text;
    };
    const HintStringTable& hintStrings() const { return hintStrings_; }
    const PlayerUseHint& playerUseHint() const { return playerUseHint_; }
    // The use hint of a usable vehicle (0x534d40 case 0xc, entity type 0xc): hint 2, and when its weapon (entityState +2, the vehicle's
    // turretWeapon) has a useHintString (weapon def +0x56c non-empty) that string (def +0x574); otherwise no text.
    PlayerUseHint vehicleUseHint(const std::string& turretWeapon);
    // A float dvar by name (cvars, case-insensitive) or `def` when it is unset or not a number.
    float cvarFloat(const std::string& name, float def) const;
    // After the player's death: no view weapon (ent+2 = 0, cgame 0x4b6a0a) and the dead view yaw (ps+0x10c).
    bool playerDeadWeaponless() const { return playerDeadWeaponless_; }
    // P_DamageFeedback (0x4e7040): damageEvent (ps+0xf8) counts damage frames; pitch/yaw (ps+0x100/+0xfc) are the
    // damage direction's angles · 256/360 (255/255 for world damage), count (ps+0x104) = min(127, blood · 100 /
    // max health). The aim spread scale also grows by count (ps+0x5b0).
    struct DamageFeedback { int event = 0, pitch = 0, yaw = 0, count = 0; };
    const DamageFeedback& playerDamageFeedback() const { return damageFeedback_; }
    // One player-state event of the own player (pmove): its cgame sounds and, for a hard landing, the fall damage.
    void applyPlayerEvent(int ev, int parm);
    // Shell shock of the player (shellshock / stopshellshock builtins, src/game/shellshockworld.cpp): the precached
    // name index (1-based, 0 = none), the start time and the duration; shellShockRecord() is the loaded record, null
    // without an active shock. The shock is over once timeMs() >= startMs + durationMs.
    struct ShellShockState {
        int index = 0;
        int64_t startMs = 0;
        int durationMs = 0;
    };
    const ShellShockState& shellShock() const { return shellShock_; }
    const ShockRecord* shellShockRecord() const;
    // earthquake (0x4f4330) sends event 0xc7 (scale in event param, duration ms, origin, radius) to the clients: kept as a log for the
    // front end (TODO(spec): the cgame handler's shake). viewkick (0x4f7a40) stores client +0x8658 (count) and +0x865c (player origin - arg).
    struct Earthquake { float scale; int durationMs; Vec3 origin; float radius; int64_t startMs; };
    const std::vector<Earthquake>& earthquakes() const { return earthquakes_; }
    const std::optional<Vec3>& sunlightOverride() const { return sunlight_; }
    struct ViewKick { int count = 0; Vec3 dir{}; };
    const ViewKick& playerViewKick() const { return viewKick_; }
    // pm_flags 0x100000 (friendly under the crosshair, within g_friendlyfireDist) and 0x200000 (enemy within the
    // weapon's enemyCrosshairRange), set by the client think (0x535070).
    uint32_t playerCrosshairFlags() const { return crosshairFlags_; }
    // The crosshair name target (ps+0x8678, 0x535070) of the last update as the two HUD lines 0x534aa0 sends as configstrings
    // 0xb (name) and 0xc (weapon display name) for an actor; empty when there is no target or it has no name.
    // ps+0x8678 as an entity number (-1 = none): the entity islookingat (0x5399b0) compares against.
    int crosshairTarget() const { return crosshairTarget_; }
    const std::string& crosshairNameLine() const { return crosshairName_; }
    const std::string& crosshairWeaponLine() const { return crosshairWeaponLine_; }
    std::optional<float> playerDeadYaw() const { return playerDeadYaw_; }
    bool playerTurretEye(Vec3& eye);
    // Per rendered frame on a turret: the pitch/yaw the turret takes toward `view`
    // inside its arcs, and tag_player at a given pitch/yaw, so the drawn gun and the eye use one pair.
    bool playerTurretAnglesFor(const Vec3& view, float& pitch, float& yaw);
    bool playerTurretEyeAt(float pitch, float yaw, Vec3& eye);
    // The weapon of the turret the player uses (ps+0x568's weaponinfo), "" when not on one.
    // The turretWeapon of the vehicle the player rides ("" when none). 0xf70e50 is ps+0x568 (cg ps base 0xf708e8, weapon ps+0xb0 =
    // 0xf70998), the entity number VEH_LinkPlayer stores there (0x511774, 0x7ff = none); 0x4bd000 reads that entity's centity byte
    // +0xa2 = entityState weapon (+2), which the vehicle setup writes (0x50d8a3) as the weapon index of its turretWeapon name.
    std::string playerVehicleTurretWeapon();
    std::string playerTurretWeapon() const { return playerOnTurret() && playerTurret_ && playerTurret_->turret ? playerTurret_->turret->weapon : std::string(); }
    // The view clamp on a turret (PM_UpdateViewAngles 0x50b120 with eFlags & 0x300): pitch and yaw within
    // base ± range (ps+0xe8 / +0xec, +0xf0 / +0xf4).
    void clampPlayerView(float& pitch, float& yaw) const;
    // Getting off a turret (0x51b3d0): the origin to continue from and the stance to return to (client stance
    // event 0x8e prone / 0x8d crouch / 0x8c stand from turret +0x68; −1 none). Taken once by the front end.
    struct TurretRelease { Vec3 origin; int stance = -1; };
    std::optional<TurretRelease> takeTurretRelease() { return std::exchange(turretRelease_, std::nullopt); }
    // Script teleports of the player for the front end's player state: setorigin (0x538410 → SetClientOrigin
    // 0x4e8890) and setplayerangles (0x538560 → 0x4e8940).
    std::optional<Vec3> takePlayerTeleport() { return std::exchange(playerTeleport_, std::nullopt); }
    std::optional<Vec3> takePlayerViewAngles() { return std::exchange(playerViewAngles_, std::nullopt); }
    // playerlinktodelta / playerlinktoabsolute: the player is placed by the link (pm_type 1, no own movement);
    // absolute links also set pm_flags 0x8000 / 0x10000000 (src/game/playerlink.cpp).
    bool playerLinked() const { return playerLink_.has_value(); }
    // The cheat commands noclip / ufo (G_ClientCommand 0x4ea9b1 / 0x4ea9d8 -> 0x4ea170 / 0x4ea220): behind the cheat gate
    // (0x4e9500: sv_cheats set, else the client command `print "GAME_CHEATSNOTENABLED"`; player health above 0, else
    // `print "GAME_MUSTBEALIVECOMMAND"`) they toggle client+0x8628 / +0x862c and print GAME_NOCLIPON/OFF, GAME_UFOON/OFF.
    // Return whether the flag was toggled.
    bool cheatNoclip() { return cheatToggle(noclip_, "GAME_NOCLIPON", "GAME_NOCLIPOFF"); }
    bool cheatUfo() { return cheatToggle(ufo_, "GAME_UFOON", "GAME_UFOOFF"); }
    // god / demigod / notarget (0x4e9f90 / 0x4ea030 / 0x4ea0d0, same gate 0x4e9500): toggle client-entity flags +0x128 & 1 / 2 / 4 and print
    // GAME_GODMODE_ON/OFF, GAME_DEMI_GODMODE_ON/OFF, GAME_NOTARGETON/OFF. Effects: god is read by combat ($godmode on the player);
    // Consumers: god and demigod in G_Damage (0x5282b3 / 0x5283d2: the entity objects carry `$godmode` / `$demigod`), notarget in the enemy search
    // 0x54e230 (0x54e326), the cover script check 0x54bb30 (0x54bbc9) and getclosestenemysqdist 0x53c290 (0x53c37b): `$notarget` next to `ignoreme`.
    // TODO(spec): the entity event at 0x4e776b (damage 100000, means of death 0xc) that god / demigod (flags & 3) skip is not ported.
    bool cheatGod();
    bool cheatDemigod();
    bool cheatNotarget();
    bool playerGod() const { return god_; }
    bool playerDemigod() const { return demigod_; }
    bool playerNotarget() const { return notarget_; }
    bool playerNoclip() const { return noclip_; }
    bool playerUfo() const { return ufo_; }
    // ClientThink's pm_type for the player (0x4e7c05..0x4e7c5a): noclip 2, ufo 3, else by health and the link
    // (pmClientType, pmove.h). `linked` is the playerlinkto link (TODO(spec): other entity links, ent+0x1c8).
    int playerPmType() const;
    // The key table of the client (buttonpressed / getcommandfromkey); null in a headless run, where no key is bound or down.
    const ClientInput* clientInput = nullptr;
    // Bone controls of a turret model (0x51d740 / 0x4ab0d0) for turret pitch and yaw relative to its angles.
    // Client prone tilt of an actor's tag_origin at client time `nowMs` (0x4c1cc0): mode 1 (prone) pitch = f x +0x168,
    // roll 0; mode 2 (corpse blend) pitch f x +0x168, roll f x +0x16c; lift f x +0x164; false when neither applies.
    bool actorProneTilt(const Entity& e, int64_t nowMs, float& pitch, float& roll, float& lift) const;
    static std::vector<std::pair<int, assets::Quat>> turretBoneControls(const assets::XModelParts& parts, float pitch, float yaw,
                                                                         float drop = 0.0f);
    // The ridden vehicle's tag_body axes (rows forward, left, up); false when the player is not in a vehicle.
    bool playerVehicleBodyAxis(std::array<Vec3, 3>& axis);
    // The tank direction ownerdraws' (90 / 91, cgame 0x4aa1b0 / 0x4aa360) world yaws: the ridden vehicle's angles[1] (centity +0x158)
    // and the yaw of its tag_barrel forward (0x4ab9e0 + axis to angles 0x424b70); false when not in a vehicle.
    bool playerVehicleHudYaws(float& bodyYaw, float& barrelYaw, bool& hasBarrel);

    // Bounds of brush submodel "*N" (lump 35), used by triggers and brush models.
    bool submodelBounds(const std::string& model, Vec3& mins, Vec3& maxs) const;
    std::string vehicleCollmap(Entity& vehicle);
    std::string lookupVehicleCollmap(Entity& vehicle);
    bool tryPushEntity(Entity& ent, Entity& pusher, const Vec3& move, const Vec3& turn);
    void updateVehicleEngineSound(Entity& e);
    void vehicleJolt(Entity& e, const Vec3& dir, float intensity, float speedFraction = 0.0f, float decel = 0.0f);
    void vehicleTouch(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles);
    void vehicleTouchTriggers(Entity& v); // G_TouchTriggers (0x4e7320) for a vehicle (src/game/vehicletriggers.cpp)

    // Built-ins the scripts called that are not implemented yet, with call counts.
    const std::map<std::string, int>& missingBuiltins() const { return missing_; }
    // Called-by-scripts names that have no implementation (registered as counting no-ops by registerMissingStubs).
    const std::set<std::string>& stubbedBuiltins() const { return stubbed_; }
    std::function<void(const std::string&)> print = [](const std::string&) {};

    // Sound output, set by the front end. playSound(alias, origin or nullptr for local, &duration seconds)
    // returns a handle (0 = nothing played); stopSound(handle).
    std::function<int(const std::string&, const Vec3*, float*)> playSound = [](const std::string&, const Vec3*, float*) { return 0; };
    std::function<bool(const std::string&)> soundExists = [](const std::string&) { return true; }; // soundexists (0x4ef380 -> 0x428320)
    std::function<void(int)> stopSound = [](int) {};
    // Vehicle engine loop: a low/high alias pair blended by a fraction (Mixer::playBlend / setBlend).
    std::function<int(const std::string&, const std::string&, float, const Vec3&)> playBlendSound =
        [](const std::string&, const std::string&, float, const Vec3&) { return 0; };
    std::function<void(int, float, const Vec3&)> setBlendSound = [](int, float, const Vec3&) {};

    // Live script HUD elements (kind "hudelem"); fields as set by scripts, "$…" fields by built-ins.
    const std::vector<ObjectPtr>& hudElems() const { return hudElems_; }
    // HUD grenade indicator input (cgame 0x4be210 reads pos.trBase / pos.trDelta of eType 3 entities whose weapon has
    // projExplosionType "grenade", def +0x390 == 0): one entry per live grenade missile.
    struct HudGrenade { Vec3 base, delta; };
    std::vector<HudGrenade> hudGrenades();
    // HUD stock of the player's weapon when it has a shared cap group (0x4dcd60 group sum); -1 otherwise (use the weapon's own counters).
    int hudGroupStock(const std::string& weapon);

    const std::array<Objective, 16>& objectives() const { return objectives_; }
    // The worldspawn "northyaw" (configstring 10) as degrees; 0 when empty.
    float northYaw() const { return float(std::atof(northYaw_.c_str())); }
    // Compass friendlies (ownerdraw 87, server 0x4db2ee..0x4db37b): with drawcompassfriendlies (0x4f4450, flag 0x1c61050) on, every
    // living ally actor whose `drawoncompass` field (actor +0x2120) is set is listed with its origin and yaw.
    // pingEndMs: the entry's sound-ping end (0xf79a88 +16, 0 = none), read by the friendly pass of 0x4c0f20.
    struct CompassMarker { Vec3 pos{}; float yaw = 0; int64_t pingEndMs = 0; };
    std::vector<CompassMarker> compassFriendlies() const;
    // Compass sound pings (0x4c0e20): an actor's weapon fire (EV_FIRE_WEAPON 0x4b7800 -> 0x4b79bb, length of the fire sound) and an
    // actor's MG42 fire (0x4bb0b6, 50 ms at the turret origin). Axis actors' pings are drawn by ownerdraw 87 (compassping_enemyfiring).
    const CompassPings& compassPings() const { return compassPings_; }
    // Registers a ping for an actor (team string from its `team` field) at pos lasting durationMs (0x4c0e20).
    void compassPing(const Entity& actor, const Vec3& pos, int durationMs);
    // Compass tanks (ownerdraw 88, 0x4c1700): vehicles with addvehicletocompass set (+0x190), entity type 0xc: origin and yaw.
    std::vector<CompassMarker> compassVehicles() const;
    // Test hook (IW2_KILL_VEHICLES): damage an entity as an explosion without an attacker.
    // Debug (iw2game --item-test): a weapon item at the player's origin and the player's stock / clip of a weapon.
    void debugSpawnItem(const std::string& weapon);
    // Test hook: a weapon item of `weapon` at `origin` flying with `velocity` (TR_GRAVITY from now), run by G_RunItem (runItems).
    Entity& debugDropItem(const std::string& weapon, const Vec3& origin, const Vec3& velocity);
    static constexpr int kItemNoGround = 0x7ff; // groundEntityNum (+0xc) ENTITYNUM_NONE
    // Test hook: a rocket of `weapon` fired by the player from `start` along `dir` (G_FireRocket 0x527810).
    Entity& debugSpawnRocket(const std::string& weapon, const Vec3& start, const Vec3& dir) { return spawnRocket(*player_, weapon, start, dir); }
    Entity& debugSpawnGrenade(const std::string& weapon, const Vec3& start, const Vec3& velocity, int fuseMs) {
        return spawnGrenade(*player_, weapon, start, velocity, fuseMs);
    }
    // Test hook: a weapon item of `weapon` with the given item ammo (stock, clip; 0 = unset) at the player, used like F (0x5341a0 ->
    // Pickup_Weapon). Returns the dropped item (the swapped-out weapon), or nullptr.
    Entity* debugUseWeaponItem(const std::string& weapon, int stock, int clip);
    void debugSetClip(const std::string& weapon, int rounds);
    std::pair<int, int> debugAmmo(const std::string& weapon);
    // Tests: the hit-location multiplier (0x528100) for an attacker holding `weapon` ("" = no weapon).
    // Vehicle immunity filter of G_Damage (0x5122f0, called at the top of 0x528050 when ent+0x108 is set): true = ignored.
    static bool vehicleIgnoresDamage(const VehicleInfo& info, int mod, int dflags);
    float debugLocMultiplier(const std::string& weapon, const std::string& location) {
        player_->obj->set("$weapon", script::Value(weapon));
        return hitLocationMultiplier(weapon.empty() ? nullptr : player_, location);
    }
    void debugDamage(Entity& e, int amount);
    // Test hooks (iw2game IW2_TOUCH_TEST / IW2_ROCKET_TEST): the vehicle touch callback with the vehicle placed 100 units
    // behind `t` moving at 400 units/s towards it; a vehicle rocket fired from 300 units away at `t`.
    void debugVehicleTouch(Entity& v, Entity& t);
    void debugVehicleRocket(Entity& v, Entity& t);
    bool drawCompassFriendlies() const { return drawCompassFriendlies_; }
    void setDrawCompassFriendlies(bool on) { drawCompassFriendlies_ = on; }
    std::vector<ClientCommand> takeClientCommands() { return std::exchange(clientCommands_, {}); }
    // Script menus (src/game/menus.cpp): the client command "cmd mr <menu index> <response>" (exe 0x4ea700) notifies the player
    // "menuresponse" (menu name, response). menuNames() is the precached list (configstrings 0x4ad+index).
    void menuResponse(int menuIndex, const std::string& response);
    const std::vector<std::string>& menuNames() const { return menus_; }
    // freezecontrols(): client +0x8630 (0x5392e0); the front end ignores movement and button input while it is set.
    bool controlsFrozen() const { return controlsFrozen_; }
    // ClientThink 0x4e7bc0: pm_flags 0x8000 is set by freezecontrols (client +0x8630) or an absolute playerlinkto (+0x8698).
    bool playerControlsFrozen() const { return controlsFrozen_ || (playerLink_ && playerLink_->absolute); }
    // A level change requested by changelevel/missionsuccess/missionfailed that is due now (empty otherwise).
    std::optional<LevelChange> dueLevelChange() const {
        if (levelChange_.pending && timeMs_ >= levelChange_.atMs) return levelChange_;
        return std::nullopt;
    }
    // The server frame's handling of a due change (0x4d9f90): clears the due time (0x1c60f54) and returns what happens. With the
    // movie-pending flag (0x1c60fe4) the flag is cleared and either the stored cinematic plays (`cinematic <name> 0`, whose end
    // loads the map) or, without a name, the client gets `openvictoryscreen` (victoryScreen set, also queued as a client command)
    // and the map stays stored for `nextlevel`. Otherwise loadgame_continue (failed) or the map load (0x4d9f00).
    std::optional<LevelChange> takeDueLevelChange();
    // The stored next map (0x1c57d88) and the persistent flag of the last changelevel (0x1c60ff0), read by `nextlevel` (0x4d9f00).
    const std::string& storedNextMap() const { return levelChange_.map; }
    bool storedPersistent() const { return levelChange_.persistent; }
    // Player state handed over by a persistent level change (0x4d9dd1 -> 0x4e6c20 writes the client into a memory save). Set before
    // start(); prepareStart() gives it to the new player before the level script runs (stance, origin, view angles and pm_flags
    // are not part of the save).
    // Persistent level change (writer 0x4e6c20, reader 0x4e6de0): the player entity's +0x13c health and +0x140 maxhealth (range table
    // 0x56eeb0), the client's stats words ps+0x108..+0x117 (range table 0x56eec8: +0x108 health copy, +0x10c dead yaw, +0x110 max
    // health, +0x114 a byte flag) and the weapons. TODO(spec): the +0x114 byte flag (no port counterpart).
    struct CarriedPlayer {
        Inventory inventory;
        std::string weapon;
        std::optional<int> health, maxhealth; // entity +0x13c / +0x140 (the stats words +0x108 / +0x110 are copies of them)
        std::optional<float> deadYaw;         // ps+0x10c
    };
    void setCarriedPlayer(CarriedPlayer c) { carriedPlayer_ = std::move(c); }
    std::optional<CarriedPlayer> carriedPlayerState();
    // cinematic(name [, timeout]) (0x4f4220) only records the movie (client state 0x1c60ff4, up to 63 characters); it is played when
    // the level change falls due (0x4d9f90 runs `cinematic <name> 0`, whose end continues the level load, 0x49db20 -> 0x4d9f00).
    // The front end takes it with takeCinematic() right before it loads the next map.
    std::string takeCinematic() { return std::exchange(cinematic_, {}); }
    const std::string& mapName() const { return mapName_; }
    // Real (wall-clock) time in ms for the timers the original runs on timeGetTime (the death screen's
    // g_deathDelay). Hosts that never set it (iw2game) run them on world time.
    void setRealTimeMs(int64_t ms) { realMs_ = ms; }

    // Effects started by scripts since the last call: path, origin, forward.
    struct FxEvent { std::string path; Vec3 origin; Vec3 forward; int entity = -1; std::string tag; }; // entity/tag: bolted (0x4853f0)
    // World position of an entity tag (lower case), false when the entity or tag is gone.
    bool entityTagPosition(int number, const std::string& tag, Vec3& out);
    // The tag's world position and axis rows (forward, left, up = the bone rotation applied to x, y, z); hasAxes is false for
    // vehicle tags, whose rotation the bone-control path does not provide.
    bool entityTagOrientation(int number, const std::string& tag, Vec3& origin, std::array<Vec3, 3>& axes, bool& hasAxes);
    std::vector<FxEvent> takeFxEvents() { return std::exchange(fxEvents_, {}); }
    // Screen-shake events for the front end (ViewShake, cgame 0x4bf5e0): MG42 fire (event 0xaf: 0.05, 100 ms, radius 100) and earthquake (0xc7).
    struct ShakeEvent { Vec3 origin; float scale; int durationMs; float radius; int64_t startMs; };
    std::vector<ShakeEvent> takeShakeEvents() { return std::exchange(shakeEvents_, {}); }
    std::vector<ShakeEvent> shakeEvents_;
    std::vector<FxEvent> fxEvents_;
    // Bullet tracers requested by impact events (0x4b8140): muzzle point and impact point; the host applies the
    // cg_tracerchance roll and draws them (FxSystem::addTracer).
    struct TracerEvent { Vec3 from, to; };
    std::vector<TracerEvent> takeTracerEvents() { return std::exchange(tracerEvents_, {}); }
    std::vector<TracerEvent> tracerEvents_;
    // Sight-blocking particles of the effect runtime, rebuilt by the host after each effects update (0x48b000).
    void setFxSightBlockers(std::vector<FxSightBlocker> b) { fxSightBlockers_ = std::move(b); }
    // Visibility (0..1) through sight-blocking effects between two points (0x48b090).
    float fxVisibility(const Vec3& start, const Vec3& end) const;
    bool actorSightClear(Entity& actor, Entity& target, const Vec3& from, const Vec3& to, float maxDistSq);

    // The player's weapon inventory (throws for non-players).
    // Out-of-ammo weapon switch (cgame 0x4b7090): the weapon to select, or empty to keep the current one.
    std::string outOfAmmoSwitch(const Inventory& inv, const std::string& current);
    // Debug: spawns a weapon_<name> item at the player (iw2view IW2_DEBUG_ITEM, to exercise the touch pickup live).
    void debugSpawnWeaponItem(const std::string& name);
    Inventory& inventory(Entity& player);

    // Dvar names compare case-insensitively (dvar lookup 0x431260 -> 0x43f6d0, the same compare dvarStrList uses): scripts read
    // getCvar("g_gameSkill") for the dvar registered as g_gameskill.
    struct CvarNameLess {
        bool operator()(const std::string& a, const std::string& b) const {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                                [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
        }
    };
    std::map<std::string, std::string, CvarNameLess> cvars;
    // Key bindings of the front end: command (e.g. "+attack") → key names (getkeybinding).
    std::map<std::string, std::vector<std::string>> keyBindings;

    // Weapon definition (weapons/sp/<name>: "WEAPONFILE\\key\\value\\…"), or nullptr.
    const std::map<std::string, std::string>* weapon(const std::string& name);
    AnimLibrary& animLibrary() { return animLib_; }
    const CollisionWorld& collision() const { return *collision_; }

    // Player fires the current weapon from `eye` along `forward` (unit vector).
    // lastShot: the clip ran empty with this shot (event 0xa0 instead of 0x9e: the last-shot sound replaces the fire sound).
    // FireWeapon for the player (0x531c40). spreadDeg is the aim spread angle (fireSpreadDegrees, pmove.h), gunOffset the
    // usercmd gun offset added to the eye. Returns the velocity the shooter loses (a projectile weapon: forward * 64).
    Vec3 playerFire(const Vec3& eye, const Vec3& forward, bool lastShot = false, float spreadDeg = 0, const Vec3& gunOffset = {0, 0, 0});
    // Player weapon events (src/game/playeractions.cpp): the melee blow, the offhand throw and the offhand pick.
    void playerMelee(const Vec3& eye, const Vec3& viewAngles);
    void meleeBlow(Entity& attacker, const Vec3& eye, const Vec3& f, const Vec3& r, const Vec3& u, const std::string& weapon);
    void aiMelee(Entity& a);
    // trigger_damage (triggerdamage.cpp).
    void spawnTriggerDamage(Entity& e);
    void triggerDamageApply(Entity& e, Entity* attacker, int damage, int mod);
    void triggerDamageHit(Entity& e, Entity* attacker, int amount, const std::string& mod);
    void triggerDamageUse(Entity& e); // 0x529450
    void triggerDamageSegment(Entity* attacker, const Vec3& start, const Vec3& end, int damage, int mod, bool grenadeTouchOnly = false);
    void playerThrowGrenade(const std::string& weaponName, const Vec3& eye, const Vec3& viewAngles, const Vec3& playerVelocity,
                            int cookedFuseMs = 0);
    std::string playerOffhandFor(const std::string& offhandClass);
    void playerGrenadeInHand(const Vec3& eye);
    // Script notify on the player entity (ClientEvents 0x4e75f0: binocular events 0xa8..0xac -> "binocular_enter", "_exit",
    // "_fire", "_release", "_drop" via 0x46d0f0).
    void notifyPlayer(const std::string& name);
    // 0x4dc660: the first registered weapon with weaponType binoculars, nullptr when none is registered (here: loaded).
    const std::map<std::string, std::string>* binocularWeapon();
    bool isAlive(const Entity& e) const;
    bool takesDamage(const Entity& e) const;
    void bulletImpact(Entity& shooter, const std::map<std::string, std::string>* def, const Vec3& start, const Vec3& end,
                      const TraceResult& tr, Entity* hit);
    int64_t lastBloodMs_ = -100000; // time of the last flesh impact with blood (0xf7e8c0)
    bool canSee(Entity& from, Entity& to);
    struct Stats { int shots = 0, hits = 0, kills = 0, grenades = 0, grenadeExplosions = 0, bulletImpacts = 0, lookatTouches = 0, bulletPenetrations = 0, playerNoise = 0, actorNoise = 0, actorFoliage = 0, floatingActors = 0, groundHit = 0, groundMiss = 0, grenadeFlee = 0, grenadeCower = 0, grenadeFleeRetry = 0, rockets = 0, followLinks = 0, followReleases = 0, waterSplashes = 0; } stats;
    const PathGraph& paths() const { return paths_; }

private:
    Entity& spawnEntity(const std::string& classname);
    // force = the exe's third argument of the spawn core 0x532200: stalingradspawn passes 1 (skips the telefrag and the
    // player-sight tests), dospawn passes 0.
    Entity* spawnActor(Entity& spawner, bool force = true, const Value& targetname = Value());
    // Spawn checks (src/game/spawnchecks.cpp): the telefrag test 0x4e8500, the actor-box sight probe 0x532100, the
    // force-spawn eviction 0x4fcaf0 and the player's r.maxs[2] (stance box from pmove).
    bool spawnWouldTelefrag(const Entity& spawner);
    bool sightToActorBox(const Vec3& eye, const Vec3& pos, int pass1, int pass2) const;
    bool freeActorForForceSpawn();
    float playerBoxHeight() const;
    // Script delete (0x4ef570): notify "death", then G_FreeEntity (0x4d7c30).
    void deleteEntityScripted(Entity& e);
    // 0x1c61048: set by G_InitGame (0x4d99cb: 1, or 2 for a savegame load) and cleared by 0x4da540 once the level is
    // loaded (0x448aab, after the entity spawn, the level script's start and the first G_RunFrame). Here: until the end
    // of runLevelMain() or loadState().
    bool levelInitializing_ = true;
    // 0x1c61064: the entity number G_RunFrame's entity pass is running (0x4db022..0x4db0d3), -1 outside it. Here the
    // actor think loop sets it.
    int runEntityNumber_ = -1;
    Entity* entityOf(const Value& v);
    void spawnMapEntities();
    void dropPathNodes(); // 0x515f80 / 0x515d70: path nodes put on the ground at level start
    void dropSpawners();  // 0x5325f0: spawners put on the ground at level start
    bool actorPoolUnlimited_ = false; // debug (--spawn-all): the 32-slot actor pool limit of spawnActor is not applied
    bool mapStartSpawn_ = false; // spawnActor runs for the map-start placed actors (their drop sees the earlier entities only)
    void setLevelStartDropClips();
    void registerBuiltins();
    void registerMissingStubs();
    void runTriggers();
    bool triggerContact(Entity& e, const Vec3& absmin, const Vec3& absmax);
    void useAnimTree(Entity& e, const std::string& tree);
    void registerAnimBuiltins();
    void thinkActor(Entity& e);
    void updatePlayerMoved();
    void debugActorCounts();
    // G_RunFrame stage 8 (0x4db048): G_RunEntity (0x4da7e0) of one entity, once per frame (+0x130), its link parent
    // (+0x1c8) first; runEntityPass runs every entity in number order.
    void runEntity(Entity& e);
    void runEntityPass();
    std::vector<int> entityRunFrame_; // by entity number: the frame number of the last G_RunEntity (+0x130)
    int64_t frameNumber_ = 0;         // level.framenum (0x1c60530)
    void moveActor(Entity& e, float dt);

    // Combat (src/game/combat.cpp)
    struct BoneXform {
        Vec3 position;
        assets::Quat rotation;
    };
    const assets::XModelParts* skeleton(const std::string& model);
    std::unordered_map<std::string, BoneXform> entityBones(Entity& e);
    // The entity's models (body, then attachments) posed in entity space, with their hit boxes.
    std::vector<HitBoxModel> entityModels(Entity& e);
    // G_LocationalTrace (FUN_0044bf00 with locational set): world and entity clips, actors by hit boxes when a
    // priority map is given; `location` gets the hit location (0 when none).
    TraceResult locationalTrace(const Vec3& start, const Vec3& end, uint32_t mask, const Entity* pass, const uint8_t* priority,
                                int& location);
    Vec3 eyePosition(Entity& e);
    void damage(Entity& target, Entity* attacker, int amount, const Vec3& dir, const Vec3& point, const std::string& mod,
                const std::string& location = "none", int dflags = 0);
    // attacker: the entity credited with the damage (nullptr = the world).
    bool playerLastShot_ = false;
    Entity* fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount, Entity* attacker,
                      const std::string& weaponName = std::string(), int depth = 0);
    int weaponDamage(const std::string& weaponName);
    // Accuracy graph of a weapon for the target kind (weapon fields aiVsPlayerAccuracyGraph / aiVsAiAccuracyGraph); null without one.
    const AccuracyGraph* accuracyGraph(const std::string& weaponName, bool targetClient);
    std::map<std::string, std::optional<AccuracyGraph>> accuracyGraphs_;
    void updatePerception(Entity& e);
    // Perception helpers (src/game/perception.cpp)
    bool isSentient(const Entity& e) const;
    Vec3 sightEye(Entity& e, Vec3* forward);
    bool sightTest(Entity& from, const Vec3& eye, const Vec3& forward, const Vec3& target, float fovCos, float maxDistSq);
    bool sightCheck(Entity& actor, Entity& target);
    bool seesWithin(Entity& actor, Entity& target, int64_t withinMs); // 0x54ce10
    int threatOf(Entity& actor, Entity& target);
    void selectEnemy(Entity& actor);
    void setEnemy(Entity& actor, Entity* enemy, bool notify);
    void updateLastEnemySightPos(Entity& actor); // 0x4fe830
    int nearestNodeWithin(const Vec3& p, float radius) const;
    void registerCombatBuiltins();
    void registerSoundBuiltins();
    void registerShellShockBuiltins();
    void registerHudBuiltins();
    void registerInventoryBuiltins();
    void registerMoverBuiltins();
    // animscripted (src/game/animscripted.cpp).
    void registerScriptedAnimBuiltins();
    void registerPlayerLinkBuiltins();
    void updatePlayerLink();
    bool linkParentFrame(const Link& l, Vec3& origin, Axis& axis);
    void startScriptedAnim(Entity& e, const Vec3& origin, const Vec3& angles, int anim, int root, const std::string& notify,
                           bool deathplant);
    void updateScriptedAnim(Entity& e);
    void updateScriptedPose(Entity& e, bool slide);
    void stopScriptedAnim(Entity& e);
    int scriptedThink(Entity& e);
    void registerCampaignBuiltins();
    void objectiveMessage(int state, const std::string& text);
    std::array<Objective, 16> objectives_{};
    std::vector<ClientCommand> clientCommands_;
    bool noclip_ = false, ufo_ = false; // client+0x8628 / +0x862c
    bool god_ = false, demigod_ = false, notarget_ = false; // entity flags +0x128 & 1 / 2 / 4
    bool cheatToggle(bool& flag, const char* on, const char* off);
    std::vector<std::string> shockNames_;                 // precacheshellshock names (index - 1)
    std::vector<std::shared_ptr<const ShockRecord>> shockRecords_; // parallel to shockNames_
    ShellShockState shellShock_;
    int64_t lastObjectiveMessageMs_ = -1000;
    LevelChange levelChange_;
    std::string cinematic_; // pending cinematic name (video/<name>.roq)
    bool moviePending_ = false; // 0x1c60fe4: set by missionsuccess (0x4f4066) and cinematic (0x4f4220), consumed when the change is due
    std::optional<CarriedPlayer> carriedPlayer_;
    int exitMs_ = 0;        // level change / cinematic exit time (0x1c60fec)
    // Death screen (docs/spec/campaign.md "Death screen"): 0x4e8e10 and its timers.
    void openDeathScreen();
    void runDeathScreen();
    int64_t realNowMs() const { return realMs_ >= 0 ? realMs_ : timeMs_; }
    int64_t realMs_ = -1;
    int64_t playerRespawnMs_ = -1;   // client +0x866c: level time + 1700 when the player died (0x527e85)
    bool reloading_ = false;         // g_reloading
    int64_t loadGameAtRealMs_ = -1;  // 0x1c60f58: when loadgame_continue runs
    void runMover(Entity& e);
    void pruneMovers();
    void followLink(Entity& child);
    Entity* entityByNumber(int number);
    std::map<int, MoverState> movers_;
    std::map<int, Link> links_;
    std::map<int, Inventory> inventories_;
    bool autoPickup_ = true;          // setautopickup (client +0x86ac == 0)
    void touchItems();                // G_TouchTriggers for weapon items (0x4e7320 -> Touch_Item 0x521a90)
    bool touchWeaponItem(Entity& item); // Pickup_Weapon (0x521170) by touch; true when the item was used up
    int weaponRoom(Inventory& inv, const std::string& weapon); // 0x4dcc00
    int hudStock(Inventory& inv, const std::string& weapon); // 0x4dcd60 (HUD stock incl. the shared-ammo-cap group sum)
    void trimSharedAmmo(Inventory& inv, const std::string& weapon); // 0x520f3d..0x520ff5 (sharedAmmoCap over-fill)
    uint32_t meleeSeed_ = 0x89abcdef; // rand() state (0x5a56f4) for the melee damage spread
    std::string weaponField(const std::string& weaponName, const char* key);
    int weaponInt(const std::string& weaponName, const char* key);
    void fillClip(Inventory& inv, const std::string& weapon);
    void addAmmo(Inventory& inv, const std::string& weapon, int amount, bool fillClipToo);
    bool giveWeapon(Inventory& inv, const std::string& weapon);
    void takeWeapon(Inventory& inv, const std::string& weapon);
    std::vector<ObjectPtr> hudElems_;
    int startSound(const std::string& alias, const Vec3* origin, ObjectPtr notifyObj = nullptr, const std::string& notify = {});
    struct PendingNotify { int64_t atMs; ObjectPtr obj; std::string name; };
    std::vector<PendingNotify> pendingNotifies_;

    int nearestNode(const std::array<float, 3>& p) const;

    // Script menus (src/game/menus.cpp)
    void registerMenuBuiltins();
    std::vector<std::string> menus_;
    bool controlsFrozen_ = false;
    // Vehicles (src/game/vehicles.cpp)
    void registerVehicleBuiltins();
    // Turrets (src/game/turrets.cpp)
    void registerTurretBuiltins();
    void initTurret(Entity& e, const std::string& weaponInfo);
    void initMapTurrets();
    TurretState& turretOf(const Value& self);
    // Turret think (src/game/turretthink.cpp)
    void restoreDefaultDropPitch(Entity& t); // 0x51cf10
    // Actor turret state (src/game/actorturret.cpp)
    void releaseTurret(Entity& turret);
    const TurretState* nodeTurret(const PathNode& n); // the turret set by setturretnode (node +0x58), or null
    bool actorInGoal(Entity& e, float extra);
    bool goalVolumeHolds(Entity& e, const Vec3& p);
    bool workGoalVolumeHolds(Entity& e, const Vec3& p); // the working goal volume (+0x75c)
    bool inScriptGoal(Entity& e, const Vec3& p);         // 0x4fef50's test of the script goal block (+0x764)
    bool turretStateStart(Entity& e);
    int turretStateThink(Entity& e);
    int turretPoseStep(Entity& e, Entity& turret); // 0x549ba0
    bool turretObstruction(Entity& e, Entity& turret); // 0x54a8bc..0x54ab4d
    void turretStateFinish(Entity& e);
    // Actor state machine (src/game/actorstate.cpp)
    void aiRequest(Entity& e, int type, int state);
    void aiSetState(Entity& e, int state);
    bool aiPushState(Entity& e, int state);
    void aiPopState(Entity& e);
    void aiKillState(Entity& e, int state);
    void aiApplyRequests(Entity& e);
    void aiResumeBelow(Entity& e);
    bool aiStart(Entity& e, int state);
    void aiFinish(Entity& e, int state);
    void aiSuspend(Entity& e, int state);
    bool aiResume(Entity& e, int state);
    int aiThink(Entity& e, int state);
    void actorThink(Entity& e);
    void actorPerceive(Entity& e);
    void actorPain(Entity& e, Entity* attacker);
    void actorPainNode(Entity& e, Entity* attacker, bool nodeRelease);
    void actorMoveNoise(Entity& e);
    void actorFoliageSound(Entity& e); // 0x548650
    void blockFacingNodes(Entity& e);
    void actorDie(Entity& e, Entity* attacker);
    void actorBecomeCorpse(Entity& e);
    void setActorAnimScript(Entity& e, const std::string& name, int movemode, int animMode);
    void killActorAnimScript(Entity& e);
    bool actorAnimScriptDone(Entity& e);
    int exposedThink(Entity& e);
    void exposedSubstate(Entity& e);
    void exposedCombat(Entity& e);
    void exposedNoncombat(Entity& e);
    void actorFacing(Entity& e); // 0x505d80 + 0x504ca0 (goal facing)
    void setFacing(Entity& e, bool override, float pitch, float yaw);  // 0x504950
    void setFacingDir(Entity& e, bool override, const Vec3& dir);      // 0x5055d0
    void faceMotion(Entity& e, bool override);                         // 0x505630
    bool mayFaceEnemyMoving(const Entity& e, const Vec3& dir) const;   // 0x545580
    // The rest of the move frame (actorfacing.cpp; the selector above is actorFacing).
    void setLookAngles(Entity& e, float pitch, float yaw);            // 0x5049a0
    void initActorFacing(Entity& e);                                  // spawn 0x4fc7c9..0x4fc806
    void faceBlockToAngles(Entity& e, bool look);                     // 0x53e237 / 0x54b1f3: base block from the entity angles
    void turnActorByDelta(Entity& e, float yawDelta);                 // 0x504aa0
    void actorLookTurn(Entity& e);                                    // 0x504be0
    bool nodesSeeEachOther(int a, int b) const;                       // 0x517e40
    int enemyPositionNode(Entity& e, Entity& target, const SentientInfo* rec); // 0x5432f0 / 0x547330
    bool faceThreat(Entity& e, bool override);                        // 0x505790
    void actorMoveFrame(Entity& e);                                   // 0x504450 / 0x504160
    void updateActorPath(Entity& e);
    bool actorHasPath(const Entity& e) const;
    void clearActorPath(Entity& e);
    void followActorPath(Entity& e, bool run, bool wide = false);
    int stepActorPath(Entity& e, bool wide, bool run);
    bool followShrink(Entity& e, int bumped);   // 0x500d80
    void updateFollowed(Entity& e);
    void retargetFollowed(Entity& e, Entity& player); // 0x4fec50
    void scanFollowed(Entity& e);                      // 0x504630
    void standOrFollow(Entity& e, const std::string& script); // 0x504630 + 0x53ae40 / 0x53ade0            // 0x4fedc0
    void linkBumpedActor(Entity& e, int hit);  // +0x318 bookkeeping only
    void stuckRecovery(Entity& e, ActorSlide& pm, const Vec3& before); // 0x501d00 after a failed move block
    bool yieldFollowed(Entity& e, ActorSlide& pm, const SlideTraceFn& fn, const Vec3& before); // 0x501620
    bool followClear(Entity& e, const Vec3& newPos); // 0x501380
    bool nearClaimedNode(const Entity& a) const; // 0x504610
    Vec3 pathEnd(const Entity& e) const { return e.path.pts[0].pos; }
    void combatScript(Entity& e);
    std::string stopScript(Entity& e);
    bool enemyEyeTraceOk(Entity& e);
    bool muzzlePosition(Entity& e, Vec3& out);                                // 0x500b40
    bool muzzleTraceOk(Entity& e, const Vec3& muzzle, const Vec3& target);  // 0x54cf40
    bool seesEnemyRecently(Entity& e, int64_t withinMs);
    int painThink(Entity& e);
    int deathThink(Entity& e);
    int customThink(Entity& e);
    int negotiationThink(Entity& e);               // 0x54b760
    void replanFromSentient(Entity& e);            // 0x5037e0
    void moveActorKind(Entity& e, int kind, const Vec3& delta, float dt); // actor physics for the anim modes 4-6 (kinds 2-4)
    bool negotiationTrigger(Entity& e, const Vec3& o, bool grounded); // the state 9 branch of 0x503ec0
    void registerActorStateBuiltins();
    // Cover nodes and claiming (src/game/actorcover.cpp)
    bool nodeUsable(Entity& e, int node);
    void linkOverlappingNodes();                    // 0x52d990 (lazily, once)
    void claimNode(Entity& e, int node, bool takeOver = false);  // takeOver: 0x519050 (evicts a claiming owner without the 0x40 flag test)
    void releaseNode(Entity& e);
    void giveUpClaimedNode(Entity& e);
    void blockNodeForTeam(int node, Entity& e);
    // Friendly chains (src/game/friendlychain.cpp)
    void buildFriendlyChains();
    void setFriendlyChain(Entity& sentient, int node);
    void touchFriendlyChain(Entity& trigger, Entity& other);
    void followFriendlyChain(Entity& e);
    void updateWorkingGoal(Entity& e);                                  // 0x505f70
    bool actorGoal(Entity& e, Vec3& pos, float& radius, float& height); // the goal the movement code reads
    bool coverScriptAt(Entity& e, int node, bool checkEnemy, std::string& script);
    bool nodeFacesEnemy(Entity& e, int node);
    bool claimedNodeGood(Entity& e, int node);
    float coverScore(Entity& e, int node);
    std::vector<int> findCoverNodes(Entity& e, int better, int ignore, int prefer, size_t max);
    int sentientNode(Entity& e);          // 0x515040 (cached)
    int sentientNodePlanes(Entity& e, const std::vector<PathPlane>& planes); // 0x5150d0 (uncached, with planes)
    bool keepClaimedNode(Entity& e);
    bool atClaimedNode(Entity& e);
    bool inActorGoal(Entity& e, const Vec3& p);
    void updateClaimedNode(Entity& e);
    void updateClaimedNodeCore(Entity& e); // 0x4fe740
    bool nearClaimedNodeOf(const Entity& a, const Vec3& pos) const; // 0x502660
    bool pathToPoint(Entity& e, const Vec3& target, bool allowTraversal = true); // 0x503650 (third argument)
    bool pathToSentient(Entity& e, const Vec3& target);
    // AI events (src/game/actorevents.cpp): 0x535770 / 0x535860 / dispatch 0x535d70
    void actorLearns(Entity& target, Entity& about); // 0x500c50
    bool actorKnowsRecently(const Entity& target, const Entity& about, int64_t ms) const; // 0x54ddc0
    void aiEventDispatch(Entity& source, Entity& target, int type); // 0x535d70
public:
    // G_ClientEvents (0x4e75f0), the AI part: the player's pmove events since the last call (ring of 4) tell the enemy
    // actors around: run steps and landings (events 1..0x17, 0x46..0x73) as AI event 2 (radius 512), walk steps
    // (0x18..0x2e) as event 3 (radius 256), prone steps (0x2f..0x45) nothing; none while crouched (pm_flags 2).
    void playerPmEvents(int eventSeq, const int (&events)[4], bool crouched);
private:
    int playerEventSeq_ = 0;
    // Receivers of an AI event by team (the mask in EAX of 0x535770: 0 = the source's enemy team, 1 << team otherwise).
    enum class AiHear { All, Enemy, Own };
    void painBroadcast(Entity& e); // 0x54d300
    void aiEventPoint(Entity& source, int type, const Vec3& pos, float radius2 = 0, bool forced = true, AiHear hear = AiHear::All); // 0x535770
    void aiEventLine(Entity& source, int type, const Vec3& a, const Vec3& b, float radius2 = 0,
                     bool forced = true); // 0x535860 + 0x535f20
    // Path planner (src/game/actorpath.cpp)
    void addDangerPlane(Entity& e, const Entity& source, const Vec3& a, const Vec3& b); // 0x54b3b0 (slot 0x54b370)
    void expireDangerPlanes(Entity& e);  // 0x54b4d0 (from the actor think 0x4ff030)
    void removeDangerPlanesFrom(int sourceNumber); // 0x54b5a0 (sentient release 0x4fe170)
    std::vector<PathPlane> dangerPlanesOf(const Entity& e) const; // 0x54b670
    bool hasFreeDangerPlane(const Entity& e) const; // 0x54b610
    bool nodeBeyondPlanes(int node, const std::vector<PathPlane>& planes) const; // 0x542710
    std::vector<std::pair<int, float>> nodesInRange(const Vec3& center, float radius, float height, size_t max,
                                                    uint32_t typeMask) const;
    int queryNode(const Vec3& pos, float radius, bool lowered, uint32_t typeMask, const std::vector<PathPlane>& planes,
                  int* first);
    bool searchNodes(int team, int start, int goal, const Vec3& goalPos, const std::vector<PathPlane>& planes,
                     std::vector<int>& chain, float capSq = -1.0f, const std::function<bool(const Vec3&)>* nodeOk = nullptr);
    bool pathToPointNear(Entity& e, const Vec3& target, float capSq);
    // 0x541da0: useGoalPos is its fifth argument (the goal point is added when it differs from the goal node), allowTraversal its
    // sixth (path flag 0x10; a negotiation pair on the chain refuses the whole path when it is false). Returns false on refusal.
    bool buildActorPath(Entity& e, const std::vector<int>& chain, const Vec3& goalPos, bool useGoalPos = true, bool allowTraversal = true);
    void dropPathGoalPoints(ActorPath& p, int n);
    void pathClearBudget(ActorPath& p, const Vec3& o);
    void pathLookReset(Entity& e, const Vec3& o, bool correct);
    void pathLookNew(Entity& e, const Vec3& o);
    void pathBackUp(ActorPath& p, const Vec3& o);
    void pathAdvance(ActorPath& p, const Vec3& o);
    void pathCorrectBudget(ActorPath& p, const Vec3& o);
    void pathLookAhead(Entity& e, const Vec3& o, bool shrink);
    void pathLookPoint(Entity& e, const Vec3& o, const Vec3& pt, bool shrink, float segLen, int index, float area);
    void pathFarDir(ActorPath& p, const Vec3& o);
    void updatePathLook(Entity& e, const Vec3& o, bool shrink, bool correct);
    bool replanActorPath(Entity& e, const Vec3& o);
    // Path following (src/game/actorfollow.cpp)
    bool aiReach(const Vec3& start, const Vec3& target, int passEnt, uint32_t mask, Vec3& out, float step);
    bool aiReachPast(Vec3 start, const Vec3& target, const std::vector<int>& ents, int passEnt, uint32_t mask,
                     Vec3& out);
    bool pathLookClear(const ActorPath& p, const Vec3& o, const Vec3& target);
    void resolveBlocker(Entity& a);
    bool holdsPosition(Entity& e);
    bool pathDetour(Entity& e, const Vec3& o, const Vec3& a, const Vec3& b, int k, const std::vector<int>& ents,
                    uint32_t mask, bool split);
    bool pathToNodeDirect(Entity& e, int node, bool withPlanes = false); // 0x503890 (param 3: the plane variant)
    bool linkBlocked(size_t node, size_t link, int team) const;
    // Grenades (src/game/grenades.cpp, src/game/actorgrenade.cpp)
    float gravity();
    float weaponFloat(const std::string& weaponName, const char* key);
    Vec3 grenadeAngles(const GrenadeState& g, int64_t atMs) const;
    Vec3 grenadePosition(const GrenadeState& g, int64_t atMs) const;
    Vec3 grenadeVelocity(const GrenadeState& g, int64_t atMs) const;
    Entity& spawnGrenade(Entity& thrower, const std::string& weaponName, const Vec3& start, const Vec3& velocity, int fuseMs);
    bool bounceGrenade(Entity& e, const TraceResult& tr, int64_t hitMs);
    int nextRand() { meleeSeed_ = meleeSeed_ * 0x343fd + 0x269ec3; return int(meleeSeed_ >> 0x11 & 0x7fff); } // rand() (0x5a56f4)
    void radiusDamage(const Vec3& origin, Entity* attacker, float inner, float outer, float radius, Entity* ignore,
                      const std::string& mod);
    bool canDamage(Entity& t, const Vec3& from, float radius, const Vec3* at = nullptr);
    bool ignorePlayerRadiusDamage_ = false; // setplayerignoreradiusdamage (0x1c61058)
    bool sparePlayerRadiusDamage_ = false;  // in effect during radiusdamage (0x1c61054)
    void explodeGrenade(Entity& e);
    // Impact effects (cgame 0x4b9090): impact type (0 bullet_small_normal … 9 grenade_explode, 10 rocket_explode)
    // × surface type (0 default … 22 asphalt) → effect file, "" for none.
    const std::string& impactEffect(int type, int surface);
    TraceResult missileTrace(const Vec3& from, const Vec3& to, uint32_t clip, int pass, const Vec3& trDelta); // 0x526d6b..0x526df7
    std::vector<std::array<std::string, 23>> impactEffects_;
    void forgetGrenade(Entity& g);
    void runGrenade(Entity& e);
    Entity& spawnRocket(Entity& parent, const std::string& weaponName, const Vec3& start, const Vec3& dir);
    void missileExplosionEvent(const std::string& weaponName, const Vec3& at, const Vec3& dir, int surface);
    void rocketImpact(Entity& e, const TraceResult& tr);
    void runRocket(Entity& e);
    void pruneMissiles(); // drops removed grenades / rockets from their lists after the entity pass
    void destabilizeRocket(Entity& e); // 0x526960
    // Weapon items (classname weapon_*): LaunchItem (0x521f70 → 0x521e00) gives a dropped item a gravity
    // trajectory; G_RunItem (0x523040) moves it and stops it where it lands; FinishSpawningItem (0x5225e0) drops a
    // map-placed one to the floor. `launchItem` starts the fall of a dropped item from its dropper.
    void launchItem(Entity& item, const Entity& dropper);
    void runItem(Entity& e);
    void scriptSpawnItem(Entity& item); // G_SpawnItem 0x522c70, runtime branch
    void predictGrenadeLanding(Entity& e);
    bool solveThrow(Entity& actor, const std::string& method, const Vec3& start, const Vec3& aim, Vec3& vel);
    bool throwArcClear(Entity& actor, const Vec3& start, const Vec3& vel, const Vec3& target);
    bool throwSolution(Entity& actor, const Vec3& from, const Vec3& armOffset, const std::string& method, const Vec3& target,
                       bool anyway, Vec3& start, Vec3& vel);
    Vec3 bounceAim(Entity& actor, const std::string& weapon, const Vec3& start, const Vec3& target); // 0x51ebe0
    float randomRange(float lo, float hi);                       // flrand 0x4d8480 on the rand() state 0x5a56f4
    float grenadeHintArc() const;                                // ai_debugGrenadeHintArc (0x1c13f58)
    bool grenadeHintLobThrow(Entity& actor, const Vec3& start, const Vec3& target, Vec3& vel);  // 0x51f050
    bool grenadeHintFromLanding(Entity& actor, const Vec3& landing, Vec3& vel);                // 0x51f320
    std::vector<Vec3> grenadeHints_;  // info_grenade_hint origins in spawn order (0x1c8f180, count 0x1c6103c), max 512
    void releaseHeldGrenade(Entity& actor);
    void registerGrenadeBuiltins();
    float grenadeDangerRadius(Entity& g);
    bool safeFromGrenade(Entity& e, Entity& g, const Vec3& pos);
    void setActorGrenade(Entity& e, Entity* g);
    void grenadePing(Entity& g);
    bool grenadeStateStart(Entity& e);
    void grenadeStateFinish(Entity& e);
    void grenadeReact(Entity& e, bool keepPath);
    bool pathNearPoint(Entity& e, const Vec3& p, float r2);
    int grenadeCoverNode(Entity& e, float r);
    bool pathAwayFrom(Entity& e, const Vec3& p, float dist, bool usePlanes = false);
    bool grenadeAcquire(Entity& e);
    bool grenadeGiveUp(Entity& e, bool skipAcquire);
    void grenadeHitCharacter(Entity& grenade, Entity* hit);
    int grenadeAcquireThink(Entity& e);
    int grenadeThrowBackThink(Entity& e);
    void grenadeCower(Entity& e);
    int grenadeStateThink(Entity& e);
    // Bad places (src/game/badplace.cpp)
    bool insideBadPlace(const BadPlace& b, const Vec3& p) const;
    bool inBadPlace(const Vec3& p) const;
    void markBadPlaceLinks(const BadPlace& b, int delta);
    void removeBadPlace(size_t i);
    void addBadPlace(const std::string& name, int durationMs, uint32_t teamMask, BadPlace data);
    void recheckBadPlaceActors();
    void runBadPlaces();
    bool badPlaceFlee(Entity& e);
    bool fleePathToNode(Entity& e, int node, bool withPlanes = false);
    int badPlaceThink(Entity& e);
    void registerBadPlaceBuiltins();
    void connectEntityPaths(Entity& e);
    void disconnectEntityPaths(Entity& e);
    bool pathToNode(Entity& e, int node);
    bool pathToGoal(Entity& e);
    bool pathToSideGoal(Entity& e, float side); // 0x502a5b..0x502c58 (actorpath.cpp)
    bool stopForEnemy(Entity& e, bool fighting, int node, bool hadPath);           // 0x503200
    bool enemyNearPathAhead(Entity& e, const Vec3& ep, const Vec3& look, bool hadPath); // 0x502e80
    bool claimedNodeSeesEnemyNode(Entity& e, Entity& enemy);                        // 0x502fc0
    bool aiMoveCheck(Entity& e, const Vec3& target, float step, Vec3& out);
    void registerCoverBuiltins();
    void registerProneBuiltins(); // src/game/proneworld.cpp
    void proneEnter(Entity& e, int ms);      // 0x514b30
    void proneExit(Entity& e, int ms);       // 0x514c40
    void proneRefit(Entity& e, bool fit);    // 0x5022b0
    void actorProneFrame(Entity& e);         // the prone part of the actor think 0x4ff030
    void proneCorpseStart(Entity& e, bool conversion); // 0x548980 (death start) / 0x4fdba0 (corpse) blend start
    void corpseProneFit(Entity& e, bool smooth);       // 0x4fd9e0
    void syncEntityClips(); // src/game/entityclips.cpp
    void thinkTurret(Entity& t);
    void turretFireSound(Entity& t, bool playerOwner);
    void shootTurret(Entity& t);
    bool turretTag(Entity& t, const char* tag, Vec3& pos);
    bool turretAnglesTo(Entity& t, const Vec3& point, Vec3& angles);
    bool turretAimSentient(Entity& t, Entity& target, Vec3& point, Vec3& angles);
    bool turretTargetValid(Entity& t, Entity& target, float* distSq);
    bool turretEngageSentient(Entity& t, Entity& target);
    bool turretEngagePoint(Entity& t, Entity* target, const Vec3& point);
    void turretUpdateState(Entity& t, bool aiming, const Vec3& point, const Vec3& angles);
    void turretIdle(Entity& t);
    void turretTurn(Entity& t, bool tracking);
    void turretSetState(Entity& t, int state);
    void linkVehicleNodes();
    void thinkVehicle(Entity& e);
    VehicleState& vehicleState(const script::Value& self);
    int vehicleNodeIndex(const script::Value& v) const;
    const VehicleInfo* vehicleInfo(const std::string& type);
    void updateVehicleTurret(Entity& e);
    Vec3 vehicleTagPosition(Entity& e, const char* tag, bool& found, Vec3* turnedForward = nullptr);
    void linkPlayerToVehicle(Entity& vehicle, Entity& player);
    void unlinkPlayerFromVehicle(Entity& vehicle, Entity& player);
    void alignVehicleToGround(Entity& e, Vec3& origin, Vec3& angles, float baseZ, const Vec3& prevAngles, bool settle);
    void vehiclePathMove(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles);
    void vehicleFreeMove(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles);
    void vehiclePlayerAim(Entity& e);
    void useVehicle(Entity& vehicle, Entity& user);
    bool vehicleUseBounds(Entity& e, Vec3& mins, Vec3& maxs);
    void runPlayerUse();
    // Player use of entities (src/game/playeruse.cpp).
    struct UseCandidate { Entity* e; float score; };
    std::vector<UseCandidate> scanUsables(const Vec3& eye, const Vec3& forward);
    Vec3 playerEye();
    void findPlayerUseEntity();
    std::string pickupHintText(const std::string& w);
    void pickUpWeaponItem(Entity& item);
    void usePlayerEntity(Entity& e);
    bool turretUsableByPlayer(Entity& t);
    void playerUseTurret(Entity& t);
    void thinkPlayerTurret();
    void releasePlayerTurret(Entity& t);
    int playerUseState_ = 0;             // player entity +0x112: 1 on a turret, 2 getting off
    Entity* playerTurret_ = nullptr;     // ps+0x568
    uint32_t playerEFlags_ = 0;          // ps eFlags 0x100 / 0x200 (both: on a standing turret)
    float viewClampBase_[2] = {0, 0}, viewClampRange_[2] = {0, 0}; // ps+0xe8 / +0xec, +0xf0 / +0xf4
    int playerUseEntity_ = -1;           // ps+0x580: what the use key would use now
    bool playerDeadWeaponless_ = false;  // player_die cleared the entity's weapon (ent+2 = 0)
    DamageFeedback damageFeedback_;
    std::vector<Earthquake> earthquakes_;
    std::optional<Vec3> sunlight_;          // setsunlight override (config string 9); empty after resetsunlight
    ViewKick viewKick_;
    uint32_t crosshairFlags_ = 0;
    std::string crosshairName_, crosshairWeaponLine_;
    void updateCrosshairTarget();
    int damageBlood_ = 0;                // client +0x8658
    Vec3 damageFrom_{};                  // +0x865c
    bool damageFromWorld_ = false;       // +0x8668
    void damageFeedback();
    std::optional<float> playerDeadYaw_; // ps+0x10c from LookAtKiller
    HintStringTable hintStrings_;       // sethintstring configstrings 0x4cd..0x4ec (0x4f0da0)
    PlayerUseHint playerUseHint_;       // ps+0x578 / +0x57c
    std::optional<TurretRelease> turretRelease_;
    std::optional<Vec3> playerTeleport_, playerViewAngles_;
    struct PlayerLink {
        int parent = -1;
        std::string tag;
        float fraction = 0.0f; // client +0x8694
        bool absolute = false; // client +0x8698
        bool haveRef = false;  // client +0x8680
        assets::Quat ref;      // client +0x8684 (kept unconjugated here)
    };
    std::optional<PlayerLink> playerLink_;
    PlayerCommand playerCmd_;
    uint32_t prevButtons_ = 0; // the buttons of the previous command (meleebuttonpressed)
    uint32_t playerOldButtons_ = 0;
    uint32_t pmFlags_ = 0;
    std::vector<VehicleNode> vnodes_;
    std::map<std::string, std::unique_ptr<VehicleInfo>> vehicleInfos_;
    std::unordered_map<const script::Object*, int> vnodeIndex_;

    const fs::Vfs& vfs_;
    std::map<std::string, char> keyTypes_; // radiant/*.txt map key types
    std::string mapName_;
    std::vector<uint8_t> bspData_;
    std::unique_ptr<assets::D3dBsp> bsp_;
    AnimLibrary animLib_;
    script::VM vm_;
    std::vector<std::unique_ptr<Entity>> entities_;
    std::unordered_map<const script::Object*, Entity*> byObject_;
    std::vector<PathNode> nodes_;
    bool nodeLinksDone_ = false;
    // The node kd-tree (built once by 0x52de40): `nodeTreeOrder_` is the node array as the build permutes it, the tree
    // nodes are {axis (-1 = leaf), split, left, right} or, for a leaf, {-1, -, count, first}.
    struct NodeTreeEnt { int axis; float split; int a, b; };
    mutable std::vector<int> nodeTreeOrder_;
    mutable std::vector<NodeTreeEnt> nodeTree_;
    mutable size_t nodeTreeFor_ = ~size_t(0);
    int buildNodeTree(int first, int count) const;
    FriendlyChains chains_;
    std::vector<ObjectPtr> vehicleNodes_;
    Entity* player_ = nullptr;
    Vec3 playerLastPos_{};                    // sentient +0x10: the player's origin at the last frame (0x4e8194)
    bool playerMoved_ = false;                // client byte +0x86b6: the player moved 0.1 units or more (xy) since the last frame (0x4e81e5)
    int64_t timeMs_ = 0;
    float fogSightSq_ = 3.4028235e38f;    // 0x1c61038: ((far - near) * 0.828 + near)² of linear fog, else FLT_MAX (0x4e96e0)
    float cullDist_ = 0;                  // setculldist (configstring 8): renderer far-plane distance, docs/spec/render-hints.md
    std::string northYaw_ = "0";
    bool drawCompassFriendlies_ = false;
    CompassPings compassPings_;
    int crosshairTarget_ = -1; // ps+0x8678 as an entity number (updateCrosshairTarget)
    std::vector<FxSightBlocker> fxSightBlockers_;
    int nextEntityNumber_ = 0;
    int32_t nextFx_ = 0;
    std::vector<std::string> fxNames_;   // loadfx index → effect path
    struct LoopedFx { int id; float delayS; Vec3 origin; int64_t nextMs; };
    std::vector<LoopedFx> loopedFx_;
    std::map<std::string, int> missing_;
    std::set<std::string> stubbed_;
    std::vector<std::pair<Vec3, Vec3>> submodels_;
    std::unique_ptr<CollisionWorld> collision_;
    std::map<std::string, std::optional<assets::XModelParts>> skeletons_;
    std::map<std::string, std::vector<assets::XBoneBox>> boneBoxes_; // xmodel bone hit boxes, filled by skeleton()
    std::map<std::string, std::pair<Vec3, Vec3>> modelBounds_;
    std::map<std::string, std::vector<assets::XCollSurf>> collSurfs_; // xmodel collision surfaces, filled by skeleton()
    ObjectPtr worldEntityObj_; // the world entity (g_entities[2046] = 0x1c13b08), the attacker of damage without one
    uint32_t entityContents(const Entity& e) const;          // gentity +0xc0 (setcontents 0x4efd70; spawn values 0x4fc74f / 0x4e90e2 / 0x533440 / 0x533480 / 0x5334d0)
    static bool isScriptMover(const Entity& e);              // eType 5 (0x5332b0): script_model, script_brushmodel, script_origin
    void vehicleTouchDamage(Entity& vehicle, Entity& other); // 0x512d40 (src/game/vehicles.cpp)
    float hitLocationMultiplier(const Entity* attacker, const std::string& location); // 0x528100 (combat.cpp)
    std::vector<float> locDmgTable_;
    bool locDmgTableLoaded_ = false;
    Value attackerValue(Entity* attacker); // 0x5280b8 / 0x5280c9: a null attacker becomes the world entity (combat.cpp)
    std::map<std::string, std::string> collmapByModel_; // vehicle model → collmap brush model (src/game/entityclips.cpp)        // xmodel mins / maxs, filled by skeleton()
    std::mt19937 rng_{12345};
    std::shared_ptr<std::mt19937> scriptRng_; // randomint / randomfloat (registerCoreBuiltins)
    int perceptionTick_ = 0;
    PathGraph paths_;
    struct NodeSearch { int id = 0; int next = -1, prev = -1; int parent = -1; float g = 0, h = 0; bool queued = false; };
    Vec3 playerPrev_{};                                       // player origin last frame (its velocity)
    bool havePlayerPrev_ = false;
    std::vector<NodeSearch> search_;                          // per-node A* state (+0x5c … +0x70)
    int searchId_ = 0;                                        // 0x1c61044
    std::vector<std::vector<std::array<uint8_t, 4>>> linkBlock_; // per link and team: bad places blocking it (+8)
    std::vector<std::vector<uint8_t>> linkOff_;                  // per link: disconnectpaths count (link +6; disabled links leave the enabled range, 0x519560)
    std::map<int, std::vector<std::pair<int, int>>> pathRecs_;   // per entity number: links it disabled (records 0x1c8da40)
    std::array<BadPlace, 32> badPlaces_{};
    std::vector<Entity*> grenades_;
    std::vector<Entity*> rockets_;
    // save queue (src/game/savestate.cpp)
    struct PendingSave { std::string name, description, image; int type = 0, flags = 0, saveId = 0; };
    std::vector<PendingSave> pendingSaves_;  // 0x825538, at most 3
    bool autosaveThisFrame_ = false;         // 0x8259e0
    int saveCounter_ = 0;                    // 0x1b15ac4
    bool saveRecentlyLoaded_ = false;        // 0x1b15ac0
    bool saveFailed_ = false;
    bool commitRequested_ = false;           // 0x1b15ac1
    std::optional<std::pair<int, SaveFile>> uncommitted_; // savegamenocommit result waiting for commitsave
    std::vector<SaveFile> saveFiles_;
    void registerSaveBuiltins();
    void processSaves();
    std::map<std::string, std::optional<std::map<std::string, std::string>>> weapons_;
};

} // namespace iw2::game
