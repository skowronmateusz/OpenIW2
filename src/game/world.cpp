#include "game/world.h"

#include "script/builtins_core.h"
#include "script/parser.h"

#include <cstdio>
#include <cstdlib>
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
    : vfs_(vfs), mapName_(std::move(mapName)), vm_([this](const std::string& p) -> std::optional<std::string> {
          auto d = vfs_.read(p);
          if (!d) return std::nullopt;
          return std::string(d->begin(), d->end());
      }) {
    auto d = vfs_.read("maps/" + mapName_ + ".d3dbsp");
    if (!d) throw std::runtime_error("map not found: " + mapName_);
    bspData_ = std::move(*d);
    bsp_ = std::make_unique<assets::D3dBsp>(bspData_);
    vm_.print = [this](const std::string& s) { print(s); };
    registerCoreBuiltins(vm_);
    registerBuiltins();
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
    const ObjectPtr* playerStart = nullptr;
    std::vector<ObjectPtr> starts;
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
        if (startsWith(cls, "node_")) {
            PathNode n;
            n.obj = vm_.newObject(cls);
            n.type = cls;
            fill(n.obj);
            nodes_.push_back(std::move(n));
        } else if (cls == "info_vehicle_node") {
            auto o = vm_.newObject(cls);
            fill(o);
            vehicleNodes_.push_back(o);
        } else if (startsWith(cls, "actor_")) {
            Entity& e = spawnEntity(cls);
            e.isSpawner = true;
            fill(e.obj);
            e.obj->set("count", e.obj->get("count").isDefined() ? e.obj->get("count") : Value(int32_t(1)));
        } else if (kEntityClasses.count(cls)) {
            Entity& e = spawnEntity(cls);
            fill(e.obj);
            if (cls == "info_player_start") starts.push_back(e.obj);
        }
    }
    if (!starts.empty()) playerStart = &starts.front();

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
}

void World::start() {
    spawnMapEntities();
    vm_.level()->set("script", Value(mapName_));
    vm_.start("maps/" + mapName_ + ".gsc", "main");
}

void World::frame() {
    timeMs_ += 50;
    vm_.runFrame(timeMs_);
}

void World::registerBuiltins() {
    auto findEntities = [this](const Value& value, const Value& key) {
        if (!key.isDefined()) throw std::runtime_error("getent/getentarray need a key");
        const std::string k = lower(key.toString());
        std::vector<ObjectPtr> out;
        for (const auto& e : entities_)
            if (!e->obj->removed && !e->isSpawner && e->obj->get(k) == value) out.push_back(e->obj);
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

    vm_.registerBuiltin("spawn", [this](CallContext& c) {
        Entity& e = spawnEntity(c.arg(0).toString());
        if (c.arg(1).isDefined()) e.obj->set("origin", c.arg(1));
        if (c.arg(2).isDefined()) e.obj->set("spawnflags", c.arg(2));
        return Value(e.obj);
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
    vm_.registerBuiltin("loadfx", [this](CallContext&) { return Value(nextFx_++); });
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
