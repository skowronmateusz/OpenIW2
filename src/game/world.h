// Game world: map entities, path nodes, the player, and the script VM that drives them.
#pragma once

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "script/vm.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::game {

using script::ObjectPtr;
using script::Value;
using script::Vec3;

// Script-visible entity. Native state lives in the object's fields so scripts and engine share one view.
struct Entity {
    ObjectPtr obj;
    int number = 0;
    std::string classname;
    bool isPlayer = false;
    bool isSpawner = false;
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

    // Moves the player entity (set by the movement code each frame).
    void setPlayerOrigin(const Vec3& origin, const Vec3& angles);

    // Bounds of brush submodel "*N" (lump 35), used by triggers and brush models.
    bool submodelBounds(const std::string& model, Vec3& mins, Vec3& maxs) const;

    // Built-ins the scripts called that are not implemented yet, with call counts.
    const std::map<std::string, int>& missingBuiltins() const { return missing_; }
    std::function<void(const std::string&)> print = [](const std::string&) {};

    std::map<std::string, std::string> cvars;

private:
    Entity& spawnEntity(const std::string& classname);
    Entity* entityOf(const Value& v);
    void spawnMapEntities();
    void registerBuiltins();
    void registerMissingStubs();
    void runTriggers();

    const fs::Vfs& vfs_;
    std::string mapName_;
    std::vector<uint8_t> bspData_;
    std::unique_ptr<assets::D3dBsp> bsp_;
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
};

} // namespace iw2::game
