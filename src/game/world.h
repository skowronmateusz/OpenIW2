// Game world: map entities, path nodes, the player, and the script VM that drives them.
#pragma once

#include "game/bulletdamage.h"
#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/animtree.h"
#include "game/axis.h"
#include "game/inventory.h"
#include "game/movers.h"
#include "game/campaign.h"
#include "game/fx.h"
#include "game/collision.h"
#include "game/friendlychain.h"
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
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::game {

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

// The actor's state machine (actor +0x8 … +0xa4, +0x104, +0x204 … +0x220, +0x25c, +0x704).
struct ActorAi {
    std::array<int, 5> stack{AiExposed};      // +0x8, depth +0x1c
    int depth = 0;
    int64_t baseStateMs = 0;                  // +0x20 time the bottom state was last set
    struct Request { int type, state; };      // 1 replace, 2 push, 3 pop
    std::vector<Request> requests;            // +0x30, count +0x88
    std::array<int, 5> predicted{AiExposed};  // +0x8c: the stack once the requests are applied, depth +0xa0
    int predictedDepth = 0;
    int substate = 0;                         // +0x28
    int64_t perceptionFrameMs = -1;           // +0x24
    int64_t painStartMs = 0;                  // +0xa4
    int facing = 0;                           // +0x104 (TODO(spec): its effect on the actor's yaw)
    int orient = 0;                           // +0x114 orientmode override (0 = none, else a facing mode 1..6)
    int64_t meleeFrameMs = -1;                // +0xcc: level time of the last melee blow (one per frame, 0x550c7a)
    float orientTarget = 0;                   // +0x120 override turn target (degrees, 16-bit quantised)
    int animMode = 0, animModeOverride = 0;   // +0x218 / +0x21c
    int movemode = 0;                         // +0x214: 0 stop, 1 stop_soon, 2 walk, 3 run
    bool animDeltaGoalWeights = false;        // +0x220 anim deltas weigh by goal weights (TODO(spec): the states that set it)
    script::Value customScript;               // +0x208 animcustom function
    std::string negotiationScript;            // +0x208 for state 9: the begin node's animscript key (animscripts/traverse/<key>, 0x543760)
    bool alive = true;                        // +0x25c
    int64_t stopSoonMs = 0;                   // +0x704
    int64_t planTimeMs = 0;                   // +0x720 earliest time of the next path plan without a path
    int moveNoiseMs = 0;                      // actor +0x23c.. (0x224 struct +0x1c): the movement noise accumulator 0x5485a0
    int64_t detourMs = 0;                     // +0x724 earliest time of the next detour / blocked wait
    struct Entity* blocker = nullptr;         // +0x728 actor whose blocking this one waits out (itself: blocked)
    struct Entity* blockerEnt = nullptr;      // +0x72c the sentient blocking the way
    struct Entity* followed = nullptr;        // +0x2148 actor being followed (TODO(spec): set from +0x318)
    int64_t thoughtMs = -1;                   // last think (its next think, gentity +0x138, is 50 ms later)
    bool pathToEnemy = false;                 // +0x788 pathing towards the enemy (set by 0x503200, TODO(spec))
    std::vector<int> coverNodes;              // +0x798 findcovernode result, count +0x1738 (best last)
    std::vector<int> reacquireNodes;          // +0x173c findreacquirenode result, count +0x1764 (best last)
    bool corpse = false;                      // turned into a corpse (0x4fdba0)
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
};

// A rocket missile entity (G_FireRocket 0x527810; src/game/rockets.cpp).
struct RocketState {
    std::string weapon;
    struct Entity* parent = nullptr; // +0x134
    int64_t explodeMs = 0;           // +0x138 think
    int64_t trTime = 0;              // s.pos: linear (2)
    Vec3 trBase{}, trDelta{};
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
    int spawnflags = 0;             // +0x4: 0x40 priority (setnodepriority), 0x8000 see 0x54bb30, 1 not a cover candidate
    Entity* owner = nullptr;        // +0x40: sentient that claimed it (or just left it)
    int64_t ownerUntilMs = 0;       // +0x44: kNodeClaimed while claimed, else the owner keeps it until then
    std::array<int64_t, 4> teamBlockedMs{}; // +0x44 + 4·team: unusable for that team until then (1 axis, 2 allies)
};

class World {
public:
    World(const fs::Vfs& vfs, std::string mapName);

    // Spawns map entities and the player, then runs maps/<map>.gsc::main().
    void start();
    // Advances game time by one server frame (50 ms).
    void frame(bool advanceTime = true);

    script::VM& vm() { return vm_; }
    const assets::D3dBsp& bsp() const { return *bsp_; }
    int64_t timeMs() const { return timeMs_; }

    Entity* player() { return player_; }
    const std::vector<std::unique_ptr<Entity>>& entities() const { return entities_; }
    const std::vector<PathNode>& nodes() const { return nodes_; }

    // Debug: spawns one AI from each actor spawner (exercises the aitype/animscripts path).
    int spawnAllActors();

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
                a.obj->set("$goalnodeangles", Value());
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
    const PlayerUseHint& playerUseHint() const { return playerUseHint_; }
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
    const std::string& crosshairNameLine() const { return crosshairName_; }
    const std::string& crosshairWeaponLine() const { return crosshairWeaponLine_; }
    std::optional<float> playerDeadYaw() const { return playerDeadYaw_; }
    bool playerTurretEye(Vec3& eye);
    // Per rendered frame on a turret: the pitch/yaw the turret takes toward `view`
    // inside its arcs, and tag_player at a given pitch/yaw, so the drawn gun and the eye use one pair.
    bool playerTurretAnglesFor(const Vec3& view, float& pitch, float& yaw);
    bool playerTurretEyeAt(float pitch, float yaw, Vec3& eye);
    // The weapon of the turret the player uses (ps+0x568's weaponinfo), "" when not on one.
    // The turretWeapon of the vehicle the player rides ("" when none). TODO(spec): the original reads the weapon index from
    // cg_entities[0xf70e50]+0xa2 (0x4bd000); that it is the vehicle's turret weapon is a hypothesis.
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
    // Bone controls of a turret model (0x51d740 / 0x4ab0d0) for turret pitch and yaw relative to its angles.
    static std::vector<std::pair<int, assets::Quat>> turretBoneControls(const assets::XModelParts& parts, float pitch, float yaw);
    // The ridden vehicle's tag_body axes (rows forward, left, up); false when the player is not in a vehicle.
    bool playerVehicleBodyAxis(std::array<Vec3, 3>& axis);

    // Bounds of brush submodel "*N" (lump 35), used by triggers and brush models.
    bool submodelBounds(const std::string& model, Vec3& mins, Vec3& maxs) const;
    std::string vehicleCollmap(Entity& vehicle);
    bool tryPushEntity(Entity& ent, Entity& pusher, const Vec3& move, const Vec3& turn);
    void updateVehicleEngineSound(Entity& e);
    void vehicleJolt(Entity& e, const Vec3& dir, float intensity, float speedFraction = 0.0f, float decel = 0.0f);
    void vehicleTouch(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles);

    // Built-ins the scripts called that are not implemented yet, with call counts.
    const std::map<std::string, int>& missingBuiltins() const { return missing_; }
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

    const std::array<Objective, 16>& objectives() const { return objectives_; }
    // The worldspawn "northyaw" (configstring 10) as degrees; 0 when empty.
    float northYaw() const { return float(std::atof(northYaw_.c_str())); }
    // Compass friendlies (ownerdraw 87, server 0x4db2ee..0x4db37b): with drawcompassfriendlies (0x4f4450, flag 0x1c61050) on, every
    // living ally actor whose `drawoncompass` field (actor +0x2120) is set is listed with its origin and yaw.
    struct CompassMarker { Vec3 pos{}; float yaw = 0; };
    std::vector<CompassMarker> compassFriendlies() const;
    // Compass tanks (ownerdraw 88, 0x4c1700): vehicles with addvehicletocompass set (+0x190), entity type 0xc: origin and yaw.
    std::vector<CompassMarker> compassVehicles() const;
    // Test hook (IW2_KILL_VEHICLES): damage an entity as an explosion without an attacker.
    // Debug (iw2game --item-test): a weapon item at the player's origin and the player's stock / clip of a weapon.
    void debugSpawnItem(const std::string& weapon);
    void debugSetClip(const std::string& weapon, int rounds);
    std::pair<int, int> debugAmmo(const std::string& weapon);
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
    // A level change requested by changelevel/missionsuccess/missionfailed that is due now (empty otherwise).
    std::optional<LevelChange> dueLevelChange() const {
        if (levelChange_.pending && timeMs_ >= levelChange_.atMs) return levelChange_;
        return std::nullopt;
    }
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
    std::vector<FxEvent> takeFxEvents() { return std::exchange(fxEvents_, {}); }
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
    void playerFire(const Vec3& eye, const Vec3& forward, bool lastShot = false);
    // Player weapon events (src/game/playeractions.cpp): the melee blow, the offhand throw and the offhand pick.
    void playerMelee(const Vec3& eye, const Vec3& viewAngles);
    void meleeBlow(Entity& attacker, const Vec3& eye, const Vec3& f, const Vec3& r, const Vec3& u, const std::string& weapon);
    void aiMelee(Entity& a);
    // trigger_damage (triggerdamage.cpp).
    void spawnTriggerDamage(Entity& e);
    void triggerDamageApply(Entity& e, Entity* attacker, int damage, int mod);
    void triggerDamageHit(Entity& e, Entity* attacker, int amount, const std::string& mod);
    void triggerDamageSegment(Entity* attacker, const Vec3& start, const Vec3& end, int damage, int mod);
    void playerThrowGrenade(const std::string& weaponName, const Vec3& eye, const Vec3& viewAngles, const Vec3& playerVelocity);
    std::string playerOffhandFor(const std::string& offhandClass);
    void playerGrenadeInHand(const Vec3& eye);
    bool isAlive(const Entity& e) const;
    bool takesDamage(const Entity& e) const;
    void bulletImpact(Entity& shooter, const std::map<std::string, std::string>* def, const Vec3& start, const Vec3& end,
                      const TraceResult& tr, Entity* hit);
    int64_t lastBloodMs_ = -100000; // time of the last flesh impact with blood (0xf7e8c0)
    bool canSee(Entity& from, Entity& to);
    struct Stats { int shots = 0, hits = 0, kills = 0, grenades = 0, grenadeExplosions = 0, bulletImpacts = 0, lookatTouches = 0, bulletPenetrations = 0, playerNoise = 0, actorNoise = 0, groundHit = 0, groundMiss = 0, grenadeFlee = 0, grenadeCower = 0, grenadeFleeRetry = 0, rockets = 0; } stats;
    const PathGraph& paths() const { return paths_; }

private:
    Entity& spawnEntity(const std::string& classname);
    // force = the exe's third argument of the spawn core 0x532200: stalingradspawn passes 1 (skips the telefrag test),
    // dospawn passes 0.
    Entity* spawnActor(Entity& spawner, bool force = true);
    Entity* entityOf(const Value& v);
    void spawnMapEntities();
    void registerBuiltins();
    void registerMissingStubs();
    void runTriggers();
    bool triggerContact(Entity& e, const Vec3& absmin, const Vec3& absmax);
    void useAnimTree(Entity& e, const std::string& tree);
    void registerAnimBuiltins();
    void thinkActors();
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
                const std::string& location = "none");
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
    int threatOf(Entity& actor, Entity& target);
    void selectEnemy(Entity& actor);
    void setEnemy(Entity& actor, Entity* enemy, bool notify);
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
    void runScriptedModels();
    int scriptedThink(Entity& e);
    void registerCampaignBuiltins();
    void objectiveMessage(int state, const std::string& text);
    std::array<Objective, 16> objectives_{};
    std::vector<ClientCommand> clientCommands_;
    std::vector<std::string> shockNames_;                 // precacheshellshock names (index - 1)
    std::vector<std::shared_ptr<const ShockRecord>> shockRecords_; // parallel to shockNames_
    ShellShockState shellShock_;
    int64_t lastObjectiveMessageMs_ = -1000;
    LevelChange levelChange_;
    std::string cinematic_; // pending cinematic name (video/<name>.roq)
    int exitMs_ = 0;        // level change / cinematic exit time (0x1c60fec)
    // Death screen (docs/spec/campaign.md "Death screen"): 0x4e8e10 and its timers.
    void openDeathScreen();
    void runDeathScreen();
    int64_t realNowMs() const { return realMs_ >= 0 ? realMs_ : timeMs_; }
    int64_t realMs_ = -1;
    int64_t playerRespawnMs_ = -1;   // client +0x866c: level time + 1700 when the player died (0x527e85)
    bool reloading_ = false;         // g_reloading
    int64_t loadGameAtRealMs_ = -1;  // 0x1c60f58: when loadgame_continue runs
    void runMovers();
    Entity* entityByNumber(int number);
    std::map<int, MoverState> movers_;
    std::map<int, Link> links_;
    std::map<int, Inventory> inventories_;
    bool autoPickup_ = true;          // setautopickup (client +0x86ac == 0)
    void touchItems();                // G_TouchTriggers for weapon items (0x4e7320 -> Touch_Item 0x521a90)
    bool touchWeaponItem(Entity& item); // Pickup_Weapon (0x521170) by touch; true when the item was used up
    int weaponRoom(Inventory& inv, const std::string& weapon); // 0x4dcc00
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
    void thinkTurrets();
    void restoreDefaultDropPitch(Entity& t); // 0x51cf10
    // Actor turret state (src/game/actorturret.cpp)
    void releaseTurret(Entity& turret);
    bool actorInGoal(Entity& e, float extra);
    bool turretStateStart(Entity& e);
    int turretStateThink(Entity& e);
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
    void actorMoveNoise(Entity& e);
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
    void updateActorPath(Entity& e);
    bool actorHasPath(const Entity& e) const;
    void clearActorPath(Entity& e);
    void followActorPath(Entity& e, bool run, bool wide = false);
    int stepActorPath(Entity& e, bool wide, bool run);
    Vec3 pathEnd(const Entity& e) const { return e.path.pts[0].pos; }
    void combatScript(Entity& e);
    std::string stopScript(Entity& e);
    bool seesEnemyRecently(Entity& e, int64_t withinMs);
    int painThink(Entity& e);
    int deathThink(Entity& e);
    int customThink(Entity& e);
    int negotiationThink(Entity& e);               // 0x54b760
    void replanFromSentient(Entity& e);            // 0x5037e0
    bool negotiationTrigger(Entity& e, const Vec3& o, bool grounded); // the state 9 branch of 0x503ec0
    void registerActorStateBuiltins();
    // Cover nodes and claiming (src/game/actorcover.cpp)
    bool nodeUsable(Entity& e, int node);
    void claimNode(Entity& e, int node);
    void releaseNode(Entity& e);
    void giveUpClaimedNode(Entity& e);
    void blockNodeForTeam(int node, Entity& e);
    // Friendly chains (src/game/friendlychain.cpp)
    void buildFriendlyChains();
    void setFriendlyChain(Entity& sentient, int node);
    void touchFriendlyChain(Entity& trigger, Entity& other);
    void followFriendlyChain(Entity& e);
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
    bool pathToPoint(Entity& e, const Vec3& target);
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
                     std::vector<int>& chain);
    void buildActorPath(Entity& e, const std::vector<int>& chain, const Vec3& goalPos);
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
    void radiusDamage(const Vec3& origin, Entity* attacker, float inner, float outer, float radius, Entity* ignore,
                      const std::string& mod);
    bool canDamage(Entity& t, const Vec3& from, float radius, const Vec3* at = nullptr);
    bool ignorePlayerRadiusDamage_ = false; // setplayerignoreradiusdamage (0x1c61058)
    bool sparePlayerRadiusDamage_ = false;  // in effect during radiusdamage (0x1c61054)
    void explodeGrenade(Entity& e);
    // Impact effects (cgame 0x4b9090): impact type (0 bullet_small_normal … 9 grenade_explode, 10 rocket_explode)
    // × surface type (0 default … 22 asphalt) → effect file, "" for none.
    const std::string& impactEffect(int type, int surface);
    std::vector<std::array<std::string, 23>> impactEffects_;
    void forgetGrenade(Entity& g);
    void runGrenade(Entity& e);
    void runGrenades();
    Entity& spawnRocket(Entity& parent, const std::string& weaponName, const Vec3& start, const Vec3& dir);
    void missileExplosionEvent(const std::string& weaponName, const Vec3& at, const Vec3& dir, int surface);
    void rocketImpact(Entity& e, const TraceResult& tr);
    void runRockets();
    // Weapon items (classname weapon_*): LaunchItem (0x521f70 → 0x521e00) gives a dropped item a gravity
    // trajectory; G_RunItem (0x523040) moves it and stops it where it lands; FinishSpawningItem (0x5225e0) drops a
    // map-placed one to the floor. `launchItem` starts the fall of a dropped item from its dropper.
    void launchItem(Entity& item, const Entity& dropper);
    void runItems();
    void predictGrenadeLanding(Entity& e);
    bool solveThrow(Entity& actor, const std::string& method, const Vec3& start, const Vec3& aim, Vec3& vel);
    bool throwArcClear(Entity& actor, const Vec3& start, const Vec3& vel, const Vec3& target);
    bool throwSolution(Entity& actor, const Vec3& from, const Vec3& armOffset, const std::string& method, const Vec3& target,
                       bool anyway, Vec3& start, Vec3& vel);
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
    bool grenadeGiveUp(Entity& e);
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
    bool aiMoveCheck(Entity& e, const Vec3& target, float step, Vec3& out);
    void registerCoverBuiltins();
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
    void thinkVehicles();
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
    FriendlyChains chains_;
    std::vector<ObjectPtr> vehicleNodes_;
    Entity* player_ = nullptr;
    int64_t timeMs_ = 0;
    float cullDist_ = 0;                  // setculldist (configstring 8), unused
    std::string northYaw_ = "0";
    bool drawCompassFriendlies_ = false;
    std::vector<FxSightBlocker> fxSightBlockers_;
    int nextEntityNumber_ = 0;
    int32_t nextFx_ = 0;
    std::vector<std::string> fxNames_;   // loadfx index → effect path
    struct LoopedFx { int id; float delayS; Vec3 origin; int64_t nextMs; };
    std::vector<LoopedFx> loopedFx_;
    std::map<std::string, int> missing_;
    std::vector<std::pair<Vec3, Vec3>> submodels_;
    std::unique_ptr<CollisionWorld> collision_;
    std::map<std::string, std::optional<assets::XModelParts>> skeletons_;
    std::map<std::string, std::vector<assets::XBoneBox>> boneBoxes_; // xmodel bone hit boxes, filled by skeleton()
    std::map<std::string, std::pair<Vec3, Vec3>> modelBounds_;
    std::map<std::string, std::vector<assets::XCollSurf>> collSurfs_; // xmodel collision surfaces, filled by skeleton()
    ObjectPtr worldEntityObj_; // the world entity (g_entities[2046] = 0x1c13b08), the attacker of damage without one
    void vehicleTouchDamage(Entity& vehicle, Entity& other); // 0x512d40 (src/game/vehicles.cpp)
    Value attackerValue(Entity* attacker); // 0x5280b8 / 0x5280c9: a null attacker becomes the world entity (combat.cpp)
    std::map<std::string, std::string> collmapByModel_; // vehicle model → collmap brush model (src/game/entityclips.cpp)        // xmodel mins / maxs, filled by skeleton()
    std::mt19937 rng_{12345};
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
    std::map<std::string, std::optional<std::map<std::string, std::string>>> weapons_;
};

} // namespace iw2::game
