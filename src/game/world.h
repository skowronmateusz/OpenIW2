// Game world: map entities, path nodes, the player, and the script VM that drives them.
#pragma once

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/animtree.h"
#include "game/inventory.h"
#include "game/movers.h"
#include "game/campaign.h"
#include "game/fx.h"
#include "game/collision.h"
#include "game/hitboxes.h"
#include "game/pathnodes.h"
#include "game/perception.h"
#include "game/turrets.h"
#include "game/vehicles.h"
#include "script/vm.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::game {

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
    int animMode = 0, animModeOverride = 0;   // +0x218 / +0x21c
    int movemode = 0;                         // +0x214: 0 stop, 1 stop_soon, 2 walk, 3 run
    bool animDeltaGoalWeights = false;        // +0x220 anim deltas weigh by goal weights (TODO(spec): the states that set it)
    script::Value customScript;               // +0x208 animcustom function
    bool alive = true;                        // +0x25c
    int64_t stopSoonMs = 0;                   // +0x704
    int64_t planTimeMs = 0;                   // +0x720 earliest time of the next path plan without a path
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
    std::unique_ptr<GrenadeState> grenade; // grenade missile (src/game/grenades.cpp)
    int claimedNode = -1;                  // sentient +0x2c: claimed path node (script field "node")
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
    void frame();

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
    };
    void setPlayerCommand(const PlayerCommand& c) { playerCmd_ = c; }
    // pm_flags bits of the vehicle code: 0x20000 in a vehicle, 0x40000 the view yaw turns with the vehicle.
    uint32_t playerPmFlags() const { return pmFlags_; }
    // On a turret (ps eFlags & 0x300, set by the turret use 0x51db50).
    bool playerOnTurret() const { return (playerEFlags_ & 0x300u) != 0; }
    // The eye on a turret (0x4e8b70 → 0x4d7730): the turret's tag_player.
    bool playerTurretEye(Vec3& eye);
    // The view clamp on a turret (PM_UpdateViewAngles 0x50b120 with eFlags & 0x300): pitch and yaw within
    // base ± range (ps+0xe8 / +0xec, +0xf0 / +0xf4).
    void clampPlayerView(float& pitch, float& yaw) const;
    // Getting off a turret (0x51b3d0): the origin to continue from and the stance to return to (client stance
    // event 0x8e prone / 0x8d crouch / 0x8c stand from turret +0x68; −1 none). Taken once by the front end.
    struct TurretRelease { Vec3 origin; int stance = -1; };
    std::optional<TurretRelease> takeTurretRelease() { return std::exchange(turretRelease_, std::nullopt); }
    // Bone controls of a turret model (0x51d740 / 0x4ab0d0) for turret pitch and yaw relative to its angles.
    static std::vector<std::pair<int, assets::Quat>> turretBoneControls(const assets::XModelParts& parts, float pitch, float yaw);
    // The ridden vehicle's tag_body axes (rows forward, left, up); false when the player is not in a vehicle.
    bool playerVehicleBodyAxis(std::array<Vec3, 3>& axis);

    // Bounds of brush submodel "*N" (lump 35), used by triggers and brush models.
    bool submodelBounds(const std::string& model, Vec3& mins, Vec3& maxs) const;

    // Built-ins the scripts called that are not implemented yet, with call counts.
    const std::map<std::string, int>& missingBuiltins() const { return missing_; }
    std::function<void(const std::string&)> print = [](const std::string&) {};

    // Sound output, set by the front end. playSound(alias, origin or nullptr for local, &duration seconds)
    // returns a handle (0 = nothing played); stopSound(handle).
    std::function<int(const std::string&, const Vec3*, float*)> playSound = [](const std::string&, const Vec3*, float*) { return 0; };
    std::function<void(int)> stopSound = [](int) {};

    // Live script HUD elements (kind "hudelem"); fields as set by scripts, "$…" fields by built-ins.
    const std::vector<ObjectPtr>& hudElems() const { return hudElems_; }

    const std::array<Objective, 16>& objectives() const { return objectives_; }
    std::vector<ClientCommand> takeClientCommands() { return std::exchange(clientCommands_, {}); }
    // A level change requested by changelevel/missionsuccess/missionfailed that is due now (empty otherwise).
    std::optional<LevelChange> dueLevelChange() const {
        if (levelChange_.pending && timeMs_ >= levelChange_.atMs) return levelChange_;
        return std::nullopt;
    }
    const std::string& mapName() const { return mapName_; }

    // Effects started by scripts since the last call: path, origin, forward.
    struct FxEvent { std::string path; Vec3 origin; Vec3 forward; };
    std::vector<FxEvent> takeFxEvents() { return std::exchange(fxEvents_, {}); }
    std::vector<FxEvent> fxEvents_;
    // Sight-blocking particles of the effect runtime, rebuilt by the host after each effects update (0x48b000).
    void setFxSightBlockers(std::vector<FxSightBlocker> b) { fxSightBlockers_ = std::move(b); }
    // Visibility (0..1) through sight-blocking effects between two points (0x48b090).
    float fxVisibility(const Vec3& start, const Vec3& end) const;

    // The player's weapon inventory (throws for non-players).
    Inventory& inventory(Entity& player);

    std::map<std::string, std::string> cvars;
    // Key bindings of the front end: command (e.g. "+attack") → key names (getkeybinding).
    std::map<std::string, std::vector<std::string>> keyBindings;

    // Weapon definition (weapons/sp/<name>: "WEAPONFILE\\key\\value\\…"), or nullptr.
    const std::map<std::string, std::string>* weapon(const std::string& name);
    AnimLibrary& animLibrary() { return animLib_; }
    const CollisionWorld& collision() const { return *collision_; }

    // Player fires the current weapon from `eye` along `forward` (unit vector).
    void playerFire(const Vec3& eye, const Vec3& forward);
    // Player weapon events (src/game/playeractions.cpp): the melee blow, the offhand throw and the offhand pick.
    void playerMelee(const Vec3& eye, const Vec3& viewAngles);
    void playerThrowGrenade(const std::string& weaponName, const Vec3& eye, const Vec3& viewAngles, const Vec3& playerVelocity);
    std::string playerOffhandFor(const std::string& offhandClass);
    void playerGrenadeInHand(const Vec3& eye);
    bool isAlive(const Entity& e) const;
    bool canSee(Entity& from, Entity& to);
    struct Stats { int shots = 0, hits = 0, kills = 0, grenades = 0, grenadeExplosions = 0; } stats;
    const PathGraph& paths() const { return paths_; }

private:
    Entity& spawnEntity(const std::string& classname);
    Entity* spawnActor(Entity& spawner);
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
    Entity* fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount, Entity* attacker);
    int weaponDamage(const std::string& weaponName);
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
    void registerHudBuiltins();
    void registerInventoryBuiltins();
    void registerMoverBuiltins();
    void registerCampaignBuiltins();
    void objectiveMessage(int state, const std::string& text);
    std::array<Objective, 16> objectives_{};
    std::vector<ClientCommand> clientCommands_;
    int64_t lastObjectiveMessageMs_ = -1000;
    LevelChange levelChange_;
    void runMovers();
    Entity* entityByNumber(int number);
    std::map<int, MoverState> movers_;
    std::map<int, Link> links_;
    std::map<int, Inventory> inventories_;
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
    int ambientHandle_ = 0, musicHandle_ = 0;
    int nearestNode(const std::array<float, 3>& p) const;

    // Vehicles (src/game/vehicles.cpp)
    void registerVehicleBuiltins();
    // Turrets (src/game/turrets.cpp)
    void registerTurretBuiltins();
    void initTurret(Entity& e, const std::string& weaponInfo);
    void initMapTurrets();
    TurretState& turretOf(const Value& self);
    // Turret think (src/game/turretthink.cpp)
    void thinkTurrets();
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
    void actorDie(Entity& e, Entity* attacker);
    void actorBecomeCorpse(Entity& e);
    void setActorAnimScript(Entity& e, const std::string& name, int movemode, int animMode);
    void killActorAnimScript(Entity& e);
    bool actorAnimScriptDone(Entity& e);
    int exposedThink(Entity& e);
    void exposedSubstate(Entity& e);
    void exposedCombat(Entity& e);
    void exposedNoncombat(Entity& e);
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
    void registerActorStateBuiltins();
    // Cover nodes and claiming (src/game/actorcover.cpp)
    bool nodeUsable(Entity& e, int node);
    void claimNode(Entity& e, int node);
    void releaseNode(Entity& e);
    void giveUpClaimedNode(Entity& e);
    void blockNodeForTeam(int node, Entity& e);
    bool coverScriptAt(Entity& e, int node, bool checkEnemy, std::string& script);
    bool nodeFacesEnemy(Entity& e, int node);
    bool claimedNodeGood(Entity& e, int node);
    float coverScore(Entity& e, int node);
    std::vector<int> findCoverNodes(Entity& e, int better, int ignore, int prefer, size_t max);
    int sentientNode(Entity& e);
    bool keepClaimedNode(Entity& e);
    bool atClaimedNode(Entity& e);
    bool inActorGoal(Entity& e, const Vec3& p);
    void updateClaimedNode(Entity& e);
    bool pathToPoint(Entity& e, const Vec3& target);
    // Path planner (src/game/actorpath.cpp)
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
    bool pathToNodeDirect(Entity& e, int node);
    bool linkBlocked(size_t node, size_t link, int team) const;
    // Grenades (src/game/grenades.cpp, src/game/actorgrenade.cpp)
    float gravity();
    float weaponFloat(const std::string& weaponName, const char* key);
    Vec3 grenadePosition(const GrenadeState& g, int64_t atMs) const;
    Vec3 grenadeVelocity(const GrenadeState& g, int64_t atMs) const;
    Entity& spawnGrenade(Entity& thrower, const std::string& weaponName, const Vec3& start, const Vec3& velocity, int fuseMs);
    bool bounceGrenade(Entity& e, const TraceResult& tr, int64_t hitMs);
    void radiusDamage(const Vec3& origin, Entity* attacker, float inner, float outer, float radius, Entity* ignore,
                      const std::string& mod);
    bool canDamage(Entity& t, const Vec3& from, float radius, const Vec3* at = nullptr);
    void explodeGrenade(Entity& e);
    void forgetGrenade(Entity& g);
    void runGrenade(Entity& e);
    void runGrenades();
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
    bool pathAwayFrom(Entity& e, const Vec3& p, float dist);
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
    bool fleePathToNode(Entity& e, int node);
    int badPlaceThink(Entity& e);
    void registerBadPlaceBuiltins();
    bool pathToNode(Entity& e, int node);
    bool pathToGoal(Entity& e);
    bool aiMoveCheck(Entity& e, const Vec3& target, float step, Vec3& out);
    void registerCoverBuiltins();
    void syncEntityClips(); // src/game/entityclips.cpp
    void thinkTurret(Entity& t);
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
    Vec3 vehicleTagPosition(Entity& e, const char* tag, bool& found);
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
    std::optional<TurretRelease> turretRelease_;
    PlayerCommand playerCmd_;
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
    std::vector<ObjectPtr> vehicleNodes_;
    Entity* player_ = nullptr;
    int64_t timeMs_ = 0;
    std::string northYaw_ = "0";
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
    std::map<std::string, std::pair<Vec3, Vec3>> modelBounds_;        // xmodel mins / maxs, filled by skeleton()
    std::mt19937 rng_{12345};
    int perceptionTick_ = 0;
    PathGraph paths_;
    struct NodeSearch { int id = 0; int next = -1, prev = -1; int parent = -1; float g = 0, h = 0; bool queued = false; };
    Vec3 playerPrev_{};                                       // player origin last frame (its velocity)
    bool havePlayerPrev_ = false;
    std::vector<NodeSearch> search_;                          // per-node A* state (+0x5c … +0x70)
    int searchId_ = 0;                                        // 0x1c61044
    std::vector<std::vector<std::array<uint8_t, 4>>> linkBlock_; // per link and team: bad places blocking it (+8)
    std::array<BadPlace, 32> badPlaces_{};
    std::vector<Entity*> grenades_;
    std::map<std::string, std::optional<std::map<std::string, std::string>>> weapons_;
};

} // namespace iw2::game
