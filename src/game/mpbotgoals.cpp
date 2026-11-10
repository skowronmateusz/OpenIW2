// Multiplayer bot objectives (docs/spec/mp-bots.md, "Objectives"; an OpenIW2 extension): what a bot walks to in each
// gametype, read from the gametype script's own state (flags, bomb zones, radios), the goal API for C++ and GSC, and the
// weapon choice by the server's scr_allow_* cvars (docs/spec/mp-bots.md, "Weapon classes").
#include "game/mpbots.h"
#include "game/mpgame.h"
#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace iw2::game {

using script::CallContext;
using script::Value;

namespace {
Vec3 vecOf(const Value& v) { return v.isVector() ? v.asVector() : Vec3{0, 0, 0}; }


// Every weapon of the nations' weapon menus (ui_mp/scriptmenus/weapon_<nation>.menu responses): the server cvar that
// allows it (_weapons.gsc:93-205) and its weapon class. The class column is the class World::mpWeaponClass derives from
// the weapon file; a test checks the two agree for every row, so the table can validate a --weapons list before any
// map (and any weapon file) is loaded.
struct MenuWeapon {
    const char* weapon;
    const char* cvar;
    const char* cls;
};
const MenuWeapon kWeapons[] = {
    {"greasegun_mp", "scr_allow_greasegun", "smg"},
    {"m1carbine_mp", "scr_allow_m1carbine", "rifle"},
    {"m1garand_mp", "scr_allow_m1garand", "rifle"},
    {"springfield_mp", "scr_allow_springfield", "sniper"},
    {"thompson_mp", "scr_allow_thompson", "smg"},
    {"bar_mp", "scr_allow_bar", "mg"},
    {"sten_mp", "scr_allow_sten", "smg"},
    {"enfield_mp", "scr_allow_enfield", "bolt"},
    {"enfield_scope_mp", "scr_allow_enfieldsniper", "sniper"},
    {"bren_mp", "scr_allow_bren", "mg"},
    {"PPS42_mp", "scr_allow_pps42", "smg"},
    {"mosin_nagant_mp", "scr_allow_nagant", "bolt"},
    {"SVT40_mp", "scr_allow_svt40", "rifle"},
    {"mosin_nagant_sniper_mp", "scr_allow_nagantsniper", "sniper"},
    {"ppsh_mp", "scr_allow_ppsh", "smg"},
    {"mp40_mp", "scr_allow_mp40", "smg"},
    {"kar98k_mp", "scr_allow_kar98k", "bolt"},
    {"g43_mp", "scr_allow_g43", "rifle"},
    {"kar98k_sniper_mp", "scr_allow_kar98ksniper", "sniper"},
    {"mp44_mp", "scr_allow_mp44", "mg"},
    {"shotgun_mp", "scr_allow_shotgun", "shotgun"},
};
struct NationMenu {
    const char* menu;
    std::vector<const char*> weapons;
};
const NationMenu kMenus[] = {
    {"weapon_american", {"m1carbine_mp", "m1garand_mp", "thompson_mp", "bar_mp", "springfield_mp", "greasegun_mp", "shotgun_mp"}},
    {"weapon_british", {"enfield_mp", "sten_mp", "bren_mp", "m1garand_mp", "thompson_mp", "enfield_scope_mp", "shotgun_mp"}},
    {"weapon_russian", {"mosin_nagant_mp", "SVT40_mp", "PPS42_mp", "ppsh_mp", "mosin_nagant_sniper_mp", "shotgun_mp"}},
    {"weapon_german", {"kar98k_mp", "g43_mp", "mp40_mp", "mp44_mp", "kar98k_sniper_mp", "shotgun_mp"}},
};
} // namespace

const std::vector<std::string>& mpWeaponClassNames() {
    static const std::vector<std::string> names{"rifle", "bolt", "sniper", "smg", "mg", "shotgun"};
    return names;
}

const std::vector<std::string>& mpWeaponNations() {
    static const std::vector<std::string> nations{"american", "british", "russian", "german"};
    return nations;
}

std::string mpMenuWeaponClass(const std::string& weapon) {
    for (const MenuWeapon& w : kWeapons)
        if (weapon == w.weapon) return w.cls;
    return {};
}

std::string mpMenuPrimaryClasses(const std::string& nation) {
    std::string out;
    for (const std::string& w : mpMenuWeapons("weapon_" + nation)) {
        const std::string c = mpMenuWeaponClass(w);
        if (out.find(c) == std::string::npos) out += (out.empty() ? "" : ",") + c;
    }
    return out;
}

std::string mpParseWeaponList(const std::string& text, MpWeaponSelection& out) {
    const auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
        return s;
    };
    const std::string primaryNames = join(mpWeaponClassNames());
    std::vector<std::string> tokens;
    std::string cur;
    for (char ch : text + ",") {
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == ';') {
            if (!cur.empty()) tokens.push_back(cur);
            cur.clear();
        } else {
            cur += char(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    bool all = false, grenades = false, none = false;
    std::vector<std::string> primaries;
    for (const std::string& t : tokens) {
        if (t == "all") all = true;
        else if (t == "none") none = true;
        else if (t == "grenades") grenades = true;
        else if (std::find(mpWeaponClassNames().begin(), mpWeaponClassNames().end(), t) != mpWeaponClassNames().end()) primaries.push_back(t);
        else return "unknown weapon class '" + t + "' (valid: " + primaryNames + ", grenades, all)";
    }
    if (none && (all || grenades || !primaries.empty())) return "'none' cannot be combined with other weapon classes";
    if (all) {
        primaries = mpWeaponClassNames();
        grenades = true;
    }
    if (primaries.empty())
        return std::string(grenades ? "grenades alone leave a player no weapon to choose in the weapon menu"
                                    : "no weapon class given") +
               ": list at least one of " + primaryNames;
    // Canonical order, no duplicates.
    std::vector<std::string> ordered;
    for (const std::string& c : mpWeaponClassNames())
        if (std::find(primaries.begin(), primaries.end(), c) != primaries.end()) ordered.push_back(c);
    // Each nation's menu must still offer a weapon: a player of that nation could not otherwise pick one and join.
    for (const std::string& nation : mpWeaponNations()) {
        bool any = false;
        for (const std::string& w : mpMenuWeapons("weapon_" + nation))
            any = any || std::find(ordered.begin(), ordered.end(), mpMenuWeaponClass(w)) != ordered.end();
        if (!any)
            return "the " + nation + " weapon menu would have no weapon for the list '" + join(ordered) + "' (the " + nation +
                   " menu offers: " + mpMenuPrimaryClasses(nation) + ")";
    }
    out.primaries = ordered;
    out.grenades = grenades;
    out.classesCvar = join(ordered);
    out.classesCvar.erase(std::remove(out.classesCvar.begin(), out.classesCvar.end(), ' '), out.classesCvar.end());
    return {};
}

std::vector<std::string> mpMenuWeapons(const std::string& menu) {
    for (const NationMenu& m : kMenus)
        if (menu == m.menu) return {m.weapons.begin(), m.weapons.end()};
    return {};
}

std::string mpWeaponAllowCvar(const std::string& weapon) {
    for (const MenuWeapon& w : kWeapons)
        if (weapon == w.weapon) return w.cvar;
    return {};
}

// The class of a menu weapon from its weapon file: weaponClass "spread" is a shotgun, "smg" a submachine gun; a "rifle"
// with an ADS overlay (a scope) is a sniper rifle (every scoped rifle is also bolt-action, so this test comes first), one
// without semiAuto an automatic rifle or machine gun ("mg": BAR, Bren, MP44), one with boltAction a bolt-action rifle
// ("bolt"), the rest semi-automatic rifles ("rifle").
std::string World::mpWeaponClass(const std::string& weapon) {
    const std::string cls = weaponField(weapon, "weaponClass");
    if (cls == "spread") return "shotgun";
    if (cls == "smg") return "smg";
    if (cls != "rifle") return cls;
    if (!weaponField(weapon, "adsOverlayShader").empty()) return "sniper";
    if (weaponField(weapon, "semiAuto") == "0") return "mg";
    if (weaponField(weapon, "boltAction") == "1") return "bolt";
    return "rifle";
}

// Sets every menu weapon's scr_allow_* cvar from the class list (the classes allowed, "rifle,smg,..."); the grenade
// cvars stay as they are. Read by _weapons.gsc at game start and every 5 s (_weapons.gsc:207-222).
void World::mpSetAllowedWeaponClasses(const std::vector<std::string>& classes) {
    for (const MenuWeapon& w : kWeapons) {
        const bool on = std::find(classes.begin(), classes.end(), mpWeaponClass(w.weapon)) != classes.end();
        cvars[w.cvar] = on ? "1" : "0";
    }
}

std::vector<std::string> World::mpAllowedWeapons(const std::string& menu) {
    std::vector<std::string> out;
    for (const std::string& w : mpMenuWeapons(menu)) {
        const auto it = cvars.find(mpWeaponAllowCvar(w));
        if (it == cvars.end() || it->second.empty() || std::atoi(it->second.c_str()) != 0) out.push_back(w);
    }
    return out;
}

// The bot's choice on a weapon menu: an allowed weapon of the bot's class preference (bot_weaponclass, a class name
// or empty for any), picked at random among them; the menu's first allowed weapon when none of the preferred class is.
std::string World::mpBotPickWeapon(const MpBot& b, const std::string& menu) {
    const std::vector<std::string> allowed = mpAllowedWeapons(menu);
    if (allowed.empty()) return mpMenuWeapons(menu).empty() ? "thompson_mp" : mpMenuWeapons(menu)[0];
    std::string pref = b.weaponClass;
    if (pref.empty()) {
        const auto it = cvars.find("bot_weaponclass");
        if (it != cvars.end()) pref = it->second;
    }
    std::vector<std::string> pick;
    for (const std::string& w : allowed)
        if (pref.empty() || pref == "any" || mpWeaponClass(w) == pref) pick.push_back(w);
    if (pick.empty()) pick = allowed;
    return pick[size_t(nextRand()) % pick.size()];
}

// An entity's centre: a brush model's bounds centre moved by its origin, else the origin.
Vec3 World::mpEntityCenter(const Entity& e) const {
    const Vec3 o = vecOf(e.obj->get("origin"));
    const Value m = e.obj->get("model");
    if (m.isString() && m.asString().size() > 1 && m.asString()[0] == '*') {
        const size_t i = size_t(std::atoi(m.asString().c_str() + 1));
        if (i < submodels_.size()) {
            const auto& [mn, mx] = submodels_[i];
            return {o[0] + (mn[0] + mx[0]) * 0.5f, o[1] + (mn[1] + mx[1]) * 0.5f, o[2] + mn[2]};
        }
    }
    return o;
}

// What the bot should do now: the goal set through the API, else the gametype's objective for the bot's role.
BotObjective World::mpBotObjective(const MpBot& b, Entity& self) {
    BotObjective o;
    if (b.goal.active) {
        o.valid = true;
        o.pos = b.goal.pos;
        if (b.goal.entity >= 0) {
            Entity* t = entityByNumber(b.goal.entity);
            if (!t || t->obj->removed) return BotObjective{};
            o.pos = mpEntityCenter(*t);
            if (b.goal.use) o.useTarget = t;
        }
        o.radius = b.goal.radius;
        o.use = b.goal.use;
        o.what = "goal";
        return o;
    }
    const std::string gt = mp_->gametype;
    const std::string team = self.obj->get("sessionteam").toString();
    if (team != "allies" && team != "axis") return o;
    const std::string enemyTeam = team == "allies" ? "axis" : "allies";
    // The bot's rank among the bots of its team (by client number) splits the roles.
    int rank = 0;
    for (const MpBot& ob : bots_->bots) {
        if (ob.client >= b.client) continue;
        Entity* oe = mp_->clients[size_t(ob.client)].ent;
        if (oe && oe->obj->get("sessionteam").toString() == team) ++rank;
    }
    std::string role = b.role;
    if (role == "auto" || role.empty()) role = rank % 2 == 0 ? "attack" : "defend";
    if (role == "hunt") return o;
    auto byTarget = [this](const std::string& name) {
        std::vector<Entity*> out;
        for (const auto& e : entities_)
            if (!e->obj->removed && e->obj->get("targetname").toString() == name) out.push_back(e.get());
        return out;
    };
    auto carrierOf = [this](const script::ObjectPtr& flag) -> Entity* {
        for (const MpClient& cl : mp_->clients)
            if (cl.ent && cl.state != MpClientState::Free && cl.ent->obj->get("flag") == Value(flag)) return cl.ent;
        return nullptr;
    };

    if (gt == "ctf") {
        // ctf.gsc:1051-1084: the flag triggers (targetname allied_flag / axis_flag) keep home_origin and atbase; a carrier
        // has the flag in its `flag` field (ctf.gsc:1178); touching the own flag at its base with the enemy flag captures
        // it, touching a dropped own flag returns it (ctf.gsc:1093-1160).
        auto own = byTarget(team == "allies" ? "allied_flag" : "axis_flag");
        auto enemy = byTarget(team == "allies" ? "axis_flag" : "allied_flag");
        if (own.empty() || enemy.empty()) return o;
        Entity* ownFlag = own[0];
        Entity* enemyFlag = enemy[0];
        o.valid = true;
        if (self.obj->get("flag").isDefined()) {
            o.pos = vecOf(ownFlag->obj->get("home_origin"));
            o.radius = 8;
            o.what = "capture";
            return o;
        }
        if (role == "attack") {
            if (Entity* c = carrierOf(enemyFlag->obj)) {
                o.pos = vecOf(c->obj->get("origin"));
                o.radius = 200;
                o.what = "escort";
            } else {
                o.pos = vecOf(enemyFlag->obj->get("origin"));
                o.radius = 8;
                o.what = "take flag";
            }
            return o;
        }
        if (ownFlag->obj->get("atbase").truthy()) {
            o.pos = vecOf(ownFlag->obj->get("home_origin"));
            o.radius = 350;
            o.what = "guard flag";
        } else if (Entity* c = carrierOf(ownFlag->obj)) {
            o.pos = vecOf(c->obj->get("origin"));
            o.radius = 0;
            o.what = "chase carrier";
        } else {
            o.pos = vecOf(ownFlag->obj->get("origin"));
            o.radius = 8;
            o.what = "return flag";
        }
        return o;
    }
    if (gt == "sd") {
        // sd.gsc:1433-1545: the bomb zones (targetname bombzone, trigger_use_touch for game["attackers"]) are planted by
        // holding use inside (sd.gsc:1629-1760); then the bomb trigger (targetname bombtrigger) is defused the same way
        // by the defenders; level.bombplanted tells which.
        const bool attacker = vm_.game()->get("attackers").toString() == team;
        const bool planted = vm_.level()->get("bombplanted").truthy();
        auto zones = byTarget("bombzone");
        auto bomb = byTarget("bombtrigger");
        if (planted && !bomb.empty()) {
            o.valid = true;
            o.pos = mpEntityCenter(*bomb[0]);
            if (attacker) {
                o.radius = 300;
                o.what = "guard bomb";
            } else {
                o.radius = 0;
                o.use = true;
                o.useTarget = bomb[0];
                o.what = "defuse";
            }
            return o;
        }
        if (zones.empty()) return o;
        Entity* zone = zones[size_t(rank) % zones.size()];
        o.valid = true;
        o.pos = mpEntityCenter(*zone);
        if (attacker && role == "attack") {
            o.radius = 0;
            o.use = true;
            o.useTarget = zone;
            o.what = "plant";
        } else {
            o.radius = attacker ? 600 : 400;
            o.what = attacker ? "push zone" : "guard zone";
        }
        return o;
    }
    if (gt == "hq") {
        // hq.gsc:1096-1130, 1244-1262: level.radio holds the radios; the shown one (hidden false) is captured or held by
        // standing within its radius and level.zradioradius in height.
        const Value radios = vm_.level()->get("radio");
        if (!radios.isArray()) return o;
        for (const auto& [k, v] : std::get<script::ArrayPtr>(radios.v)->items) {
            if (!v.isObject() || v.asObject()->get("hidden").truthy()) continue;
            const auto r = v.asObject();
            o.valid = true;
            o.pos = vecOf(r->get("origin"));
            const float radius = r->get("radius").isNumber() ? r->get("radius").asFloat() : 120.0f;
            o.radius = radius * 0.6f;
            o.what = r->get("team").toString() == team ? "hold radio" : "take radio";
            if (role == "defend" && r->get("team").toString() == team) o.radius = radius * 2.0f;
            return o;
        }
        return o;
    }
    (void)enemyTeam;
    return o; // dm, tdm: hunt
}

void World::mpBotSetGoal(int client, const Vec3& pos, float radius, bool use) {
    if (!bots_) return;
    for (MpBot& b : bots_->bots)
        if (b.client == client) {
            b.goal = BotGoal{true, pos, -1, radius, use};
            b.path.clear();
        }
}

void World::mpBotSetGoalEntity(int client, int entity, float radius, bool use) {
    if (!bots_) return;
    for (MpBot& b : bots_->bots)
        if (b.client == client) {
            b.goal = BotGoal{true, {}, entity, radius, use};
            b.path.clear();
        }
}

void World::mpBotClearGoal(int client) {
    if (!bots_) return;
    for (MpBot& b : bots_->bots)
        if (b.client == client) {
            b.goal = BotGoal{};
            b.path.clear();
        }
}

void World::mpBotSetRole(int client, const std::string& role) {
    if (!bots_) return;
    if (role != "auto" && role != "attack" && role != "defend" && role != "hunt")
        throw std::runtime_error("bot role must be auto, attack, defend or hunt");
    for (MpBot& b : bots_->bots)
        if (b.client == client) b.role = role;
}

void World::mpBotSetWeaponClass(int client, const std::string& cls) {
    if (!bots_) return;
    for (MpBot& b : bots_->bots)
        if (b.client == client) b.weaponClass = cls;
}

std::string World::mpBotObjectiveName(int client) {
    if (!bots_) return {};
    for (const MpBot& b : bots_->bots)
        if (b.client == client) return b.objective;
    return {};
}

// GSC: player isBot(); bot botSetGoal(origin | entity, radius [, holdUse]); bot botClearGoal();
// bot botSetRole("auto" | "attack" | "defend" | "hunt"); bot botSetWeaponClass(class or "").
void World::registerBotGoalBuiltins() {
    auto botClient = [this](CallContext& c) -> int {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0 || !mpIsBot(cl)) throw std::runtime_error("not a bot");
        return cl;
    };
    vm_.registerBuiltin("isbot", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        return Value(int32_t(cl >= 0 && mpIsBot(cl) ? 1 : 0));
    });
    vm_.registerBuiltin("botsetgoal", [this, botClient](CallContext& c) {
        const int cl = botClient(c);
        const float radius = c.argc() > 1 && c.arg(1).isNumber() ? c.arg(1).asFloat() : 64.0f;
        const bool use = c.argc() > 2 && c.arg(2).truthy();
        if (Entity* t = entityOf(c.arg(0))) mpBotSetGoalEntity(cl, t->number, radius, use);
        else if (c.arg(0).isVector()) mpBotSetGoal(cl, c.arg(0).asVector(), radius, use);
        else throw std::runtime_error("botsetgoal: an origin or an entity");
        return Value();
    });
    vm_.registerBuiltin("botcleargoal", [this, botClient](CallContext& c) {
        mpBotClearGoal(botClient(c));
        return Value();
    });
    vm_.registerBuiltin("botsetrole", [this, botClient](CallContext& c) {
        mpBotSetRole(botClient(c), c.arg(0).toString());
        return Value();
    });
    vm_.registerBuiltin("botsetweaponclass", [this, botClient](CallContext& c) {
        mpBotSetWeaponClass(botClient(c), c.arg(0).toString());
        return Value();
    });
}

} // namespace iw2::game
