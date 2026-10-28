#include "game/world.h"
#include "game/actorfields.h"
#include "game/actorslide.h"
#include "game/playerevents.h"

#include "script/builtins_core.h"
#include "script/parser.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace iw2::game {

using namespace script;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Classes from the executable's spawn table (docs/spec/entities.md) that remain script-visible entities.
// misc_model, misc_prefab, script_struct, func_group, info_null and light are consumed at load (inferred).
const std::set<std::string> kEntityClasses = {
    "info_player_start", "info_notnull", "info_notnull_big", "info_grenade_hint", "info_volume", "trigger_use",
    "trigger_use_touch", "trigger_multiple", "trigger_radius", "trigger_disk", "trigger_friendlychain", "trigger_hurt",
    "trigger_once", "trigger_damage", "trigger_lookat", "misc_mg42", "misc_turret", "corona", "sound_blend",
    "script_brushmodel", "script_model", "script_origin", "script_vehicle", "script_vehicle_collmap"};

// Map key/values become typed script fields. Keys named in radiant/*.txt ("<type> <key>", types float, int, string,
// vector; parsed at 0x4686c0, looked up by 0x4682d0) take that type: atoi, atof, sscanf "%f %f %f", or the text
// (entity 0x4f8ff0, node 0x515bf0). Native string fields stay text.
// TODO(spec): the original drops keys that are neither native nor listed; here they stay, typed by their text
// (numeric text becomes an int or float and three numbers a vector), because engine code reads spawn keys
// (turret arcs, cursorhint, …) from the same object.
Value parseFieldValue(const std::string& key, const std::string& v, const std::map<std::string, char>& keyTypes) {
    if (key == "targetname" || key == "target" || key == "classname" || key == "model" || key == "script_noteworthy")
        return Value(v);
    if (auto t = keyTypes.find(key); t != keyTypes.end()) {
        switch (t->second) {
        case 'i': return Value(int32_t(std::atoi(v.c_str())));
        case 'f': return Value(float(std::atof(v.c_str())));
        case 'v': {
            Vec3 vec{};
            std::sscanf(v.c_str(), "%f %f %f", &vec[0], &vec[1], &vec[2]);
            return Value(vec);
        }
        default: return Value(v);
        }
    }
    char* end = nullptr;
    const long i = std::strtol(v.c_str(), &end, 10);
    if (!v.empty() && *end == 0) return Value(int32_t(i));
    const float f = std::strtof(v.c_str(), &end);
    if (!v.empty() && *end == 0) return Value(f);
    Vec3 vec{};
    int consumed = 0;
    if (std::sscanf(v.c_str(), "%f %f %f%n", &vec[0], &vec[1], &vec[2], &consumed) == 3 && size_t(consumed) == v.size())
        return Value(vec);
    return Value(v);
}

// radiant/*.txt key type lists (0x4686c0): "float|int|string|vector name" per line, // comments.
std::map<std::string, char> loadKeyTypes(const fs::Vfs& vfs) {
    std::map<std::string, char> out;
    for (const std::string& path : vfs.list("radiant/")) {
        if (path.size() < 4 || lower(path.substr(path.size() - 4)) != ".txt") continue;
        const auto data = vfs.read(path);
        if (!data) continue;
        std::istringstream in(std::string(data->begin(), data->end()));
        std::string line;
        while (std::getline(in, line)) {
            if (const size_t c = line.find("//"); c != std::string::npos) line.resize(c);
            std::istringstream ls(line);
            std::string type, name;
            if (!(ls >> type >> name)) continue;
            const char t = type == "float" ? 'f' : type == "int" ? 'i' : type == "string" ? 's' : type == "vector" ? 'v' : 0;
            if (t) out[lower(name)] = t;
        }
    }
    return out;
}

ArrayPtr toArray(const std::vector<ObjectPtr>& objs) {
    auto arr = std::make_shared<Array>();
    int32_t i = 0;
    for (const auto& o : objs) {
        ArrayKey k;
        k.i = i++;
        arr->set(k, Value(o));
    }
    return arr;
}

} // namespace

World::World(const fs::Vfs& vfs, std::string mapName)
    : vfs_(vfs), mapName_(std::move(mapName)), animLib_(vfs), vm_([this](const std::string& p) -> std::optional<std::string> {
          auto d = vfs_.read(p);
          if (!d) return std::nullopt;
          return std::string(d->begin(), d->end());
      }) {
    // Mission-progress dvars the exe registers at start (archived; docs/spec/savegame.md): mis_01..mis_10 int 0, mis_difficulty string of 50 '0'.
    for (int i = 1; i <= 10; ++i) cvars.emplace(std::string("mis_") + (i < 10 ? "0" : "") + std::to_string(i), "0");
    cvars.emplace("mis_difficulty", std::string(50, '0'));
    auto d = vfs_.read("maps/" + mapName_ + ".d3dbsp");
    if (!d) throw std::runtime_error("map not found: " + mapName_);
    bspData_ = std::move(*d);
    bsp_ = std::make_unique<assets::D3dBsp>(bspData_);
    keyTypes_ = loadKeyTypes(vfs_);
    collision_ = std::make_unique<CollisionWorld>(*bsp_);
    const auto models = bsp_->lump(35);
    for (size_t i = 0; i + 48 <= models.size(); i += 48) {
        Vec3 mn, mx;
        std::memcpy(mn.data(), models.data() + i, 12);
        std::memcpy(mx.data(), models.data() + i + 12, 12);
        submodels_.push_back({mn, mx});
    }
    vm_.print = [this](const std::string& s) { print(s); };
    registerCoreBuiltins(vm_, std::getenv("IW2_SCRIPT_SEED") ? uint32_t(std::strtoul(std::getenv("IW2_SCRIPT_SEED"), nullptr, 10)) : 1u); // IW2_SCRIPT_SEED: debug only, varies script randomness
    registerBuiltins();
    registerAnimBuiltins();
    registerCombatBuiltins();
    registerSoundBuiltins();
    registerShellShockBuiltins();
    registerMenuBuiltins();
    registerHudBuiltins();
    registerInventoryBuiltins();
    registerMoverBuiltins();
    registerScriptedAnimBuiltins();
    registerPlayerLinkBuiltins();
    registerCampaignBuiltins();
    registerVehicleBuiltins();
    registerTurretBuiltins();
    registerActorStateBuiltins();
    registerMissingStubs();
}

Entity& World::spawnEntity(const std::string& classname) {
    auto e = std::make_unique<Entity>();
    e->obj = vm_.newObject(classname);
    e->classname = classname;
    e->number = nextEntityNumber_++;
    e->obj->set("classname", Value(classname));
    e->obj->set("origin", Value(Vec3{0, 0, 0}));
    e->obj->set("angles", Value(Vec3{0, 0, 0}));
    // Native integer entity fields (table at 0x56f4a8: spawnflags, count, health, dmg, maxhealth) of a zeroed entity.
    for (const char* f : {"spawnflags", "count", "health", "dmg", "maxhealth"}) e->obj->set(f, Value(int32_t(0)));
    if (classname == "script_vehicle") e->obj->set("$takedamage", Value(true)); // 0x50d8f4 (vehicle spawn, ent+0x111 = 1)
    byObject_[e->obj.get()] = e.get();
    entities_.push_back(std::move(e));
    return *entities_.back();
}

Entity* World::entityOf(const Value& v) {
    if (!v.isObject()) return nullptr;
    auto it = byObject_.find(v.asObject().get());
    return it == byObject_.end() ? nullptr : it->second;
}

void World::spawnMapEntities() {
    std::vector<Entity*> mapStartActors;
    const ObjectPtr* playerStart = nullptr;
    std::vector<ObjectPtr> starts;
    // script_struct entities become script structs: the engine runs codescripts/struct::initstructs() once and
    // codescripts/struct::createstruct() per struct, then sets the key/values on it (FUN_004fa5c0,
    // script names from FUN_004eb460).
    vm_.start("codescripts/struct.gsc", "initstructs");
    for (const auto& be : bsp_->entities()) {
        const std::string* clsp = be.get("classname");
        if (!clsp) continue;
        const std::string cls = *clsp;
        auto fill = [&](const ObjectPtr& o) {
            for (const auto& [k, v] : be.fields) {
                const std::string key = lower(k);
                o->set(key, parseFieldValue(key, v, keyTypes_));
            }
        };
        if (cls == "worldspawn") {
            // SP_worldspawn (0x4fa4cb) stores the "northyaw" key (default "", empty becomes "0") in configstring 10
            // (0x448b80; table 0x815420), which getnorthyaw reads.
            const std::string* ny = be.get("northyaw");
            northYaw_ = ny && !ny->empty() ? *ny : "0";
        } else if (cls == "script_struct") {
            vm_.start("codescripts/struct.gsc", "createstruct");
            const Value list = vm_.level()->get("struct");
            if (list.isArray() && std::get<script::ArrayPtr>(list.v)->size() > 0) {
                const Value st = std::get<script::ArrayPtr>(list.v)->items.back().second;
                if (st.isObject()) fill(st.asObject());
            }
        } else if (startsWith(cls, "node_")) {
            PathNode n;
            n.obj = vm_.newObject(cls);
            n.type = cls;
            // Native node fields (table 0x5a93b8) live in zero-filled hunk memory (0x5166fc → 0x426f30), so the
            // numeric ones read 0 when the map gives no key.
            n.obj->set("radius", Value(0.0f));
            n.obj->set("spawnflags", Value(int32_t(0)));
            n.obj->set("origin", Value(Vec3{0, 0, 0}));
            n.obj->set("angles", Value(Vec3{0, 0, 0}));
            fill(n.obj);
            n.obj->set("type", Value(pathNodeTypeName(cls))); // engine field with a getter (0x515b60)
            n.typeNum = pathNodeTypeNum(cls);
            const Value sf = n.obj->get("spawnflags");
            n.spawnflags = sf.isInt() ? sf.asInt() : 0;
            // Node key parse (0x52d280 via 0x52d0b0, key table 0x5a9670): the yaw (+0x20) starts at the sentinel -1; the
            // "angles" key (type 0xb, 0x52d1f4) is read as "%f %f %f" and only AngleNormalize360(angles[1]) (0x4240a0) is
            // stored. A yaw still at -1 becomes 0, any other yaw sets spawnflag 0x8000 (0x52d2ba..0x52d2cd): "node has an
            // angle", which the node facing (0x505dcc, 0x505935) and the goal check (0x4fef89) test. The script field
            // `angles` (type 0xb, getter 0x4f9e21) is (0, yaw, 0).
            bool hasAngles = false;
            for (const auto& [k, v] : be.fields) hasAngles = hasAngles || lower(k) == "angles";
            if (hasAngles) {
                float yaw = n.obj->get("angles").isVector() ? n.obj->get("angles").asVector()[1] : 0.0f;
                while (yaw < 0.0f) yaw += 360.0f;
                while (yaw >= 360.0f) yaw -= 360.0f;
                n.obj->set("angles", Value(Vec3{0.0f, yaw, 0.0f}));
                n.spawnflags |= 0x8000;
                n.obj->set("spawnflags", Value(int32_t(n.spawnflags)));
            }
            nodes_.push_back(std::move(n));
        } else if (cls == "info_vehicle_node" || cls == "info_vehicle_node_rotate") {
            auto o = vm_.newObject(cls);
            fill(o);
            vehicleNodes_.push_back(o);
        } else if (startsWith(cls, "actor_")) {
            Entity& e = spawnEntity(cls);
            e.isSpawner = true;
            fill(e.obj);
            bool hasCount = false;
            for (const auto& [k, v] : be.fields) hasCount = hasCount || lower(k) == "count";
            if (!hasCount) e.obj->set("count", Value(int32_t(1)));
            const Value flags = e.obj->get("spawnflags");
            if (!(flags.isInt() && (flags.asInt() & 1))) mapStartActors.push_back(&e);
        } else if (kEntityClasses.count(cls)) {
            Entity& e = spawnEntity(cls);
            fill(e.obj);
            if (cls == "info_player_start") starts.push_back(e.obj);
            if (cls == "trigger_damage") spawnTriggerDamage(e);
            if (cls == "trigger_hurt") { // spawn 0x5292a0: dmg 0 becomes 5; spawnflags 1 (start off) selects handler 6, else 7
                const Value dv = e.obj->get("dmg");
                if (!dv.isNumber() || dv.asInt() == 0) e.obj->set("dmg", Value(int32_t(5)));
                const Value sf = e.obj->get("spawnflags");
                if (sf.isNumber() && (sf.asInt() & 1)) e.obj->set("$hurtOff", Value(true));
            }
        } else if (startsWith(cls, "weapon_") && weapon(cls.substr(7))) {
            // weapon_<name> with a known weapon is an item entity (0x4f96d0 → item spawn 0x522c70).
            // TODO(spec): item spawn details (model from the weapon, drop to floor, pickup).
            Entity& e = spawnEntity(cls);
            fill(e.obj);
        }
    }
    if (!starts.empty()) playerStart = &starts.front();

    std::vector<std::array<float, 3>> nodePositions;
    for (const auto& n : nodes_) nodePositions.push_back(n.obj->get("origin").isVector() ? n.obj->get("origin").asVector() : Vec3{0, 0, 0});
    try {
        paths_ = PathGraph(bsp_->lump(38), std::move(nodePositions));
    } catch (const std::exception& e) {
        print(std::string("path data: ") + e.what());
    }
    buildFriendlyChains(); // 0x52d630; its errors are fatal

    Entity& p = spawnEntity("player");
    p.isPlayer = true;
    // Client spawn 0x4e991e-0x4e9945: health = maxhealth = g_player_maxhealth (set by skill, see start()).
    const int maxhealth = std::atoi(cvars["g_player_maxhealth"].c_str());
    p.obj->set("health", Value(int32_t(maxhealth)));
    p.obj->set("maxhealth", Value(int32_t(maxhealth)));
    p.obj->set("team", Value("allies"));
    if (playerStart) {
        p.obj->set("origin", (*playerStart)->get("origin"));
        p.obj->set("angles", (*playerStart)->get("angles"));
    }
    player_ = &p;

    // AI placed without the SPAWNER flag spawn at load (inferred from the SPAWNER flag description in the
    // aitype scripts: "makes this a spawner instead of a guy").
    for (Entity* sp : mapStartActors) spawnActor(*sp);
}

// Spawns an AI from an actor spawner: copies the spawner's key/values, runs aitype/<class>.gsc::main() on the new
// actor, notifies the spawner "spawned", and starts animscripts/init.gsc::main() (docs/spec/ai.md).
Entity* World::spawnActor(Entity& spawner, bool force) {
    const Value count = spawner.obj->get("count");
    if (count.isInt() && count.asInt() <= 0) return nullptr;

    // dospawn (arg3 == 0) refuses a spawnpoint whose box (origin + (-15,-15,0)..(15,15,70), 0x5a5bdc..0x5a5bf0) holds a
    // living body: an entity with contents & 0x200c000 that is a client in state < 4 or an actor with health > 0
    // (0x4e8500). The message is "couldn't spawn from %s because spawnpoint would telefrag" and the result undefined
    // (0x532253-0x53227e). stalingradspawn passes force and skips the test. The "player can see the spawnpoint"
    // test (0x532288) is skipped when the global at 0x1c61048 is set, which it is in-game (SP), so it is not done.
    // TODO(spec): the player's contents bits and the client-state meaning of the <4 compare.
    if (!force) {
        const Value so = spawner.obj->get("origin");
        if (so.isVector()) {
            const Vec3 sp = so.asVector();
            bool blocked = false;
            for (const auto& e : entities_) {
                if (e.get() == &spawner || e->obj->removed || !(e->isActor || e->isPlayer)) continue;
                if (e->isActor && (e->ai.corpse || !e->ai.alive)) continue;
                const Value eo = e->obj->get("origin");
                if (!eo.isVector()) continue;
                const Vec3 p = eo.asVector();
                // actor box (-15,-15,0)..(15,15,72) against the spawn box (-15,-15,0)..(15,15,70), touching counts
                const float height = e->isPlayer ? 70.0f : 72.0f; // TODO(spec): the player's box height
                if (p[0] - 15 <= sp[0] + 15 && p[0] + 15 >= sp[0] - 15 && p[1] - 15 <= sp[1] + 15 && p[1] + 15 >= sp[1] - 15 &&
                    p[2] <= sp[2] + 70 && p[2] + height >= sp[2]) {
                    if (e->isPlayer && !isAlive(*e)) continue;
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                const Value tn = spawner.obj->get("targetname");
                print("^3couldn't spawn from " + (tn.isString() ? tn.toString() : std::string("<unnamed>")) +
                      " because spawnpoint would telefrag");
                return nullptr;
            }
        }
    }
    // The actor pool has 32 slots (FUN_004fc8d0 scans 0x20 slots of 0x21a4 bytes); a corpse conversion (0x4fdba0 ->
    // 0x4fc960) or a freed entity returns its slot. With no free slot the spawn fails with "Actor allocation failed",
    // the fresh entity is freed (0x4d7c30) and dospawn reports "couldn't spawn from %s because there are no free
    // actors" (0x532398), returning undefined; the spawner's count is untouched (it is decremented only after a
    // successful spawn, 0x5325ca). TODO(spec): the spawner flag 2 retry that frees the farthest actor (0x4fcaf0).
    {
        int live = 0;
        for (const auto& e : entities_)
            if (e->isActor && !e->obj->removed && !e->ai.corpse) ++live;
        if (live >= 32) {
            const Value tn = spawner.obj->get("targetname");
            print("^3couldn't spawn from " + (tn.isString() ? tn.toString() : std::string("<unnamed>")) +
                  " because there are no free actors");
            return nullptr;
        }
    }
    if (count.isInt()) spawner.obj->set("count", Value(int32_t(count.asInt() - 1)));

    const std::string cls = spawner.classname;
    Entity& a = spawnEntity(cls);
    a.isActor = true;
    a.obj->kind = "actor";
    // Spawned AI keep the spawner's key/values except its targetname; map-start AI (no spawner flag 1) are the
    // placed entity itself, so they keep it. A spawner whose count runs out is removed. (Inferred from scripts
    // that getent() a spawner by targetname and later getent() the AI by script_noteworthy, e.g. eldaba
    // intro_actors_setup; the actor spawn 0x5325ca itself only decrements a positive count, TODO(spec) where the
    // spent spawner is freed.)
    const Value flags = spawner.obj->get("spawnflags");
    const bool placed = !(flags.isInt() && (flags.asInt() & 1));
    for (const auto& [k, v] : spawner.obj->fields)
        if (k != "spawnflags" && k != "count" && (placed || k != "targetname")) a.obj->set(k, v);
    a.obj->set("classname", Value(cls));
    a.obj->set("team", Value(startsWith(cls, "actor_axis") ? "axis" : startsWith(cls, "actor_ally") ? "allies" : "neutral"));
    useAnimTree(a, "generic_human"); // AI use the generic_human tree (inferred from animscripts' #using_animtree)
    applyActorFieldDefaults(*a.obj); // docs/spec/ai.md
    // movemode is a byte enum shown as "stop", "stop soon", "walk", "run" (getter 0x536960); the zeroed actor
    // starts at "stop". TODO(spec): anim_pose's initial value (a string index, 0 after allocation) is inferred.
    a.obj->set("movemode", Value("stop"));
    a.obj->set("anim_pose", Value("stand"));
    const std::string aitype = "aitype/" + cls.substr(6) + ".gsc";
    try {
        vm_.start(aitype, "main", {}, Value(a.obj));
    } catch (const std::exception& e) {
        print("aitype " + aitype + ": " + e.what());
    }
    // 0x4fdee0 / spawn 0x532558: animscripts/init runs as a plain thread (not the actor's animscript), then the
    // bottom state (exposed) starts, and only then is the spawner notified "spawned" (0x5325ad).
    try {
        vm_.start("animscripts/init.gsc", "main", {}, Value(a.obj));
    } catch (const std::exception& ex) {
        print(std::string("animscripts/init: ") + ex.what());
    }
    a.ai.baseStateMs = timeMs_;
    aiStart(a, a.ai.stack[0]);
    vm_.notify(spawner.obj, "spawned", {Value(a.obj)});
    // A map-start actor (placed entity) is replaced by its actor; real spawners (flag 1) stay findable (0x5325ca-0x5325d8 only
    // decrement the count; demolition tank() calls dospawn on getentarray("wounded_drag")[0] and then [1]).
    if (placed && count.isInt() && count.asInt() - 1 <= 0) spawner.obj->removed = true;
    return &a;
}

void World::start() {
    // maps/_load.gsc sets level.script from getcvar("mapname"), so the cvar must name the running map before the
    // level script starts (docs/spec/ui.md, intro screen).
    cvars["mapname"] = mapName_;
    // Skill-health dvar cluster (registered in FUN_00449b10, defaults per the 1.3 exe): g_gameskill range 0..3
    // (0x446b33, default 1), g_player_maxhealth 500, player_healthEasy/Medium/Hard/Fu 500/275/165/115,
    // player_deathInvulnerableTime 1000 ms. DEVIATION: g_gameskill defaults to 0 (easy), not the original's 1,
    // because openiw2 has no menu UI to pick a difficulty (Mateusz 2026-10-27).
    if (cvars.find("g_gameskill") == cvars.end()) cvars["g_gameskill"] = "0";
    cvars["player_healthEasy"] = "500";
    cvars["player_healthMedium"] = "275";
    cvars["player_healthHard"] = "165";
    cvars["player_healthFu"] = "115";
    cvars["g_player_maxhealth"] = "500";
    cvars["player_deathInvulnerableTime"] = "1000";
    // FUN_004467b0 ("apply game skill", called once at game start 0x4d8788): pick the per-skill player health
    // into g_player_maxhealth and set player_deathInvulnerableTime to 4000/1000/100/100 ms for skill 0..3.
    // This is the ONLY engine-side skill scaling — damage and NPC health are skill-blind (G_Damage 0x528050).
    static const char* const kSkillHealthCvar[4] = {"player_healthEasy", "player_healthMedium", "player_healthHard", "player_healthFu"};
    static const int kSkillDeathInvulMs[4] = {4000, 1000, 100, 100};
    int skill = std::atoi(cvars["g_gameskill"].c_str());
    if (skill < 0) skill = 0;
    if (skill > 3) skill = 3;
    cvars["g_player_maxhealth"] = cvars[kSkillHealthCvar[skill]];
    cvars["player_deathInvulnerableTime"] = std::to_string(kSkillDeathInvulMs[skill]);
    spawnMapEntities();
    linkVehicleNodes();
    initMapTurrets();
    syncEntityClips();
    // 0x516b70 (after the entity spawn 0x4fa5c0, 0x4fdf30; before the first frame 0x4da970): every entity with the DYNAMICPATH
    // flags 0x300 -- set by script_brushmodel spawn 0x533440 when spawnflags bit 1 is set (0x53346c) -- disconnects its paths
    // (0x519860). TODO(spec): that the level main() runs after this point (assumed: the map scripts rely on it, e.g. moscow's
    // ai_table_clip_down is made solid again later without a disconnectpaths call).
    for (const auto& ep : entities_) {
        if (ep->classname != "script_brushmodel" || ep->obj->removed) continue;
        const Value sf = ep->obj->get("spawnflags");
        if (sf.isInt() && (sf.asInt() & 1)) disconnectEntityPaths(*ep);
    }
    vm_.level()->set("script", Value(mapName_));
    vm_.start("maps/" + mapName_ + ".gsc", "main", {}, Value(vm_.level())); // self = level: maps/duhoc_assault.gsc main() threads read self fields (inferred)
    // Threads the init code woke by notify were appended to the current frame list (counter 0x9bef20), which the first
    // Scr_RunFrame runs before the next frame's list that holds the timed waits (VM_Notify 0x46ca60, Scr_RunFrame 0x46f5c0).
    // TODO(spec): whether the exe runs a frame at time 0 before the first 50 ms step; only the order is evidenced.
    vm_.drainReady();
    // The exe's script lists are keyed by a frame counter (0x9bef20): a thread that init code left in `wait 0.05` is in the
    // NEXT frame's list, so something already ran the current one. moscow/matmata need that: matmata's start trigger_multiple
    // (the player stands inside it) must fire and spawn the jeep before ridein's `wait 0.05` ends and does getent("jeep").
    // So one game frame runs at time 0 before the first 50 ms step. TODO(spec): the exe's server start-up frames (count and
    // time step) are not read.
    frame(false);
}

Entity* World::entityByNumber(int number) {
    for (const auto& e : entities_)
        if (e->number == number) return e.get();
    return nullptr;
}

void World::frame(bool advanceTime) {
    if (advanceTime) timeMs_ += 50;
    if (player_) { // the player's velocity for the AI (TODO(spec): the original reads ps->velocity)
        const Value po = player_->obj->get("origin");
        if (po.isVector()) {
            const Vec3 o = po.asVector();
            if (havePlayerPrev_) player_->velocity = {(o[0] - playerPrev_[0]) * 20.0f, (o[1] - playerPrev_[1]) * 20.0f, (o[2] - playerPrev_[2]) * 20.0f};
            playerPrev_ = o;
            havePlayerPrev_ = true;
        }
    }
    syncEntityClips();
    runMovers();
    if (fxEvents_.size() > 4096) fxEvents_.erase(fxEvents_.begin(), fxEvents_.end() - 4096); // nobody draining
    for (auto& l : loopedFx_)
        if (timeMs_ >= l.nextMs) {
            fxEvents_.push_back({fxNames_[size_t(l.id)], l.origin, Vec3{0, 0, 1}});
            l.nextMs = timeMs_ + int64_t(l.delayS * 1000.0f);
        }
    for (const auto& e : entities_) {
        if (!e->anim || e->obj->removed) continue;
        ObjectPtr obj = e->obj;
        e->anim->update(0.05f, [this, obj](const std::string& notifyName, const std::string& note) {
            vm_.notify(obj, notifyName, {Value(note)});
        });
    }
    runTriggers();
    touchItems();
    for (size_t i = 0; i < pendingNotifies_.size();) {
        if (pendingNotifies_[i].atMs <= timeMs_) {
            const auto p = pendingNotifies_[i];
            pendingNotifies_.erase(pendingNotifies_.begin() + long(i));
            vm_.notify(p.obj, p.name);
        } else {
            ++i;
        }
    }
    vm_.runFrame(timeMs_);
    runPlayerUse(); // TODO(spec): the client think (0x4da970) order relative to the script frame is not read
    thinkVehicles();
    thinkTurrets();
    runGrenades();
    runRockets(); // TODO(spec): G_RunFrame runs every missile in entity order
    runItems();
    runBadPlaces(); // TODO(spec): where G_RunFrame calls 0x523a60 relative to the actors is not read
    runScriptedModels();
    thinkActors();
    updatePlayerLink(); // ClientEndFrame (0x4e7f90) after the entities. TODO(spec): the exact frame order
    runDeathScreen();
}

const std::map<std::string, std::string>* World::weapon(const std::string& name) {
    const std::string key = lower(name);
    auto it = weapons_.find(key);
    if (it == weapons_.end()) {
        std::optional<std::map<std::string, std::string>> def;
        if (auto d = vfs_.read("weapons/sp/" + key)) {
            std::string text(d->begin(), d->end());
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); ++i)
                if (i == text.size() || text[i] == '\\') { parts.push_back(text.substr(start, i - start)); start = i + 1; }
            if (!parts.empty() && parts[0] == "WEAPONFILE") {
                def.emplace();
                for (size_t i = 1; i + 1 < parts.size(); i += 2) (*def)[lower(parts[i])] = parts[i + 1];
            }
        }
        it = weapons_.emplace(key, std::move(def)).first;
    }
    return it->second ? &*it->second : nullptr;
}

int World::nearestNode(const std::array<float, 3>& p) const {
    int best = -1;
    float bestD = 1e30f;
    for (size_t i = 0; i < paths_.size(); ++i) {
        const auto& q = paths_.position(i);
        const float d = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + 4.0f * (p[2] - q[2]) * (p[2] - q[2]);
        if (d < bestD) { bestD = d; best = int(i); }
    }
    return best;
}

// Every actor thinks once per frame (src/game/actorstate.cpp); corpses no longer think.
void World::thinkActors() {
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& e = *entities_[i];
        if (!e.isActor || e.obj->removed || e.ai.corpse) continue;
        {
            // lookforward: unit vector of the actor's facing (native field, docs/spec/ai.md).
            const Value ang = e.obj->get("angles");
            const float yaw = ang.isVector() ? ang.asVector()[1] * float(M_PI) / 180.0f : 0.0f;
            e.obj->set("lookforward", Value(Vec3{std::cos(yaw), std::sin(yaw), 0}));
        }
        e.ai.thoughtMs = timeMs_;
        e.velocity = {0, 0, 0}; // set again by this frame's move (moveActor)
        actorThink(e);
        if (!e.ai.corpse) actorFacing(e);
        if (!e.ai.corpse) actorMoveNoise(e);
        if (!e.ai.corpse) blockFacingNodes(e);
        if (!e.ai.corpse) actorProneFrame(e); // 0x4ff1de: prone refit after a move or turn
    }
    // Debug: every 5 s a count of actors by state/substate, animscript (done: its thread ended) and path.
    if (std::getenv("IW2_AI_DEBUG") && timeMs_ % 5000 == 0) {
        std::map<std::string, int> counts;
        for (const auto& ep : entities_) {
            const Entity& e = *ep;
            if (!e.isActor || e.obj->removed) continue;
            counts["st" + std::to_string(e.ai.stack[size_t(e.ai.depth)]) + "/" + std::to_string(e.ai.substate) + " " + e.animScript +
                   (e.animScriptThread && vm_.isThreadAlive(e.animScriptThread) ? "" : "(done)") + (e.ai.corpse ? " corpse" : "") +
                   (actorHasPath(e) ? " path" : "")]++;
        }
        std::string line = "AI " + std::to_string(timeMs_ / 1000) + "s:";
        for (const auto& [k, n] : counts) line += " [" + k + "]x" + std::to_string(n);
        print(line);
    }
}

// The actor's move along its path (0x504160 anim mode 1 → 0x505230, then 0x503ec0). The lookahead is updated
// (0x544ca0), then the actor moves along it by the frame distance scaled by how far the far direction turns away
// (dot^0.333, at least 0.6), no further than the lookahead point; the height follows the lookahead slope.
// Stop soon (0x503ec0): once the lookahead reached the end and the end is within a quarter second of travel
// (|velocity|² · 0.0625) the actor has 500 ms left (+0x704); when they are over the path is cleared.
// The frame distance is the horizontal length of this frame's anim delta (0x5041ad: 0x503e40 → 0x478550, current
// weights since the move script choices 0x53af20 / 0x53afe0 clear actor byte +0x220), so an actor whose move script has
// not set a moving animation yet stands still. Not modelled: 0x500d80 (the bumped entity +0x318, the lookahead shrink flag), the facing blend
// (+0x2150), the side offset (+0x73c) and the actor physics (0x501xxx) that clips the move.
void World::moveActor(Entity& e, float dt) {
    ActorPath& p = e.path;
    // A followed actor (+0x2148) skips the path walk (0x5041a3 -> 0x504228): 0x544ca0 only updates the lookahead, with the shrink
    // flag 0x500d80(bumped entity), the desired delta stays zero (0x504262..0x504268), the mode word is 1 and neither the facing
    // nor the stop-soon check 0x503ec0 run. The physics 0x501d00 then calls the yield step 0x501620 (anim mode 1 or 7), which
    // moves the actor out of the followed entity's way (yieldFollowed); it runs with or without a path.
    // TODO(spec): the trail 0x505520 / 0x504f60 (+0x2150 ring) only feeds the facing blend (0x504e70 / 0x505630), which is not modelled.
    const bool followed = e.ai.followed != nullptr;
    const bool yieldNow = followed && e.ai.animMode == 1 && actorSlideEnabled() && e.ai.yieldMs != timeMs_;
    if (p.count <= 0 && !yieldNow) return;
    auto o = e.obj->get("origin").asVector();
    float dist = 0.0f;
    if (e.anim && !followed) {
        const AnimDelta d = e.anim->delta(e.ai.animDeltaGoalWeights);
        dist = std::sqrt(d.trans[0] * d.trans[0] + d.trans[1] * d.trans[1]);
    }
    const Vec3 before = o;
    if (p.count > 0) updatePathLook(e, o, followed && followShrink(e, e.ai.bumpedEnt), false);
    if (followed && p.count > 0) e.ai.trail.add(timeMs_, p.lookDir[0], p.lookDir[1]); // 0x505520 (0x504248)
    if (p.count <= 0 && !yieldNow) return;
    float step = 0.0f;
    if (p.count > 0) {
        float turn = p.farDir[1] * p.lookDir[1] + p.farDir[0] * p.lookDir[0];
        if (turn < 1e-6f) turn = 1e-6f;
        float f = std::pow(turn, 0.333f);
        if (f < 0.6f) f = 0.6f;
        step = followed ? 0.0f : f * dist;
        if (step > p.lookDist) step = p.lookDist;
        o[0] += p.lookDir[0] * step;
        o[1] += p.lookDir[1] * step;
        o[2] += p.lookDir[2] * step;
    }
    if (step > 0.0f) {
        const float yaw = std::atan2(p.lookDir[1], p.lookDir[0]) * 180.0f / float(M_PI);
        e.obj->set("angles", Value(Vec3{0, yaw, 0}));
        e.obj->set("$motionyaw", Value(yaw));
        // desiredangle (actor +0x124): the engine refreshes it at the end of every movement frame from the
        // active turn-target pair (FUN_00504ca0, called by FUN_00504160 after its turn; degrees, the units of
        // angles[1] — animscripts/stop.gsc:555 reads AngleClamp(desiredangle - angles[1])). We have no turn
        // pairs yet (TODO(spec): facing modes, FUN_00504aa0), so while path-moving the target is the look
        // direction, like the angles write above.
        e.obj->set("desiredangle", Value(yaw));
    }
    bool grounded = false;
    if (actorSlideEnabled()) {
        // The actor slide move (actorslide.cpp, exe 0x5487c0 and callees) instead of the ground snap below. Block set-up from
        // the exe: init 0x500cd0 gives mask 0x2820011 (+0x68), pass entity = the gentity number (+0x3c), ground entity 0x7fe,
        // step 50 ms, gravity zero until 0x501de0 copies the g_gravity dvar (0x1b15ee8, default 800) into +0x44 every frame;
        // the path step 0x504160 sets the mode word +0x40 to 1 (0x5041c7 / 0x504258) before 0x505230 fills the delta.
        // The trace box is the actor r.mins/r.maxs (+0xa8/+0xb4), written once at spawn (0x4fc709..0x4fc731: -15,-15,0 .. 15,15,72) and by no other actor code.
        ActorSlide pm;
        for (int a = 0; a < 3; ++a) {
            pm.origin[a] = before[a];
            pm.moveDelta[a] = o[a] - before[a];
        }
        pm.dt = dt;
        pm.timeMs = int(dt * 1000.0f + 0.5f);
        pm.mode = 1;
        pm.gravity = gravity();
        pm.mask = e.ai.noPush ? 0x2820011u : 0x820011u; // actor +0x28c, see actorMoveNoise
        pm.groundEnt = uint16_t(e.ai.groundEnt);
        for (int a = 0; a < 3; ++a) pm.velocity[a] = e.velocity[a];
        const SlideTraceFn fn = [&](const float s[3], const float en[3]) {
            const TraceResult tr = collision_->trace({s[0], s[1], s[2]}, {en[0], en[1], en[2]}, {-15, -15, 0}, {15, 15, 72}, pm.mask, e.number);
            SlideTrace t;
            t.fraction = tr.fraction;
            for (int a = 0; a < 3; ++a) t.normal[a] = tr.normal[a];
            t.surface = tr.contents;
            t.surfFlags = tr.surfaceFlags;
            t.entity = tr.fraction < 1.0f || tr.startSolid ? uint16_t(tr.entity < 0 ? 0x7fe : tr.entity) : uint16_t(0x7ff);
            t.startSolid = tr.startSolid;
            t.allSolid = tr.allSolid;
            return t;
        };
        if (yieldNow) {
            e.ai.yieldMs = timeMs_;
            if (!yieldFollowed(e, pm, fn, before)) {
                for (int a = 0; a < 3; ++a) pm.origin[a] = before[a]; // 0x501620 returned 0: no move this frame
            }
        } else {
            moveBlock(pm, fn);
        }
        for (int a = 0; a < 3; ++a) o[a] = pm.origin[a];
        e.ai.groundEnt = pm.groundEnt;
        grounded = pm.groundEnt != 0x7ff; // ENTITYNUM_NONE: no ground
        linkBumpedActor(e, pm.hitEnt);
        // groundtype (actor +0x24c = move block +0x28, the surface type of the ground probe); getter 0x5367b0: 0 is undefined,
        // 1..22 the surface table 0x5a0344 (the pmove names), anything else "default".
        e.obj->set("groundtype", pm.surfaceType == 0 ? Value() : Value(std::string(surfaceAliasName(pm.surfaceType >= 23 ? 0 : pm.surfaceType))));
    } else {
        // Stand on the ground: the actor clip mask (0x2820011, without the player-only clip 0x10000 that stands in front of the moscow weapons table and made the commissar rise onto the node height).
        const TraceResult tr = collision_->trace({o[0], o[1], o[2] + 18.0f}, {o[0], o[1], o[2] - 64.0f}, {-15, -15, 0}, {15, 15, 70}, Contents::ActorClipMask);
        grounded = !tr.startSolid && tr.fraction < 1.0f;
        if (grounded) o[2] = tr.endPos[2];
    }
    e.obj->set("origin", Value(o));
    e.velocity = {(o[0] - before[0]) / dt, (o[1] - before[1]) / dt, (o[2] - before[2]) / dt};
    // 0x503ec0: the negotiation branch (stop soon unset, on the ground, flag 0x40, a traversal point): within the squared
    // distance |velocity|^2 * 0.005 + 1e-6 (2D) of the begin point the negotiation state is pushed.
    if (followed) return; // 0x503ec0 is part of the path walk
    if (e.ai.stopSoonMs == 0) {
        if (!grounded || !(p.flags & 0x40)) return; // 0x503ee..0x503ef5: not on the ground or no flag 0x40
        if (p.traversal > 0) {
            negotiationTrigger(e, o, true);
            return;
        }
    }
    const PathPoint& end = p.pts[size_t(p.traversal)];
    const float ex = end.pos[0] - o[0], ey = end.pos[1] - o[1];
    if (e.ai.stopSoonMs == 0) {
        const float d2 = ex * ex + ey * ey;
        if (d2 != 0.0f && !(p.flags & 1)) return;
        if ((e.velocity[1] * e.velocity[1] + e.velocity[0] * e.velocity[0]) * 0.0625f < d2) return;
        e.ai.stopSoonMs = timeMs_ + 500;
    }
    if (e.ai.stopSoonMs - timeMs_ <= 0) {
        pathClearBudget(p, o);
        p.count = p.planned = 0;
        e.ai.detourMs = 0;
    }
}

// The actor physics for a movement kind other than 1 (block mode +0x40 = actor +0x264: anim mode 4 -> 2, 5 -> 3, 6 -> 4): the
// delta (actor +0x250, already turned by the yaw +0x124 in 0x501d00) goes through the same actor slide block as the path
// step (moveBlock: kind 3 adds the delta without collision, kind 4 slides in 3D, kind 2 is the ground-bound step).
void World::moveActorKind(Entity& e, int kind, const Vec3& delta, float dt) {
    const Vec3 before = e.obj->get("origin").asVector();
    ActorSlide pm;
    for (int a = 0; a < 3; ++a) {
        pm.origin[a] = before[a];
        pm.moveDelta[a] = delta[size_t(a)];
        pm.velocity[a] = e.velocity[a];
    }
    pm.dt = dt;
    pm.timeMs = int(dt * 1000.0f + 0.5f);
    pm.mode = kind;
    pm.gravity = gravity();
    pm.mask = e.ai.noPush ? 0x2820011u : 0x820011u;
    pm.groundEnt = uint16_t(e.ai.groundEnt);
    const SlideTraceFn fn = [&](const float s[3], const float en[3]) {
        const TraceResult tr = collision_->trace({s[0], s[1], s[2]}, {en[0], en[1], en[2]}, {-15, -15, 0}, {15, 15, 72}, pm.mask, e.number);
        SlideTrace t;
        t.fraction = tr.fraction;
        for (int a = 0; a < 3; ++a) t.normal[a] = tr.normal[a];
        t.surface = tr.contents;
        t.surfFlags = tr.surfaceFlags;
        t.entity = tr.fraction < 1.0f || tr.startSolid ? uint16_t(tr.entity < 0 ? 0x7fe : tr.entity) : uint16_t(0x7ff);
        t.startSolid = tr.startSolid;
        t.allSolid = tr.allSolid;
        return t;
    };
    moveBlock(pm, fn);
    const Vec3 o{pm.origin[0], pm.origin[1], pm.origin[2]};
    e.obj->set("origin", Value(o));
    e.ai.groundEnt = pm.groundEnt;
    e.velocity = {(o[0] - before[0]) / dt, (o[1] - before[1]) / dt, (o[2] - before[2]) / dt};
    linkBumpedActor(e, pm.hitEnt);
}

bool World::negotiationTrigger(Entity& e, const Vec3& o, bool grounded) {
    ActorPath& p = e.path;
    if (!grounded) return false;
    const PathPoint& b = p.pts[size_t(p.traversal)];
    const float dx = b.pos[0] - o[0], dy = b.pos[1] - o[1];
    const float v2 = e.velocity[0] * e.velocity[0] + e.velocity[1] * e.velocity[1] + e.velocity[2] * e.velocity[2];
    if (dx * dx + dy * dy > v2 * 0.005f + 0.000001f) return false;
    if (!aiPushState(e, AiNegotiation)) return false;
    if (b.node >= 0) {
        const Value k = nodes_[size_t(b.node)].obj->get("animscript");
        e.ai.negotiationScript = k.isString() ? k.asString() : std::string(); // 0x543760
    }
    return true;
}

void World::useAnimTree(Entity& e, const std::string& tree) {
    const AnimTree* t = animLib_.tree(tree);
    if (!t) throw std::runtime_error("unknown animtree " + tree);
    if (e.animTree == t->name() && e.anim) return;
    e.animTree = t->name();
    e.anim = std::make_unique<AnimState>(*t, animLib_);
}

// setanim and friends (signatures as documented in the shipped animscripts, e.g.
// "setflaggedanimknoballrestart(notifyName, anim, rootAnim, goalWeight, goalTime, rate)"). Defaults are inferred.
void World::registerAnimBuiltins() {
    auto node = [this](CallContext& c, const Value& v) -> std::pair<Entity*, int> {
        Entity* e = entityOf(c.self);
        if (!e || !e->anim) throw std::runtime_error("entity has no animtree");
        if (!std::holds_alternative<script::AnimRef>(v.v)) throw std::runtime_error("expected an animation, got " + v.typeName());
        const int n = e->anim->tree().find(std::get<script::AnimRef>(v.v).name);
        if (n < 0) throw std::runtime_error("animation " + std::get<script::AnimRef>(v.v).name + " is not in animtree " + e->animTree);
        return {e, n};
    };
    struct Variant { const char* name; bool flagged, knob, knobAll, restart; };
    static const Variant variants[] = {
        {"setanim", false, false, false, false}, {"setanimrestart", false, false, false, true},
        {"setanimknob", false, true, false, false}, {"setanimknobrestart", false, true, false, true},
        {"setanimknoball", false, true, true, false}, {"setanimknoballrestart", false, true, true, true},
        {"setflaggedanim", true, false, false, false}, {"setflaggedanimrestart", true, false, false, true},
        {"setflaggedanimknob", true, true, false, false}, {"setflaggedanimknobrestart", true, true, false, true},
        {"setflaggedanimknoball", true, true, true, false}, {"setflaggedanimknoballrestart", true, true, true, true},
    };
    for (const Variant& var : variants) {
        vm_.registerBuiltin(var.name, [node, var](CallContext& c) {
            size_t i = 0;
            AnimState::SetArgs a;
            if (var.flagged) a.notify = c.arg(i++).toString();
            auto [e, n] = node(c, c.arg(i++));
            if (var.knobAll) {
                a.knobAllRoot = node(c, c.arg(i++)).second;
            }
            a.knob = var.knob;
            a.restart = var.restart;
            if (c.arg(i).isDefined()) a.weight = c.arg(i).asFloat();
            ++i;
            if (c.arg(i).isDefined()) a.blendTime = c.arg(i).asFloat();
            ++i;
            if (c.arg(i).isDefined()) a.rate = c.arg(i).asFloat();
            e->anim->set(n, a);
            return Value();
        });
    }
    vm_.registerBuiltin("clearanim", [node](CallContext& c) {
        auto [e, n] = node(c, c.arg(0));
        e->anim->clear(n, c.arg(1).isDefined() ? c.arg(1).asFloat() : 0.2f);
        return Value();
    });
    vm_.registerBuiltin("getanimtime", [node](CallContext& c) {
        auto [e, n] = node(c, c.arg(0));
        return Value(e->anim->time(n));
    });
    vm_.registerBuiltin("getanimlength", [this](CallContext& c) {
        if (!std::holds_alternative<script::AnimRef>(c.arg(0).v)) throw std::runtime_error("getanimlength needs an animation");
        const assets::XAnim* a = animLib_.anim(std::get<script::AnimRef>(c.arg(0).v).name);
        return Value(a ? a->length() : 0.0f);
    });
    vm_.registerBuiltin("animhasnotetrack", [this](CallContext& c) {
        if (!std::holds_alternative<script::AnimRef>(c.arg(0).v)) throw std::runtime_error("animhasnotetrack needs an animation");
        const assets::XAnim* a = animLib_.anim(std::get<script::AnimRef>(c.arg(0).v).name);
        const std::string note = lower(c.arg(1).toString());
        if (a)
            for (const auto& n : a->notes)
                if (lower(n.name) == note) return Value(true);
        return Value(false);
    });
}

int World::spawnAllActors() {
    std::vector<Entity*> spawners;
    for (const auto& e : entities_)
        if (e->isSpawner && !e->obj->removed) spawners.push_back(e.get());
    int n = 0;
    for (Entity* sp : spawners) n += spawnActor(*sp) != nullptr;
    return n;
}

void World::setPlayerOrigin(const Vec3& origin, const Vec3& angles) {
    if (!player_) return;
    player_->obj->set("origin", Value(origin));
    player_->obj->set("angles", Value(angles));
}

bool World::submodelBounds(const std::string& model, Vec3& mins, Vec3& maxs) const {
    if (model.size() < 2 || model[0] != '*') return false;
    const int i = std::atoi(model.c_str() + 1);
    if (i <= 0 || size_t(i) >= submodels_.size()) return false;
    mins = submodels_[size_t(i)].first;
    maxs = submodels_[size_t(i)].second;
    return true;
}

// SV_EntityContact (0x447830): does the box absmin..absmax touch trigger `e`? trigger_radius (shape flags 0x11 at
// ent+0xa2, spawn 0x528ee0: mins (-radius, -radius, 0), maxs (radius, radius, height)) is a vertical cylinder: the
// box must reach above the base and below the top, and the box centre must lie within radius + half the box width
// (x) of the axis. Brush triggers: a zero-length trace of the box against the brush model at the entity's origin and
// angles (CM_TransformedBoxTrace 0x417c20, mask -1) that starts solid.
// TODO(spec): trigger_disk (flag 0x20, spawn 0x529070 not decoded): 0x447830 then touches when the centre distance is
// at least radius + half width - 64; only AI-only disks (spawnflags 8) exist in the SP maps.
bool World::triggerContact(Entity& e, const Vec3& absmin, const Vec3& absmax) {
    const Value ov = e.obj->get("origin");
    const Vec3 o = ov.isVector() ? ov.asVector() : Vec3{0, 0, 0};
    if (e.classname == "trigger_radius") {
        const float r = e.obj->get("radius").isNumber() ? e.obj->get("radius").asFloat() : 0.0f;
        const float h = e.obj->get("height").isNumber() ? e.obj->get("height").asFloat() : 0.0f;
        if (!(absmax[2] > o[2]) || !(o[2] + h > absmin[2])) return false;
        const float cx = (absmin[0] + absmax[0]) * 0.5f, cy = (absmin[1] + absmax[1]) * 0.5f;
        const float rr = absmax[0] - cx + r, dx = cx - o[0], dy = cy - o[1];
        return dx * dx + dy * dy < rr * rr;
    }
    const Value m = e.obj->get("model");
    if (!m.isString() || m.asString().size() < 2 || m.asString()[0] != '*') return false;
    const int model = std::atoi(m.asString().c_str() + 1);
    const Value av = e.obj->get("angles");
    const Vec3 a = av.isVector() ? av.asVector() : Vec3{0, 0, 0};
    return collision_->traceModel(model, o, a, {0, 0, 0}, {0, 0, 0}, absmin, absmax, 0xffffffffu).startSolid;
}

// G_TouchTriggers for the player (0x4e7320): the entities in the player's area whose contents match the client mask
// 0x40000000 are tested with SV_EntityContact (0x447830) against the player's absolute box, and their touch function
// runs. Trigger contents (0x528dc0): 0x40000000 unless spawnflags & 8, so those triggers never fire for the player.
// Inferred: every server frame while touching; trigger_once is removed after it fires. Player box: (-15,-15,0)-(15,15,70)
// around the origin (TODO(spec): the stance-dependent r.maxs).
void World::runTriggers() {
    if (!player_) return;
    const Value po = player_->obj->get("origin");
    if (!po.isVector()) return;
    const Vec3 p = po.asVector();
    const Vec3 pmin{p[0] - 15, p[1] - 15, p[2]}, pmax{p[0] + 15, p[1] + 15, p[2] + 70};
    std::vector<Entity*> fired;
    for (const auto& e : entities_) {
        if (e->obj->removed || e->isSpawner || !startsWith(e->classname, "trigger_")) continue;
        if (e->classname == "trigger_damage" || e->classname == "trigger_lookat" || e->classname == "trigger_use" ||
            e->classname == "trigger_use_touch" || e->classname == "trigger_disk")
            continue;
        if (e->obj->get("$disabled").truthy()) continue;
        const Value sf = e->obj->get("spawnflags");
        // trigger_hurt (spawn 0x5292a0) sets its contents to 0x405c0008 itself, so spawnflags 8 does not exclude it.
        if (e->classname == "trigger_hurt") {
            if (e->obj->get("$hurtOff").truthy() || !triggerContact(*e, pmin, pmax)) continue;
            // Touch function 0x5291d0: the next hit is due at ent+0x15c (spawnflags 0x10: +1000 ms, else +50 ms);
            // G_Damage(other, self, self, no direction, no point, dmg, flags (spawnflags 8) << 2, MOD_TRIGGER_HURT); then
            // spawnflags 0x20 switches it off (handler 6). Spawnflags 2 skips actors (the player is not one).
            // TODO(spec): the damage flag 0x20 (spawnflags 8) is not modelled by damage(); actors do not touch triggers here.
            const int flags = sf.isNumber() ? sf.asInt() : 0;
            const Value nt = e->obj->get("$hurtNext");
            if (nt.isNumber() && nt.asInt() > timeMs_) continue;
            e->obj->set("$hurtNext", Value(int32_t(timeMs_ + ((flags & 0x10) ? 1000 : 50))));
            vm_.notify(e->obj, "trigger", {Value(player_->obj)}); // the touch record 0x528cb0
            const Value dv = e->obj->get("dmg");
            damage(*player_, e.get(), dv.isNumber() ? dv.asInt() : 5, Vec3{0, 0, 0}, Vec3{0, 0, 0}, "MOD_TRIGGER_HURT");
            if (flags & 0x20) e->obj->set("$hurtOff", Value(true));
            continue;
        }
        if (sf.isNumber() && (sf.asInt() & 8)) continue;
        if (triggerContact(*e, pmin, pmax)) fired.push_back(e.get());
    }
    for (Entity* e : fired) {
        if (e->obj->removed) continue;
        if (e->classname == "trigger_friendlychain") touchFriendlyChain(*e, *player_); // touch 0x529140
        vm_.notify(e->obj, "trigger", {Value(player_->obj)});
        if (e->classname == "trigger_once") e->obj->removed = true;
    }
}

void World::registerBuiltins() {
    auto findEntities = [this](const Value& value, const Value& key) {
        if (!key.isDefined()) throw std::runtime_error("getent/getentarray need a key");
        const std::string k = lower(key.toString());
        std::vector<ObjectPtr> out;
        for (const auto& e : entities_)
            if (!e->obj->removed && e->obj->get(k) == value) out.push_back(e->obj); // spawners included
        return out;
    };
    vm_.registerBuiltin("getent", [findEntities](CallContext& c) -> Value {
        auto r = findEntities(c.arg(0), c.arg(1));
        if (r.size() > 1) throw std::runtime_error("getent used with more than one entity matching " + c.arg(0).toString());
        return r.empty() ? Value() : Value(r[0]);
    });
    vm_.registerBuiltin("getentarray", [findEntities](CallContext& c) {
        if (!c.arg(0).isDefined()) throw std::runtime_error("getentarray without arguments is not supported");
        return Value(toArray(findEntities(c.arg(0), c.arg(1))));
    });

    auto findNodes = [this](const Value& value, const Value& key) {
        const std::string k = lower(key.toString());
        std::vector<ObjectPtr> out;
        for (const auto& n : nodes_)
            if (n.obj->get(k) == value) out.push_back(n.obj);
        return out;
    };
    vm_.registerBuiltin("getnode", [findNodes](CallContext& c) -> Value {
        auto r = findNodes(c.arg(0), c.arg(1));
        if (r.size() > 1) throw std::runtime_error("getnode used with more than one node matching " + c.arg(0).toString());
        return r.empty() ? Value() : Value(r[0]);
    });
    vm_.registerBuiltin("getnodearray", [findNodes](CallContext& c) { return Value(toArray(findNodes(c.arg(0), c.arg(1)))); });
    vm_.registerBuiltin("getallnodes", [this](CallContext&) {
        std::vector<ObjectPtr> all;
        for (const auto& n : nodes_) all.push_back(n.obj);
        return Value(toArray(all));
    });
    auto findVehicleNodes = [this](const Value& value, const Value& key) {
        const std::string k = lower(key.toString());
        std::vector<ObjectPtr> out;
        for (const auto& n : vehicleNodes_)
            if (n->get(k) == value) out.push_back(n);
        return out;
    };
    vm_.registerBuiltin("getvehiclenode", [findVehicleNodes](CallContext& c) -> Value {
        auto r = findVehicleNodes(c.arg(0), c.arg(1));
        return r.empty() ? Value() : Value(r[0]);
    });
    vm_.registerBuiltin("getvehiclenodearray", [findVehicleNodes](CallContext& c) { return Value(toArray(findVehicleNodes(c.arg(0), c.arg(1)))); });
    vm_.registerBuiltin("getallvehiclenodes", [this](CallContext&) { return Value(toArray(vehicleNodes_)); });

    vm_.registerBuiltin("getspawnerarray", [this](CallContext&) {
        std::vector<ObjectPtr> out;
        for (const auto& e : entities_)
            if (e->isSpawner && !e->obj->removed) out.push_back(e->obj);
        return Value(toArray(out));
    });
    vm_.registerBuiltin("getspawnerteamarray", [this](CallContext& c) {
        const std::string team = c.arg(0).toString();
        const char* prefix = team == "axis" ? "actor_axis_" : team == "allies" ? "actor_ally_" : "actor_neutral_";
        std::vector<ObjectPtr> out;
        for (const auto& e : entities_)
            if (e->isSpawner && !e->obj->removed && startsWith(e->classname, prefix)) out.push_back(e->obj);
        return Value(toArray(out));
    });
    // getaiarray(team) (0x4ec9f0) lists actors whose actor +0x25c is set; the die callback (0x4ff530) clears it, so
    // dead actors are left out.
    vm_.registerBuiltin("getaiarray", [this](CallContext& c) {
        std::vector<ObjectPtr> out;
        for (const auto& e : entities_)
            if (!e->obj->removed && e->obj->kind == "actor" && isAlive(*e) && (!c.arg(0).isDefined() || e->obj->get("team") == c.arg(0)))
                out.push_back(e->obj);
        return Value(toArray(out));
    });

    // 0x4f4450: stores GetInt(0) at 0x1c61050; the per-frame actor pass (0x4db31f) then marks allies with drawoncompass.
    vm_.registerBuiltin("drawcompassfriendlies", [this](CallContext& c) {
        drawCompassFriendlies_ = c.arg(0).isDefined() && c.arg(0).truthy();
        return Value();
    });

    auto doSpawn = [this](CallContext& c, bool force) -> Value {
        Entity* sp = entityOf(c.self);
        if (!sp || !sp->isSpawner) throw std::runtime_error("dospawn can only be called on actor spawners");
        Entity* a = spawnActor(*sp, force);
        return a ? Value(a->obj) : Value();
    };
    vm_.registerBuiltin("dospawn", [doSpawn](CallContext& c) { return doSpawn(c, false); });           // 0x4edff0: arg3 = 0
    vm_.registerBuiltin("stalingradspawn", [doSpawn](CallContext& c) { return doSpawn(c, true); });    // 0x4ee120: arg3 = 1
    vm_.registerBuiltin("attach", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("attach on a non-entity");
        e->attachments.push_back({c.arg(0).toString(), c.arg(1).isDefined() ? lower(c.arg(1).toString()) : std::string()});
        return Value();
    });
    vm_.registerBuiltin("detach", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("detach on a non-entity");
        const std::string m = c.arg(0).toString(), t = c.arg(1).isDefined() ? lower(c.arg(1).toString()) : std::string();
        for (auto it = e->attachments.begin(); it != e->attachments.end(); ++it)
            if (lower(it->model) == lower(m) && (t.empty() || it->tag == t)) { e->attachments.erase(it); return Value(); }
        throw std::runtime_error("detach: model " + m + " is not attached");
    });
    vm_.registerBuiltin("detachall", [this](CallContext& c) {
        if (Entity* e = entityOf(c.self)) e->attachments.clear();
        return Value();
    });
    vm_.registerBuiltin("getattachsize", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        return Value(int32_t(e ? e->attachments.size() : 0));
    });
    vm_.registerBuiltin("getattachmodelname", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        const int i = c.arg(0).asInt();
        if (!e || i < 0 || size_t(i) >= e->attachments.size()) throw std::runtime_error("getattachmodelname: bad index");
        return Value(e->attachments[size_t(i)].model);
    });
    vm_.registerBuiltin("getattachtagname", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        const int i = c.arg(0).asInt();
        if (!e || i < 0 || size_t(i) >= e->attachments.size()) throw std::runtime_error("getattachtagname: bad index");
        return Value(e->attachments[size_t(i)].tag);
    });
    vm_.registerBuiltin("useanimtree", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (e && c.arg(0).isDefined()) useAnimTree(*e, std::get<script::AnimTreeRef>(c.arg(0).v).name);
        return Value();
    });
    vm_.registerBuiltin("stopuseanimtree", [this](CallContext& c) {
        if (Entity* e = entityOf(c.self)) { e->anim.reset(); e->animTree.clear(); }
        return Value();
    });
    auto weaponField = [this](const Value& name, const std::string& field) -> std::string {
        const auto* w = weapon(name.toString());
        if (!w) throw std::runtime_error("unknown weapon " + name.toString());
        auto it = w->find(field);
        return it == w->end() ? std::string() : it->second;
    };
    vm_.registerBuiltin("getweaponmodel", [weaponField](CallContext& c) { return Value(weaponField(c.arg(0), "worldmodel")); });
    vm_.registerBuiltin("weaponfightdist", [weaponField](CallContext& c) { return Value(float(std::atof(weaponField(c.arg(0), "fightdist").c_str()))); });
    vm_.registerBuiltin("weaponmaxdist", [weaponField](CallContext& c) { return Value(float(std::atof(weaponField(c.arg(0), "maxdist").c_str()))); });
    vm_.registerBuiltin("weaponclipsize", [weaponField](CallContext& c) { return Value(int32_t(std::atoi(weaponField(c.arg(0), "clipsize").c_str()))); });
    vm_.registerBuiltin("weaponissemiauto", [weaponField](CallContext& c) { return Value(weaponField(c.arg(0), "semiauto") == "1"); });
    vm_.registerBuiltin("weaponisboltaction", [weaponField](CallContext& c) { return Value(weaponField(c.arg(0), "boltaction") == "1"); });
    vm_.registerBuiltin("weapontype", [weaponField](CallContext& c) { return Value(weaponField(c.arg(0), "weapontype")); });
    vm_.registerBuiltin("weaponclass", [weaponField](CallContext& c) { return Value(weaponField(c.arg(0), "weaponclass")); });
    // Angle of the actor's motion relative to its facing, in degrees (0 = moving forward). Inferred from
    // animscripts/utility.gsc QuadrantAnimWeights(getMotionAngle() + 180) picking "front" for forward motion.
    vm_.registerBuiltin("getmotionangle", [](CallContext& c) {
        const auto o = c.self.asObject();
        const Value mv = o->get("$motionyaw"), an = o->get("angles");
        if (!mv.isNumber() || !an.isVector()) return Value(0.0f);
        float d = mv.asFloat() - an.asVector()[1];
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        return Value(d);
    });
    vm_.registerBuiltin("getentitynumber", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("getentitynumber on a non-entity");
        return Value(int32_t(e->number));
    });
    vm_.registerBuiltin("getentnum", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("getentnum on a non-entity");
        return Value(int32_t(e->number));
    });
    // gethitenttype 0x5400c0: "none" for 0x7ff, "obstacle" when the bumped entity's r.contents (ent +0x128) has bit 0x400, else "world".
    // The bumped entity (+0x318) is only tracked with IW2_ACTOR_SLIDE (World::moveActor), so it is "none" otherwise.
    vm_.registerBuiltin("gethitenttype", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("gethitenttype on a non-actor");
        const int b = e->ai.bumpedEnt;
        if (b == 0x7ff) return Value(std::string("none"));
        if (b != 0x7fe)
            for (const EntityClip& k : collision_->entityClips())
                if (k.id == b && (k.contents & 0x400)) return Value(std::string("obstacle"));
        return Value(std::string("world"));
    });
    // AI goals are stored for the movement code; the actor notifies "goal" when it arrives (not implemented yet).
    vm_.registerBuiltin("setgoalnode", [this](CallContext& c) {
        if (c.self.asObject()->get("$goalfrozen").truthy()) return Value(); // IW2_TRAVERSE_SCENE keeps its own goal
        auto o = c.self.asObject();
        if (c.arg(0).isObject()) {
            o->set("$goalpos", c.arg(0).asObject()->get("origin"));
            o->set("$goalent", Value()); // 0x540880 clears the goal entity (+0x77c)
            if (Entity* self = entityOf(c.self); self && !goalVolumeHolds(*self, c.arg(0).asObject()->get("origin").isVector() ? c.arg(0).asObject()->get("origin").asVector() : Vec3{0, 0, 0})) o->set("$goalvolume", Value()); // 0x5408fe..: the volume must contain the new goal
        }
        return Value();
    });
    vm_.registerBuiltin("setgoalpos", [this](CallContext& c) {
        if (c.self.asObject()->get("$goalfrozen").truthy()) return Value(); // IW2_TRAVERSE_SCENE keeps its own goal
        c.self.asObject()->set("$goalpos", c.arg(0));
        c.self.asObject()->set("$goalent", Value());
        if (Entity* self = entityOf(c.self); self && c.arg(0).isVector() && !goalVolumeHolds(*self, c.arg(0).asVector())) c.self.asObject()->set("$goalvolume", Value()); // 0x5409bd
        return Value();
    });
    vm_.registerBuiltin("setgoalentity", [this](CallContext& c) {
        static const bool log = std::getenv("IW2_CHAIN_LOG") != nullptr; // debug: the script site of every setgoalentity
        if (log) print("setgoalentity at " + std::to_string(timeMs_) + " ms from " + c.vm.describeThread(c.thread.id));
        if (c.self.asObject()->get("$goalfrozen").truthy()) return Value();
        c.self.asObject()->set("$goalent", c.arg(0));
        c.self.asObject()->set("$goalvolume", Value()); // 0x540a5f
        return Value();
    });
    // 0x540aa0: errors "cannot set goal volume when a goal entity is set" and "... which does not contain goal position".
    vm_.registerBuiltin("setgoalvolume", [this](CallContext& c) {
        if (c.self.asObject()->get("$goalfrozen").truthy()) return Value();
        Entity* self = entityOf(c.self);
        Entity* vol = entityOf(c.arg(0));
        if (self && vol) {
            if (self->obj->get("$goalent").isObject()) throw std::runtime_error("cannot set goal volume when a goal entity is set");
            const Value gp = self->obj->get("$goalpos");
            if (!triggerContact(*vol, gp.isVector() ? gp.asVector() : Vec3{0, 0, 0}, gp.isVector() ? gp.asVector() : Vec3{0, 0, 0}))
                throw std::runtime_error("cannot set goal volume which does not contain goal position");
        }
        c.self.asObject()->set("$goalvolume", c.arg(0));
        return Value();
    });
    vm_.registerBuiltin("spawn", [this](CallContext& c) {
        Entity& e = spawnEntity(c.arg(0).toString());
        if (c.arg(1).isDefined()) e.obj->set("origin", c.arg(1));
        if (c.arg(2).isDefined()) e.obj->set("spawnflags", c.arg(2));
        return Value(e.obj);
    });
    // spawnvehicle(model, targetname, vehicletype, origin, angles): a script_vehicle entity; its path state is
    // created on first use (src/game/vehicles.cpp).
    vm_.registerBuiltin("spawnvehicle", [this](CallContext& c) {
        Entity& e = spawnEntity("script_vehicle");
        e.obj->set("model", c.arg(0));
        e.obj->set("targetname", c.arg(1));
        e.obj->set("vehicletype", c.arg(2));
        if (c.arg(3).isVector()) e.obj->set("origin", c.arg(3));
        if (c.arg(4).isVector()) e.obj->set("angles", c.arg(4));
        // No health is written by spawnvehicle (0x4ecf50), the vehicle spawn 0x511af0 / 0x50d820 or G_Spawn: the entity starts with health 0
        // (zeroed), the scripts of path vehicles set self.health themselves (_crusader.gsc etc.). Engine sounds treat health < 1 as dead (0x510be0).
        e.obj->set("health", Value(int32_t(0)));
        e.obj->set("$takedamage", Value(true)); // 0x50d8f4: ent+0x111 = 1 in the vehicle spawn (0x50d820..0x50d900)
        return Value(e.obj);
    });
    // Load-time hints and profiling markers have no effect on the simulation.
    for (const char* noop : {"precachemodel", "precacheshader", "precacheitem", "precacheturret",
                             "precacherumble", "precachestring", "precachevehicle", "prof_begin", "prof_end"})
        vm_.registerBuiltin(noop, [](CallContext&) { return Value(); });
    // setviewmodel(model): the player's first-person hands model (read by the front end).
    vm_.registerBuiltin("setviewmodel", [](CallContext& c) {
        if (c.self.isObject()) c.self.asObject()->set("$viewmodel", c.arg(0));
        return Value();
    });
    vm_.registerBuiltin("clearenemy", [](CallContext& c) {
        if (c.self.isObject()) c.self.asObject()->set("enemy", Value());
        return Value();
    });
    vm_.registerBuiltin("delete", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("delete on a non-entity");
        if (e->isPlayer) throw std::runtime_error("cannot delete the player");
        connectEntityPaths(*e); // 0x4d7c30: freeing an entity with the DYNAMICPATH flag gives its paths back (0x5197a0)
        // delete (0x4ef570): notify "death" (0x46d0f0), then G_FreeEntity (0x4d7c30), which frees the script object (0x4670b0).
        // TODO(spec): the other entity frees (grenades, rockets, items, trigger_once, ...) still only mark the object removed.
        vm_.notify(e->obj, "death");
        vm_.freeObject(e->obj);
        return Value();
    });
    vm_.registerBuiltin("setmodel", [](CallContext& c) {
        c.self.asObject()->set("model", c.arg(0));
        return Value();
    });
    // makefakeai (0x4f82a0): the entity must be a script_model (eType 5, else "makeFakeAI must be applied to a script_model",
    // string 0x5853c4); it clears ent+0xa2 bits 1|2, sets the box mins (-15,-15,0) maxs (15,15,72) (0xa8..0xbc), ORs 0x8000
    // into ent+0xc0 and relinks (0x44b2d0). Recorded as script fields; TODO(spec): what the exe's traces/damage do with the
    // 0x8000 contents of a fake AI (bullets only hit script models through their xmodel surfaces here).
    vm_.registerBuiltin("makefakeai", [](CallContext& c) {
        const ObjectPtr o = c.self.asObject();
        if (o->get("classname").toString() != "script_model") throw std::runtime_error("makeFakeAI must be applied to a script_model");
        o->set("$fakeai_mins", Value(Vec3{-15, -15, 0}));
        o->set("$fakeai_maxs", Value(Vec3{15, 15, 72}));
        o->set("$fakeai", Value(true));
        return Value();
    });
    // getstance (0x4ee330): only for a client entity ("GetStance is only defined for players.", 0x587544); the string by
    // ps pm_flags (client+0xc): bit 1 -> prone, bit 2 -> crouch, else stand (the three strings are runtime script strings, so
    // which text sits at 0x1c7f0b0 / 0x1c7f056 / 0x1c7f0da is inferred from pm_flags 1 = prone, 2 = crouch, world.h).
    vm_.registerBuiltin("getstance", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("GetStance is only defined for players.");
        return Value(std::string(playerCmd_.stance == 2 ? "prone" : playerCmd_.stance == 1 ? "crouch" : "stand"));
    });
    // meleebuttonpressed (0x5388d0): player-only; (client+0x8640 | client+0x8634) & 4 -> int 1/0. 0x8640 is taken as the
    // current usercmd buttons and 0x8634 as the previous frame's (TODO(spec): confirm the pair); the bit is the melee button (4).
    vm_.registerBuiltin("meleebuttonpressed", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("meleebuttonpressed: entity is not a player");
        return Value(int32_t(((playerCmd_.buttons | prevButtons_) & 4u) ? 1 : 0));
    });
    // enableaimassist / disableaimassist (0x4f81c0 / 0x4f8230): set / clear the entity flag 0x800 (entity+0x10). Stored as the
    // script-invisible field $aimassist; nothing in the port reads it yet (TODO(spec): the console auto-aim consumer).
    vm_.registerBuiltin("enableaimassist", [this](CallContext& c) {
        if (Entity* e = entityOf(c.self)) e->obj->set("$aimassist", Value(int32_t(1)));
        return Value();
    });
    vm_.registerBuiltin("disableaimassist", [this](CallContext& c) {
        if (Entity* e = entityOf(c.self)) e->obj->set("$aimassist", Value(int32_t(0)));
        return Value();
    });
    // pushplayer (0x5407a0): actor-only (the pointer at entity+0x100, error 0x591798); arg true clears, false sets the bit 0x2000000 of actor+0x28c (stored as $nopush;
    // nothing consumes it yet, TODO(spec): the pmove effect of that bit).
    vm_.registerBuiltin("pushplayer", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("pushplayer: entity is not an actor");
        e->obj->set("$nopush", Value(int32_t(c.args.empty() || !c.arg(0).truthy() ? 1 : 0)));
        return Value();
    });
    // playerads (0x538a60): player-only (error 0x590ab8 otherwise); returns the float at client +0xb8 (the aim fraction, 0..1).
    vm_.registerBuiltin("playerads", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("playerads: entity is not a player");
        return Value(playerCmd_.adsFrac);
    });
    // isfiring / isthrowinggrenade / ismeleeing (0x539aa0 / 0x539b60 / 0x539c10): player-only (error 0x590ab8 for a non-client);
    // an int from the weapon state ps+0xb4: firing 3, 4, 10, 11; throwing a grenade 12..16 (0xc..0x10); meleeing 10, 11.
    // The value of playerCmd_.weaponState is the front end's copy of ps+0xb4 (world.h).
    auto weaponStateQuery = [this](const char* name, int lo, int hi, std::vector<int> extra) {
        vm_.registerBuiltin(name, [this, lo, hi, extra, name](CallContext& c) {
            Entity* e = entityOf(c.self);
            if (!e || !e->isPlayer) throw std::runtime_error(std::string(name) + ": entity is not a player");
            const int ws = playerCmd_.weaponState;
            bool r = lo <= hi && ws >= lo && ws <= hi;
            for (int x : extra) r = r || ws == x;
            return Value(int32_t(r ? 1 : 0));
        });
    };
    weaponStateQuery("isfiring", 1, 0, {3, 4, 10, 11});
    weaponStateQuery("isthrowinggrenade", 12, 16, {});
    weaponStateQuery("ismeleeing", 1, 0, {10, 11});
    vm_.registerBuiltin("hide", [](CallContext& c) { c.self.asObject()->set("$hidden", Value(true)); return Value(); });
    vm_.registerBuiltin("show", [](CallContext& c) { c.self.asObject()->set("$hidden", Value()); return Value(); });
    vm_.registerBuiltin("getorigin", [](CallContext& c) { return c.self.asObject()->get("origin"); });
    vm_.registerBuiltin("isalive", [this](CallContext& c) {
        Entity* e = entityOf(c.arg(0));
        return Value(e && !e->obj->removed && e->obj->get("health").isDefined() && e->obj->get("health").asInt() > 0);
    });
    vm_.registerBuiltin("isplayer", [this](CallContext& c) { Entity* e = entityOf(c.arg(0)); return Value(e && e->isPlayer); });
    vm_.registerBuiltin("isai", [this](CallContext& c) { Entity* e = entityOf(c.arg(0)); return Value(e && e->obj->kind == "actor"); });
    vm_.registerBuiltin("issentient", [this](CallContext& c) {
        Entity* e = entityOf(c.arg(0));
        return Value(e && (e->isPlayer || e->obj->kind == "actor"));
    });

    vm_.registerBuiltin("getcvar", [this](CallContext& c) { return Value(cvars[c.arg(0).toString()]); });
    vm_.registerBuiltin("getdebugcvar", [this](CallContext& c) { return Value(cvars[c.arg(0).toString()]); });
    vm_.registerBuiltin("getcvarint", [this](CallContext& c) { return Value(int32_t(std::atoi(cvars[c.arg(0).toString()].c_str()))); });
    vm_.registerBuiltin("getdebugcvarint", [this](CallContext& c) { return Value(int32_t(std::atoi(cvars[c.arg(0).toString()].c_str()))); });
    vm_.registerBuiltin("getcvarfloat", [this](CallContext& c) { return Value(float(std::atof(cvars[c.arg(0).toString()].c_str()))); });
    // A localized string value is stored as its reference (the VM keeps it as a string; inferred from
    // matmata.gsc setCvar("ui_deadquote", &"MATMATA_…") read back by the dead quote, 0x4aa2a0).
    vm_.registerBuiltin("setcvar", [this](CallContext& c) {
        const Value v = c.arg(1);
        cvars[c.arg(0).toString()] = std::holds_alternative<LocalizedString>(v.v) ? std::get<LocalizedString>(v.v).key : v.toString();
        return Value();
    });
    // setmissioncvar (0x4f40b0): the name must be one of mis_01..mis_10, mis_difficulty (table 0x5a974c, else "Invalid mission dvar name");
    // the dvar must exist; an int dvar (type 5) takes the int argument, a string dvar (type 7) the string argument; other types error.
    vm_.registerBuiltin("setmissioncvar", [this](CallContext& c) {
        const std::string name = c.arg(0).toString();
        static const char* const valid[] = {"mis_01", "mis_02", "mis_03", "mis_04", "mis_05", "mis_06", "mis_07", "mis_08", "mis_09", "mis_10", "mis_difficulty"};
        bool ok = false;
        for (const char* v : valid) ok = ok || name == v;
        if (!ok) throw std::runtime_error("Invalid mission dvar name: " + name);
        if (name == "mis_difficulty") cvars[name] = c.arg(1).toString();
        else cvars[name] = std::to_string(c.arg(1).asInt());
        return Value();
    });
    vm_.registerBuiltin("setsavedcvar", [this](CallContext& c) { cvars[c.arg(0).toString()] = c.arg(1).toString(); return Value(); });
    vm_.registerBuiltin("getdifficulty", [this](CallContext&) {
        const std::string d = cvars["g_gameskill"];
        static const char* names[] = {"easy", "medium", "hard", "fu"};
        const int i = d.empty() ? 1 : std::clamp(std::atoi(d.c_str()), 0, 3);
        return Value(names[i]);
    });
    // Effects (docs/spec/effects.md). TODO(spec): the argument order of playfx/playloopedfx is read from the
    // shipped scripts' calls (id, origin[, forward]) and (id, repeat seconds, origin), not from the engine.
    vm_.registerBuiltin("loadfx", [this](CallContext& c) {
        fxNames_.push_back(c.arg(0).toString());
        return Value(nextFx_++);
    });
    auto fxPath = [this](const Value& id) -> std::string {
        if (!id.isNumber()) return {};
        const int i = id.asInt();
        return i >= 0 && size_t(i) < fxNames_.size() ? fxNames_[size_t(i)] : std::string();
    };
    vm_.registerBuiltin("playfx", [this, fxPath](CallContext& c) {
        const std::string path = fxPath(c.arg(0));
        if (!path.empty() && c.arg(1).isVector())
            fxEvents_.push_back({path, c.arg(1).asVector(), c.args.size() > 2 && c.arg(2).isVector() ? c.arg(2).asVector() : Vec3{0, 0, 1}});
        return Value();
    });
    vm_.registerBuiltin("playfxontag", [this, fxPath](CallContext& c) {
        const std::string path = fxPath(c.arg(0));
        Entity* e = entityOf(c.arg(1));
        if (path.empty() || !e) return Value();
        Vec3 at = e->obj->get("origin").asVector();
        const auto bones = entityBones(*e);
        std::string tag = c.arg(2).toString();
        for (char& ch : tag) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (auto it = bones.find(tag); it != bones.end()) at = it->second.position;
        // EV_PLAY_FX_ON_TAG: the client plays the effect with an attachment record (entity, tag) (0x4853f0), so the
        // `relative` elements follow the tag.
        fxEvents_.push_back({path, at, Vec3{0, 0, 1}, e->number, tag});
        return Value();
    });
    vm_.registerBuiltin("playloopedfx", [this, fxPath](CallContext& c) {
        if (fxPath(c.arg(0)).empty() || !c.arg(2).isVector()) return Value();
        loopedFx_.push_back({c.arg(0).asInt(), std::max(0.05f, c.arg(1).asFloat()), c.arg(2).asVector(), timeMs_});
        return Value();
    });
    vm_.registerBuiltin("print", [this](CallContext& c) { print(c.arg(0).toString()); return Value(); });
    // getnorthyaw (0x4f4a30): configstring 10 (0x815434, the worldspawn "northyaw" key) copied to a 32-byte
    // buffer (at most 31 characters, 0x553b70) and converted with atof (0x553d7f).
    vm_.registerBuiltin("getnorthyaw", [this](CallContext&) {
        return Value(float(std::atof(northYaw_.substr(0, 31).c_str())));
    });
    // getpartname(model, index) (0x4f5bc0): the model's bone name at index; exactly two arguments.
    vm_.registerBuiltin("getpartname", [this](CallContext& c) {
        if (c.argc() != 2) throw std::runtime_error("Incorrect usage for getpartname()");
        const assets::XModelParts* parts = skeleton(c.arg(0).toString());
        if (!parts) throw std::runtime_error("bad model");
        const int i = c.arg(1).asInt();
        if (i < 0 || size_t(i) >= parts->bones.size())
            throw std::runtime_error("index out of range (0 - " + std::to_string(int(parts->bones.size()) - 1) + ")");
        return Value(parts->bones[size_t(i)].name);
    });
}

void World::registerMissingStubs() {
    std::set<std::string> defined, called;
    for (const auto& name : vfs_.list()) {
        if (name.size() < 4 || name.substr(name.size() - 4) != ".gsc") continue;
        const auto data = vfs_.read(name);
        try {
            const auto tree = parseScript(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), name);
            std::function<void(const Node&)> walk = [&](const Node& n) {
                if (n.kind == NodeKind::Function) defined.insert(lower(n.text));
                if (n.kind == NodeKind::Call && !n.isPointerCall && n.children.size() > 1 &&
                    n.children[1]->text.find("::") == std::string::npos)
                    called.insert(lower(n.children[1]->text));
                for (const auto& c : n.children) walk(*c);
            };
            walk(*tree);
        } catch (const std::exception&) {
        }
    }
    for (const auto& f : called)
        if (!defined.count(f) && !vm_.hasBuiltin(f))
            vm_.registerBuiltin(f, [this, f](CallContext&) {
                ++missing_[f];
                return Value();
            });
}

} // namespace iw2::game

namespace iw2::game {
bool World::entityTagPosition(int number, const std::string& tag, Vec3& out) {
    Entity* e = entityByNumber(number);
    if (!e) return false;
    if (e->vehicle) { // the vehicle's bone controls (jolt, turret yaw, barrel pitch) move the flash tags
        bool found = false;
        const Vec3 p = vehicleTagPosition(*e, tag.c_str(), found);
        if (found) out = p;
        return found;
    }
    const auto bones = entityBones(*e);
    const auto it = bones.find(tag);
    if (it == bones.end()) return false;
    out = it->second.position;
    return true;
}
} // namespace iw2::game
