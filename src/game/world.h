// Game world: map entities, path nodes, the player, and the script VM that drives them.
#pragma once

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/animtree.h"
#include "game/collision.h"
#include "game/pathnodes.h"
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

    std::map<std::string, std::string> cvars;

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
    Vec3 eyePosition(Entity& e);
    void damage(Entity& target, Entity* attacker, int amount, const Vec3& dir, const Vec3& point, const std::string& mod);
    Entity* traceSentients(const Vec3& start, const Vec3& end, const Entity* ignore, float& fraction);
    Entity* fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount);
    int weaponDamage(const std::string& weaponName);
    void updatePerception(Entity& e);
    void registerCombatBuiltins();
    void registerSoundBuiltins();
    void registerHudBuiltins();
    std::vector<ObjectPtr> hudElems_;
    int startSound(const std::string& alias, const Vec3* origin, ObjectPtr notifyObj = nullptr, const std::string& notify = {});
    struct PendingNotify { int64_t atMs; ObjectPtr obj; std::string name; };
    std::vector<PendingNotify> pendingNotifies_;
    int ambientHandle_ = 0, musicHandle_ = 0;
    int nearestNode(const std::array<float, 3>& p) const;

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
    std::map<std::string, int> missing_;
    std::vector<std::pair<Vec3, Vec3>> submodels_;
    std::unique_ptr<CollisionWorld> collision_;
    std::map<std::string, std::optional<assets::XModelParts>> skeletons_;
    std::mt19937 rng_{12345};
    int perceptionTick_ = 0;
    PathGraph paths_;
    std::map<std::string, std::optional<std::map<std::string, std::string>>> weapons_;
};

} // namespace iw2::game
