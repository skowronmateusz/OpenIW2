// Game world: map entities, path nodes, the player, and the script VM that drives them.
#pragma once

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/animtree.h"
#include "game/inventory.h"
#include "game/movers.h"
#include "game/campaign.h"
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

// Script-visible entity. Native state lives in the object's fields so scripts and engine share one view.
struct Attachment {
    std::string model;
    std::string tag;
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
    // Movement towards the script goal.
    std::vector<std::array<float, 3>> path;
    size_t pathIndex = 0;
    std::array<float, 3> pathGoal{};
    std::unique_ptr<VehicleState> vehicle; // script_vehicle path state (src/game/vehicles.cpp)
    std::unique_ptr<TurretState> turret;   // misc_turret / misc_mg42 / spawnturret (src/game/turrets.cpp)
    ObjectPtr usingTurret;                 // actor: turret it uses (actor +0x2114)
    // Perception (src/game/perception.cpp): what this actor knows about each sentient, and how many actors have
    // this sentient as their enemy (sentient +0x24).
    std::unordered_map<const script::Object*, SentientInfo> sentientInfo;
    int attackers = 0;
    int64_t eyeFrameMs = -1;               // per-frame cache of the eye (0x500910 caches by frame time)
    Vec3 eyePos{}, eyeForward{};
};

struct PathNode {
    ObjectPtr obj;
    std::string type; // classname, e.g. node_pathnode
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
    bool isAlive(const Entity& e) const;
    bool canSee(Entity& from, Entity& to);
    struct Stats { int shots = 0, hits = 0, kills = 0; } stats;
    const PathGraph& paths() const { return paths_; }

private:
    Entity& spawnEntity(const std::string& classname);
    Entity* spawnActor(Entity& spawner);
    Entity* entityOf(const Value& v);
    void spawnMapEntities();
    void registerBuiltins();
    void registerMissingStubs();
    void runTriggers();
    void useAnimTree(Entity& e, const std::string& tree);
    void registerAnimBuiltins();
    void startAnimScript(Entity& e, const std::string& name);
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
    Entity* fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount);
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
    bool actorTurretThink(Entity& e);
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
    void alignVehicleToGround(Entity& e, Vec3& origin, Vec3& angles, float baseZ, const Vec3& prevAngles, bool settle);
    std::vector<VehicleNode> vnodes_;
    std::map<std::string, std::unique_ptr<VehicleInfo>> vehicleInfos_;
    std::unordered_map<const script::Object*, int> vnodeIndex_;

    const fs::Vfs& vfs_;
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
    std::mt19937 rng_{12345};
    int perceptionTick_ = 0;
    PathGraph paths_;
    std::map<std::string, std::optional<std::map<std::string, std::string>>> weapons_;
};

} // namespace iw2::game
