// Multiplayer flow (docs/spec/mp.md "End of the map", docs/spec/mp-ui.md): map_rotate, the MP registered defaults, the
// create-server panel's weapon classes through the game's class API, and pers after a disconnect of client 0.
// The world part needs the game files (IW2_MAIN).

#include "game/maprotation.h"
#include "game/world.h"
#include "ui/mpserver.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::MapRotateResult;
using iw2::script::Value;

void rotation() {
    std::map<std::string, std::string> cv;
    auto get = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    auto set = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    // No rotation: forced restart of the current map.
    MapRotateResult r = iw2::game::mapRotate(get, set);
    CHECK(r.kind == MapRotateResult::Kind::Restart);
    CHECK(r.console.back().rfind("No map specified in sv_mapRotation", 0) == 0);
    // gametype / map pairs are taken in order; the rest stays in sv_mapRotationCurrent, then it starts over.
    cv["g_gametype"] = "dm";
    cv["sv_mapRotation"] = "gametype tdm map mp_carentan  MAP \"mp_toujane\" bogus gametype dm map mp_dawnville";
    r = iw2::game::mapRotate(get, set);
    CHECK(r.kind == MapRotateResult::Kind::Map && r.map == "mp_carentan" && cv["g_gametype"] == "tdm" && r.gametypeChanged);
    CHECK(cv["sv_mapRotationCurrent"].find("mp_toujane") != std::string::npos);
    r = iw2::game::mapRotate(get, set);
    CHECK(r.kind == MapRotateResult::Kind::Map && r.map == "mp_toujane" && cv["g_gametype"] == "tdm" && !r.gametypeChanged);
    r = iw2::game::mapRotate(get, set);
    CHECK(r.kind == MapRotateResult::Kind::Map && r.map == "mp_dawnville" && cv["g_gametype"] == "dm");
    bool unknown = false;
    for (const auto& l : r.console) unknown = unknown || l == "Unknown keyword 'bogus' in sv_mapRotation.\n";
    CHECK(unknown);
    CHECK(cv["sv_mapRotationCurrent"].empty());
    r = iw2::game::mapRotate(get, set); // used up: from the start again
    CHECK(r.map == "mp_carentan");
    // A keyword without its value forces the restart.
    cv["sv_mapRotation"] = "gametype dm map";
    cv["sv_mapRotationCurrent"] = "";
    r = iw2::game::mapRotate(get, set);
    CHECK(r.kind == MapRotateResult::Kind::Restart);
    CHECK(r.console.back().rfind("No map specified after 'map' keyword", 0) == 0);
}

void defaults() {
    CHECK(iw2::ui::mpRegisteredDefault("sv_hostname") == "CoD2Host");
    CHECK(iw2::ui::mpRegisteredDefault("sv_maxclients") == "20");
    CHECK(iw2::ui::mpRegisteredDefault("sv_minping") == "0"); // the menu's lower-case name
    CHECK(iw2::ui::mpRegisteredDefault("sv_pure") == "1");
    CHECK(iw2::ui::mpRegisteredDefault("nosuchdvar").empty());
    // Extension texts: English and Polish (Windows-1250), every key in both.
    const auto en = iw2::ui::mpext::localizedStrings("english"), pl = iw2::ui::mpext::localizedStrings("polish");
    CHECK(en.size() == pl.size() && !en.empty());
    CHECK(en[0].second == "Bots & weapons" && pl[0].second == "Boty i bro\xf1");
}

void world(const char* main) {
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    // The panel's classes reach the game through iw2_weaponclasses: only snipers on, grenades off.
    std::map<std::string, std::string> cv;
    auto get = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    auto set = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    for (const auto& c : iw2::ui::mpext::weaponClasses())
        cv[std::string("ui_iw2_allow_") + c.id] = std::string(c.id) == "sniper" ? "1" : "0";
    iw2::ui::mpext::applyWeaponClasses(get, set);
    iw2::game::World w(vfs, "mp_toujane", true);
    for (const auto& [k, v] : cv) w.cvars[k] = v;
    w.mpStart("dm");
    for (const char* menu : {"weapon_american", "weapon_german"}) {
        const auto allowed = w.mpAllowedWeapons(menu);
        CHECK(allowed.size() == 1);
        for (const auto& wpn : allowed) CHECK(w.mpWeaponClass(wpn) == "sniper");
    }
    CHECK(w.cvars["scr_allow_fraggrenades"] == "0");
    // Disconnect of client 0: the next client 0 starts with an empty pers.
    const int c0 = w.mpConnect("local");
    w.mpBegin(c0);
    w.frame();
    std::get<iw2::script::ArrayPtr>(w.mpClientEntity(c0)->obj->get("pers").v)->set(iw2::script::ArrayKey{true, 0, "team"}, Value("allies"));
    w.mpDisconnect(c0);
    const int again = w.mpConnect("local2");
    CHECK(again == 0);
    const Value pers = w.mpClientEntity(again)->obj->get("pers");
    CHECK(pers.isArray() && std::get<iw2::script::ArrayPtr>(pers.v)->get(iw2::script::ArrayKey{true, 0, "team"}) == nullptr);
}

} // namespace

int main() {
    rotation();
    defaults();
    if (const char* main = std::getenv("IW2_MAIN")) world(main);
    else std::cout << "IW2_MAIN not set, world part skipped\n";
    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
