// Multiplayer HUD feedback, implemented from the spec in docs/spec/mp-hud.md. Server VAs are VAs in the 1.3 Linux dedicated
// server; client VAs are VAs in the patch 1.3 MP executable.
#include "game/mphud.h"
#include "game/world.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace iw2::game {

using script::CallContext;
using script::Value;

namespace {
const char* const kMods[15] = {"MOD_UNKNOWN",  "MOD_PISTOL_BULLET", "MOD_RIFLE_BULLET", "MOD_GRENADE",       "MOD_GRENADE_SPLASH",
                               "MOD_PROJECTILE", "MOD_PROJECTILE_SPLASH", "MOD_MELEE",   "MOD_HEAD_SHOT",     "MOD_CRUSH",
                               "MOD_TELEFRAG", "MOD_FALLING",       "MOD_SUICIDE",      "MOD_TRIGGER_HURT", "MOD_EXPLOSIVE"};

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string name31(const std::string& s) { return s.substr(0, 31); }
} // namespace

int mpModIndex(const std::string& mod) {
    for (int i = 0; i < 15; ++i)
        if (mod == kMods[i]) return i;
    return 0;
}

MpObituary mpMakeObituary(int victim, int attacker, const std::string& weapon, const std::string& mod) {
    MpObituary ev;
    ev.victim = victim;
    ev.attacker = attacker;
    const int m = mpModIndex(mod);
    if (m == 7 || m == 8 || m == 12 || m == 11 || m == 9) {
        ev.modByte = true;
        ev.mod = m;
    } else {
        ev.weapon = weapon;
    }
    return ev;
}

ObituaryLine obituaryLine(const MpObituary& ev, const KillIconInfo& weapon, const std::string& victimName, int victimTeam,
                          const std::string& attackerName, int attackerTeam, int localClient) {
    ObituaryLine l;
    // Icon (0x4e0418..0x4e04e8): the weapon's killIcon (empty -> killicondied), wide 2.8 x 1.4, flip; a means-of-death byte
    // selects a fixed 1.4 x 1.4 icon.
    int mod = 0;
    if (ev.modByte) {
        mod = ev.mod;
        l.icon = "killicondied";
    } else if (weapon.killIcon.empty()) {
        l.icon = "killicondied";
    } else {
        l.icon = weapon.killIcon;
        if (weapon.wide) l.iconW = 2.8f;
        l.flip = weapon.flip;
    }
    switch (mod) {
    case 7: l.icon = "killiconmelee"; break;
    case 8: l.icon = "killiconheadshot"; break;
    case 9: l.icon = "killiconcrush"; break;
    case 11: l.icon = "killiconfalling"; break;
    case 12: l.icon = "killiconsuicide"; break;
    case 13: l.icon = "killicondied"; break;
    default: break;
    }
    if (mod != 0) l.iconW = 1.4f, l.flip = false;
    l.victim = name31(victimName) + "^7";
    l.victimTeam = victimTeam;
    const bool hasAttacker = ev.attacker >= 0 && ev.attacker != ev.victim;
    if (hasAttacker) {
        l.attacker = name31(attackerName) + "^7";
        l.attackerTeam = attackerTeam;
        l.teammate = attackerTeam != 0 && attackerTeam == victimTeam;
        if (ev.attacker == localClient) {
            l.centerPrint = "CGAME_YOUKILLED";
            l.centerName = victimName.substr(0, 31) + "^7";
        } else if (ev.victim == localClient) {
            l.centerPrint = "CGAME_YOUWEREKILLED";
            l.centerName = name31(attackerName);
        }
    }
    if (!l.centerPrint.empty())
        l.centerText = l.teammate ? l.centerPrint + "\x15^1&&2^7 " + l.centerName + "\x14" + "CGAME_TEAMMATE" : l.centerPrint + "\x15" + l.centerName;
    return l;
}

std::array<float, 4> mpTeamColor(int team, const std::string& axisDvar, const std::string& alliesDvar) {
    std::array<float, 4> c{1, 1, 1, 1};
    if (team != 1 && team != 2) return c;
    c = team == 1 ? std::array<float, 4>{1.0f, 0.5f, 0.5f, 1.0f} : std::array<float, 4>{0.5f, 0.5f, 1.0f, 1.0f};
    const std::string& dv = team == 1 ? axisDvar : alliesDvar;
    if (!dv.empty()) {
        std::istringstream in(dv);
        std::array<float, 4> v{};
        int n = 0;
        while (n < 4 && in >> v[size_t(n)]) ++n;
        if (n >= 3) {
            c[0] = v[0], c[1] = v[1], c[2] = v[2];
            if (n == 4) c[3] = v[3];
        }
    }
    c[3] = 1.0f; // 0x4d9e50 keeps the caller's alpha (the line starts from 1)
    return c;
}

int World::mpTeamOf(const Entity* e) const {
    if (!e) return 0;
    return mpTeamNumber(e->obj->get("sessionteam").toString());
}

int World::mpLocalTeam() const {
    if (!mp_ || mp_->localClient < 0) return 0;
    const MpClient& cl = mp_->clients[size_t(mp_->localClient)];
    return cl.ent ? mpTeamOf(cl.ent) : 0;
}

std::vector<MpObituary> World::takeMpObituaries() {
    std::vector<MpObituary> out;
    if (mp_) out.swap(mp_->obituaries);
    return out;
}

ObituaryLine World::mpObituaryLine(const MpObituary& ev) {
    KillIconInfo info;
    if (!ev.modByte && !ev.weapon.empty()) {
        info.killIcon = weaponField(ev.weapon, "killIcon");
        info.wide = std::atoi(weaponField(ev.weapon, "wideKillIcon").c_str()) != 0;
        info.flip = std::atoi(weaponField(ev.weapon, "flipKillIcon").c_str()) != 0;
    }
    Entity* v = mpClientEntity(ev.victim);
    Entity* a = ev.attacker >= 0 ? mpClientEntity(ev.attacker) : nullptr;
    if (ev.hasClientInfo)
        return obituaryLine(ev, info, ev.victimName, ev.victimTeam, ev.attackerName, ev.attackerTeam, mp_->localClient);
    const auto nameOf = [this](int cl) { return mp_->clients[size_t(cl)].name; };
    return obituaryLine(ev, info, v ? nameOf(ev.victim) : std::string(), mpTeamOf(v), a ? nameOf(ev.attacker) : std::string(), mpTeamOf(a),
                        mp_->localClient);
}

// Script head icons for the local view (0x4dbcc0 / 0x4dbba0): headicon + headiconteam on every active client but the
// local one (first person), at origin z + 82, size cg_scriptIconSize + 10. TODO(spec): the head tag position (+21), the
// voice / connection / killcam icons.
std::vector<World::MpHeadIcon> World::mpHeadIcons() const {
    std::vector<MpHeadIcon> out;
    if (!mp_) return out;
    const int localTeam = mpLocalTeam();
    const auto it = cvars.find("cg_scriptIconSize");
    const float size = (it != cvars.end() && !it->second.empty() ? float(std::atof(it->second.c_str())) : 0.0f) + 10.0f;
    for (size_t i = 0; i < mp_->clients.size(); ++i) {
        const MpClient& cl = mp_->clients[i];
        if (cl.state != MpClientState::Active || !cl.ent || int(i) == mp_->localClient) continue;
        if (cl.ent->obj->get("sessionstate").toString() != "playing") continue;
        const Value icon = cl.ent->obj->get("headicon");
        if (!icon.isString() || icon.asString().empty()) continue;
        const int team = mpTeamNumber(cl.ent->obj->get("headiconteam").toString());
        if (!mpHeadIconVisible(team, localTeam)) continue;
        MpHeadIcon h;
        h.material = icon.asString();
        h.pos = cl.ent->obj->get("origin").asVector();
        h.pos[2] += 82.0f;
        h.size = size;
        out.push_back(h);
    }
    return out;
}

// Crosshair trace (0x4c9700): view origin + 8192 along the view direction, mask 0x2000001, the local client ignored; a hit
// client number (< 64) is returned, else -1.
int World::mpCrosshairClient(const Vec3& eye, const Vec3& forward) {
    if (!mp_) return -1;
    const Entity* self = mpClientEntity(mp_->localClient);
    const Vec3 end{eye[0] + forward[0] * 8192.0f, eye[1] + forward[1] * 8192.0f, eye[2] + forward[2] * 8192.0f};
    int loc = 0;
    const TraceResult tr = locationalTrace(eye, end, 0x2000001u, self, nullptr, loc);
    if (tr.entity < 0) return -1;
    Entity* hit = entityByNumber(tr.entity);
    return hit ? mpClientOf(*hit) : -1;
}

bool World::mpObjectiveVisible(const Objective& o) const {
    if (!mp_ || o.team.empty() || o.team == "none") return true;
    const int lt = mpLocalTeam();
    return lt == mpTeamNumber(o.team);
}

void World::mpHudFrame() {
    // objective_onEntity: the position follows the entity (0x8112e04).
    for (Objective& o : objectives_) {
        if (o.entity < 0) continue;
        Entity* e = entityByNumber(o.entity);
        if (!e || e->obj->removed) {
            o.entity = -1;
            continue;
        }
        const Vec3 p = e->obj->get("origin").asVector();
        o.positions[0] = {p[0], p[1], p[2]};
    }
}

void World::registerMpHudBuiltins() {
    // obituary(victim, attacker, weapon, mod) (0x8113128): a broadcast event; every client builds its own line.
    vm_.registerBuiltin("obituary", [this](CallContext& c) {
        Entity* v = entityOf(c.arg(0));
        Entity* a = c.arg(1).isObject() ? entityOf(c.arg(1)) : nullptr;
        const int vc = v ? mpClientOf(*v) : -1;
        const int ac = a ? mpClientOf(*a) : -1;
        const MpObituary ev = mpMakeObituary(vc, ac, lower(c.arg(2).toString()), c.arg(3).toString());
        const auto nm = [](Entity* e) { return e ? e->obj->get("name").toString() : std::string(); };
        mp_->prints.push_back("obituary " + nm(v) + " by " + nm(a) + " " + c.arg(2).toString() + " " + c.arg(3).toString());
        // CG_Obituary: "target out of range" for a victim outside the client slots (0x4e0527); no line then.
        if (vc >= 0) {
            mp_->obituaries.push_back(ev);
            mp_->obituaryLog.push_back(ev);
        }
        return Value();
    });
    // playLocalSound(alias) (0x80fd990): self must be a client; the sound command goes to that client only.
    vm_.registerBuiltin("playlocalsound", [this](CallContext& c) {
        if (c.argc() > 0) noteSound(c.arg(0).toString());
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        if (cl == mp_->localClient) startSound(c.arg(0).toString(), nullptr);
        return Value();
    });
    // objective_add(index, state [, origin [, icon]]) (0x811299c): MP has no text argument; states empty / invisible / current.
    auto index = [](const Value& v) {
        const int i = v.asInt();
        if (i < 0 || i > 15) throw std::runtime_error("index " + std::to_string(i) + " is an illegal objective index. Valid indexes are 0 to 15");
        return i;
    };
    auto state = [](const Value& v) {
        const std::string s = lower(v.toString());
        if (s == "empty") return 0;
        if (s == "invisible") return 2;
        if (s == "current") return 4;
        throw std::runtime_error("Illegal objective state \"" + v.toString() + "\". Valid states are \"empty\", \"invisible\", \"current\"");
    };
    vm_.registerBuiltin("objective_add", [this, index, state](CallContext& c) {
        if (c.args.size() < 2)
            throw std::runtime_error("objective_add needs at least the first two parameters out of its parameter list of: index state "
                                     "position iconshader");
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        o = Objective{};
        o.state = state(c.arg(1));
        if (c.args.size() > 2) {
            const Vec3 p = c.arg(2).asVector();
            o.positions[0] = {p[0], p[1], p[2]};
        }
        if (c.args.size() > 3) o.icon = c.arg(3).toString();
        return Value();
    });
    vm_.registerBuiltin("objective_state", [index, state, this](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))].state = state(c.arg(1));
        return Value();
    });
    vm_.registerBuiltin("objective_onentity", [this, index](CallContext& c) {
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        Entity* e = entityOf(c.arg(1));
        if (!e) throw std::runtime_error("objective_onEntity: not an entity");
        o.entity = e->number;
        mpHudFrame();
        return Value();
    });
    vm_.registerBuiltin("objective_position", [this, index](CallContext& c) {
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        const Vec3 p = c.arg(1).asVector();
        o.entity = -1;
        o.positions[0] = {p[0], p[1], p[2]};
        return Value();
    });
    // objective_team(index, team) (0x8112f8a): allies, axis or none; a team objective is shown only to that team.
    vm_.registerBuiltin("objective_team", [this](CallContext& c) {
        const int i = c.arg(0).asInt();
        if (i < 0 || i > 15)
            throw std::runtime_error("index " + std::to_string(i) + " is an illegal objective index. Valid indexes are 0 to 15\n");
        const std::string t = lower(c.arg(1).toString());
        if (t != "allies" && t != "axis" && t != "none")
            throw std::runtime_error("Illegal team string '" + c.arg(1).toString() + "'. Must be allies, axis, or none.");
        objectives_[size_t(i)].team = t;
        return Value();
    });
    // hudelem setWayPoint(constantSize) (0x81044c0): the element becomes type 13, a world sprite at (x, y, z) drawn by the
    // client (0x4ddd30 -> 0x4ddcb0); constant size takes the width as a screen fraction × 0.0043.
    vm_.registerBuiltin("setwaypoint", [](CallContext& c) {
        const auto h = c.self.asObject();
        h->set("$waypoint", Value(int32_t(1)));
        h->set("$waypointconst", Value(int32_t(c.args.empty() ? 0 : c.arg(0).asInt())));
        return Value();
    });
}

} // namespace iw2::game
