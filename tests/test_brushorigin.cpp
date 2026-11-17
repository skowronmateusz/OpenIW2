// Brush-model positions on every shipped map (docs/spec/entities.md "Brush-model positions"). For each map entity with a "*N" model:
// - the `.origin` field is the map's "origin" key, or (0, 0, 0) without one (field getter 0x4f9d86: a plain vector at ent +0xdc);
// - `getorigin` is the centre of the linked box (0x4ee250), which lies inside that box; it is (0, 0, 0) only when the box holds that point (the eldaba objective class: an origin-less trigger placed by getorigin must not land on the map origin);
// - the loop-sound position is the origin plus the unrotated submodel centre (0x4aaf90), the box centre for an unrotated entity.
// One line per map: brush entities, origin-less ones, rotated ones, ones whose centre lies outside the world box. Needs the game files (IW2_MAIN); IW2_BRUSHMAPS=<map,...> limits the maps.
#include "fs/vfs.h"
#include "game/axis.h"
#include "game/world.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {
int failures = 0;
#define CHECK(cond, what)                                                                            \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond " (" << what << ")\n"; \
            ++failures;                                                                              \
        }                                                                                            \
    } while (0)

using iw2::script::Vec3;
bool same(const Vec3& a, const Vec3& b, float eps = 0.01f) {
    return std::fabs(a[0] - b[0]) <= eps && std::fabs(a[1] - b[1]) <= eps && std::fabs(a[2] - b[2]) <= eps;
}
bool inside(const Vec3& p, const Vec3& mn, const Vec3& mx, float slack = 0.01f) {
    for (size_t i = 0; i < 3; ++i)
        if (p[i] < mn[i] - slack || p[i] > mx[i] + slack) return false;
    return true;
}
Vec3 parseVec(const std::string* s) {
    Vec3 v{0, 0, 0};
    if (s) std::sscanf(s->c_str(), "%f %f %f", &v[0], &v[1], &v[2]);
    return v;
}
std::string fmt(const Vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.1f %.1f %.1f)", double(v[0]), double(v[1]), double(v[2]));
    return b;
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    std::vector<std::string> maps;
    for (const auto& p : vfs.list("maps/")) {
        if (p.size() < 13 || p.compare(p.size() - 7, 7, ".d3dbsp") != 0) continue;
        maps.push_back(p.substr(5, p.size() - 12)); // "mp/mp_x" for the MP maps
    }
    if (const char* only = std::getenv("IW2_BRUSHMAPS")) {
        maps.clear();
        std::stringstream ss(only);
        for (std::string m; std::getline(ss, m, ',');) maps.push_back(m);
    }
    if (!std::getenv("IW2_BRUSHMAPS")) CHECK(maps.size() >= 50, maps.size()); // 39 single-player + 15 multiplayer maps
    size_t total = 0, originless = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const std::string& path : maps) {
        const bool mp = path.rfind("mp/", 0) == 0;
        const std::string name = mp ? path.substr(3) : path;
        iw2::game::World w(vfs, name, mp);
        w.prepareStart();
        Vec3 wmin, wmax;
        CHECK(w.worldBounds(wmin, wmax), name);
        std::map<std::string, iw2::game::Entity*> byModel;
        for (const auto& e : w.entities()) {
            const auto m = e->obj->get("model");
            if (m.isString() && !m.asString().empty() && m.asString()[0] == '*') byModel[m.asString()] = e.get();
        }
        int n = 0, noKey = 0, rotated = 0, spawned = 0, outside = 0;
        for (const auto& be : w.bsp().entities()) {
            const std::string* model = be.get("model");
            if (!model || model->empty() || (*model)[0] != '*') continue;
            ++n;
            const std::string* cls = be.get("classname");
            const std::string what = name + " " + (cls ? *cls : "?") + " " + *model;
            const std::string* key = be.get("origin");
            if (!key) ++noKey;
            const auto it = byModel.find(*model);
            if (it == byModel.end()) continue; // not an entity in the world (e.g. consumed at spawn)
            ++spawned;
            const auto& o = it->second->obj;
            const auto ov = o->get("origin");
            CHECK(ov.isVector(), what);
            if (!ov.isVector()) continue;
            const Vec3 org = ov.asVector();
            // the origin field is the map key, or zero without one
            CHECK(same(org, parseVec(key)), what + " origin " + fmt(org));
            Vec3 mn, mx;
            CHECK(w.submodelBounds(*model, mn, mx), what);
            const auto av = o->get("angles");
            const Vec3 ang = av.isVector() ? av.asVector() : Vec3{0, 0, 0};
            if (ang[0] != 0 || ang[1] != 0 || ang[2] != 0) ++rotated;
            Vec3 bmn = mn, bmx = mx;
            iw2::game::brushLinkBounds(ang, bmn, bmx);
            for (size_t i = 0; i < 3; ++i) bmn[i] += org[i], bmx[i] += org[i];
            const Vec3 centre{(bmn[0] + bmx[0]) * 0.5f, (bmn[1] + bmx[1]) * 0.5f, (bmn[2] + bmx[2]) * 0.5f};
            // getorigin: the box centre, inside the box and the world; zero only when the box holds the map origin
            const auto g = w.scriptGetOrigin(o);
            CHECK(g.isVector() && same(g.asVector(), centre, 0.05f), what + " getorigin vs " + fmt(centre));
            if (!g.isVector()) continue;
            CHECK(inside(g.asVector(), bmn, bmx), what);
            // Outside the world box is map data, not a fault: vehicle collision templates (script_vehicle_collmap) are built around the
            // map origin, and demolition's two plane_trigger brushes lie beyond the world. Counted only.
            if (!inside(g.asVector(), wmin, wmax, 1.0f)) ++outside;
            if (same(g.asVector(), Vec3{0, 0, 0}, 0.0f)) CHECK(inside(Vec3{0, 0, 0}, bmn, bmx), what + " getorigin at the map origin");
            // loop sound: origin + the unrotated submodel centre; the box centre when not rotated
            const auto l = w.loopSoundOrigin(o);
            CHECK(l.has_value(), what);
            if (!l) continue;
            const Vec3 local{org[0] + (mn[0] + mx[0]) * 0.5f, org[1] + (mn[1] + mx[1]) * 0.5f, org[2] + (mn[2] + mx[2]) * 0.5f};
            CHECK(same(*l, local, 0.05f), what + " loop " + fmt(*l));
            if (!key) CHECK(inside(*l, bmn, bmx, 0.05f), what + " origin-less loop outside its box " + fmt(*l)); // no origin brush: never rotated
            if (ang[0] == 0 && ang[1] == 0 && ang[2] == 0) CHECK(same(*l, centre, 0.05f), what);
        }
        total += size_t(n);
        originless += size_t(noKey);
        std::cout << name << ": " << n << " brush entities (" << spawned << " spawned), " << noKey << " without an origin key, " << rotated
                  << " rotated, " << outside << " outside the world box\n";
    }
    // eldaba: the documents trigger has no origin key; its origin field is zero, getorigin and the loop sound position are its centre
    if (std::find(maps.begin(), maps.end(), "eldaba") != maps.end()) {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / ("iw2_brushorigin_" + std::to_string(::getpid()));
        fs::create_directories(dir / "maps");
        std::ofstream(dir / "maps" / "zz_brushorigin.gsc") << "loop()\n{\n\te = getent(\"documents\", \"targetname\");\n"
                                                              "\te playloopsound(\"bigfire\");\n}\n";
        iw2::fs::Vfs vfs2;
        vfs2.mountDirectory(main);
        vfs2.mountDirectory(dir.string());
        iw2::game::World w(vfs2, "eldaba");
        w.prepareStart();
        iw2::game::Entity* doc = nullptr;
        for (const auto& e : w.entities())
            if (e->obj->get("targetname").toString() == "documents" && e->classname == "trigger_use") doc = e.get();
        CHECK(doc != nullptr, "eldaba documents");
        if (doc) {
            CHECK(same(doc->obj->get("origin").asVector(), Vec3{0, 0, 0}, 0.0f), "eldaba documents origin field");
            const Vec3 g = w.scriptGetOrigin(doc->obj).asVector();
            const auto l = w.loopSoundOrigin(doc->obj);
            CHECK(std::fabs(g[0]) + std::fabs(g[1]) + std::fabs(g[2]) > 1.0f, "eldaba documents getorigin");
            CHECK(l && same(*l, g, 0.05f), "eldaba documents loop position");
            // the built-in itself hands that position to the sound output
            std::optional<Vec3> played;
            w.playSound = [&](const std::string&, const Vec3* at, float*, int) {
                if (at) played = *at;
                return 1;
            };
            w.vm().start("maps/zz_brushorigin.gsc", "loop", {}, iw2::script::Value(w.vm().level()));
            w.vm().drainReady();
            CHECK(played && same(*played, g, 0.05f), "eldaba documents playloopsound position");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::cout << total << " brush entities on " << maps.size() << " maps, " << originless << " without an origin key, "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s\n";
    if (failures) return 1;
    std::cout << "ok\n";
    return 0;
}
