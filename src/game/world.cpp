#include "game/world.h"
#include "game/actorfields.h"

#include "script/builtins_core.h"
#include "script/parser.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
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

// Map key/values become typed script fields. Numeric text becomes an int or float and three numbers become a
// vector (inferred: scripts use values such as script_delay and script_health directly as numbers).
Value parseFieldValue(const std::string& key, const std::string& v) {
    if (key == "targetname" || key == "target" || key == "classname" || key == "model" || key == "script_noteworthy")
        return Value(v);
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
    auto d = vfs_.read("maps/" + mapName_ + ".d3dbsp");
    if (!d) throw std::runtime_error("map not found: " + mapName_);
    bspData_ = std::move(*d);
    bsp_ = std::make_unique<assets::D3dBsp>(bspData_);
    collision_ = std::make_unique<CollisionWorld>(*bsp_);
    const auto models = bsp_->lump(35);
    for (size_t i = 0; i + 48 <= models.size(); i += 48) {
        Vec3 mn, mx;
        std::memcpy(mn.data(), models.data() + i, 12);
        std::memcpy(mx.data(), models.data() + i + 12, 12);
        submodels_.push_back({mn, mx});
    }
    vm_.print = [this](const std::string& s) { print(s); };
    registerCoreBuiltins(vm_);
    registerBuiltins();
    registerAnimBuiltins();
    registerCombatBuiltins();
    registerSoundBuiltins();
    registerHudBuiltins();
    registerInventoryBuiltins();
    registerMoverBuiltins();
    registerCampaignBuiltins();
    registerVehicleBuiltins();
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
    e->obj->set("spawnflags", Value(int32_t(0))); // native entity fields with default 0
    e->obj->set("count", Value(int32_t(0)));
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
                o->set(key, parseFieldValue(key, v));
            }
        };
        if (cls == "script_struct") {
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
            fill(n.obj);
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

    Entity& p = spawnEntity("player");
    p.isPlayer = true;
    p.obj->set("health", Value(int32_t(100)));
    p.obj->set("maxhealth", Value(int32_t(100)));
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
Entity* World::spawnActor(Entity& spawner) {
    const Value count = spawner.obj->get("count");
    if (count.isInt() && count.asInt() <= 0) return nullptr;
    if (count.isInt()) spawner.obj->set("count", Value(int32_t(count.asInt() - 1)));

    const std::string cls = spawner.classname;
    Entity& a = spawnEntity(cls);
    a.isActor = true;
    a.obj->kind = "actor";
    // Spawned AI keep the spawner's key/values except its targetname; map-start AI (no spawner flag 1) are the
    // placed entity itself, so they keep it. A spawner whose count runs out is removed. (Inferred from scripts
    // that getent() a spawner by targetname and later getent() the AI by script_noteworthy.)
    const Value flags = spawner.obj->get("spawnflags");
    const bool placed = !(flags.isInt() && (flags.asInt() & 1));
    for (const auto& [k, v] : spawner.obj->fields)
        if (k != "spawnflags" && k != "count" && (placed || k != "targetname")) a.obj->set(k, v);
    a.obj->set("classname", Value(cls));
    a.obj->set("team", Value(startsWith(cls, "actor_axis") ? "axis" : startsWith(cls, "actor_ally") ? "allies" : "neutral"));
    useAnimTree(a, "generic_human"); // AI use the generic_human tree (inferred from animscripts' #using_animtree)
    applyActorFieldDefaults(*a.obj); // docs/spec/ai.md
    // TODO(spec): movemode and anim_pose are engine-maintained string fields whose initial values are not
    // specified yet; these two are inferred.
    a.obj->set("movemode", Value("run"));
    a.obj->set("anim_pose", Value("stand"));
    const std::string aitype = "aitype/" + cls.substr(6) + ".gsc";
    try {
        vm_.start(aitype, "main", {}, Value(a.obj));
    } catch (const std::exception& e) {
        print("aitype " + aitype + ": " + e.what());
    }
    vm_.notify(spawner.obj, "spawned", {Value(a.obj)});
    if (count.isInt() && count.asInt() - 1 <= 0) spawner.obj->removed = true;
    startAnimScript(a, "init");
    return &a;
}

void World::start() {
    // maps/_load.gsc sets level.script from getcvar("mapname"), so the cvar must name the running map before the
    // level script starts (docs/spec/ui.md, intro screen).
    cvars["mapname"] = mapName_;
    spawnMapEntities();
    linkVehicleNodes();
    vm_.level()->set("script", Value(mapName_));
    vm_.start("maps/" + mapName_ + ".gsc", "main", {}, Value(vm_.level())); // self = level: maps/duhoc_assault.gsc main() threads read self fields (inferred)
}

Entity* World::entityByNumber(int number) {
    for (const auto& e : entities_)
        if (e->number == number) return e.get();
    return nullptr;
}

void World::frame() {
    timeMs_ += 50;
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
    thinkVehicles();
    thinkActors();
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

// Engine animscripts (docs/spec/ai.md): the previous one gets "killanimscript", then animscripts/<name>::main()
// starts on the actor.
void World::startAnimScript(Entity& e, const std::string& name) {
    if (!e.animScript.empty()) vm_.notify(e.obj, "killanimscript");
    e.animScript = name;
    e.animScriptStartMs = timeMs_;
    try {
        e.animScriptThread = vm_.start("animscripts/" + name + ".gsc", "main", {}, Value(e.obj));
    } catch (const std::exception& ex) {
        print("animscripts/" + name + ": " + ex.what());
        e.animScriptThread = 0;
    }
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

// Minimal AI state machine (inferred placeholder, docs/spec/ai.md): an actor with a goal outside its goal radius
// runs "move" and follows the node graph; on arrival it is notified "goal" and runs "stop". When any animscript
// ends on its own, the actor returns to "stop".
void World::thinkActors() {
    const bool perceive = (perceptionTick_++ % 4) == 0; // every 0.2 s (inferred cadence)
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (!e.isActor || e.obj->removed) continue;
        if (!isAlive(e)) continue; // the death animscript runs on its own
        if (perceive) updatePerception(e);
        {
            // lookforward: unit vector of the actor's facing (native field, docs/spec/ai.md).
            const Value ang = e.obj->get("angles");
            const float yaw = ang.isVector() ? ang.asVector()[1] * float(M_PI) / 180.0f : 0.0f;
            e.obj->set("lookforward", Value(Vec3{std::cos(yaw), std::sin(yaw), 0}));
        }
        if (e.animScript == "init" && e.animScriptThread && vm_.isThreadAlive(e.animScriptThread)) continue;
        const Value goal = e.obj->get("$goalpos");
        const Value origin = e.obj->get("origin");
        if (goal.isVector() && origin.isVector()) {
            const auto g = goal.asVector(), o = origin.asVector();
            const Value rv = e.obj->get("goalradius");
            const float radius = std::max(rv.isNumber() ? rv.asFloat() : 0.0f, 16.0f);
            const float dist = std::sqrt((g[0] - o[0]) * (g[0] - o[0]) + (g[1] - o[1]) * (g[1] - o[1]));
            if (dist > radius) {
                if (e.animScript != "move" || e.pathGoal != g) {
                    e.path.clear();
                    const int a = nearestNode(o), b = nearestNode(g);
                    for (int n : paths_.findPath(a, b)) e.path.push_back(paths_.position(size_t(n)));
                    e.path.push_back(g);
                    e.pathIndex = 0;
                    e.pathGoal = g;
                    if (e.animScript != "move") startAnimScript(e, "move");
                }
                moveActor(e, 0.05f);
                continue;
            }
            if (e.animScript == "move") {
                e.path.clear();
                vm_.notify(e.obj, "goal");
                startAnimScript(e, "stop");
                continue;
            }
        }
        // With a live enemy and nowhere to go, fight (inferred: the engine runs "combat" for exposed AI).
        Entity* enemy = entityOf(e.obj->get("enemy"));
        if (enemy && isAlive(*enemy) && e.animScript == "stop") {
            startAnimScript(e, "combat");
            continue;
        }
        if (e.animScriptThread && vm_.isThreadAlive(e.animScriptThread)) continue;
        // A script's main may return while threads it started keep acting (e.g. combat starting a melee thread),
        // so give it time before re-running the state script (inferred).
        if (timeMs_ - e.animScriptStartMs < 1000) continue;
        if (e.animScript == "stop" && !(enemy && isAlive(*enemy))) continue; // stop ended on its own; do not spin
        startAnimScript(e, enemy && isAlive(*enemy) ? "combat" : "stop");
    }
}

void World::moveActor(Entity& e, float dt) {
    if (e.pathIndex >= e.path.size()) return;
    auto o = e.obj->get("origin").asVector();
    float speed = e.anim ? e.anim->moveSpeed() : 0.0f;
    if (speed < 1.0f) speed = 150.0f; // fallback while the move script has not picked an animation yet
    float budget = speed * dt;
    while (budget > 0 && e.pathIndex < e.path.size()) {
        const auto& t = e.path[e.pathIndex];
        const float dx = t[0] - o[0], dy = t[1] - o[1];
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d <= budget) {
            o[0] = t[0];
            o[1] = t[1];
            budget -= d;
            ++e.pathIndex;
        } else {
            o[0] += dx / d * budget;
            o[1] += dy / d * budget;
            const float yaw = std::atan2(dy, dx) * 180.0f / float(M_PI);
            e.obj->set("angles", Value(Vec3{0, yaw, 0}));
            e.obj->set("$motionyaw", Value(yaw));
            budget = 0;
        }
    }
    // Stand on the ground.
    const TraceResult tr = collision_->trace({o[0], o[1], o[2] + 18.0f}, {o[0], o[1], o[2] - 64.0f}, {-15, -15, 0}, {15, 15, 70}, Contents::PlayerSolid);
    if (!tr.startSolid && tr.fraction < 1.0f) o[2] = tr.endPos[2];
    e.obj->set("origin", Value(o));
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

// Fires "trigger" on triggers the player is touching (inferred behaviour: every server frame while touching;
// trigger_once is removed after it fires). Player box: (-15,-15,0)-(15,15,70) around the origin.
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
            e->classname == "trigger_use_touch")
            continue;
        if (e->obj->get("$disabled").truthy()) continue;
        bool touching = false;
        if (e->classname == "trigger_radius") {
            const Vec3 o = e->obj->get("origin").asVector();
            const float r = e->obj->get("radius").isDefined() ? e->obj->get("radius").asFloat() : 0.0f;
            const float h = e->obj->get("height").isDefined() ? e->obj->get("height").asFloat() : 0.0f;
            const float dx = std::max({o[0] - pmax[0], pmin[0] - o[0], 0.0f}), dy = std::max({o[1] - pmax[1], pmin[1] - o[1], 0.0f});
            touching = dx * dx + dy * dy <= r * r && pmax[2] >= o[2] && pmin[2] <= o[2] + h;
        } else {
            Vec3 mn, mx;
            const Value m = e->obj->get("model");
            if (!m.isString() || !submodelBounds(m.asString(), mn, mx)) continue;
            touching = pmin[0] <= mx[0] && pmax[0] >= mn[0] && pmin[1] <= mx[1] && pmax[1] >= mn[1] && pmin[2] <= mx[2] && pmax[2] >= mn[2];
        }
        if (touching) fired.push_back(e.get());
    }
    for (Entity* e : fired) {
        if (e->obj->removed) continue;
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
    vm_.registerBuiltin("getaiarray", [this](CallContext& c) {
        std::vector<ObjectPtr> out;
        for (const auto& e : entities_)
            if (!e->obj->removed && e->obj->kind == "actor" && (!c.arg(0).isDefined() || e->obj->get("team") == c.arg(0)))
                out.push_back(e->obj);
        return Value(toArray(out));
    });

    auto doSpawn = [this](CallContext& c) -> Value {
        Entity* sp = entityOf(c.self);
        if (!sp || !sp->isSpawner) throw std::runtime_error("dospawn can only be called on actor spawners");
        Entity* a = spawnActor(*sp);
        return a ? Value(a->obj) : Value();
    };
    vm_.registerBuiltin("dospawn", doSpawn);
    vm_.registerBuiltin("stalingradspawn", doSpawn);
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
    // AI goals are stored for the movement code; the actor notifies "goal" when it arrives (not implemented yet).
    vm_.registerBuiltin("setgoalnode", [](CallContext& c) {
        auto o = c.self.asObject();
        if (c.arg(0).isObject()) o->set("$goalpos", c.arg(0).asObject()->get("origin"));
        return Value();
    });
    vm_.registerBuiltin("setgoalpos", [](CallContext& c) { c.self.asObject()->set("$goalpos", c.arg(0)); return Value(); });
    vm_.registerBuiltin("setgoalentity", [](CallContext& c) { c.self.asObject()->set("$goalent", c.arg(0)); return Value(); });
    vm_.registerBuiltin("setgoalvolume", [](CallContext& c) { c.self.asObject()->set("$goalvolume", c.arg(0)); return Value(); });
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
        e.obj->set("health", Value(int32_t(1000)));
        return Value(e.obj);
    });
    // Load-time hints and profiling markers have no effect on the simulation.
    for (const char* noop : {"precachemodel", "precacheshader", "precacheitem", "precacheturret", "precacheshellshock",
                             "precacherumble", "precachestring", "precachevehicle", "precachemenu", "prof_begin", "prof_end"})
        vm_.registerBuiltin(noop, [](CallContext&) { return Value(); });
    // setviewmodel(model): the player's first-person hands model (read by the front end).
    vm_.registerBuiltin("setviewmodel", [](CallContext& c) {
        if (c.self.isObject()) c.self.asObject()->set("$viewmodel", c.arg(0));
        return Value();
    });
    vm_.registerBuiltin("getwheelsurface", [](CallContext&) { return Value("dirt"); }); // inferred default
    vm_.registerBuiltin("clearenemy", [](CallContext& c) {
        if (c.self.isObject()) c.self.asObject()->set("enemy", Value());
        return Value();
    });
    vm_.registerBuiltin("delete", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("delete on a non-entity");
        if (e->isPlayer) throw std::runtime_error("cannot delete the player");
        e->obj->removed = true;
        vm_.notify(e->obj, "death");
        return Value();
    });
    vm_.registerBuiltin("setmodel", [](CallContext& c) {
        c.self.asObject()->set("model", c.arg(0));
        return Value();
    });
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
    vm_.registerBuiltin("setcvar", [this](CallContext& c) { cvars[c.arg(0).toString()] = c.arg(1).toString(); return Value(); });
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
        fxEvents_.push_back({path, at, Vec3{0, 0, 1}});
        return Value();
    });
    vm_.registerBuiltin("playloopedfx", [this, fxPath](CallContext& c) {
        if (fxPath(c.arg(0)).empty() || !c.arg(2).isVector()) return Value();
        loopedFx_.push_back({c.arg(0).asInt(), std::max(0.05f, c.arg(1).asFloat()), c.arg(2).asVector(), timeMs_});
        return Value();
    });
    vm_.registerBuiltin("print", [this](CallContext& c) { print(c.arg(0).toString()); return Value(); });
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
