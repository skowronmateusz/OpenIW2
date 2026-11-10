// Multiplayer create-server front end, implemented from the spec (docs/spec/mp-ui.md).
#include "ui/mpserver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <strings.h>

namespace iw2::ui {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Tokens of a text file: quoted strings, bare words, { and }; // and /* */ comments are skipped.
std::vector<std::string> tokens(const std::string& t) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') { while (i < t.size() && t[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            const size_t e = t.find("*/", i + 2);
            i = e == std::string::npos ? t.size() : e + 2;
            continue;
        }
        if (c == '{' || c == '}') { out.emplace_back(1, c); ++i; continue; }
        if (c == '"') {
            const size_t e = t.find('"', i + 1);
            out.push_back(t.substr(i + 1, (e == std::string::npos ? t.size() : e) - i - 1));
            i = e == std::string::npos ? t.size() : e + 1;
            continue;
        }
        size_t e = i;
        while (e < t.size() && !std::isspace(static_cast<unsigned char>(t[e])) && t[e] != '{' && t[e] != '}' && t[e] != '"') ++e;
        out.push_back(t.substr(i, e - i));
        i = e;
    }
    return out;
}

std::string readText(const fs::Vfs& vfs, const std::string& path) {
    const auto d = vfs.read(path);
    return d ? std::string(d->begin(), d->end()) : std::string();
}

} // namespace

std::vector<std::string> MpServerSetup::loadGametypes(const fs::Vfs& vfs) {
    std::vector<std::string> warnings;
    gametypes_.clear();
    const std::string dir = "maps/mp/gametypes/";
    for (const std::string& path : vfs.list(dir)) {
        if (path.find('/', dir.size()) != std::string::npos) continue;
        std::string name = path.substr(dir.size());
        if (name.size() < 4 || lower(name.substr(name.size() - 4)) != ".gsc") continue;
        if (name[0] == '_') continue; // helper scripts such as _callbacksetup
        name = name.substr(0, name.size() - 4);
        if (int(gametypes_.size()) == kMaxGametypes) {
            warnings.push_back("Too many game type scripts found! Only loading the first " + std::to_string(kMaxGametypes - 1));
            break;
        }
        Gametype g{name, name};
        const auto tok = tokens(readText(vfs, dir + name + ".txt"));
        if (!tok.empty()) g.display = tok[0];
        gametypes_.push_back(g);
    }
    if (gametypes_.empty()) warnings.push_back("No game type scripts found in maps/mp/gametypes folder");
    return warnings;
}

void MpServerSetup::parseArenas(const std::string& text) {
    const auto tok = tokens(text);
    size_t i = 0;
    while (i < tok.size() && int(arenas_.size()) < kMaxArenas) {
        if (tok[i] != "{") { ++i; continue; }
        ++i;
        std::string map, longName, gt;
        bool hasGt = false;
        while (i < tok.size() && tok[i] != "}") {
            const std::string key = lower(tok[i]);
            const std::string val = i + 1 < tok.size() ? tok[i + 1] : std::string();
            if (key == "map") map = val;
            else if (key == "longname") longName = val;
            else if (key == "gametype") gt = val, hasGt = true;
            i += 2;
        }
        ++i;
        Arena a;
        a.map = map;
        a.longName = longName;
        a.loadScreen = "loadscreen_" + map;
        if (!hasGt || gt.empty()) a.mask = ~0u; // no gametype key: every gametype (0x547955)
        else {
            for (const auto& t : tokens(gt))
                for (size_t g = 0; g < gametypes_.size(); ++g)
                    if (strcasecmp(t.c_str(), gametypes_[g].name.c_str()) == 0) a.mask |= 1u << g;
        }
        arenas_.push_back(a);
    }
}

int MpServerSetup::intCvar(const GetCvar& get, const std::string& name) const { return std::atoi(get(name).c_str()); }

void MpServerSetup::markActive(int gametype) {
    for (Arena& a : arenas_) a.active = gametype >= 0 && gametype < 32 && (a.mask & (1u << gametype)) != 0;
}

int MpServerSetup::loadArenas(const fs::Vfs& vfs, const GetCvar& get, const SetCvar& set) {
    arenas_.clear();
    for (const std::string& path : vfs.list("mp/")) {
        if (path.size() < 6 || lower(path.substr(path.size() - 6)) != ".arena" || path.find('/', 3) != std::string::npos) continue;
        parseArenas(readText(vfs, path));
    }
    // 0x536110: ui_netGametype / ui_netGametypeName follow g_gametype when it names a listed gametype (registered default "dm").
    std::string gt = get("g_gametype");
    if (gt.empty()) gt = "dm";
    for (size_t g = 0; g < gametypes_.size(); ++g)
        if (gametypes_[g].name == gt) {
            set("ui_netGametype", std::to_string(g));
            set("ui_netGametypeName", gametypes_[g].name);
            break;
        }
    markActive(intCvar(get, "ui_netGametype"));
    // Menu_SetFeederSelection(createserver_maps, FEEDER_ALLMAPS, 0) runs the feeder selection of row 0. The exe then (0x536380) keeps
    // the running map selected when the client is connected; the front end has no running server here.
    if (mapCount() > 0) selectMap(0, set);
    return 0;
}

bool loadWaitsForClick(const std::string& map, bool autoContinue, bool quickReload) {
    if (map.rfind("mp/", 0) == 0) return false;
    return !autoContinue && !quickReload;
}

MpLoadScreenText mpLoadScreenText(const fs::Vfs& vfs, const std::string& gametype, const std::string& map) {
    MpLoadScreenText t{gametype, map};
    MpServerSetup s;
    s.loadGametypes(vfs);
    for (const auto& g : s.gametypes())
        if (lower(g.name) == lower(gametype)) t.gametype = g.display;
    for (const std::string& path : vfs.list("mp/")) {
        if (path.size() < 6 || lower(path.substr(path.size() - 6)) != ".arena" || path.find('/', 3) != std::string::npos) continue;
        s.parseArenas(readText(vfs, path));
    }
    for (const auto& a : s.arenas())
        if (lower(a.map) == lower(map) && !a.longName.empty()) {
            t.map = a.longName;
            break;
        }
    return t;
}

int MpServerSetup::cycleGametype(int step, const GetCvar& get, const SetCvar& set) {
    const int n = int(gametypes_.size());
    if (n == 0) return -1;
    int v = intCvar(get, "ui_netGametype");
    v = step < 0 ? (v == 0 ? n - 1 : v - 1) : (v + 1 == n ? 0 : v + 1);
    if (v < 0 || v >= n) v = 0;
    set("ui_netGametype", std::to_string(v));
    set("ui_netGametypeName", gametypes_[size_t(v)].name);
    markActive(v);
    const int cur = intCvar(get, "ui_currentNetMap");
    if (cur >= 0 && cur < int(arenas_.size()) && arenas_[size_t(cur)].active) return rowOfArena(cur); // 0x5362d0
    // 0x536220: the current map is not allowed: row 0 of the list is selected (its selection sets ui_currentNetMap).
    if (mapCount() == 0) return -1;
    selectMap(0, set);
    return 0;
}

std::string MpServerSetup::gametypeLabel(const GetCvar& get) const {
    const int v = intCvar(get, "ui_netGametype");
    return v >= 0 && v < int(gametypes_.size()) ? gametypes_[size_t(v)].display : std::string();
}

int MpServerSetup::mapCount() const {
    return int(std::count_if(arenas_.begin(), arenas_.end(), [](const Arena& a) { return a.active; }));
}

int MpServerSetup::arenaOfRow(int row) const {
    int r = 0;
    for (size_t i = 0; i < arenas_.size(); ++i)
        if (arenas_[i].active && r++ == row) return int(i);
    return -1;
}

int MpServerSetup::rowOfArena(int arena) const {
    int r = 0;
    for (size_t i = 0; i < arenas_.size(); ++i) {
        if (!arenas_[i].active) continue;
        if (int(i) == arena) return r;
        ++r;
    }
    return 0; // 0x5362f9: not listed -> row 0
}

std::string MpServerSetup::mapText(int row) const {
    const int a = arenaOfRow(row);
    return a < 0 ? std::string() : arenas_[size_t(a)].longName;
}

void MpServerSetup::selectMap(int row, const SetCvar& set) {
    const int a = arenaOfRow(row);
    if (a < 0) return;
    set("ui_currentMap", std::to_string(a));
    set("ui_currentNetMap", std::to_string(a));
}

std::string MpServerSetup::loadScreenOfCurrent(const GetCvar& get) const {
    const int a = intCvar(get, "ui_currentNetMap");
    return a >= 0 && a < int(arenas_.size()) ? arenas_[size_t(a)].loadScreen : std::string();
}

std::string MpServerSetup::startServer(const GetCvar& get, const SetCvar& set) const {
    set("cg_thirdPerson", "0");
    set("dedicated", std::to_string(intCvar(get, "ui_dedicated")));
    const int g = intCvar(get, "ui_netGametype");
    if (g >= 0 && g < int(gametypes_.size())) set("g_gametype", gametypes_[size_t(g)].name);
    const int a = intCvar(get, "ui_currentNetMap");
    if (a < 0 || a >= int(arenas_.size())) return {};
    return "wait ; wait ; map " + arenas_[size_t(a)].map + "\n";
}

bool menuStartListens(const char* iw2ListenEnv) {
    return !(iw2ListenEnv && std::atoi(iw2ListenEnv) == 0);
}

bool mpOwnerDrawFlagShown(int flag, int netSource) {
    if ((flag & kShowFavoriteServers) && netSource != 2) return false;
    if ((flag & kShowNotFavoriteServers) && netSource == 2) return false;
    return true;
}

int mpCycleNetSource(int netSource, int step) { return ((netSource + step) % 3 + 3) % 3; }

std::string mpNetSourceLabel(int netSource) {
    static const char* const names[] = {"EXE_LOCAL", "EXE_INTERNET", "EXE_FAVORITES"};
    return names[netSource >= 0 && netSource < 3 ? netSource : 0];
}

std::string mpRegisteredDefault(const std::string& name) {
    // Registrations of the MP executable (name, type, default, range, flags 0x1000+), in its server dvar block
    // 0x4597f0..0x459ec0 and the gametype dvar at 0x457af5.
    static const std::pair<const char*, const char*> defaults[] = {
        {"sv_maxclients", "20"},     // 0x45982b: int, 1..64, flags 0x1025
        {"ui_browserShowFull", "1"}, {"ui_browserShowEmpty", "1"}, {"ui_browserShowPassword", "1"}, // 0x531c1c..: int, flags 0x1001
        {"ui_browserShowNoPassword", "1"}, {"ui_browserShowPure", "0"}, {"ui_browserShowDedicated", "0"},
        {"ui_browserMod", "-1"}, {"ui_browserFriendlyfire", "-1"}, {"ui_browserKillcam", "-1"}, {"ui_browserShowPunkBuster", "-1"},
        {"sv_hostname", "CoD2Host"}, // 0x459869: string, flags 0x1005
        {"sv_punkbuster", "0"},      // 0x4598b0: bool, flags 0x1015
        {"sv_maxRate", "0"},         // 0x4598c8: int, 0..25000
        {"sv_minPing", "0"},         // 0x4598fa: int, 0..999
        {"sv_maxPing", "0"},         // 0x45993f: int, 0..999
        {"sv_floodProtect", "1"},    // bool, after sv_maxPing
        {"sv_allowAnonymous", "0"},  // bool
        {"sv_voice", "0"},           // bool, flags 0x100d
        {"sv_voiceQuality", "1"},    // int, 0..9
        {"sv_pure", "1"},            // 0x459a9f: bool, flags 0x100c
        {"sv_mapRotation", ""},      // 0x459e94: string, flags 0x1000
        {"sv_mapRotationCurrent", ""}, // 0x459eba: string, flags 0x1000
        {"g_gametype", "dm"},        // 0x457af5: string
    };
    for (const auto& [k, v] : defaults)
        if (strcasecmp(name.c_str(), k) == 0) return v;
    return {};
}

// ---- openiw2 extension ----
namespace mpext {

std::vector<std::pair<std::string, std::string>> localizedStrings(const std::string& language) {
    // openiw2's own texts (not from the game files). Polish in Windows-1250, the code page of the Polish game's strings.
    struct Text {
        const char *key, *en, *pl;
    };
    static const Text texts[] = {
        {"IW2_BOTS_WEAPONS", "Bots & weapons", "Boty i bro\xf1"},
        {"IW2_BOTS_WEAPONS_TITLE", "Bots & weapons (openiw2)", "Boty i bro\xf1 (openiw2)"},
        {"IW2_BOTS", "Bots:", "Boty:"},
        {"IW2_BOT_SKILL", "Bot skill:", "Poziom bot\xf3w:"},
        {"IW2_SKILL_EASY", "Easy", "\xa3" "atwy"},
        {"IW2_SKILL_MEDIUM", "Medium", "\x8c" "redni"},
        {"IW2_SKILL_HARD", "Hard", "Trudny"},
        {"IW2_SKILL_VETERAN", "Veteran", "Weteran"},
        {"IW2_CLASS_RIFLE", "Semi-auto rifles:", "Karabiny samopowtarzalne:"},
        {"IW2_CLASS_BOLT", "Bolt-action rifles:", "Karabiny powtarzalne:"},
        {"IW2_CLASS_SMG", "Submachine guns:", "Pistolety maszynowe:"},
        {"IW2_CLASS_SNIPER", "Sniper rifles:", "Karabiny snajperskie:"},
        {"IW2_CLASS_MG", "Automatic rifles / MG:", "Karabiny automatyczne:"},
        {"IW2_CLASS_SHOTGUN", "Shotgun:", "Strzelba:"},
        {"IW2_CLASS_GRENADES", "Grenades:", "Granaty:"},
        {"IW2_ERR_NO_WEAPON", "Allow at least one weapon class besides grenades. With no weapon to choose, nobody could join the game.",
         "Zezw\xf3" "l na co najmniej jedn\xb9" " klas\xea" " broni poza granatami. Bez broni do wyboru nikt nie m\xf3" "g\xb3" "by do\xb3" "\xb9" "czy\xe6" " do gry."},
        {"IW2_ERR_NATION_WEAPON", "Every nation needs a weapon to choose. Allow more weapon classes (for example rifles, submachine guns or sniper rifles).",
         "Ka\xbf" "da nacja musi mie\xe6" " bro\xf1" " do wyboru. Zezw\xf3" "l na wi\xea" "cej klas broni (na przyk\xb3" "ad karabiny, pistolety maszynowe lub karabiny snajperskie)."},
    };
    const bool polish = strcasecmp(language.c_str(), "polish") == 0;
    std::vector<std::pair<std::string, std::string>> out;
    for (const Text& t : texts) out.emplace_back(t.key, polish ? t.pl : t.en);
    return out;
}

const std::vector<WeaponClass>& weaponClasses() {
    static const std::vector<WeaponClass> classes = {
        {"rifle", "@IW2_CLASS_RIFLE"}, {"bolt", "@IW2_CLASS_BOLT"}, {"smg", "@IW2_CLASS_SMG"}, {"sniper", "@IW2_CLASS_SNIPER"},
        {"mg", "@IW2_CLASS_MG"}, {"shotgun", "@IW2_CLASS_SHOTGUN"}, {"grenades", "@IW2_CLASS_GRENADES"},
    };
    return classes;
}

std::string weaponList(const MpServerSetup::GetCvar& get) {
    std::string list;
    for (const auto& c : weaponClasses()) {
        const std::string v = get(std::string("ui_iw2_allow_") + c.id);
        if (v.empty() || std::atof(v.c_str()) != 0.0) list += (list.empty() ? "" : ",") + std::string(c.id);
    }
    return list;
}

void applyWeaponClasses(const MpServerSetup::GetCvar& get, const MpServerSetup::SetCvar& set) {
    std::string list;
    for (const auto& c : weaponClasses()) {
        const std::string v = get(std::string("ui_iw2_allow_") + c.id);
        const bool on = v.empty() || std::atof(v.c_str()) != 0.0;
        if (std::string(c.id) == "grenades") {
            set("scr_allow_fraggrenades", on ? "1" : "0");
            set("scr_allow_smokegrenades", on ? "1" : "0");
        } else if (on) {
            list += (list.empty() ? "" : ",") + std::string(c.id);
        }
    }
    set("iw2_weaponclasses", list.empty() ? "none" : list);
}

void initDefaults(const MpServerSetup::GetCvar& get, const MpServerSetup::SetCvar& set) {
    for (const auto& c : weaponClasses())
        if (get(std::string("ui_iw2_allow_") + c.id).empty()) set(std::string("ui_iw2_allow_") + c.id, "1");
    if (get("ui_iw2_bots").empty()) set("ui_iw2_bots", "0");
    if (get("ui_iw2_botskill").empty()) set("ui_iw2_botskill", "1");
}

int botCount(const MpServerSetup::GetCvar& get) {
    int n = std::max(0, std::atoi(get("ui_iw2_bots").c_str()));
    const std::string mc = get("sv_maxclients");
    if (!mc.empty()) n = std::min(n, std::max(0, std::atoi(mc.c_str()) - 1));
    return n;
}

int botSkill(const MpServerSetup::GetCvar& get) { return std::clamp(std::atoi(get("ui_iw2_botskill").c_str()), 0, 3); }

const char* menuSource() {
    // openiw2's own menus (not from the game files). "iw2_createserver_extbutton" adds a button to createserver's bottom bar (rect of
    // createserver_buttons: 0 415 640 100); it swaps the settings column (createserver_serversettings / settings_<gametype>, rect
    // 0 2 370 400, items from origin 50 120) for "iw2_createserver_ext". Item types: 1 BUTTON, 11 YESNO, 12 MULTI.
    return R"MENU({
	menuDef
	{
		name "iw2_createserver_extbutton"
		fullscreen 0
		rect 0 415 640 100
		visible 1
		onEsc { close createserver; }
		itemDef
		{
			name iw2_button_ext
			text "@IW2_BOTS_WEAPONS"
			type 1
			dvar "ui_netGametypeName"
			rect 400 0 130 22
			textalignx 0
			textaligny 18
			textscale .35
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
			action { play "mouse_click"; closeForGameType "settings_%s"; close createserver_serversettings; open iw2_createserver_ext; }
		}
	}
	menuDef
	{
		name "iw2_createserver_ext"
		fullscreen 0
		rect 0 2 370 400
		visible 1
		onEsc { close createserver; }
		itemDef
		{
			name iw2_heading
			text "@IW2_BOTS_WEAPONS_TITLE"
			type 1
			decoration
			rect 52.5 105 265 10
			textalign 1
			textalignx 132
			textaligny 10
			textscale .35
			forecolor .75 .75 .75 1
			visible 1
		}
		itemDef
		{
			name iw2_bots_label
			text "@IW2_BOTS"
			type 1
			decoration
			rect 50 120 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_bots
			text " "
			type 12
			dvar "ui_iw2_bots"
			dvarFloatList { "0" 0 "1" 1 "2" 2 "3" 3 "4" 4 "5" 5 "6" 6 "7" 7 "8" 8 "10" 10 "12" 12 "15" 15 }
			rect 50 120 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_botskill_label
			text "@IW2_BOT_SKILL"
			type 1
			decoration
			rect 50 135 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_botskill
			text " "
			type 12
			dvar "ui_iw2_botskill"
			dvarFloatList { "@IW2_SKILL_EASY" 0 "@IW2_SKILL_MEDIUM" 1 "@IW2_SKILL_HARD" 2 "@IW2_SKILL_VETERAN" 3 }
			rect 50 135 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_rifle_label
			text "@IW2_CLASS_RIFLE"
			type 1
			decoration
			rect 50 158 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_rifle
			text " "
			type 11
			dvar "ui_iw2_allow_rifle"
			rect 50 158 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_bolt_label
			text "@IW2_CLASS_BOLT"
			type 1
			decoration
			rect 50 173 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_bolt
			text " "
			type 11
			dvar "ui_iw2_allow_bolt"
			rect 50 173 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_smg_label
			text "@IW2_CLASS_SMG"
			type 1
			decoration
			rect 50 188 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_smg
			text " "
			type 11
			dvar "ui_iw2_allow_smg"
			rect 50 188 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_sniper_label
			text "@IW2_CLASS_SNIPER"
			type 1
			decoration
			rect 50 203 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_sniper
			text " "
			type 11
			dvar "ui_iw2_allow_sniper"
			rect 50 203 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_mg_label
			text "@IW2_CLASS_MG"
			type 1
			decoration
			rect 50 218 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_mg
			text " "
			type 11
			dvar "ui_iw2_allow_mg"
			rect 50 218 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_shotgun_label
			text "@IW2_CLASS_SHOTGUN"
			type 1
			decoration
			rect 50 233 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_shotgun
			text " "
			type 11
			dvar "ui_iw2_allow_shotgun"
			rect 50 233 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_grenades_label
			text "@IW2_CLASS_GRENADES"
			type 1
			decoration
			rect 50 248 160 13
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
		itemDef
		{
			name iw2_allow_grenades
			text " "
			type 11
			dvar "ui_iw2_allow_grenades"
			rect 50 248 320 13
			textalignx 170
			textaligny 11
			textscale .25
			textstyle 3
			forecolor .9 .9 .9 1
			visible 1
		}
	}
}
)MENU";
}

} // namespace mpext

} // namespace iw2::ui
