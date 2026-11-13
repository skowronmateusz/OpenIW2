// The multiplayer World behind the network server core; implemented from the spec (docs/spec/net.md sections 7, 8
// and 11, docs/spec/mp-hud.md, docs/spec/mp-chat.md).
#include "netgame/worldgame.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "game/mphud.h"
#include "game/playerevents.h"
#include "game/mpkillcam.h"
#include "game/mpspectate.h"
#include "game/mpplayers.h"
#include "game/world.h"
#include "net/netfields.h"
#include "net/oob.h"
#include "net/playerstate.h"
#include "netgame/netmove.h"
#include "util/log.h"

namespace iw2::netgame {

using game::Entity;
using game::World;
using script::Value;

namespace {

void putF(uint8_t* p, int off, float f) { std::memcpy(p + off, &f, 4); }
void putI(uint8_t* p, int off, int32_t i) { std::memcpy(p + off, &i, 4); }
float getF(const uint8_t* p, int off) {
    float f;
    std::memcpy(&f, p + off, 4);
    return f;
}
int32_t getI(const uint8_t* p, int off) {
    int32_t i;
    std::memcpy(&i, p + off, 4);
    return i;
}
void putF(std::vector<uint8_t>& v, int off, float f) { putF(v.data(), off, f); }
void putI(std::vector<uint8_t>& v, int off, int32_t i) { putI(v.data(), off, i); }

// HUD enumerations as small indices (openiw2 order; TODO(spec): the 1.3 codes).
const char* const kHorz[] = {"subleft", "left", "center", "right", "fullscreen", "noscale", "alignto640", "center_safearea"};
const char* const kVert[] = {"subtop", "top", "middle", "bottom", "fullscreen", "noscale", "alignto480", "center_safearea"};
const char* const kAlignX[] = {"left", "center", "right"};
const char* const kAlignY[] = {"top", "middle", "bottom"};
const char* const kFonts[] = {"default", "bigfixed", "smallfixed", "objective", "big", "small", "hudbig", "hudsmall"};

template <size_t N>
int enumIndex(const char* const (&names)[N], const Value& v, int def) {
    if (!v.isString()) return def;
    std::string s = v.asString();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return int(i);
    return def;
}
template <size_t N>
const char* enumName(const char* const (&names)[N], int i, int def) {
    return names[(i >= 0 && size_t(i) < N) ? size_t(i) : size_t(def)];
}

float num(const Value& v, float def = 0.0f) { return v.isNumber() ? v.asFloat() : def; }

uint32_t packColor(const Value& rgb, float alpha) {
    script::Vec3 c{1, 1, 1};
    if (rgb.isVector()) c = rgb.asVector();
    auto b = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    return b(c[0]) | b(c[1]) << 8 | b(c[2]) << 16 | b(alpha) << 24;
}
void unpackColor(uint32_t c, script::Vec3& rgb, float& alpha) {
    rgb = {float(c & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f, float((c >> 16) & 0xff) / 255.0f};
    alpha = float((c >> 24) & 0xff) / 255.0f;
}

// HUD element types on the wire: 13 is the waypoint (docs/spec/mp-hud.md), 5 / 8 / 9 timer up and clocks (hud.cpp);
// the others are openiw2 choices, TODO(spec).
constexpr int kHudText = 1, kHudValue = 2, kHudTimer = 3, kHudMaterial = 4, kHudTimerUp = 5, kHudClock = 8,
              kHudClockUp = 9, kHudWaypoint = 13;

}  // namespace

// A key of a weapon file (lower case), "" when the weapon or the key is missing.
static std::string weaponKey(game::World& w, const std::string& weapon, const char* key) {
    const auto* def = w.weapon(weapon);
    if (!def) return {};
    const auto it = def->find(key);
    return it == def->end() ? std::string() : it->second;
}

int ConfigstringRange::index(const std::string& nameIn) {
    if (nameIn.empty()) return -1;
    std::string name = nameIn;
    if (lower_)
        for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == name) return int(i);
    if (int(names_.size()) >= count_) return -1;  // TODO(spec): the original's overflow error
    names_.push_back(name);
    if (onAdd) {
        if (list_) onAdd(base_, joined());
        else onAdd(base_ + int(names_.size()) - 1, name);
    }
    return int(names_.size()) - 1;
}

std::string ConfigstringRange::joined() const {
    std::string s;
    for (const std::string& n : names_) {
        if (!s.empty()) s += ' ';
        s += n;
    }
    return s;
}

void ConfigstringRange::emit(std::map<int, std::string>& cs) const {
    if (list_) {
        if (!names_.empty()) cs[base_] = joined();
        return;
    }
    for (size_t i = 0; i < names_.size(); ++i) cs[base_ + int(i)] = names_[i];
}

std::vector<std::string> weaponList(const std::map<int, std::string>& configstrings) {
    std::vector<std::string> out;
    for (int n = 1;; ++n) {
        std::string w = weaponNameFromList(configstrings, n);
        if (w.empty()) break;
        out.push_back(std::move(w));
    }
    return out;
}

std::string weaponAmmoName(game::World& w, const std::string& weapon, bool clip) {
    const auto* def = w.weapon(weapon);
    if (!def) return weapon;
    const auto it = def->find(clip ? "clipname" : "ammoname");
    return it == def->end() || it->second.empty() ? weapon : it->second;
}

int weaponAmmoSlot(game::World& w, const std::vector<std::string>& list, const std::string& weapon, bool clip) {
    const std::string target = weaponAmmoName(w, weapon, clip);
    std::vector<std::string> seen;
    for (const std::string& n : list) {
        const std::string a = weaponAmmoName(w, n, clip);
        if (std::find(seen.begin(), seen.end(), a) == seen.end()) seen.push_back(a);
        if (a == target) return int(std::find(seen.begin(), seen.end(), a) - seen.begin());
    }
    return -1;
}

std::string weaponNameFromList(const std::map<int, std::string>& configstrings, int n) {
    if (n <= 0) return {};
    const auto it = configstrings.find(kWeaponListConfigstring);
    if (it == configstrings.end()) return {};
    int i = 1;
    size_t p = 0;
    const std::string& s = it->second;
    while (p <= s.size()) {
        size_t e = s.find(' ', p);
        if (e == std::string::npos) e = s.size();
        if (i == n) return s.substr(p, e - p);
        ++i;
        p = e + 1;
    }
    return {};
}

std::string hudStringToConfig(const Value& v) {
    if (std::holds_alternative<script::LocalizedString>(v.v)) return std::get<script::LocalizedString>(v.v).key;
    if (v.isDefined() && !v.toString().empty()) return kPlainHudStringMark + v.toString();
    return {};
}

Value hudStringFromConfig(const std::string& s) {
    if (!s.empty() && s[0] == kPlainHudStringMark) return Value(s.substr(1));
    return Value(script::LocalizedString{s});
}

void encodeHudElem(uint8_t* out, const script::Object& h, HudStrings& s) {
    std::memset(out, 0, size_t(net::kHudElemSize));
    int type = kHudText;
    if (h.get("$waypoint").truthy()) type = kHudWaypoint;
    else if (h.get("$clockdur").isNumber()) type = h.get("$clockup").truthy() ? kHudClockUp : kHudClock;
    else if (h.get("$timerend").isNumber()) type = h.get("$timerup").truthy() ? kHudTimerUp : kHudTimer;
    else if (h.get("$shader").isString()) type = kHudMaterial;
    else if (h.get("$value").isNumber()) type = kHudValue;
    putI(out, 0, type);
    putF(out, 4, num(h.get("x")));
    putF(out, 8, num(h.get("y")));
    putF(out, 12, num(h.get("z")));
    putF(out, 16, num(h.get("fontscale"), 1.0f));
    putI(out, 20, enumIndex(kFonts, h.get("font"), 0));
    putI(out, 24, enumIndex(kAlignX, h.get("alignx"), 0) | enumIndex(kAlignY, h.get("aligny"), 0) << 2);
    putI(out, 28, enumIndex(kHorz, h.get("horzalign"), 1) | enumIndex(kVert, h.get("vertalign"), 1) << 3);
    putI(out, 32, int32_t(packColor(h.get("color"), num(h.get("alpha"), 1.0f))));
    if (h.get("$fadestart").isNumber()) {
        putI(out, 36, int32_t(packColor(h.get("$fadecolor"), num(h.get("$fadealpha"), 1.0f))));
        putI(out, 40, int32_t(num(h.get("$fadestart"))));
        putI(out, 44, std::clamp(int(num(h.get("$fadetime"))), 0, 0xffff));
    }
    if (s.strings) {
        const int label = s.strings->index(hudStringToConfig(h.get("label")));
        putI(out, 48, label + 1);
        if (type == kHudText) putI(out, 116, s.strings->index(hudStringToConfig(h.get("$text"))) + 1);
    }
    putI(out, 52, std::clamp(int(num(h.get("$width"))), 0, 1023));
    putI(out, 56, std::clamp(int(num(h.get("$height"))), 0, 1023));
    if (s.materials && h.get("$shader").isString()) putI(out, 60, s.materials->index(h.get("$shader").asString()) + 1);
    if (h.get("$scalestart").isNumber()) {
        putI(out, 64, std::clamp(int(num(h.get("$scalew"))), 0, 1023));
        putI(out, 68, std::clamp(int(num(h.get("$scaleh"))), 0, 1023));
        putI(out, 72, int32_t(num(h.get("$scalestart"))));
        putI(out, 76, std::clamp(int(num(h.get("$scaletime"))), 0, 0xffff));
    }
    if (h.get("$movestart").isNumber()) {
        putF(out, 80, num(h.get("$movex")));
        putF(out, 84, num(h.get("$movey")));
        putI(out, 96, int32_t(num(h.get("$movestart"))));
        putI(out, 100, std::clamp(int(num(h.get("$movetime"))), 0, 0xffff));
    }
    putI(out, 104, int32_t(num(h.get("$timerend"))));
    putI(out, 108, int32_t(num(h.get("$clockdur"))));
    if (type == kHudWaypoint) putI(out, 108, h.get("$waypointconst").truthy() ? 1 : 0);
    putF(out, 112, num(h.get("$value")));
    putF(out, 120, num(h.get("sort")));
    putI(out, 124, h.get("foreground").truthy() ? 1 : 0);
}

script::ObjectPtr decodeHudElem(const uint8_t* in, const std::function<script::ObjectPtr()>& newObject,
                                const std::map<int, std::string>& configstrings, int64_t timeOffset) {
    const int type = getI(in, 0);
    if (type == 0) return nullptr;
    script::ObjectPtr h = newObject();
    auto cs = [&](int base, int idx) -> const std::string* {
        if (idx <= 0) return nullptr;
        auto it = configstrings.find(base + idx - 1);
        return it == configstrings.end() ? nullptr : &it->second;
    };
    auto t = [&](int off) { return Value(float(int64_t(getI(in, off)) + timeOffset)); };
    h->set("x", Value(getF(in, 4)));
    h->set("y", Value(getF(in, 8)));
    h->set("z", Value(getF(in, 12)));
    h->set("fontscale", Value(getF(in, 16)));
    h->set("font", Value(std::string(enumName(kFonts, getI(in, 20), 0))));
    h->set("alignx", Value(std::string(enumName(kAlignX, getI(in, 24) & 3, 0))));
    h->set("aligny", Value(std::string(enumName(kAlignY, (getI(in, 24) >> 2) & 3, 0))));
    h->set("horzalign", Value(std::string(enumName(kHorz, getI(in, 28) & 7, 1))));
    h->set("vertalign", Value(std::string(enumName(kVert, (getI(in, 28) >> 3) & 7, 1))));
    script::Vec3 rgb;
    float alpha;
    unpackColor(uint32_t(getI(in, 32)), rgb, alpha);
    h->set("color", Value(rgb));
    h->set("alpha", Value(alpha));
    if (getI(in, 40) != 0 || getI(in, 44) != 0) {
        unpackColor(uint32_t(getI(in, 36)), rgb, alpha);
        h->set("$fadecolor", Value(rgb));
        h->set("$fadealpha", Value(alpha));
        h->set("$fadestart", t(40));
        h->set("$fadetime", Value(float(getI(in, 44))));
    }
    if (const std::string* l = cs(kStringConfigstrings, getI(in, 48))) h->set("label", hudStringFromConfig(*l));
    const int w = getI(in, 52), ht = getI(in, 56);
    if (getI(in, 72) != 0 || getI(in, 76) != 0) {
        h->set("$scalew", Value(int32_t(getI(in, 64))));
        h->set("$scaleh", Value(int32_t(getI(in, 68))));
        h->set("$scalestart", t(72));
        h->set("$scaletime", Value(float(getI(in, 76))));
    }
    if (getI(in, 96) != 0 || getI(in, 100) != 0) {
        h->set("$movex", Value(getF(in, 80)));
        h->set("$movey", Value(getF(in, 84)));
        h->set("$movestart", t(96));
        h->set("$movetime", Value(float(getI(in, 100))));
    }
    const std::string* mat = cs(kMaterialConfigstrings, getI(in, 60));
    switch (type) {
    case kHudText:
        if (const std::string* s = cs(kStringConfigstrings, getI(in, 116))) h->set("$text", hudStringFromConfig(*s));
        break;
    case kHudValue: h->set("$value", Value(getF(in, 112))); break;
    case kHudTimerUp: h->set("$timerup", Value(int32_t(1))); [[fallthrough]];
    case kHudTimer: h->set("$timerend", t(104)); break;
    case kHudClockUp:
    case kHudClock:
        h->set("$timerend", t(104));
        h->set("$clockdur", Value(int32_t(getI(in, 108))));
        h->set("$clockup", Value(int32_t(type == kHudClockUp ? 1 : 0)));
        [[fallthrough]];
    case kHudMaterial:
        if (mat) h->set("$shader", Value(*mat));
        h->set("$width", Value(int32_t(w)));
        h->set("$height", Value(int32_t(ht)));
        break;
    case kHudWaypoint:
        h->set("$waypoint", Value(int32_t(1)));
        h->set("$waypointconst", Value(int32_t(getI(in, 108))));
        if (mat) h->set("$shader", Value(*mat));
        h->set("$width", Value(int32_t(w)));
        h->set("$height", Value(int32_t(ht)));
        break;
    default: break;
    }
    h->set("sort", Value(getF(in, 120)));
    if (getI(in, 124)) h->set("foreground", Value(int32_t(1)));
    return h;
}

WorldGame::WorldGame(World* w, std::string map, std::string gametype, int serverId, int maxClients)
    : w_(w), map_(std::move(map)), gametype_(std::move(gametype)), serverId_(serverId), maxClients_(maxClients) {
    // A name added after the gamestate goes to the clients as a configstring command. Sounds are added when a script
    // first plays them (playLocalSound's MP_hit_alert): without it a client could not resolve that sound command.
    for (ConfigstringRange* r : {&weapons_, &materials_, &strings_, &models_, &hints_, &sounds_})
        r->onAdd = [this](int index, const std::string& value) { announce(index, value); };
    if (w_) setWorld(w_, map_, gametype_, serverId_);
}

WorldGame::~WorldGame() = default;

void WorldGame::setWorld(World* w, std::string map, std::string gametype, int serverId) {
    w_ = w;
    map_ = std::move(map);
    gametype_ = std::move(gametype);
    serverId_ = serverId;
    mp_.clear();
    moves_.clear();
    otherRings_.clear();
    turretRings_.clear();
    turretStopsSeen_.clear();
    grenadeRings_.clear();
    sentOffhand_.clear();
    framePrev_.clear(), frameCur_.clear(), subPlaces_.clear();
    frameSeenTime_ = -1, subTime_ = -1;
    printsSent_ = chatSent_ = obituariesSeen_ = explosionsSeen_ = clientCommandsSent_ = 0;
    for (ConfigstringRange* r : {&weapons_, &materials_, &strings_, &models_, &effects_, &sounds_, &hints_, &headIcons_}) r->clear();
    // The game registers the default weapon first, so it is weapon 1, then every precached item in order; models,
    // materials and strings follow their precache order too (docs/spec/net.md section 11).
    // The seeded names reach the clients with the gamestate; no configstring commands for them.
    seeding_ = true;
    weapons_.index("defaultweapon_mp");
    models_.index("xmodel/defaultweapon");
    if (w_) {
        // Map entities spawn before the gametype script runs: their turret weapons and models come first, in the
        // order of the map's entity list (observed on mp_toujane).
        const auto mapEnts = w_->bsp().entities();
        worldspawn_.assign(mapEnts.begin(), mapEnts.begin() + (mapEnts.empty() ? 0 : 1));
        for (const auto& be : mapEnts) {
            if (const std::string* wi = be.get("weaponinfo"); wi && !wi->empty()) registerWeaponNames(*wi);
            if (const std::string* md = be.get("model"); md && !md->empty() && (*md)[0] != '*') models_.index(*md);
        }
        syncNames();
    }
    seeding_ = false;
    restartsSeen_ = w_ ? w_->mpState().restarts : 0;
    levelStartMs_ = 0;
    lastDynamic_ = dynamicConfigstrings();
    wireOf_.clear();
    wireUsed_.assign(size_t(kFirstEventEntity), false);
    events_.clear();
    if (!w_) return;
    // A previous state's counters: anything already in the world's logs belongs to before the clients came.
    const auto& st = w_->mpState();
    printsSent_ = st.prints.size();
    chatSent_ = st.chat.size();
    clientCommandsSent_ = st.clientCommands.size();
    obituariesSeen_ = st.obituaryLog.size();
    explosionsSeen_ = st.explosionLog.size();
}

// A registered weapon also registers its loop / stop fire sounds and its use / drop hint strings (observed with the
// 1.3 server for the map's turret weapon: configstrings 591..594 and 1278, 1279).
namespace {
std::string lowerName(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
} // namespace

// Names the scripts registered (at load or later in the game) in precache order; new ones are announced.
void WorldGame::syncNames() {
    for (const std::string& n : w_->precached("precacheitem")) registerWeaponNames(n);
    for (const std::string& n : w_->precached("precachemodel")) models_.index(n);
    // Material names are kept in lower case (observed: "objectiveA" is "objectivea" in the 1.3 gamestate).
    for (const std::string& n : w_->precached("precacheshader")) materials_.index(n);
    for (const std::string& n : w_->precached("precachestring")) strings_.index(n);
    for (const std::string& n : w_->precached("loadfx")) effects_.index(n);
    for (const std::string& n : w_->precached("precacheheadicon")) headIcons_.index(n);
    // Sounds the scripts play are registered when first played (observed: announcer aliases in sd after the
    // weapons' sounds).
    for (const std::string& n : w_->precached("sound")) sounds_.index(n);
    // setHintString texts after the weapons' hint strings (observed at 1280 in sd after the turret's two).
    for (int i = 0; i < 32; ++i)
        if (const std::string& h = w_->hintStrings().at(i); !h.empty()) hints_.index(h);
}

void WorldGame::registerWeaponNames(const std::string& weapon) {
    if (weapons_.index(weapon) < 0 || !w_) return;
    const auto* def = w_->weapon(weapon);
    if (!def) return;
    for (const char* k : {"loopfiresound", "loopfiresoundplayer", "stopfiresound", "stopfiresoundplayer"})
        if (auto it = def->find(k); it != def->end() && !it->second.empty()) sounds_.index(it->second);
    for (const char* k : {"usehintstring", "drophintstring"})
        if (auto it = def->find(k); it != def->end() && !it->second.empty()) hints_.index(it->second);
}

namespace {
// Engine dvars the 1.3 server lists for its clients with their defaults (observed in the 1.3 gamestate,
// docs/spec/net.md section 11); the game's team dvars follow the world's cvars.
const std::pair<const char*, const char*> kSyncedDvars[] = {
    {"bg_aimSpreadMoveSpeedThreshold", "11"}, {"bg_bobAmplitudeDucked", "0.0075"}, {"bg_bobAmplitudeProne", "0.03"},
    {"bg_bobAmplitudeStanding", "0.007"}, {"bg_bobMax", "8"}, {"bg_fallDamageMaxHeight", "480"},
    {"bg_fallDamageMinHeight", "256"}, {"bg_foliagesnd_fastinterval", "500"}, {"bg_foliagesnd_maxspeed", "180"},
    {"bg_foliagesnd_minspeed", "40"}, {"bg_foliagesnd_resetinterval", "500"}, {"bg_foliagesnd_slowinterval", "1500"},
    {"bg_ladder_yawcap", "100"}, {"bg_prone_yawcap", "85"}, {"friction", "5.5"}, {"g_ScoresBanner_Allies", ""},
    {"g_ScoresBanner_Axis", ""}, {"g_ScoresBanner_None", "mpflag_none"}, {"g_ScoresBanner_Spectators", "mpflag_spectator"},
    {"g_TeamColor_Allies", ""}, {"g_TeamColor_Axis", ""}, {"g_TeamName_Allies", ""}, {"g_TeamName_Axis", ""},
    {"inertiaAngle", "0"}, {"inertiaDebug", "0"}, {"inertiaMax", "50"}, {"jump_height", "39"},
    {"jump_ladderPushVel", "128"}, {"jump_slowdownEnable", "1"}, {"jump_spreadAdd", "64"}, {"jump_stepSize", "18"},
    {"mantle_check_angle", "60"}, {"mantle_check_radius", "0.1"}, {"mantle_check_range", "20"}, {"mantle_debug", "0"},
    {"mantle_enable", "1"}, {"mantle_view_yawcap", "60"}, {"player_adsExitDelay", "0"},
    {"player_backSpeedScale", "0.7"}, {"player_breath_fire_delay", "0"}, {"player_breath_gasp_lerp", "6"},
    {"player_breath_gasp_scale", "4.5"}, {"player_breath_gasp_time", "1"}, {"player_breath_hold_lerp", "4"},
    {"player_breath_hold_time", "4.5"}, {"player_breath_snd_delay", "1"}, {"player_breath_snd_lerp", "2"},
    {"player_dmgtimer_flinchTime", "500"}, {"player_dmgtimer_maxTime", "750"}, {"player_dmgtimer_minScale", "0"},
    {"player_dmgtimer_stumbleTime", "500"}, {"player_dmgtimer_timePerPoint", "100"},
    {"player_footstepsThreshhold", "0"}, {"player_moveThreshhold", "10"}, {"player_scopeExitOnDamage", "0"},
    {"player_spectateSpeedScale", "2"}, {"player_strafeSpeedScale", "0.8"}, {"player_toggleBinoculars", "1"},
    {"player_turnAnims", "0"}, {"player_view_pitch_down", "85"}, {"player_view_pitch_up", "85"}, {"stopspeed", "100"},
};

} // namespace

std::map<int, std::string> WorldGame::dynamicConfigstrings() const {
    std::map<int, std::string> cs;
    if (!w_) return cs;
    if (!w_->ambientConfig().empty()) cs[kAmbientConfigstring] = w_->ambientConfig();
    // 13: the server time of the level's start, 0 on a new map, the time of the restart after a map_restart
    // (observed in sd after round restarts; set by SP_worldspawn, 0x811b5ea).
    cs[13] = std::to_string(levelStartMs_);
    if (!w_->fogConfig().empty()) cs[kFogConfigstring] = w_->fogConfig();
    // Dvar names at 142.. sorted by name ignoring case, each value 96 configstrings later (observed).
    std::map<std::string, std::pair<std::string, std::string>> dvars;  // lower name -> (name, value)
    auto cvar = [&](const std::string& n, const std::string& def) {
        const auto it = w_->cvars.find(n);
        return it == w_->cvars.end() ? def : it->second;
    };
    for (const auto& [n, d] : kSyncedDvars) dvars[lowerName(n)] = {n, cvar(n, d)};
    // The team colours are colour dvars: each component is kept as a byte and printed as byte / 255, four
    // components, alpha 0 when the script gives three (observed "0.25098 0.25098 0.74902 0" for ".25 .25 .75").
    for (const char* n : {"g_TeamColor_Allies", "g_TeamColor_Axis"}) {
        auto& v = dvars[lowerName(n)].second;
        float f[4] = {0, 0, 0, 0};
        if (v.empty() || std::sscanf(v.c_str(), "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]) < 3) continue;
        char buf[96];
        double q[4];
        for (int k = 0; k < 4; ++k) q[k] = std::lround(std::clamp(f[k], 0.0f, 1.0f) * 255.0f) / 255.0;
        std::snprintf(buf, sizeof buf, "%g %g %g %g", q[0], q[1], q[2], q[3]);
        v = buf;
    }
    for (const std::string& n : w_->mpState().serverInfoCvars) dvars[lowerName(n)] = {n, cvar(n, "")};
    int i = 0;
    for (const auto& [k, nv] : dvars) {
        if (i >= kDvarConfigstringCount) break;
        cs[kDvarNameConfigstrings + i] = nv.first;
        if (!nv.second.empty()) cs[kDvarValueConfigstrings + i] = nv.second;
        ++i;
    }
    return cs;
}

void WorldGame::announce(int index, const std::string& value) {
    if (sendCommand && !seeding_) sendCommand(-1, "d " + std::to_string(index) + " " + value);
}

std::map<int, std::string> WorldGame::configstrings() {
    // Server info (0) and system info (1) carry the keys of a 1.3 dedicated server in its order (docs/spec/net.md
    // section 11); keys openiw2 has no cvar for keep the server's defaults.
    std::string serverInfo, systemInfo;
    auto si = [&](const char* k, const std::string& v) { net::infoSetValueForKey(serverInfo, k, v); };
    si("g_antilag", "1");
    si("g_gametype", gametype_);
    si("gamename", "Call of Duty 2");
    si("mapname", map_);
    si("protocol", std::to_string(net::kProtocolVersion));
    si("scr_friendlyfire", "0");
    si("scr_killcam", "1");
    si("shortversion", net::kProtocolVersionString);
    si("sv_allowAnonymous", "0");
    si("sv_floodProtect", "1");
    si("sv_hostname", hostname);
    si("sv_maxclients", std::to_string(maxClients_));
    si("sv_maxPing", "0");
    si("sv_maxRate", "0");
    si("sv_minPing", "0");
    si("sv_privateClients", "0");
    si("sv_punkbuster", "0");
    si("sv_pure", "0");
    si("sv_voice", "0");
    auto sy = [&](const char* k, const std::string& v) { net::infoSetValueForKey(systemInfo, k, v); };
    sy("bg_fallDamageMaxHeight", "480");
    sy("bg_fallDamageMinHeight", "256");
    sy("g_synchronousClients", "0");
    sy("sv_cheats", "0");
    sy("sv_disableClientConsole", "0");
    sy("sv_pure", "0");
    // sv_referencedIwdNames / sv_referencedIwds: none (openiw2 never asks the client to load or download an iwd).
    sy("sv_serverid", std::to_string(serverId_));
    sy("sv_voice", "0");
    sy("sv_voiceQuality", "1");
    sy("timescale", "1");
    std::map<int, std::string> cs{{0, serverInfo}, {1, systemInfo}};
    // The map's worldspawn (SP_worldspawn, VAs in the 1.3 Linux dedicated server 0x811b56a): 2 the constant "cod"
    // (0x811b5c5), 3 "n\<ambienttrack>" when the key is set (0x811b630; ambientPlay replaces it), 4 the "message"
    // key (0x811b674), 11 "northyaw" (default "0", 0x811b6ff), 13 "%i" of a game global (observed "0" at map start,
    // 0x811b5ea, TODO(spec): which global).
    cs[2] = "cod";
    if (!worldspawn_.empty()) {
        const auto& ws = worldspawn_.front();
        if (const std::string* t = ws.get("ambienttrack"); t && !t->empty()) cs[kAmbientConfigstring] = "n\\" + *t;
        if (const std::string* m = ws.get("message"); m && !m->empty()) cs[4] = *m;
        const std::string* ny = ws.get("northyaw");
        cs[11] = ny && !ny->empty() ? *ny : "0";
    }
    // 8: the registered weapon numbers (0 included) as a bit mask in 33 hex digits, the lowest four bits first
    // (observed "fffff3…" for 21 weapons; set right after the weapon list, 0x810710d).
    {
        std::string mask(33, '0');
        const int n = int(weapons_.names().size());
        for (int b = 0; b <= n && b < 132; ++b) {
            const int d = mask[size_t(b / 4)] >= 'a' ? mask[size_t(b / 4)] - 'a' + 10 : mask[size_t(b / 4)] - '0';
            const int v = d | (1 << (b % 4));
            mask[size_t(b / 4)] = char(v < 10 ? '0' + v : 'a' + v - 10);
        }
        cs[8] = mask;
    }
    // 22: the game's winner info, "\winner\0" until a winner is set (observed; set at 0x8109356, TODO(spec): the
    // values later in the game). 31: the talking icon material (observed "talkingicon", TODO(spec): its setter).
    cs[22] = "\\winner\\0";
    // 31..: the head icons (precacheHeadIcon) in order (the scripts also precache the talking icon).
    for (auto& [k, v] : dynamicConfigstrings()) cs[k] = v;
    // Status icons (precacheStatusIcon): icon i (1-based) is configstring 22 + i (observed: 23, 24 on mp_toujane).
    const auto& icons = w_->mpState().statusIcons;
    for (size_t i = 0; i < icons.size(); ++i) cs[kStatusIconConfigstrings + int(i)] = icons[i];
    // Script menus by precache slot (docs/spec/net.md section 10): the client opens them by index.
    const auto& menus = w_->menuNames();
    for (size_t i = 0; i < menus.size(); ++i) cs[kMenuConfigstrings + int(i)] = menus[i];
    const auto& shocks = w_->shellShockNames();
    for (size_t i = 0; i < shocks.size(); ++i) cs[kShockConfigstrings + 1 + int(i)] = shocks[i];
    for (const ConfigstringRange* r : {&weapons_, &materials_, &strings_, &models_, &effects_, &sounds_, &hints_, &headIcons_}) r->emit(cs);
    return cs;
}

int WorldGame::clientNumber(int slot) {
    const auto it = mp_.find(slot);
    return it == mp_.end() ? slot : it->second;
}

bool WorldGame::clientConnect(int slot, const std::string& userinfo) {
    std::string name = net::infoValueForKey(userinfo, "name");
    if (name.empty()) name = "player";
    // Network slots take the next free MP clients (a listen server's own player keeps client 0).
    const int n = w_->mpConnect(name);
    if (n < 0) return false;
    mp_[slot] = n;
    moves_[slot] = {};
    std::printf("client %d connected as \"%s\" (mp client %d)\n", slot, name.c_str(), n);
    log::Event("net") << "connect slot " << slot << " \"" << name << "\", mp client " << n; // event lines (docs/logging.md)
    return true;
}

void WorldGame::clientBegin(int slot) {
    auto it = mp_.find(slot);
    if (it != mp_.end()) w_->mpBegin(it->second);
}

void WorldGame::clientCommand(int slot, const std::string& text) {
    auto it = mp_.find(slot);
    if (it == mp_.end()) return;
    auto tok = net::tokenize(text);
    if (tok.size() >= 4 && tok[0] == "mr") {
        // "mr <serverId> <menu index> <response>"; a menu name is accepted too. An answer to the previous map's menu
        // (still on its way at the map change) is ignored (docs/spec/mp.md "Menu responses").
        if (std::atoi(tok[1].c_str()) != serverId_) return;
        std::string menu = tok[2];
        const auto& menus = w_->menuNames();
        if (!menu.empty() && std::isdigit(static_cast<unsigned char>(menu[0])) && std::atoi(menu.c_str()) < int(menus.size()))
            menu = menus[size_t(std::atoi(menu.c_str()))];
        w_->mpMenuResponse(it->second, menu, tok[3]);
    } else if (!tok.empty() && (tok[0] == "say" || tok[0] == "say_team")) {
        // Chat (docs/spec/mp-chat.md): the rest of the line is the message.
        const size_t sp = text.find(' ');
        w_->mpSay(it->second, tok[0] == "say_team" ? 1 : 0, sp == std::string::npos ? std::string() : text.substr(sp + 1));
    } else if (!tok.empty() && tok[0] == "score") {
        // The scoreboard request (0x4dabf0 sends it at most every 2 s while shown): the "b" command (0x80fdf08).
        if (sendCommand) sendCommand(slot, w_->mpScoresCommand());
    } else {
        std::printf("client %d command: %s\n", slot, text.c_str());
    }
}

void WorldGame::clientThink(int slot, const net::UserCmd& sent) {
    // 0x80f5d2e (docs/spec/net.md section 10 "Command steps"): the command's time is clamped to the level time - 1000 ..
    // + 200; a command not newer than the last one run is not run at all (no movement, no weapon, no buttons). An older
    // one (the clock-reset exception of the server's command loop) becomes the last command time.
    net::UserCmd cmd = sent;
    cmd.serverTime = std::clamp(cmd.serverTime, serverTime() - 1000, serverTime() + 200);
    static const bool netTrace = std::getenv("IW2_NET_TRACE") != nullptr;
    if (netTrace && cmd.serverTime != sent.serverTime)
        log::tracef("net", "NETTRACE clamp slot=%d sent=%d level=%d\n", slot, sent.serverTime, serverTime());
    auto it = mp_.find(slot);
    if (it == mp_.end()) return;
    Move& m = moves_[slot];
    Entity* e = w_->mpClientEntity(it->second);
    if (!e) return;
    const int prevTime = m.lastTime;
    if (prevTime != 0 && cmd.serverTime <= prevTime) {
        m.lastTime = cmd.serverTime;
        if (cmd.serverTime < prevTime) m.track.restart(); // his command times start again from an older one
        if (std::getenv("IW2_SOAK_SHOTTRACE")) log::tracef("net", "SRVT slot=%d cmd=%d sent=%d prev=%d skipped\n", slot, cmd.serverTime, sent.serverTime, prevTime);
        return;
    }
    m.lastTime = cmd.serverTime;
    if (netTrace && prevTime && (cmd.serverTime - prevTime >= 100 || cmd.serverTime <= prevTime))
        log::tracef("net", "NETTRACE step slot=%d %d ms\n", slot, cmd.serverTime - prevTime);
    const int spawns = w_->mpState().clients[size_t(it->second)].spawnCount;
    // Debug: IW2_MP_PLACE=[name=]x,y,z[;...] puts a network client (every one, or the one of that name) at that point
    // once per spawn (docs/debug-flags.md).
    if (const char* pl = std::getenv("IW2_MP_PLACE"); pl && m.placedSpawn != spawns &&
                                                       e->obj->get("sessionstate").toString() == "playing") {
        m.placedSpawn = spawns;
        const std::string name = e->obj->get("name").toString();
        std::string all = pl;
        for (size_t start = 0; start <= all.size();) {
            size_t end = all.find(';', start);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(start, end - start);
            start = end + 1;
            if (const size_t eq = item.find('='); eq != std::string::npos) {
                if (item.substr(0, eq) != name) continue;
                item = item.substr(eq + 1);
            }
            float p[3];
            if (std::sscanf(item.c_str(), "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) {
                e->obj->set("origin", Value(script::Vec3{p[0], p[1], p[2]}));
                std::printf("IW2_MP_PLACE %s at %g %g %g\n", name.c_str(), double(p[0]), double(p[1]), double(p[2]));
                break;
            }
        }
    }
    // Debug: IW2_MP_DROP=weapon@dx,dy,dz[;...] drops those weapon items at the offset from a playing client's origin, once per spawn
    // (a weapon on the ground in front of the client, for checking the pickup hint).
    if (const char* dr = std::getenv("IW2_MP_DROP"); dr && m.dropSpawn != spawns && e->obj->get("sessionstate").toString() == "playing") {
        m.dropSpawn = spawns;
        const auto at0 = e->obj->get("origin").asVector();
        const std::string list = dr;
        for (size_t b = 0; b <= list.size();) {
            size_t en = list.find(';', b);
            if (en == std::string::npos) en = list.size();
            const std::string ent = list.substr(b, en - b);
            b = en + 1;
            const size_t at = ent.find('@');
            float q[3];
            if (at != std::string::npos && std::sscanf(ent.c_str() + at + 1, "%f,%f,%f", &q[0], &q[1], &q[2]) == 3) {
                w_->debugDropItem(ent.substr(0, at), {at0[0] + q[0], at0[1] + q[1], at0[2] + q[2]}, {0, 0, 0});
                std::printf("IW2_MP_DROP %s at %g %g %g\n", ent.substr(0, at).c_str(), double(at0[0] + q[0]), double(at0[1] + q[1]), double(at0[2] + q[2]));
            }
        }
    }
    const auto o = e->obj->get("origin").asVector();
    // A map turret holds its user behind the gun every server frame and puts him down on release (thinkPlayerTurret,
    // releasePlayerTurret through mpClientTurret): the movement goes on from there with its state kept, as the
    // original's turret code moves the player state itself.
    const bool onTurretNow = w_->mpClientOnTurret(it->second);
    if (m.havePs && m.spawnCount == spawns && (onTurretNow || m.turretMoved)) {
        m.track.restart();
        m.ps.origin = {o[0], o[1], o[2]};
        m.lastOrigin = {o[0], o[1], o[2]};
    }
    m.turretMoved = onTurretNow;
    if (!m.havePs || m.spawnCount != spawns ||
        std::fabs(o[0] - m.lastOrigin[0]) + std::fabs(o[1] - m.lastOrigin[1]) + std::fabs(o[2] - m.lastOrigin[2]) > 0.01f) {
        // A client spawn clears the player state (stance, view height, lean, aim; docs/spec/mp.md "Client spawn").
        if (m.havePs && m.spawnCount == spawns) ++stateClears;
        if (netTrace && m.havePs && m.spawnCount == spawns)
        {
            std::printf("NETTRACE reset slot=%d moved %.2f %.2f %.2f level=%d cmd=%d; last other write of the origin:\n", slot,
                        double(o[0] - m.lastOrigin[0]), double(o[1] - m.lastOrigin[1]), double(o[2] - m.lastOrigin[2]),
                        serverTime(), cmd.serverTime);
            script::OriginWatch::print(slot);
        }
        m.spawnCount = spawns;
        m.ps = {};
        m.track.restart();
        m.ps.origin = {o[0], o[1], o[2]};  // placed by the script (spawn): restart movement there
        m.psEventsDone = 0;
        m.havePs = true;
    }
    // freezecontrols (client +0x27b4, 0x80f5de9): ClientThink sets ps pm_flags 0x8000 and the move runs on a command with no move
    // bytes and only the stance buttons (docs/spec/controls.md "freezecontrols"); scripts still read the buttons as held.
    // TODO(spec): the individual consumers of the flag in the 1.3 MP player move were not located; the SP ones are applied.
    const bool frozen = w_->mpState().clients[size_t(it->second)].controlsFrozen;
    const net::UserCmd held = cmd; // the buttons as the client holds them
    if (frozen) {
        cmd.forwardmove = 0;
        cmd.rightmove = 0;
        cmd.buttons &= 0x2300;
    }
    // MP movement type by session state (docs/spec/mp-spectate.md): 0 playing, 4 spectator, 5 intermission, 6 dead; a
    // playing client at 0 health is dead too (no weapon, no lean).
    m.ps.pmType = game::mpClientPmType(game::mpSessionStateNumber(e->obj->get("sessionstate").toString()));
    if (const Value hp = e->obj->get("health"); m.ps.pmType == 0 && hp.isNumber() && hp.asFloat() <= 0) m.ps.pmType = 6;
    // The view the server sets (spawn, intermission spawn, setPlayerAngles: SetClientViewAngle 0x80f85d8): delta_angles
    // = the wanted angle as a 16-bit angle minus the last command's angle; every command's angles get the delta added
    // (PM_UpdateViewAngles), so the client's view turns there although its own angles do not jump.
    {
        const game::MpClient& vc = w_->mpState().clients[size_t(it->second)];
        if (vc.viewAngleSets != m.viewAngleSetsSeen) {
            m.viewAngleSetsSeen = vc.viewAngleSets;
            for (int k = 0; k < 3; ++k) m.deltaAngles[k] = viewAngleDelta(vc.setViewAngles[size_t(k)], m.lastCmdAngles[k]);
            m.viewPitch = vc.setViewAngles[0];
            m.viewYaw = vc.setViewAngles[1];
        }
        for (int k = 0; k < 3; ++k) m.lastCmdAngles[k] = cmd.angles[k] & 0xffff;
        cmd = withDeltaAngles(cmd, m.deltaAngles);
    }
    // The movement shared with the client's prediction (src/netgame/netmove.cpp), with this client's weapon state.
    game::PlayerInput in = inputFromUsercmd(cmd);
    const WeaponMove wm = m.combat->move();
    // On a map turret the server holds the client behind the gun and fires the turret (src/game/mpturret.cpp): no move
    // and no weapon of its own.
    const bool onTurret = w_->mpClientOnTurret(it->second);
    if (onTurret) {
        m.ps.origin = {o[0], o[1], o[2]};
        m.ps.velocity = {0, 0, 0};
    } else {
        for (const game::KnockbackHit& hit : w_->mpTakeKnockbacks(it->second)) game::applyKnockback(m.ps, hit, w_->cvarFloat("g_knockback", 1000.0f));
        setMantleAnims(w_->mpMantleAnims());
        runUsercmd(w_->collision(), m.ps, cmd, prevTime, &wm, e->number, frozen);
    }
    // The prone yaw cap turned the view (docs/spec/mp-pmove.md "Prone body"): the change goes into delta_angles[1], so
    // the client's view turns with it, and this command's view is the capped one.
    if (m.ps.viewYawCorrection != 0.0f) {
        const int dy = int(std::lround(m.ps.viewYawCorrection * 65536.0f / 360.0f));
        m.deltaAngles[1] = (m.deltaAngles[1] + dy) & 0xffff;
        cmd.angles[1] = (cmd.angles[1] + dy) & 0xffff;
        m.ps.viewYawCorrection = 0.0f;
    }
    // The movement's player-state events (0x80e00d2): into the entity's ring for the other clients, the client's own
    // ring of the killcam archive, the listen host's ears, and ClientEvents' fall damage (MP server 0x80f52da).
    m.psEventsDone = game::resyncEventSeq(m.ps.eventSeq, m.psEventsDone);
    for (; m.psEventsDone < m.ps.eventSeq; ++m.psEventsDone) {
        const int ev = m.ps.events[m.psEventsDone & 3], parm = m.ps.eventParms[m.psEventsDone & 3];
        if (game::playerStateEventSentToEntity(ev)) {
            m.entityRing.push(ev, parm);
            w_->mpPushClientEvent(it->second, ev, std::string());
        }
        w_->otherPlayerMoveEventSound(*e, ev);
        w_->clientFallEvent(*e, ev, parm);
        e = w_->mpClientEntity(it->second);
        if (!e) return;
    }
    // The weapon side: fire, aim, reload, weapon change, melee, grenades (src/netgame/netcombat.cpp).
    std::string cmdWeapon;
    if (cmd.weapon > 0 && size_t(cmd.weapon) <= weapons_.names().size()) cmdWeapon = weapons_.names()[size_t(cmd.weapon) - 1];
    if (!m.combat->weaponIndex) m.combat->weaponIndex = [this](const std::string& n) { return weaponByte(n); };
    // The antilag time (0x8090e10 -> 0x80f5614): the command's server time, at most 1000 ms back; it only goes back in
    // time when it is still ahead of the current time. 0x80f546e fires with it while g_antilag is on.
    {
        const int now = serverTime();
        const int t = std::max(cmd.serverTime, now - 1000);
        if (!(t < m.lastServerTime && now > t)) m.lastServerTime = t;
        m.combat->antilagTime = w_->cvarFloat("g_antilag", 1.0f) != 0 ? m.lastServerTime : now;
        m.combat->antilagFine = m.subFrame;
    }
    if (!onTurret) m.combat->think(*w_, it->second, cmd, m.ps, usercmdMsec(prevTime, cmd.serverTime), cmdWeapon);
    // A script's weapon change goes to the client as the selection command (docs/spec/net.md section 10 "Weapon
    // selection"; switchToWeapon 0x80fa1b6 -> 0x8120e0a).
    if (const std::string sel = m.combat->takeScriptedSelect(); !sel.empty() && sendCommand)
        if (const int index = weaponByte(sel); index > 0) sendCommand(slot, "a " + std::to_string(index));
    {  // the weapon events of the command follow its movement events in the entity's ring
        const EventRing& r = m.combat->ring();
        int added = (r.sequence - m.combatEventsDone) & 0xff;
        if (added > 128) added = 0;  // the ring started again
        for (int s = (r.sequence - std::min(added, 4)) & 0xff; s != r.sequence; s = (s + 1) & 0xff)
            m.entityRing.push(r.events[size_t(s & 3)], r.parms[size_t(s & 3)]);
        m.combatEventsDone = r.sequence;
    }
    if (std::getenv("IW2_SOAK_SHOTTRACE")) log::tracef("net", "SRVT slot=%d cmd=%d sent=%d prev=%d btn=%x state=%d clip=%d tm=%.4f\n", slot, cmd.serverTime, sent.serverTime, prevTime, unsigned(cmd.buttons), m.combat->weapon().stateNumber(), m.combat->weapon().clip(), double(m.combat->weapon().timer()));
    e = w_->mpClientEntity(it->second);
    if (!e) return;
    m.lastOrigin = {m.ps.origin[0], m.ps.origin[1], m.ps.origin[2]};
    m.track.add(cmd.serverTime, m.lastOrigin);
    script::OriginWatch::allowed = true;
    e->obj->set("origin", Value(script::Vec3{m.lastOrigin[0], m.lastOrigin[1], m.lastOrigin[2]}));
    script::OriginWatch::allowed = false;
    e->obj->set("$clipheight", Value(m.ps.stance == game::Stance::Prone ? 30.0f : m.ps.stance == game::Stance::Crouch ? 50.0f : 70.0f));
    w_->mpLinkClient(it->second); // linked after the move: the next client's command sees it there (docs/spec/mp-pmove.md)
    if (netTrace && slot >= 0 && slot < script::OriginWatch::kSlots) {
        script::OriginWatch::enabled = true;
        script::OriginWatch::object[slot] = e->obj.get();
    }
    // Intermission ignores the view input (viewInputIgnored): the view stays where the server set it.
    if (!viewInputIgnored(m.ps.pmType)) {
        in = inputFromUsercmd(cmd); // with the prone yaw cap applied above
        m.viewPitch = in.pitch;
        m.viewYaw = in.yaw;
    }
    // A prone body lies along its body yaw, not the view (the leg direction of MP exe 0x515c30), so its hit boxes stay
    // where the body check put them. TODO(spec): the separate leg and torso yaws of the player animation.
    e->obj->set("angles", Value(script::Vec3{0.0f, m.ps.stance == game::Stance::Prone ? m.ps.proneYaw : m.viewYaw, 0.0f}));
    e->velocity = {m.ps.velocity[0], m.ps.velocity[1], m.ps.velocity[2]};
    w_->mpSetButtons(it->second, static_cast<uint32_t>(held.buttons));
    w_->mpSetView(it->second, script::Vec3{m.viewPitch, m.viewYaw, 0.0f}, m.ps.viewHeight);
    w_->mpSetViewState(it->second, game::mpViewStateOf(m.combat->weapon(), m.ps)); // the recorded view state (killcam)
    w_->mpSetClientGround(it->second, m.ps.onGround, m.combat->weapon().fuseMs(), m.combat->weapon().stateNumber(), m.ps.mantling);
    // The animation input of this client (src/game/mpplayers.cpp): stance and moves as for the local player.
    if (game::MpPlayerInput* pi = w_->mpPlayerInput(it->second)) {
        pi->valid = true;
        pi->move.viewHeightTarget = m.ps.viewHeightTarget;
        pi->move.xySpeed = std::sqrt(m.ps.velocity[0] * m.ps.velocity[0] + m.ps.velocity[1] * m.ps.velocity[1]);
        pi->move.forwardMove = in.forward;
        pi->move.rightMove = in.right;
        pi->move.slow = m.ps.adsSlow || m.ps.lean != 0;
        pi->onGround = m.ps.onGround ? 1 : 0;
    }
}

const NetCombat* WorldGame::combat(int slot) const {
    auto it = moves_.find(slot);
    return it == moves_.end() ? nullptr : it->second.combat.get();
}

void WorldGame::clientDisconnect(int slot) {
    auto it = mp_.find(slot);
    if (it == mp_.end()) return;
    w_->mpDisconnect(it->second);
    std::printf("client %d disconnected\n", slot);
    // What was sent to this connection does not hold for the next client of the slot.
    killcamReplayed_.erase(it->second);
    killcamEvents_.erase(it->second);
    sentOffhand_.erase(slot);
    mp_.erase(it);
    moves_.erase(slot);
    if (slot >= 0 && slot < script::OriginWatch::kSlots) script::OriginWatch::object[slot] = nullptr;
}

void WorldGame::runFrame(int frameMs) {
    if (clientPing)
        for (const auto& [slot, client] : mp_) w_->mpSetClientPing(client, clientPing(slot));
    // The network clients' places for the snapshots of this frame (docs/spec/net.md section 10 "Player positions in a
    // snapshot"): every command of the frame has run; the frame's archive and the snapshots after it read them. A host
    // that steps its own world has run the frame already (its archive keeps the entity origins).
    static const bool plainPlayers = std::getenv("IW2_PLAYER_PLAIN") != nullptr;
    const int snapshotTime = serverTime() + (worldOwnedByHost ? 0 : frameMs);
    for (const auto& [slot, client] : mp_) {
        auto mv = moves_.find(slot);
        EvenTrack::Vec at{};
        if (plainPlayers || mv == moves_.end() || !mv->second.havePs || mv->second.trackSampledAt == snapshotTime ||
            !mv->second.track.sample(snapshotTime, at)) {
            if (plainPlayers || mv == moves_.end() || !mv->second.havePs) w_->mpClearSentOrigin(client);
            continue;
        }
        mv->second.trackSampledAt = snapshotTime;
        const auto& live = mv->second.lastOrigin;
        w_->mpSetSentOrigin(client, script::Vec3{live[0], live[1], live[2]}, script::Vec3{at[0], at[1], at[2]});
    }
    if (!worldOwnedByHost) w_->frame();
    frameLenMs_ = frameMs;
    if (!worldOwnedByHost) noteFrame();
    forwardToClients();
}

// The places the last game frame left the clients at (the bots' way between two frames, prepareSubFrame), taken once
// per world time: after the frame, or when a host-owned world is seen to have stepped.
void WorldGame::noteFrame() {
    if (w_->timeMs() == frameSeenTime_) return;
    frameSeenTime_ = w_->timeMs();
    framePrev_ = std::move(frameCur_);
    frameCur_.clear();
    const auto& st = w_->mpState();
    for (size_t n = 0; n < st.clients.size(); ++n) {
        const auto& cl = st.clients[n];
        if (cl.state == game::MpClientState::Free || !cl.ent) continue;
        const Value o = cl.ent->obj->get("origin"), a = cl.ent->obj->get("angles");
        frameCur_[int(n)] = {o.isVector() ? o.asVector() : script::Vec3{}, a.isVector() ? a.asVector()[1] : 0.0f, cl.spawnCount};
    }
}

int WorldGame::subFrameMs(int64_t nowMs) {
    if (!worldOwnedByHost) return -1;
    if (w_->timeMs() != frameSeenTime_) {
        noteFrame();
        frameSeenAtMs_ = nowMs;
    }
    return int(std::clamp<int64_t>(nowMs - frameSeenAtMs_, 0, frameLenMs_ - 1));
}

// docs/spec/net.md section 10 "Snapshot rate": the clients' places at `serverTime() + subMs`. A network client's walk
// is sampled at that time (EvenTrack); a client the game frame moves (a bot) is on the way from the frame before to
// the last frame, by the part of the frame that has passed; the host's own player and a client without a track are
// where they are. The places are kept for the antilag rewind.
void WorldGame::prepareSubFrame(int subMs) {
    static const bool plainPlayers = std::getenv("IW2_PLAYER_PLAIN") != nullptr;
    subTime_ = serverTime() + subMs;
    subPlaces_.clear();
    std::vector<std::pair<int, script::Vec3>> record;
    const auto& st = w_->mpState();
    const float f = std::clamp(float(subMs) / float(std::max(frameLenMs_, 1)), 0.0f, 1.0f);
    for (size_t n = 0; n < st.clients.size(); ++n) {
        const auto& cl = st.clients[n];
        if (cl.state == game::MpClientState::Free || !cl.ent) continue;
        const Value o = cl.ent->obj->get("origin"), a = cl.ent->obj->get("angles");
        FramePlace p{o.isVector() ? o.asVector() : script::Vec3{}, a.isVector() ? a.asVector()[1] : 0.0f, cl.spawnCount};
        Move* mv = nullptr;
        for (const auto& [slot, client] : mp_)
            if (client == int(n))
                if (auto it = moves_.find(slot); it != moves_.end()) mv = &it->second;
        if (mv) {
            EvenTrack::Vec at{};
            const auto& live = mv->lastOrigin;
            const bool moved = std::fabs(p.origin[0] - live[0]) + std::fabs(p.origin[1] - live[1]) + std::fabs(p.origin[2] - live[2]) > 0.01f;
            if (!plainPlayers && mv->havePs && !moved && mv->track.sample(subTime_, at)) p.origin = {at[0], at[1], at[2]};
        } else if (int(n) != st.localClient) {
            const auto prev = framePrev_.find(int(n));
            const auto cur = frameCur_.find(int(n));
            // Only while he stands where the frame left him and did not spawn in it, and not over a placement.
            if (prev != framePrev_.end() && cur != frameCur_.end() && prev->second.spawnCount == cl.spawnCount &&
                cur->second.spawnCount == cl.spawnCount && cur->second.origin == p.origin) {
                const script::Vec3& a0 = prev->second.origin;
                const script::Vec3& a1 = cur->second.origin;
                const float d2 = (a1[0] - a0[0]) * (a1[0] - a0[0]) + (a1[1] - a0[1]) * (a1[1] - a0[1]) + (a1[2] - a0[2]) * (a1[2] - a0[2]);
                if (d2 < 100.0f * 100.0f) {
                    for (size_t k = 0; k < 3; ++k) p.origin[k] = a0[k] + (a1[k] - a0[k]) * f;
                    float dy = std::fmod(cur->second.yaw - prev->second.yaw, 360.0f);
                    if (dy > 180.0f) dy -= 360.0f;
                    if (dy < -180.0f) dy += 360.0f;
                    p.yaw = prev->second.yaw + dy * f;
                }
            }
        }
        subPlaces_[int(n)] = p;
        if (cl.state == game::MpClientState::Active) record.emplace_back(int(n), p.origin);
    }
    w_->mpRecordSentPlaces(subTime_, std::move(record));
}

bool WorldGame::subFrameAllowed(int slot) {
    const auto it = mp_.find(slot);
    return it == mp_.end() || !killcamFrame(it->second).frame;
}

net::SnapshotView WorldGame::snapshotAt(int slot, int subMs) {
    if (subTime_ != serverTime() + subMs) prepareSubFrame(subMs);
    subSnapshot_ = true;
    net::SnapshotView v = snapshot(slot);
    subSnapshot_ = false;
    return v;
}

int WorldGame::serverTime() const { return static_cast<int>(w_->timeMs()); }

std::map<int, std::vector<uint8_t>> WorldGame::baselines() {
    // The entities of the world as the gamestate is sent (no client's view): everything but players and events.
    net::SnapshotView v = snapshot(-1);
    std::map<int, std::vector<uint8_t>> out;
    for (auto& [n, es] : v.entities)
        if (n >= kFirstWireEntity && n < kFirstEventEntity) out[n] = std::move(es);
    // Radius triggers are in the 1.3 baselines (eType 0, origin and angles) though never in a snapshot (observed).
    for (const auto& e : w_->entities()) {
        if (e->obj->removed || e->mapIndex <= 0 || e->classname != "trigger_radius") continue;
        const int n = kFirstMapEntity + e->mapIndex - 1;
        if (n >= kFirstEventEntity || out.count(n)) continue;
        std::vector<uint8_t> es(size_t(net::kEntityStateSize), 0);
        const Value o = e->obj->get("origin"), a = e->obj->get("angles");
        for (int k = 0; k < 3; ++k) {
            if (o.isVector()) putF(es, 24 + 4 * k, o.asVector()[size_t(k)]);
            if (a.isVector()) putF(es, 60 + 4 * k, a.asVector()[size_t(k)]);
        }
        out[n] = std::move(es);
    }
    return out;
}

int WorldGame::wireNumber(const Entity& e) { return wireNumber(e.number, e.mapIndex); }

int WorldGame::wireNumber(int key, int mapIndex) {
    const struct { int number, mapIndex; } e{key, mapIndex};
    auto it = wireOf_.find(e.number);
    if (it != wireOf_.end()) return it->second;
    // A map entity keeps the number the 1.3 server gives it: 71 + its position in the map's entity list (clients
    // 0..63, then 8 more reserved, observed on mp_toujane).
    if (e.mapIndex > 0 && kFirstMapEntity + e.mapIndex - 1 < kFirstEventEntity &&
        !wireUsed_[size_t(kFirstMapEntity + e.mapIndex - 1)]) {
        const int n = kFirstMapEntity + e.mapIndex - 1;
        wireUsed_[size_t(n)] = true;
        wireOf_[e.number] = n;
        return n;
    }
    // Later entities take the lowest free number after the body queue, like the event entities (eventNumber): the
    // numbers of map-list entries the game never spawns (misc_model, light, info_null, ...) are free. On mp_carentan
    // the map list has over 1800 entries, so numbering after the whole list left no number for dropped weapons and
    // grenades (docs/spec/net.md section 11).
    if (const int n = freeNumber(); n >= 0) {
        wireOf_[e.number] = n;
        return n;
    }
    return -1;  // TODO(spec): the original's entity overflow
}

void WorldGame::writePlayerEntity(std::vector<uint8_t>& es, int client, const Entity& e) {
    RemoteEntity re;
    re.number = client;
    re.eType = kEtPlayer;
    re.clientNum = client;
    // The place the other clients get: his walk sampled evenly (docs/spec/net.md section 10 "Player positions in a
    // snapshot"), or the entity's origin.
    auto o = w_->mpSnapshotOrigin(client);
    const auto a = e.obj->get("angles");
    if (a.isVector()) re.angles[1] = a.asVector()[1];
    if (subSnapshot_)  // a snapshot between the frames: the place at its time ("Snapshot rate")
        if (const auto sp = subPlaces_.find(client); sp != subPlaces_.end()) {
            o = sp->second.origin;
            re.angles[1] = sp->second.yaw;
        }
    for (int i = 0; i < 3; ++i) re.origin[i] = o[size_t(i)];
    writeEntity(es, re);
    for (const auto& [s, n] : mp_)
        if (n == client)
            if (auto mv = moves_.find(s); mv != moves_.end()) {
                putF(es, 60, mv->second.viewPitch);  // apos.trBase[0]
                putF(es, 64, mv->second.ps.stance == game::Stance::Prone ? mv->second.ps.proneYaw : mv->second.viewYaw); // apos.trBase[1] (a prone body: its body yaw)
            }
    // eFlags (observed with the 1.3 server; VAs in the 1.3 Linux dedicated server): 4 crouched, 8 prone, 0x40 while
    // the attack button is held with the weapon ready or firing (set in the movement code at 0x80e8bc6..0x80e8c38,
    // which also requires pm_type != 5, no pm_flags 0x1000 and a further check at 0x80ed5fc, TODO(spec)).
    // A client that is not playing has no entity at all (see snapshot()).
    int flags = 0;
    const Value hp = e.obj->get("health");
    if (hp.isNumber() && hp.asFloat() <= 0) flags |= kEfDead;  // TODO(spec): a dead player is normally not playing
    if (client >= 0 && size_t(client) < w_->mpState().clients.size() && (w_->mpState().clients[size_t(client)].buttons & 1u))
        flags |= kEfFiring;
    if (const game::MpPlayerInput* pi = w_->mpPlayerInput(client); pi && pi->valid) {
        if (pi->move.viewHeightTarget <= 11) flags |= kEfProne;
        else if (pi->move.viewHeightTarget <= 40) flags |= kEfCrouch;
    }
    // A client on a map turret (the player state to entity state copy, 0x80dfe64): the turret bits of eFlags and the
    // turret's entity number in otherEntityNum.
    if (client >= 0 && size_t(client) < w_->mpState().clients.size()) {
        uint32_t bits = 0;
        int lock = 0;
        if (const game::Entity* t = w_->mpClientTurretUse(client, bits, lock)) {
            flags |= int(bits);
            putI(es, 116, wireNumber(*t));
        }
    }
    if (client >= 0 && size_t(client) < w_->mpState().clients.size()) flags |= game::mpTeleportBit(w_->mpState().clients[size_t(client)].spawnCount);
    putI(es, 8, flags);
    putI(es, 200, weaponNumber(e.obj->get("$weapon").toString()));
    // legsAnim (+204): the climb or mantle animation the server plays, which the client cannot tell from the rest
    // (docs/spec/mp-players.md "Ladder and mantle move types"; TODO(spec): the 1.3 numbering, as for the body).
    if (const int la = w_->mpLegsAnimWire(client); la > 0 && la < 1024) putI(es, 204, la);
    // The weapon events of a player (docs/spec/net.md section 13): the event ring, and the lean of a network player.
    if (auto o = otherRings_.find(client); o != otherRings_.end()) {
        const EventRing& r = o->second.second;
        putI(es, 164, r.sequence);
        for (int i = 0; i < 4; ++i) {
            putI(es, 168 + 4 * i, r.events[size_t(i)]);
            putI(es, 184 + 4 * i, r.parms[size_t(i)]);
        }
    }
    for (const auto& [s, n] : mp_)
        if (n == client)
            if (auto mv = moves_.find(s); mv != moves_.end()) {
                const EventRing& r = mv->second.entityRing;
                putI(es, 164, r.sequence);
                for (int i = 0; i < 4; ++i) {
                    putI(es, 168 + 4 * i, r.events[size_t(i)]);
                    putI(es, 184 + 4 * i, r.parms[size_t(i)]);
                }
                putF(es, 212, mv->second.ps.lean);
            }
    // The velocity in pos.trDelta, as the 1.3 server's player entity carries it (0x80f56f8; the client's run animation
    // reads it).
    for (int i = 0; i < 3; ++i) putF(es, 36 + 4 * i, e.velocity[size_t(i)]);
    // Trajectory and bounds as the 1.3 server sends a player (docs/spec/net.md section 11): pos.trType 3 with
    // trDuration 50, apos.trType 1, the packed standing box (x/y 15, down 1, up 70) in solid, groundEntityNum 1022 (the
    // world) on the ground. openiw2: trTime is the snapshot time, not the player's command time, so the trajectory
    // evaluates to the origin (no 50 ms lead along the velocity).
    putI(es, 12, 3);
    putI(es, 16, subSnapshot_ ? subTime_ : serverTime());
    putI(es, 20, 50);
    putI(es, 48, 1);
    // The top follows the client's stance (its move box, docs/spec/mp-pmove.md "Players against each other").
    putI(es, 156, 15 | (1 << 8) | ((int(w_->playerClipHeight(e)) + 32) << 16));
    bool onGround = true;
    for (const auto& [s, n] : mp_)
        if (n == client)
            if (auto mv = moves_.find(s); mv != moves_.end()) onGround = mv->second.ps.onGround;
    putI(es, 124, onGround ? 1022 : 1023);
}

void WorldGame::writeInventory(std::vector<uint8_t>& ps, Entity& e) {
    // Health in stats[0] (+300) as in Quake III; TODO(spec): the 1.3 stat layout.
    const Value hp = e.obj->get("health");
    putI(ps, 300, hp.isNumber() ? int32_t(hp.asFloat()) : 0);
    const Value mh = e.obj->get("maxhealth");
    putI(ps, 308, mh.isNumber() ? int32_t(mh.asFloat()) : 100);
    game::Inventory& inv = w_->inventory(e);
    // Weapons by wire weapon number: owned bits (+1348), ammo (+324) and clip (+836) per weapon number.
    for (const std::string& wn : inv.owned) {
        const int i = weaponNumber(wn);
        if (i <= 0 || i >= 64) continue;
        uint32_t bits;
        std::memcpy(&bits, ps.data() + 1348 + 4 * (i / 32), 4);
        bits |= 1u << (i % 32);
        std::memcpy(ps.data() + 1348 + 4 * (i / 32), &bits, 4);
        // Ammo and clip by ammo / clip slot (observed with the 1.3 server; TODO(spec): the slot assignment routine
        // near the "BG_FillInAmmoItems" error text).
        const int as = weaponAmmoSlot(*w_, weapons_.names(), wn, false), cs = weaponAmmoSlot(*w_, weapons_.names(), wn, true);
        if (auto it = inv.ammo.find(weaponAmmoName(*w_, wn, false)); it != inv.ammo.end() && as >= 0 && as < 64)
            putI(ps, 324 + 4 * as, it->second);
        if (auto it = inv.clip.find(weaponAmmoName(*w_, wn, true)); it != inv.clip.end() && cs >= 0 && cs < 64)
            putI(ps, 836 + 4 * cs, it->second);
    }
    // Weapon slots (+1364): the weapon number of slot k in byte k (observed: primary in byte 1, primaryb in byte 2).
    putI(ps, 1364, weaponNumber(inv.slot[1]) << 8 | weaponNumber(inv.slot[2]) << 16);
    // The first-person hands model (viewModel, +228): its model index.
    if (const Value vm = e.obj->get("$viewmodel"); vm.isString() && !vm.asString().empty())
        putI(ps, 228, models_.index(vm.asString()) + 1);
    putI(ps, 212, weaponNumber(e.obj->get("$weapon").toString()));
    putI(ps, 208, weaponNumber(inv.offhand));
}

// An event entity takes the lowest free entity number after the body queue (observed with the 1.3 server: bullet
// impacts at 72.., the numbers of map entities the game freed): not a living map entity's number, not one in use on
// the wire, not another living event's. TODO(spec): the server's free-slot rule (freed-time delay).
int WorldGame::eventNumber() { return freeNumber(); }

// The lowest number from 72 that no map entity of the world holds (living or deleted: the client spawns the same map
// entities and finds them by number), no wire entity and no living event. TODO(spec): the server's free-slot rule
// (freed-time delay).
int WorldGame::freeNumber() {
    std::vector<bool> taken(size_t(net::kMaxEntities), false);
    for (const auto& e : w_->entities())
        if (e->mapIndex > 0 && kFirstMapEntity + e->mapIndex - 1 < net::kMaxEntities)
            taken[size_t(kFirstMapEntity + e->mapIndex - 1)] = true;
    for (const Event& e : events_) taken[size_t(e.number)] = true;
    for (int n = kFirstMapEntity; n < kFirstEventEntity; ++n)
        if (!taken[size_t(n)] && !wireUsed_[size_t(n)]) {
            wireUsed_[size_t(n)] = true;
            return n;
        }
    return -1;
}

void WorldGame::pushObituary(const game::MpObituary& ob) {
    Event ev;
    ev.number = eventNumber();
    if (ev.number < 0) return;
    ev.time = serverTime();
    ev.es.assign(size_t(net::kEntityStateSize), 0);
    putI(ev.es, 4, kEtEvents + kEventObituary);
    putI(ev.es, 84, ev.time);
    putI(ev.es, 116, ob.victim);       // +0x74 victim (docs/spec/mp-hud.md)
    putI(ev.es, 120, ob.attacker < 0 ? 1022 : ob.attacker);  // +0x78 attacker, 1022 none
    putI(ev.es, 160, ob.modByte ? (ob.mod | 0x80) : weaponNumber(ob.weapon));
    std::erase_if(events_, [&](const Event& e) { return e.number == ev.number; });
    events_.push_back(std::move(ev));
}

// A missile explosion as a temporary event entity (docs/spec/net.md section 13): eType 10 + 0xbc (grenade) or
// 10 + 0xbd (rocket) at the explosion point, the weapon number in weapon, the surface type in surfaceType and the
// direction in origin2. TODO(spec): the 1.3 server explodes the missile entity itself and sends the direction as
// eventParm (a direction byte); these fields are openiw2's choice.
WorldGame::Event WorldGame::explosionEvent(bool rocket, const std::string& weapon, const game::Vec3& at, const game::Vec3& dir,
                                           int surface) {
    Event ev;
    ev.number = eventNumber();
    if (ev.number < 0) return ev;
    ev.time = serverTime();
    ev.es.assign(size_t(net::kEntityStateSize), 0);
    putI(ev.es, 4, kEtEvents + (rocket ? kEventRocketExplode : kEventGrenadeExplode));
    putI(ev.es, 84, ev.time);
    for (int k = 0; k < 3; ++k) {
        putF(ev.es, 24 + 4 * k, at[size_t(k)]);
        putF(ev.es, 92 + 4 * k, dir[size_t(k)]);
    }
    putI(ev.es, 136, surface & 0xff);
    putI(ev.es, 200, weaponNumber(weapon));
    return ev;
}

void WorldGame::pushExplosion(const game::MpState::Explosion& ex) {
    Event ev = explosionEvent(ex.rocket, ex.weapon, ex.at, ex.dir, ex.surface);
    if (ev.number < 0) return;
    std::erase_if(events_, [&](const Event& e) { return e.number == ev.number; });
    events_.push_back(std::move(ev));
}

WorldGame::KillcamFrame WorldGame::killcamFrame(int self) const {
    KillcamFrame k;
    const game::MpArchive* ar = w_->mpArchive();
    Entity* e = self >= 0 ? w_->mpClientEntity(self) : nullptr;
    if (!ar || !e || e->obj->get("sessionstate").toString() != "spectator") return k;
    const Value sc = e->obj->get("spectatorclient"), at = e->obj->get("archivetime");
    const int follow = w_->mpClientOfNumber(sc.isNumber() ? sc.asInt() : -1);
    // archivetime is kept as integer milliseconds (docs/spec/mp-hud.md "The fields").
    int ms = at.isNumber() ? int(std::trunc(at.asFloat() * 1000.0f)) : 0;
    if (follow < 0 || follow == self || ms <= 0) return k;
    const int64_t n = game::archiveLookup(*ar, ms, game::kMpServerFps);
    const game::ArchivedFrame* f = n >= 0 ? ar->frame(n) : nullptr;
    if (!f || size_t(follow) >= f->clients.size() || !f->clients[size_t(follow)].active) return k;
    k.frame = f;
    k.client = follow;
    k.number = n;
    return k;
}

void WorldGame::writeKillcamPlayerState(std::vector<uint8_t>& ps, const KillcamFrame& kc) {
    const game::ArchivedClient& c = kc.frame->clients[size_t(kc.client)];
    const game::MpViewState& vs = c.view;
    // The followed client's player state over the spectator's (0x80f6916): movement type normal, its movement flags
    // without 0x800000 and with the follow bit 0x400000 (here the stance and aim bits of the recorded view state), and its
    // client number (the copy takes the whole player state; docs/spec/mp-hud.md "The recorded view state"). The entity
    // flags are the followed client's with the spectator's own bit 0x100000; they are not archived here (0).
    putI(ps, 4, 0);
    int pmf = kPmfFollow;
    if (vs.viewHeightTarget <= 11) pmf |= kPmfProne;
    else if (vs.viewHeightTarget <= 40) pmf |= kPmfCrouch;
    if (vs.adsHeld) pmf |= kPmfAds;
    putI(ps, 12, pmf);
    putI(ps, 160, 0);
    putI(ps, 204, kc.client);
    for (int i = 0; i < 3; ++i) {
        putF(ps, 20 + 4 * i, c.origin[size_t(i)]);
        putF(ps, 32 + 4 * i, vs.velocity[size_t(i)]);
        putF(ps, 232 + 4 * i, c.viewAngles[size_t(i)]);
    }
    // The view heights: the recorded ones (the archived eye is the origin plus the same height).
    putI(ps, 244, vs.viewHeightTarget);
    putF(ps, 248, vs.viewHeight);
    putI(ps, 8, vs.bobCycle & 0xff);
    putF(ps, 76, vs.lean);
    putF(ps, 1492, vs.aimSpread);
    putI(ps, 300, c.health);
    putI(ps, 308, c.maxHealth);
    // The weapon side: only the followed client's current weapon with its clip and stock (what the archive keeps).
    // TODO(spec): its other weapons and offhand.
    std::fill(ps.begin() + 324, ps.begin() + 836 + 4 * 64, uint8_t(0));
    std::fill(ps.begin() + 1348, ps.begin() + 1368, uint8_t(0));
    const int wn = weaponNumber(c.weapon);
    putI(ps, 212, wn);
    // The view weapon state (docs/spec/mp-hud.md "The recorded view state"): weaponstate, the view animation's key index
    // in weaponAnim and its elapsed ms in aimDownSightDelay (openiw2's meaning of those two), the aim fraction, and the
    // offhand weapon field naming the view weapon while an offhand state runs.
    putI(ps, 216, vs.weaponState & 31);
    putI(ps, 1488, game::viewAnimKeyIndex(vs.animKey));
    putI(ps, 224, vs.animMs);
    putF(ps, 220, vs.adsFrac);
    putI(ps, 208, vs.weaponState >= 12 && vs.weaponState <= 16 && vs.viewWeapon != c.weapon ? weaponNumber(vs.viewWeapon) : 0);
    // The followed client's event ring (eventSequence +164, events[4] +168, eventParms[4] +184, the player-state field
    // table of the 1.3 server): the archived player state carries its own events, and a client that runs no prediction
    // plays them as its view's (docs/spec/mp-hud.md "Network clients"): its shots, reloads, melee blows and throws.
    putI(ps, 164, c.ring.sequence & 0xff);
    for (int i = 0; i < 4; ++i) {
        putI(ps, 168 + 4 * i, c.ring.events[size_t(i)]);
        putI(ps, 184 + 4 * i, weaponByte(c.ring.weapons[size_t(i)]));
    }
    // The followed client's shell shock comes with its player state (the spectator's own one is not shown); its start
    // moves by the replay's delay, as the snapshot time stays live.
    putI(ps, 1496, c.shockIndex);
    putI(ps, 1500, c.shockIndex ? int32_t(c.shockStartMs + (serverTime() - kc.frame->timeMs)) : 0);
    putI(ps, 1504, c.shockIndex ? c.shockDurationMs : 0);
    // Its damage feedback and spawn count, as the copy of the whole player state has them (docs/spec/mp-hud.md "Hit
    // direction and damage feedback"): the viewer gets the followed client's flash, kick and icons.
    putI(ps, 284, c.damageEvent & 0xff);
    putI(ps, 288, c.damageYaw & 0xff);
    putI(ps, 292, c.damagePitch & 0xff);
    putI(ps, 296, c.damageCount & 0x7f);
    putI(ps, 320, c.spawnCount & 0xff);
    putI(ps, 228, c.hands.empty() ? 0 : models_.index(c.hands) + 1);
    if (wn > 0 && wn < 64) {
        putI(ps, 1348 + 4 * (wn / 32), int32_t(1u << (wn % 32)));
        putI(ps, 1364, wn << 8);
        const int as = weaponAmmoSlot(*w_, weapons_.names(), c.weapon, false), cs = weaponAmmoSlot(*w_, weapons_.names(), c.weapon, true);
        if (as >= 0 && as < 64) putI(ps, 324 + 4 * as, c.stock);
        if (cs >= 0 && cs < 64) putI(ps, 836 + 4 * cs, c.clip);
    }
}

net::SnapshotView WorldGame::snapshot(int slot) {
    net::SnapshotView v;
    v.serverTime = subSnapshot_ ? subTime_ : serverTime();
    if (auto mv = moves_.find(slot); mv != moves_.end()) mv->second.subFrame = subSnapshot_;
    v.ps.assign(size_t(net::kPlayerStateSize), 0);
    const int self = mp_.count(slot) ? mp_[slot] : -1;
    static const Move noMove{};
    const auto mit = moves_.find(slot);
    const Move& m = mit == moves_.end() ? noMove : mit->second;
    putI(v.ps, 0, m.lastTime);    // commandTime: the last command run (client prediction)
    putI(v.ps, 8, m.ps.bobCycle & 0xff); // bobCycle (the client's gun and view bob, docs/spec/net.md)
    putI(v.ps, 4, m.ps.pmType);   // pm_type
    int turretWire = 1023, turretLock = 0;
    // pm_flags: 1 prone, 2 crouch, 0x800000 while playing, 0x1000000 as a spectator; eFlags (+160): 4 / 8 for the
    // stance, 0x40 while firing, 2 as a spectator (observed with the 1.3 server). TODO(spec): pm_flags 0x80000, which
    // appears some time after going prone.
    {
        const int stanceFlags = m.ps.stance == game::Stance::Prone ? kPmfProne : m.ps.stance == game::Stance::Crouch ? kPmfCrouch : 0;
        const bool playing = m.ps.pmType == 0;
        const bool frozenNow = self >= 0 && size_t(self) < w_->mpState().clients.size() && w_->mpState().clients[size_t(self)].controlsFrozen;
        putI(v.ps, 12, stanceFlags | (playing ? 0x800000 : m.ps.pmType == 4 ? 0x1000000 : 0) | (frozenNow ? 0x8000 : 0));
        int ef = stanceFlags == kPmfProne ? kEfProne : stanceFlags == kPmfCrouch ? kEfCrouch : 0;
        if (self >= 0 && size_t(self) < w_->mpState().clients.size() && (w_->mpState().clients[size_t(self)].buttons & 1u) && playing)
            ef |= kEfFiring;
        if (m.ps.pmType == 4) ef = 2;
        else if (self >= 0 && size_t(self) < w_->mpState().clients.size()) ef |= game::mpTeleportBit(w_->mpState().clients[size_t(self)].spawnCount);
        // On a map turret (mount 0x810c3de, release 0x810ba83): the turret bits by the turret stance (0x100 prone,
        // 0x200 crouch, 0x300 stand), viewLockMode 1 (2 in a think that fired, 0x810b83f) and the turret's entity
        // number in viewLockEntity (1023 otherwise, below).
        uint32_t bits = 0;
        if (const game::Entity* t = self >= 0 ? w_->mpClientTurretUse(self, bits, turretLock) : nullptr) {
            ef |= int(bits);
            turretWire = wireNumber(*t);
        }
        putI(v.ps, 160, ef);
    }
    for (int i = 0; i < 3; ++i) {
        putF(v.ps, 20 + 4 * i, m.ps.origin[size_t(i)]);
        putF(v.ps, 32 + 4 * i, m.ps.velocity[size_t(i)]);
    }
    putF(v.ps, 232, m.viewPitch);  // viewangles
    putF(v.ps, 236, m.viewYaw);
    for (int k = 0; k < 3; ++k) putI(v.ps, 84 + 4 * k, m.deltaAngles[k] & 0xffff);  // delta_angles
    putI(v.ps, 204, std::max(0, self));  // clientNum
    // groundEntityNum: 1022 (the world) on the ground, 1023 none (observed with the 1.3 server; standing on another
    // entity is not tracked, TODO(spec)).
    putI(v.ps, 96, m.ps.onGround ? 1022 : 1023);
    // Fields the 1.3 server always fills (observed on mp_toujane, docs/spec/net.md section 11): view heights, the
    // movement speed and gravity, the player bounds, no view lock (1023) and no hint string (-1).
    if (m.combat) {  // the weapon timer and state of the last command, for the client's prediction (docs/spec/net.md section 10)
        putI(v.ps, 52, std::clamp(int(std::lround(std::max(0.0f, m.combat->weapon().timer()) * 1000.0f)), 0, 32767));
        putI(v.ps, 216, m.combat->weapon().stateNumber() & 31);
        // The client's own weapon events (eventSequence +164, events +168, eventParms +184): its prediction counts its
        // shots by them.
        const EventRing& r = m.combat->ring();
        putI(v.ps, 164, r.sequence & 0xff);
        for (int i = 0; i < 4; ++i) {
            putI(v.ps, 168 + 4 * i, r.events[size_t(i)]);
            putI(v.ps, 184 + 4 * i, r.parms[size_t(i)]);
        }
    }
    putI(v.ps, 244, m.ps.viewHeightTarget);       // targetViewHeight (integer-coded on the wire)
    putF(v.ps, 248, m.ps.viewHeight);             // currentViewHeight
    putI(v.ps, 80, 190);                          // speed (g_speed)
    putI(v.ps, 72, 800);                          // gravity (g_gravity)
    putF(v.ps, 1388, -15.0f);
    putF(v.ps, 1392, -15.0f);
    putF(v.ps, 1400, 15.0f);
    putF(v.ps, 1404, 15.0f);
    // boundsMax[2]: 70 in every stance (observed: the player state's box does not follow the stance).
    putF(v.ps, 1408, 70.0f);
    putI(v.ps, 1424, turretLock);                 // viewLockMode
    putI(v.ps, 1428, turretWire);                 // viewLockEntity
    putI(v.ps, 1436, -1);                         // hintStringIndex
    // The client's use search (0x8121bc6): hintType +0x598 (a weapon hint is the weapon number + 4), hintStringIndex
    // +0x59c into the hint string configstrings.
    if (self >= 0 && size_t(self) < w_->mpState().clients.size()) {
        const game::MpClient& uc = w_->mpState().clients[size_t(self)];
        int hint = uc.useHint;
        if (!uc.useHintWeapon.empty()) hint = weaponNumber(uc.useHintWeapon) > 0 ? weaponNumber(uc.useHintWeapon) + 4 : 0;
        putI(v.ps, 1432, hint);
        if (hint != 0 && !uc.useHintText.empty()) putI(v.ps, 1436, hints_.index(uc.useHintText));
    }
    Entity* selfEnt = self >= 0 ? w_->mpClientEntity(self) : nullptr;
    if (selfEnt) writeInventory(v.ps, *selfEnt);
    // The client's own shell shock (shockType / shockStartTime / shockDuration, ps+0x5d8 / +0x5dc / +0x5e0).
    if (self >= 0 && self < int(w_->mpState().clients.size())) {
        const game::MpClient& sc = w_->mpState().clients[size_t(self)];
        putI(v.ps, 144, sc.damageTimerMs);        // damageFlashTimer (finishPlayerDamage)
        putI(v.ps, 148, sc.damageDurationMs);
        putI(v.ps, 152, int(sc.flinchYaw));
        putI(v.ps, 284, sc.damageEvent & 0xff);   // damageEvent .. damageCount (P_DamageFeedback)
        putI(v.ps, 288, sc.damageYaw & 0xff);
        putI(v.ps, 292, sc.damagePitch & 0xff);
        putI(v.ps, 296, sc.damageCount & 0x7f);
        putI(v.ps, 320, sc.spawnCount & 0xff);    // stats[5]: the spawn count (ClientSpawn 0x80f926d..0x80f92d5)
        putI(v.ps, 1496, sc.shockIndex);
        putI(v.ps, 1500, int32_t(sc.shockStartMs));
        putI(v.ps, 1504, sc.shockDurationMs);
    }

    // The killcam (docs/spec/mp-hud.md "Killcam"): a spectator following a client with a positive archivetime gets the
    // followed client's player state of the archived frame archivetime ago as its own (0x80f6916), and the other
    // players at that frame (0x809a408): the client draws the replay as a live view and needs no replay code.
    const KillcamFrame kc = killcamFrame(self);
    if (kc.frame) writeKillcamPlayerState(v.ps, kc);

    // Every connected MP client (network players, the listen server's own player, bots): client state; the others
    // also as player entities.
    const auto& st = w_->mpState();
    const std::string selfTeam = selfEnt ? selfEnt->obj->get("sessionteam").toString() : std::string();
    for (size_t n = 0; n < st.clients.size() && n < 64; ++n) {
        const auto& cl = st.clients[n];
        if (cl.state == game::MpClientState::Free || !cl.ent) continue;
        std::vector<uint8_t> cs(size_t(net::kClientStateSize), 0);
        const std::string name = cl.ent->obj->get("name").toString();
        std::memcpy(cs.data() + 60, name.data(), std::min<size_t>(name.size(), 31));
        // Team (+4, 2 bits): 1 axis, 2 allies, 3 spectator (game::mpTeamNumber). TODO(spec): the 1.3 coding.
        putI(cs, 4, game::mpTeamNumber(cl.ent->obj->get("sessionteam").toString()) & 3);
        // Body model (modelindex, +8) and the attached models in attach order (+12..+32) as model indices
        // (observed: body, head, helmet).
        if (const Value md = cl.ent->obj->get("model"); md.isString() && !md.asString().empty())
            putI(cs, 8, models_.index(md.asString()) + 1);
        for (size_t a = 0; a < cl.ent->attachments.size() && a < 6; ++a)
            putI(cs, 12 + 4 * int(a), models_.index(cl.ent->attachments[a].model) + 1);
        v.clients[int(n)] = cs;
        // The killcam: the players of the archived frame, the spectator's own one included (the archived entity list
        // skips only the snapshot player state's client, which is the followed one, 0x8098f34), not the followed one
        // (its view is the camera).
        if (kc.frame) {
            if (int(n) == kc.client || n >= kc.frame->clients.size() || !kc.frame->clients[n].active) continue;
            const game::ArchivedClient& ac = kc.frame->clients[n];
            const game::ArchivedEntity* ae = kc.frame->find(ac.entity);
            if (!ae || ae->sessionstate != "playing") continue;
            // The puppet carries its archived state, not the live one: place, view, velocity, stance, lean, weapon and the
            // event ring of that frame (so its shots and reloads of the replay play, not the live ones).
            std::vector<uint8_t> es;
            writePlayerEntity(es, int(n), *cl.ent);
            for (int k = 0; k < 3; ++k) {
                putF(es, 24 + 4 * k, ac.origin[size_t(k)]);
                putF(es, 36 + 4 * k, ac.view.velocity[size_t(k)]);
            }
            putF(es, 60, ac.viewAngles[0]);
            putF(es, 64, ac.viewAngles[1]);
            int flags = getI(es.data(), 8) & ~(kEfFiring | kEfDead | kEfProne | kEfCrouch);
            if (ac.view.viewHeightTarget <= 11) flags |= kEfProne;
            else if (ac.view.viewHeightTarget <= 40) flags |= kEfCrouch;
            putI(es, 8, flags);
            putI(es, 200, weaponNumber(ac.weapon));
            putI(es, 204, w_->mpLegsAnimWire(int(n), &ac.view) & 1023); // its climb or mantle of that frame
            putF(es, 212, ac.view.lean);
            putI(es, 164, ac.ring.sequence & 0xff);
            for (int i = 0; i < 4; ++i) {
                putI(es, 168 + 4 * i, ac.ring.events[size_t(i)]);
                putI(es, 184 + 4 * i, weaponByte(ac.ring.weapons[size_t(i)]));
            }
            v.entities[int(n)] = es;
            continue;
        }
        if (int(n) == self) continue;
        // Spectators and dead clients are not sent as entities (observed: a client appears once it plays).
        if (cl.ent->obj->get("sessionstate").toString() != "playing") continue;
        std::vector<uint8_t> es;
        writePlayerEntity(es, int(n), *cl.ent);
        v.entities[int(n)] = es;
    }

    // Bodies of the body queue: number 64 + slot, eType 2, the client, eFlags with bit 2 toggled on each reuse of the slot
    // (VAs in the 1.3 Linux dedicated server: 0x811eaa4, cloneplayer 0x80fcc76; observed 0xa0002 / 0x20002).
    // legsAnim (+204) / torsoAnim (+208): the dead client's animations (cloneplayer copies ps+0x7c / ps+0x84, 0x80fcf1d).
    // openiw2 sends the death animation's node in the multiplayer tree in both; TODO(spec): the 1.3 animation numbering
    // and its toggle bit.
    auto sendBody = [&](int slot, int client, const script::Vec3& o, const script::Vec3& a, int anim, int flip, bool animating, const Trajectory* fall = nullptr) {
        if (slot < 0 || slot > 7) return;
        std::vector<uint8_t> es(size_t(net::kEntityStateSize), 0);
        RemoteEntity re;
        re.number = kFirstBodyEntity + slot;
        re.eType = kEtCorpse;
        re.clientNum = client;
        for (int i = 0; i < 3; ++i) {
            re.origin[i] = o[size_t(i)];
            re.angles[i] = a[size_t(i)];
        }
        writeEntity(es, re);
        if (fall) writeTrajectory(es, kPosTrajectory, *fall);
        putI(es, 8, 0x20000 | (animating ? kEfBodyNew : 0) | flip);
        if (anim >= 0 && anim < 1024) {
            putI(es, 204, anim);
            putI(es, 208, anim);
        }
        v.entities[re.number] = es;
    };
    // Script entities with a model (grenades, flags, the bomb, bodies, moved map models).
    std::set<int> live;
    for (const auto& e : w_->entities()) {
        if (e->isPlayer || e->obj->removed) continue;
        // The killcam: the missiles come from the archived frame (below), not the live ones.
        if (kc.frame && (e->grenade || e->rocket || e->classname.rfind("weapon_", 0) == 0)) continue; // missiles and items of the archive
        const Value model = e->obj->get("model");
        // Entities with a model, and script models without one (sent with typeIndex 0, observed).
        const bool hasModel = model.isString() && !model.asString().empty();
        // Script models and script origins are sent also without a model (observed: the two script origins the
        // exploder effects of mp_toujane spawn, 617 and 618, eType 6 with typeIndex 0).
        if (!hasModel && e->classname != "script_model" && e->classname != "script_origin") continue;
        const bool brush = hasModel && model.asString()[0] == '*';
        // Triggers without a brush model are not sent, whatever their model key says (observed: the ctf flag
        // radius triggers; they are in the baselines only).
        if (!brush && e->classname.rfind("trigger_", 0) == 0) continue;
        // A brush entity whose box lies outside the world's bounds is not sent (observed: the sd bomb trigger, placed
        // outside the map and moved further away by triggerOff, is in neither baselines nor snapshots).
        // TODO(spec): the server's exact rule (entity linking).
        if (brush) {
            game::Vec3 wmin, wmax, bmin, bmax;
            const auto ov0 = e->obj->get("origin");
            if (w_->worldBounds(wmin, wmax) && w_->submodelBounds(model.asString(), bmin, bmax) && ov0.isVector()) {
                const auto o0 = ov0.asVector();
                bool outside = false;
                for (int k = 0; k < 3; ++k)
                    outside = outside || bmin[size_t(k)] + o0[size_t(k)] > wmax[size_t(k)] || bmax[size_t(k)] + o0[size_t(k)] < wmin[size_t(k)];
                if (outside) continue;
            }
        }
        const Value ov = e->obj->get("origin");
        if (!ov.isVector()) continue;
        auto o = ov.asVector();
        const Value av = e->obj->get("angles");
        auto a = av.isVector() ? av.asVector() : script::Vec3{0, 0, 0};
        bool hidden = e->obj->get("$hidden").truthy();
        // The killcam shows a moved model (a flag, a script model) where it was at the shown time.
        if (kc.frame)
            for (const game::ArchivedEntity& ae : kc.frame->entities)
                if (ae.number == e->number) {
                    o = ae.origin, a = ae.angles, hidden = ae.hidden;
                    break;
                }
        // Bodies of the body queue: number 64 + slot, eType 2, the client, eFlags 0xa0000 with bit 2 toggled on each
        // reuse of the slot (VAs in the 1.3 Linux dedicated server: 0x811eaa4, cloneplayer 0x80fcc76; observed
        // 0xa0002 / 0x20002; the body's think clears 0x80000 at the end of the death animation).
        if (e->classname == "playercorpse") {
            // The killcam: the bodies of the archived frame (below), not the live ones.
            const Value sl = e->obj->get("$bodyslot"), bc = e->obj->get("$bodyclient"), da = e->obj->get("$deathanim");
            Trajectory fall;
            const Trajectory* fallPtr = nullptr;
            if (e->obj->get("$trtype").isNumber() && e->obj->get("$trtype").asInt() == 5) { // still in the air: the client evaluates it
                fall.type = 5;
                fall.time = int(e->obj->get("$trtime").asFloat());
                const auto b = e->obj->get("$trbase").asVector(), d = e->obj->get("$trdelta").asVector();
                for (int k = 0; k < 3; ++k) fall.base[k] = b[size_t(k)], fall.delta[k] = d[size_t(k)];
                fallPtr = &fall;
            }
            if (!kc.frame && sl.isNumber())
                sendBody(sl.asInt(), bc.isNumber() ? bc.asInt() : 0, o, a, da.isNumber() ? da.asInt() : -1, e->obj->get("$bodyflip").asInt(),
                         w_->timeMs() < e->obj->get("$bodythink").asInt(), fallPtr);
            continue;
        }
        const int wn = wireNumber(*e);
        if (wn < 0) continue;
        live.insert(e->number);
        std::vector<uint8_t> es(size_t(net::kEntityStateSize), 0);
        RemoteEntity re;
        re.number = wn;
        re.eType = kEtGeneral;
        for (int i = 0; i < 3; ++i) {
            re.origin[i] = o[size_t(i)];
            re.angles[i] = a[size_t(i)];
        }
        writeEntity(es, re);
        putI(es, 8, hidden ? kEfHidden : 0);
        // Missiles and falling items carry their trajectory (docs/spec/net.md section 11 "Trajectories"): the client
        // evaluates it in its own time instead of stepping the snapshot positions.
        if (e->grenade) {
            const game::GrenadeState& g = *e->grenade;
            // A grenade held in a hand (cooking) follows the hand: stationary at its origin, not its throw parabola.
            const bool flies = g.trType == 5 && !g.inHand;
            Trajectory tr;
            tr.type = flies ? 5 : 0;
            tr.time = int(g.trTime);
            for (int k = 0; k < 3; ++k) {
                tr.base[k] = flies ? g.trBase[size_t(k)] : o[size_t(k)];
                tr.delta[k] = flies ? g.trDelta[size_t(k)] : 0.0f;
            }
            writeTrajectory(es, kPosTrajectory, tr);
            if (g.trType != 0 && !g.inHand) {
                Trajectory ar;
                ar.type = 2;
                ar.time = int(g.aTime);
                for (int k = 0; k < 3; ++k) {
                    ar.base[k] = g.aBase[size_t(k)];
                    ar.delta[k] = g.aDelta[size_t(k)];
                }
                writeTrajectory(es, kAposTrajectory, ar);
            }
            // The surface type of its last impact (0x810d54f) and its bounce events (docs/spec/net.md section 11).
            putI(es, 136, g.impactSurface);
            if (auto r = grenadeRings_.find(e->number); r != grenadeRings_.end() && !kc.frame) {
                const EventRing& ring = r->second.second;
                putI(es, 164, ring.sequence);
                for (int i = 0; i < 4; ++i) {
                    putI(es, 168 + 4 * i, ring.events[size_t(i)]);
                    putI(es, 184 + 4 * i, ring.parms[size_t(i)]);
                }
            }
        } else if (e->rocket) {
            const game::RocketState& r = *e->rocket;
            Trajectory tr;
            tr.type = 2;
            tr.time = int(r.trTime);
            for (int k = 0; k < 3; ++k) {
                tr.base[k] = r.trBase[size_t(k)];
                tr.delta[k] = r.trDelta[size_t(k)];
            }
            writeTrajectory(es, kPosTrajectory, tr);
        } else if (const Value tt = e->obj->get("$trtype"); tt.isNumber() && (tt.asInt() == 3 || tt.asInt() == 5)) {
            const Value tb = e->obj->get("$trbase"), td = e->obj->get("$trdelta"), tm = e->obj->get("$trtime"),
                        du = e->obj->get("$trduration");
            if (tb.isVector() && td.isVector()) {
                Trajectory tr;
                tr.type = tt.asInt();
                tr.time = tm.isNumber() ? int(tm.asFloat()) : 0;
                tr.duration = du.isNumber() ? du.asInt() : 0;
                for (int k = 0; k < 3; ++k) {
                    tr.base[k] = tb.asVector()[size_t(k)];
                    tr.delta[k] = td.asVector()[size_t(k)];
                }
                writeTrajectory(es, kPosTrajectory, tr);
                if (const Value ra = e->obj->get("$arate"); ra.isVector()) { // the spin of a dropped weapon
                    Trajectory ar;
                    ar.type = 2;
                    ar.time = int(e->obj->get("$atime").asFloat());
                    for (int k = 0; k < 3; ++k) {
                        ar.base[k] = e->obj->get("$abase").asVector()[size_t(k)];
                        ar.delta[k] = ra.asVector()[size_t(k)];
                    }
                    writeTrajectory(es, kAposTrajectory, ar);
                }
            }
        }
        if (brush) {
            // Brush models: typeIndex the inline model number, solid 0xffffff; triggers are eType 0 with eFlags 1
            // (observed: a script brush model is eType 6, a trigger_hurt eType 0).
            putI(es, 140, std::atoi(model.asString().c_str() + 1));
            putI(es, 156, 0xffffff);
            if (e->classname.rfind("trigger_", 0) == 0) {
                putI(es, 4, 0);
                putI(es, 8, 1);
            }
        } else if (hasModel) {
            putI(es, 140, models_.index(model.asString()) + 1);       // typeIndex: model
        }
        // Dropped weapons (weapon_<name> items): eType 3 with the weapon number in typeIndex (observed).
        if (e->classname.rfind("weapon_", 0) == 0) {
            putI(es, 4, kEtItem);
            putI(es, 140, weaponNumber(e->classname.substr(7)));
        }
        // Turrets: eType 9 with the turret weapon (observed with the 1.3 server).
        if (const Value wi = e->obj->get("weaponinfo"); wi.isString() && !wi.asString().empty()) {
            putI(es, 4, kEtTurret);
            putI(es, 200, weaponNumber(wi.asString()));
            // The turret's aim (pitch, yaw relative to its angles) goes in angles2 (the turret think, 0x810b729..
            // 0x810b79d and 0x810c562..0x810c5d6).
            if (e->turret) {
                putF(es, 104, e->turret->pitch);
                putF(es, 108, e->turret->yaw);
                // The fire loop while the fire countdown runs (0x810b926: loopSound = loopFireSound's index).
                if (!e->turret->loopSound.empty())
                    if (const int li = sounds_.index(weaponKey(*w_, e->turret->weapon, "loopfiresound")); li >= 0) putI(es, 132, li + 1);
                if (auto r = turretRings_.find(e->number); r != turretRings_.end()) {
                    const EventRing& ring = r->second.second;
                    putI(es, 164, ring.sequence);
                    for (int i = 0; i < 4; ++i) {
                        putI(es, 168 + 4 * i, ring.events[size_t(i)]);
                        putI(es, 184 + 4 * i, ring.parms[size_t(i)]);
                    }
                }
            }
        }
        v.entities[wn] = es;
    }
    // The killcam: the missiles of the archived frame (the original's snapshot takes every entity from it, 0x809a408),
    // also those freed since (an exploded grenade), with their trajectories moved by the replay's delay, as the
    // snapshot time stays live. The wire numbers are openiw2's own (the key of an archived missile is apart from the
    // live entities'). TODO(spec): the original sends the archived entity numbers.
    if (kc.frame) {
        const int shift = serverTime() - int(kc.frame->timeMs);
        for (const game::ArchivedEntity& ae : kc.frame->entities) {
            // The bodies of that frame: none yet for a client still alive in it, also those the queue reused since.
            if (ae.bodySlot >= 0) sendBody(ae.bodySlot, ae.bodyClient, ae.origin, ae.angles, ae.bodyAnim, ae.bodyFlip, kc.frame->timeMs < ae.bodyThinkMs);
            if (!ae.itemWeapon.empty()) { // a dropped weapon of that frame (the live one may be gone or not dropped yet)
                const int itemKey = kKillcamMissileKey - ae.number;
                const int itemWire = wireNumber(itemKey, -1);
                if (itemWire < 0) continue;
                live.insert(itemKey);
                std::vector<uint8_t> ies(size_t(net::kEntityStateSize), 0);
                RemoteEntity ire;
                ire.number = itemWire;
                ire.eType = kEtItem;
                for (int i = 0; i < 3; ++i) {
                    ire.origin[i] = ae.origin[size_t(i)];
                    ire.angles[i] = ae.angles[size_t(i)];
                }
                writeEntity(ies, ire);
                putI(ies, 140, weaponNumber(ae.itemWeapon));
                v.entities[itemWire] = ies;
                continue;
            }
            if (ae.missile == 0 || ae.model.empty()) continue;
            const int key = kKillcamMissileKey - ae.number;
            const int wn = wireNumber(key, -1);
            if (wn < 0) continue;
            live.insert(key);
            std::vector<uint8_t> es(size_t(net::kEntityStateSize), 0);
            RemoteEntity re;
            re.number = wn;
            re.eType = kEtGeneral;
            for (int i = 0; i < 3; ++i) {
                re.origin[i] = ae.origin[size_t(i)];
                re.angles[i] = ae.angles[size_t(i)];
            }
            writeEntity(es, re);
            putI(es, 8, ae.hidden ? kEfHidden : 0);
            Trajectory tr;
            tr.type = ae.trType;
            tr.time = int(ae.trTime) + shift;
            for (int k = 0; k < 3; ++k) {
                tr.base[k] = ae.trBase[size_t(k)];
                tr.delta[k] = ae.trDelta[size_t(k)];
            }
            writeTrajectory(es, kPosTrajectory, tr);
            if (ae.aType != 0) {
                Trajectory ar;
                ar.type = ae.aType;
                ar.time = int(ae.aTime) + shift;
                for (int k = 0; k < 3; ++k) {
                    ar.base[k] = ae.aBase[size_t(k)];
                    ar.delta[k] = ae.aDelta[size_t(k)];
                }
                writeTrajectory(es, kAposTrajectory, ar);
            }
            putI(es, 140, models_.index(ae.model) + 1);
            v.entities[wn] = es;
        }
    }
    // Looped effects (playLoopedFx): eType 8 with the effect index in scale, the repeat delay in ms in angles2[1] and
    // the up direction in origin2 (observed with the 1.3 server: numbers after the map's entities).
    {
        const auto fx = w_->loopedFxList();
        for (size_t i = 0; i < fx.size(); ++i) {
            const int key = -1 - int(i);
            const int wn = wireNumber(key, -1);
            if (wn < 0) continue;
            live.insert(key);
            std::vector<uint8_t> es(size_t(net::kEntityStateSize), 0);
            putI(es, 4, kEtLoopFx);
            for (int k = 0; k < 3; ++k) putF(es, 24 + 4 * k, std::get<2>(fx[i])[size_t(k)]);
            putI(es, 216, std::get<0>(fx[i]) + 1);
            putF(es, 108, std::round(std::get<1>(fx[i]) * 1000.0f));  // angles2[1]
            putF(es, 100, 1.0f);                                         // origin2[2]
            v.entities[wn] = es;
        }
    }
    // Wire numbers of entities that are gone become free again.
    for (auto it = wireOf_.begin(); it != wireOf_.end();) {
        bool exists = live.count(it->first) != 0;
        if (!exists) {
            for (const auto& e : w_->entities())
                if (e->number == it->first && !e->obj->removed) exists = true;
        }
        if (exists) {
            ++it;
            continue;
        }
        wireUsed_[size_t(it->second)] = false;
        it = wireOf_.erase(it);
    }

    // Event entities (obituaries) for kEventLifeMs.
    std::erase_if(events_, [&](const Event& e) {
        const bool gone = v.serverTime - e.time > kEventLifeMs || e.time > v.serverTime;
        if (gone && e.number >= 0 && e.number < kFirstEventEntity) wireUsed_[size_t(e.number)] = false;
        return gone;
    });
    // The killcam replays the explosions of the archived frames instead of the live ones: a new event entity for each
    // explosion of a frame newly shown to this spectator (the original sends the archived snapshot's events again).
    for (const Event& e : events_) {
        const int type = getI(e.es.data(), 4) & 0xff;
        if (kc.frame && (type == kEtEvents + kEventGrenadeExplode || type == kEtEvents + kEventRocketExplode)) continue;
        v.entities[e.number] = e.es;
    }
    if (self >= 0) {
        std::vector<Event>& mine = killcamEvents_[self];
        std::erase_if(mine, [&](const Event& e) {
            const bool gone = v.serverTime - e.time > kEventLifeMs || e.time > v.serverTime || !kc.frame;
            if (gone && e.number >= 0 && e.number < kFirstEventEntity) wireUsed_[size_t(e.number)] = false;
            return gone;
        });
        int64_t& replayed = killcamReplayed_.try_emplace(self, -1).first->second;
        if (kc.frame) {
            const game::MpArchive* ar = w_->mpArchive();
            const auto [from, to] = game::killcamReplayRange(replayed, kc.number);
            replayed = kc.number;
            for (int64_t n = from; n <= to; ++n)
                if (const game::ArchivedFrame* f = ar ? ar->frame(n) : nullptr)
                    for (const game::ArchivedEvent& ae : f->events)
                        if (ae.kind == game::ArchivedEvent::Explosion)
                            if (Event ev = explosionEvent(ae.rocket, ae.name, ae.a, ae.b, ae.surface); ev.number >= 0)
                                mine.push_back(std::move(ev));
        } else {
            replayed = -1;
        }
        for (const Event& e : mine) v.entities[e.number] = e.es;
    }

    // The client's HUD elements (newHudElem for all, newClientHudElem for it, newTeamHudElem for its team) and objectives.
    if (selfEnt) {
        HudStrings hs{&materials_, &strings_};
        int archivedCount = 0, currentCount = 0;
        for (const auto& h : w_->hudElems()) {
            if (h->removed) continue;
            // Archived elements (the default) go to the array at +1960 (where the 1.3 server puts them, observed), the
            // others to the array at +5928; 31 each (docs/spec/net.md sections 7.3 and 11).
            const Value arch = h->get("archived");
            const bool archived = !arch.isDefined() || arch.truthy();
            // In the killcam the archived part is the followed client's (its ownership and team, 0x8104882 flag 1).
            // TODO(spec): the elements as they were at the archived time (the live ones are sent).
            const bool killer = kc.frame && archived;
            const Value owner = h->get("$owner");
            if (owner.isNumber() && owner.asInt() != (killer ? kc.client : self)) continue;
            const Value team = h->get("$team");
            if (team.isString() && (killer ? game::mpTeamNumber(team.asString()) != kc.frame->clients[size_t(kc.client)].team
                                           : team.asString() != selfTeam)) continue;
            int& n = archived ? archivedCount : currentCount;
            if (n >= net::kPsHudElemCount) continue;
            const int base = archived ? net::kPsHudElemsOffset : net::kPsArchivalHudElemsOffset;
            encodeHudElem(v.ps.data() + base + n * net::kHudElemSize, *h, hs);
            ++n;
        }
        const auto& objs = w_->objectives();
        for (size_t i = 0; i < objs.size(); ++i) {
            const game::Objective& ob = objs[i];
            uint8_t* p = v.ps.data() + net::kPsObjectivesOffset + int(i) * net::kObjectiveSize;
            putI(p, 0, ob.state);
            if (ob.state == 0) continue;
            for (int k = 0; k < 3; ++k) putF(p, 4 + 4 * k, ob.positions[0][size_t(k)]);
            putI(p, 16, ob.entity >= 0 ? ob.entity & 1023 : 1023);  // TODO(spec): entity numbers on the wire
            putI(p, 20, ob.team.empty() ? 0 : game::mpTeamNumber(ob.team));
            putI(p, 24, materials_.index(ob.icon) + 1);
        }
    }
    return v;
}

// Script menus, prints, chat and obituaries of the network clients become server commands and events
// (docs/spec/net.md section 10). TODO(spec): the close-menu letter (u here) and which print letter is the HUD line.
void WorldGame::forwardToClients() {
    syncNames();
    if (w_->mpState().restarts != restartsSeen_) {
        restartsSeen_ = w_->mpState().restarts;
        levelStartMs_ = serverTime();
        if (onRestart) onRestart();
    }
    // Fog, ambient and dvar configstrings follow the world: changes go out as configstring commands.
    {
        const std::map<int, std::string> now = dynamicConfigstrings();
        for (const auto& [k, v] : now)
            if (auto it = lastDynamic_.find(k); it == lastDynamic_.end() || it->second != v) announce(k, v);
        for (const auto& [k, v] : lastDynamic_)
            if (!now.count(k)) announce(k, "");
        lastDynamic_ = now;
    }
    const auto& st = w_->mpState();
    // The event rings of the host and the bots: the World's per-client ring (fire, reload, melee, offhand events by weapon
    // name) with the weapon indices of this game. Network clients have their own ring (netcombat.cpp).
    for (size_t c = 0; c < st.clients.size(); ++c) {
        bool network = false;
        for (const auto& [slot, n] : mp_) network |= n == int(c);
        if (network) continue;
        auto& [seen, ring] = otherRings_[int(c)];
        const game::MpEventRing& src = st.clients[c].events;
        ring.sequence = src.sequence;
        for (int i = 0; i < 4; ++i) {
            ring.events[size_t(i)] = src.events[size_t(i)];
            ring.parms[size_t(i)] = weaponByte(src.weapons[size_t(i)]) & 0xff;
        }
        seen = src.sequence;
    }
    // Turret shots (0x810aa3a: G_AddEvent on the turret, EV_FIRE_WEAPON_MG42, parm the gunner's entity number).
    for (const auto& e : w_->entities()) {
        if (!e->turret || e->obj->removed) continue;
        auto& [seen, ring] = turretRings_[e->number];
        const int count = e->turret->fireCount;
        if (count < seen) seen = count;
        int user = 1022;  // TODO(spec): the parm without a gunner (the server reads the user's number)
        if (e->turret->inUse && e->turret->owner)
            for (const auto& u : w_->entities())
                if (u->obj == e->turret->owner) user = u->number;
        for (int i = std::max(seen, count - 4); i < count; ++i) ring.push(kEvFireWeaponMg42, user);
        seen = count;
        // The end of the fire loop (0x810b926): the stop sound as the sound event 0xb3 with the sound's index.
        int& stops = turretStopsSeen_[e->number];
        if (e->turret->stopSounds < stops) stops = e->turret->stopSounds;
        if (e->turret->stopSounds > stops) {
            const int si = sounds_.index(weaponKey(*w_, e->turret->weapon, "stopfiresound"));
            for (; stops < e->turret->stopSounds; ++stops)
                if (si >= 0) ring.push(kEvSoundAlias, si + 1);
        }
    }
    // Grenade bounces (0x810d5ba: G_AddEvent on the missile, EV_GRENADE_BOUNCE with the surface type of the hit).
    std::set<int> grenades;
    for (const auto& e : w_->entities()) {
        if (!e->grenade || e->obj->removed) continue;
        grenades.insert(e->number);
        auto& [seen, ring] = grenadeRings_[e->number];
        const std::vector<int>& bounces = e->grenade->bounceEvents;
        for (size_t i = std::max(seen, bounces.size() > 4 ? bounces.size() - 4 : 0); i < bounces.size(); ++i) ring.push(kEvGrenadeBounce, bounces[i]);
        seen = bounces.size();
    }
    std::erase_if(grenadeRings_, [&](const auto& r) { return !grenades.count(r.first); }); // a freed grenade's ring goes with it
    // A map_restart rebuilds the world's state in place: its logs start again.
    if (st.prints.size() < printsSent_) printsSent_ = 0;
    if (st.chat.size() < chatSent_) chatSent_ = 0;
    if (st.clientCommands.size() < clientCommandsSent_) clientCommandsSent_ = 0;
    if (st.localSounds.size() < localSoundsSent_) localSoundsSent_ = 0;
    if (st.obituaryLog.size() < obituariesSeen_) obituariesSeen_ = 0;
    for (; obituariesSeen_ < st.obituaryLog.size(); ++obituariesSeen_) pushObituary(st.obituaryLog[obituariesSeen_]);
    if (st.explosionLog.size() < explosionsSeen_) explosionsSeen_ = 0;
    for (; explosionsSeen_ < st.explosionLog.size(); ++explosionsSeen_) pushExplosion(st.explosionLog[explosionsSeen_]);
    if (!sendCommand) {
        printsSent_ = st.prints.size();
        chatSent_ = st.chat.size();
        clientCommandsSent_ = st.clientCommands.size();
        localSoundsSent_ = st.localSounds.size();
        return;
    }
    // Script menus (t / u) go out with the client commands below, in the order the scripts called openMenu / closeMenu.
    for (; printsSent_ < st.prints.size(); ++printsSent_) {
        // "iprintln[<client>] text" / "iprintlnbold[<client>] text" (src/game/mpgame.cpp).
        const std::string& p = st.prints[printsSent_];
        const size_t lb = p.find('['), rb = p.find("] ");
        if (lb == std::string::npos || rb == std::string::npos || p.rfind("iprintln", 0) != 0) continue;
        const int target = std::atoi(p.c_str() + lb + 1);
        const std::string text = p.substr(rb + 2);
        const char letter = p.compare(0, lb, "iprintlnbold") == 0 ? 'g' : 'f';
        for (const auto& [slot, n] : mp_)
            if (target < 0 || target == n) sendCommand(slot, std::string(1, letter) + " \"" + text + "\"");
    }
    // A changed offhand weapon goes to the client as C <weapon number> (VAs in the 1.3 Linux dedicated server:
    // 0x8120e44, called when a give sets the empty offhand, 0x8120c0f / 0x8120c9d, and from a script method,
    // 0x80fa381). TODO(spec): the order against the give's other commands.
    for (const auto& [slot, n] : mp_) {
        Entity* pe = w_->mpClientEntity(n);
        const int off = pe ? weaponNumber(w_->inventory(*pe).offhand) : 0;
        int& last = sentOffhand_[slot];
        if (off != last && off > 0) sendCommand(slot, "C " + std::to_string(off));
        last = off;
    }
    for (; clientCommandsSent_ < st.clientCommands.size(); ++clientCommandsSent_) {
        const auto& [client, command] = st.clientCommands[clientCommandsSent_];
        for (const auto& [slot, n] : mp_)
            if (n == client) sendCommand(slot, command);
    }
    // playLocalSound (0x80fd990): the sound command s with the alias's sound index (configstring 590 + index) to that
    // client only, reliable.
    for (; localSoundsSent_ < st.localSounds.size(); ++localSoundsSent_) {
        const auto& [client, alias] = st.localSounds[localSoundsSent_];
        const int i = sounds_.index(alias);
        if (i < 0) continue;
        for (const auto& [slot, n] : mp_)
            if (n == client) sendCommand(slot, "s " + std::to_string(i + 1));
    }
    // Chat server commands ("h <text>" / "i <text>", docs/spec/mp-chat.md) to the receiving clients.
    for (; chatSent_ < st.chat.size(); ++chatSent_) {
        const auto& [client, command] = st.chat[chatSent_];
        for (const auto& [slot, n] : mp_)
            if (n == client) {
                const size_t sp = command.find(' ');
                sendCommand(slot, command.substr(0, sp) + " \"" + (sp == std::string::npos ? std::string() : command.substr(sp + 1)) + "\"");
            }
    }
}

} // namespace iw2::netgame
