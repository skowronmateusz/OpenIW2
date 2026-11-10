// The options of iw2server (docs/dedicated-server.md): the weapon list (every class against all four nations, the error
// cases), the command line parser (every option, every error), the limit cvars per gametype, and with the game files
// (IW2_MAIN): the list reaches the scr_allow_* cvars and the bots obey it, and a running server carries the score,
// time and round limits of --scorelimit / --timelimit / --roundlimit (and the command line wins over +set).
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fs/vfs.h"
#include "game/mpbots.h"
#include "game/world.h"
#include "net/oob.h"
#include "net/udp.h"
#include "ui/mpserver.h"
#include "netgame/dedicated.h"
#include "netgame/serverargs.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2;
using game::MpWeaponSelection;

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The classes of the menu weapons of each nation, written out by hand from the weapon files (the table in mpbotgoals.cpp
// is checked against the files in test_mpgametypes).
const std::map<std::string, std::set<std::string>> kNationClasses = {
    {"american", {"rifle", "sniper", "smg", "mg", "shotgun"}},
    {"british", {"rifle", "bolt", "sniper", "smg", "mg", "shotgun"}},
    {"russian", {"rifle", "bolt", "sniper", "smg", "shotgun"}},
    {"german", {"rifle", "bolt", "sniper", "smg", "mg", "shotgun"}},
};

void weaponLists() {
    // Every class alone, against all four nations: accepted exactly when every nation has a weapon of the class.
    for (const std::string& cls : game::mpWeaponClassNames()) {
        MpWeaponSelection sel;
        const std::string err = game::mpParseWeaponList(cls, sel);
        std::string missing;
        for (const std::string& nation : game::mpWeaponNations()) {
            const bool has = kNationClasses.at(nation).count(cls) != 0;
            // The menus themselves: a nation has a weapon of the class when one of its menu weapons is in it.
            bool menuHas = false;
            for (const std::string& w : game::mpMenuWeapons("weapon_" + nation)) menuHas = menuHas || game::mpMenuWeaponClass(w) == cls;
            CHECK(has == menuHas);
            if (!has && missing.empty()) missing = nation;
        }
        if (missing.empty()) {
            CHECK(err.empty());
            CHECK(sel.primaries.size() == 1 && sel.primaries[0] == cls && sel.classesCvar == cls && sel.grenades == false);
        } else {
            CHECK(contains(err, missing));  // the error names the nation
            CHECK(contains(err, "weapon menu"));
        }
        if (cls == "bolt") CHECK(contains(err, "american"));
        if (cls == "mg") CHECK(contains(err, "russian"));
    }
    // Every menu weapon is in the table with a class, and every class of the table is a known one.
    int menuWeapons = 0;
    for (const std::string& nation : game::mpWeaponNations())
        for (const std::string& w : game::mpMenuWeapons("weapon_" + nation)) {
            const std::string c = game::mpMenuWeaponClass(w);
            CHECK(!c.empty() && !game::mpWeaponAllowCvar(w).empty());
            CHECK(c == "shotgun" ? true : kNationClasses.at(nation).count(c) == 1);
            ++menuWeapons;
        }
    CHECK(menuWeapons == 7 + 7 + 6 + 6);
    // The combination of the task works for each nation.
    MpWeaponSelection sel;
    CHECK(game::mpParseWeaponList("bolt,sniper,grenades", sel).empty());
    CHECK(sel.classesCvar == "bolt,sniper" && sel.grenades);
    for (const std::string& nation : game::mpWeaponNations()) {
        bool any = false;
        for (const std::string& w : game::mpMenuWeapons("weapon_" + nation))
            any = any || game::mpMenuWeaponClass(w) == "bolt" || game::mpMenuWeaponClass(w) == "sniper";
        CHECK(any);
    }
    // all, case, spaces, order, duplicates.
    CHECK(game::mpParseWeaponList("all", sel).empty() && sel.grenades && sel.primaries.size() == game::mpWeaponClassNames().size());
    CHECK(game::mpParseWeaponList("  SMG, Rifle smg ", sel).empty() && sel.classesCvar == "rifle,smg" && !sel.grenades);
    CHECK(game::mpParseWeaponList("grenades,shotgun", sel).empty() && sel.classesCvar == "shotgun" && sel.grenades);
    // Errors: no primary class, unknown name, none combined, a nation left without a weapon.
    CHECK(contains(game::mpParseWeaponList("grenades", sel), "grenades alone"));
    CHECK(contains(game::mpParseWeaponList("", sel), "no weapon class"));
    CHECK(contains(game::mpParseWeaponList("none", sel), "no weapon class"));
    CHECK(contains(game::mpParseWeaponList("pistol", sel), "unknown weapon class 'pistol'"));
    CHECK(contains(game::mpParseWeaponList("rifle,bazooka", sel), "bazooka"));
    CHECK(contains(game::mpParseWeaponList("none,rifle", sel), "none"));
    CHECK(contains(game::mpParseWeaponList("bolt,grenades", sel), "american"));
    CHECK(game::mpParseWeaponList("mg,bolt", sel).empty()); // each alone fails a nation, together every nation has a weapon
}

// The panel's toggles (Bots & weapons) make a list that goes through the same check as --weapons: a start with only
// grenades or nothing is refused, the other combinations pass.
void panelLists() {
    std::map<std::string, std::string> cv;
    const auto get = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    const auto set = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    ui::mpext::initDefaults(get, set);
    MpWeaponSelection sel;
    CHECK(game::mpParseWeaponList(ui::mpext::weaponList(get), sel).empty() && sel.grenades && sel.primaries.size() == 6); // all on
    for (const auto& c : ui::mpext::weaponClasses()) cv[std::string("ui_iw2_allow_") + c.id] = "0";
    CHECK(ui::mpext::weaponList(get).empty() && !game::mpParseWeaponList(ui::mpext::weaponList(get), sel).empty());
    cv["ui_iw2_allow_grenades"] = "1";
    CHECK(contains(game::mpParseWeaponList(ui::mpext::weaponList(get), sel), "grenades alone"));
    cv["ui_iw2_allow_bolt"] = "1";  // bolt alone leaves the american menu empty
    CHECK(contains(game::mpParseWeaponList(ui::mpext::weaponList(get), sel), "american"));
    cv["ui_iw2_allow_sniper"] = "1";
    CHECK(game::mpParseWeaponList(ui::mpext::weaponList(get), sel).empty() && sel.classesCvar == "bolt,sniper" && sel.grenades);
    // The panel and the option write the same cvars.
    ui::mpext::applyWeaponClasses(get, set);
    CHECK(cv["iw2_weaponclasses"] == sel.classesCvar && cv["scr_allow_fraggrenades"] == "1");
}

bool parse(std::vector<std::string> args, netgame::ServerArgs& a) { return netgame::parseServerArgs(args, a); }

void commandLine() {
    netgame::ServerArgs a;
    // The example of the documentation.
    CHECK(parse({"/main", "mp_toujane", "tdm", "--bots", "6", "--bot-skill", "1", "--maxclients", "20", "--weapons",
                 "bolt,sniper,grenades", "--scorelimit", "50"}, a));
    CHECK(a.mainDir == "/main" && a.settings.map == "mp_toujane" && a.settings.gametype == "tdm");
    CHECK(a.settings.bots == 6 && a.settings.botSkill == 1 && a.settings.maxClients == 20 && a.settings.scoreLimit == 50);
    std::map<std::string, std::string> over;
    for (const auto& [k, v] : a.settings.overrideCvars) over[k] = v;
    CHECK(over["iw2_weaponclasses"] == "bolt,sniper" && over["scr_allow_fraggrenades"] == "1" && over["scr_allow_smokegrenades"] == "1");
    // Without options: defaults, nothing overridden (everything is allowed).
    netgame::ServerArgs d;
    CHECK(parse({"/main", "mp_toujane"}, d));
    CHECK(d.settings.gametype.empty() && d.settings.port == -1 && d.settings.maxClients == -1 && d.settings.bots == 0 &&
          d.settings.botSkill == 3 && d.settings.scoreLimit == -1 && d.settings.timeLimit < 0 && d.settings.roundLimit == -1 &&
          d.settings.overrideCvars.empty() && d.settings.readStdin);
    // Grenades off when the list has no grenades.
    netgame::ServerArgs g;
    CHECK(parse({"/main", "mp_toujane", "dm", "--weapons", "rifle"}, g));
    over.clear();
    for (const auto& [k, v] : g.settings.overrideCvars) over[k] = v;
    CHECK(over["scr_allow_fraggrenades"] == "0" && over["scr_allow_smokegrenades"] == "0" && over["iw2_weaponclasses"] == "rifle");
    // The other options.
    netgame::ServerArgs o;
    CHECK(parse({"/main", "mp_toujane", "sd", "--port", "30000", "--seconds", "9", "--timelimit", "7.5", "--roundlimit", "3",
                 "--no-console", "--cvar", "sv_hostname=A b", "+exec", "server.cfg", "+map_rotate"}, o));
    CHECK(o.settings.port == 30000 && o.settings.seconds == 9 && o.settings.timeLimit == 7.5 && o.settings.roundLimit == 3);
    CHECK(!o.settings.readStdin && o.settings.cvars.size() == 1 && o.settings.cvars[0].second == "A b");
    CHECK(o.settings.startup.size() == 2 && o.settings.startup[0] == "exec server.cfg" && o.settings.startup[1] == "map_rotate");
    netgame::ServerArgs h;
    CHECK(parse({"--help"}, h) && h.help);
    CHECK(netgame::serverUsage().find("--weapons") != std::string::npos);
    // Errors.
    struct Bad { std::vector<std::string> args; const char* part; };
    const std::vector<Bad> bad = {
        {{}, "main"},
        {{"--bots", "1"}, "main folder"},
        {{"/m", "mp_toujane", "tdm", "--bogus"}, "unknown option --bogus"},
        {{"/m", "mp_toujane", "tdm", "--bots"}, "needs a value"},
        {{"/m", "mp_toujane", "tdm", "--bots", "x"}, "whole number"},
        {{"/m", "mp_toujane", "tdm", "--bots", "20"}, "0..19"},
        {{"/m", "mp_toujane", "tdm", "--bot-skill", "4"}, "0..3"},
        {{"/m", "mp_toujane", "tdm", "--maxclients", "0"}, "1..20"},
        {{"/m", "mp_toujane", "tdm", "--maxclients", "21"}, "1..20"},
        {{"/m", "mp_toujane", "tdm", "--port", "70000"}, "0..65535"},
        {{"/m", "mp_toujane", "tdm", "--scorelimit", "-1"}, "0..100000"},
        {{"/m", "mp_toujane", "tdm", "--scorelimit", "5x"}, "whole number"},
        {{"/m", "mp_toujane", "tdm", "--timelimit", "1441"}, "0..1440"},
        {{"/m", "mp_toujane", "tdm", "--timelimit", "-2"}, "0..1440"},
        {{"/m", "mp_toujane", "tdm", "--timelimit", "abc"}, "needs a number"},
        {{"/m", "mp_toujane", "tdm", "--roundlimit", "2"}, "only applies to the gametype sd"},
        {{"/m", "mp_toujane", "sd", "--roundlimit", "-1"}, "0..1000"},
        {{"/m", "mp_toujane", "capture", "--bots", "1"}, "unknown gametype"},
        {{"/m", "mp_toujane", "tdm", "--weapons", "grenades"}, "--weapons: grenades alone"},
        {{"/m", "mp_toujane", "tdm", "--weapons", "none"}, "--weapons: no weapon class"},
        {{"/m", "mp_toujane", "tdm", "--weapons", ""}, "--weapons: no weapon class"},
        {{"/m", "mp_toujane", "tdm", "--weapons", "sniper,laser"}, "unknown weapon class 'laser'"},
        {{"/m", "mp_toujane", "tdm", "--weapons", "mg"}, "russian"},
        {{"/m", "mp_toujane", "tdm", "--weapons", "bolt,grenades"}, "american"},
        {{"/m", "mp_toujane", "tdm", "--cvar", "novalue"}, "name=value"},
    };
    for (const Bad& b : bad) {
        netgame::ServerArgs x;
        const bool ok = parse(b.args, x);
        if (ok || !contains(x.error, b.part)) std::cerr << "case '" << b.part << "': ok=" << ok << " error='" << x.error << "'\n";
        CHECK(!ok && contains(x.error, b.part));
    }
}

void limitCvars() {
    const std::map<std::string, std::string> names = {{"dm", "scr_dm_"}, {"tdm", "scr_tdm_"}, {"sd", "scr_sd_"}, {"ctf", "scr_ctf_"}, {"hq", "scr_hq_"}};
    for (const auto& [gt, prefix] : names) {
        netgame::DedicatedSettings s;
        s.scoreLimit = 40;
        s.timeLimit = 12.5;
        std::vector<std::pair<std::string, std::string>> out;
        CHECK(netgame::serverLimitCvars(s, gt, out).empty());
        CHECK(out.size() == 2 && out[0] == std::make_pair(prefix + "scorelimit", std::string("40")) &&
              out[1] == std::make_pair(prefix + "timelimit", std::string("12.5")));
        s.roundLimit = 4;
        out.clear();
        const std::string err = netgame::serverLimitCvars(s, gt, out);
        if (gt == "sd") CHECK(err.empty() && out.size() == 3 && out[2] == std::make_pair(std::string("scr_sd_roundlimit"), std::string("4")));
        else CHECK(contains(err, "only applies to the gametype sd"));
    }
    netgame::DedicatedSettings none;
    std::vector<std::pair<std::string, std::string>> out;
    CHECK(netgame::serverLimitCvars(none, "tdm", out).empty() && out.empty()); // nothing given: the configuration stays
    netgame::DedicatedSettings zero;
    zero.scoreLimit = 0;
    zero.timeLimit = 0;
    CHECK(netgame::serverLimitCvars(zero, "dm", out).empty() && out.size() == 2 && out[0].second == "0" && out[1].second == "0");
    netgame::DedicatedSettings unk;
    unk.scoreLimit = 1;
    CHECK(contains(netgame::serverLimitCvars(unk, "bogus", out), "unknown gametype"));
}

// The class of a weapon by its menu, through the weapon files of the loaded world.
void weaponsInGame(fs::Vfs& vfs) {
    game::World w(vfs, "mp_toujane", true);
    netgame::ServerArgs a;
    CHECK(parse({"/m", "mp_toujane", "tdm", "--weapons", "bolt,sniper,grenades"}, a));
    for (const auto& [k, v] : a.settings.overrideCvars) w.cvars[k] = v;
    w.mpStart("tdm");
    const std::set<std::string> on = {"kar98k", "enfield", "nagant", "springfield", "enfieldsniper", "nagantsniper", "kar98ksniper"};
    for (const std::string& nation : game::mpWeaponNations())
        for (const std::string& weapon : game::mpMenuWeapons("weapon_" + nation)) {
            const std::string cvar = game::mpWeaponAllowCvar(weapon);
            const bool expected = on.count(cvar.substr(10)) != 0;
            if (w.cvars[cvar] != (expected ? "1" : "0")) std::cerr << weapon << " " << cvar << "=" << w.cvars[cvar] << "\n";
            CHECK(w.cvars[cvar] == (expected ? "1" : "0"));
        }
    CHECK(w.cvars["scr_allow_fraggrenades"] == "1" && w.cvars["scr_allow_smokegrenades"] == "1");
    for (const std::string& nation : game::mpWeaponNations()) {
        const auto allowed = w.mpAllowedWeapons("weapon_" + nation);
        CHECK(!allowed.empty()); // every nation can still pick a weapon
        for (const std::string& weapon : allowed) {
            const std::string c = w.mpWeaponClass(weapon);
            CHECK(c == "bolt" || c == "sniper");
        }
    }
}

// Bots pick only listed weapons, on maps of every nation.
void botsObey(fs::Vfs& vfs, const std::string& map, const std::string& list, const std::set<std::string>& classes) {
    game::World w(vfs, map, true);
    netgame::ServerArgs a;
    CHECK(parse({"/m", map, "tdm", "--weapons", list}, a));
    for (const auto& [k, v] : a.settings.overrideCvars) w.cvars[k] = v;
    w.mpStart("tdm");
    for (int b = 0; b < 6; ++b) {
        w.mpAddBot();
        w.frame();
    }
    for (int i = 0; i < 60; ++i) w.frame();
    int bots = 0, armed = 0;
    for (size_t c = 0; c < w.mpState().clients.size(); ++c) {
        if (!w.mpIsBot(int(c))) continue;
        ++bots;
        const auto* ent = w.mpClientEntity(int(c));
        const script::Value p = ent ? ent->obj->get("pers") : script::Value();
        std::string weapon;
        if (p.isArray())
            for (const auto& [k, v] : std::get<script::ArrayPtr>(p.v)->items)
                if (k.isString && k.s == "weapon") weapon = v.toString();
        if (weapon.empty()) continue;
        ++armed;
        const std::string cls = w.mpWeaponClass(weapon);
        if (!classes.count(cls)) std::cerr << map << ": bot " << c << " chose " << weapon << " (" << cls << ")\n";
        CHECK(classes.count(cls) != 0);
    }
    CHECK(bots == 6 && armed >= 4);
    CHECK(w.vm().errors().empty());
}

// One rcon command (replies are rate limited to one per 500 ms per address, so callers space their asks).
std::string rcon(int port, const std::string& command) {
    net::UdpSocket sock;
    if (!sock.open(0, 0)) return {};
    net::NetAddress to;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(port), to, 28960);
    auto pkt = net::makeOutOfBand("rcon pw " + command);
    sock.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
    static uint8_t buf[0x20000];
    net::NetAddress from;
    const int n = sock.receive(buf, sizeof buf, from, 2000);
    if (n <= 0) return {};
    auto t = net::outOfBandText(buf, n);
    return t ? *t : std::string();
}

// The value a console cvar query prints: "name" is:"value^7".
std::string cvarValue(const std::string& reply) {
    const size_t at = reply.find("is:\"");
    const size_t end = reply.rfind("^7\"");
    return at == std::string::npos || end == std::string::npos ? std::string("<none>") : reply.substr(at + 4, end - at - 4);
}

// A running server: asks it for the given cvars over rcon, then quits it.
std::map<std::string, std::string> runServer(fs::Vfs& vfs, std::vector<std::string> args, const std::vector<std::string>& ask, int* rc) {
    netgame::ServerArgs a;
    args.insert(args.begin(), "/m");
    size_t at = 0;
    while (at < args.size() && args[at][0] != '+') ++at;  // options go before the +commands
    args.insert(args.begin() + long(at), {"--port", "0", "--no-console", "--cvar", "rcon_password=pw"});
    if (!parse(args, a)) {
        std::cerr << "server args: " << a.error << "\n";
        ++failures;
        return {};
    }
    std::atomic<int> port{0};
    std::atomic<bool> quit{false};
    a.settings.onStarted = [&](int p) { port = p; };
    a.settings.frontEnd = [&]() -> std::vector<std::string> { return quit ? std::vector<std::string>{"quit"} : std::vector<std::string>{}; };
    std::thread t([&] { *rc = netgame::runDedicated(vfs, a.settings); });
    for (int i = 0; i < 1200 && port == 0 && *rc == -1; ++i) usleep(50000);
    std::map<std::string, std::string> out;
    if (port != 0)
        for (const std::string& k : ask) {
            usleep(600000);
            out[k] = cvarValue(rcon(port, k));
        }
    quit = true;
    t.join();
    return out;
}

void limitsInServer(fs::Vfs& vfs) {
    int rc = -1;
    auto v = runServer(vfs, {"mp_toujane", "tdm", "--scorelimit", "77", "--timelimit", "12.5"},
                       {"g_gametype", "scr_tdm_scorelimit", "scr_tdm_timelimit"}, &rc);
    CHECK(rc == 0 && v["g_gametype"] == "tdm" && v["scr_tdm_scorelimit"] == "77" && v["scr_tdm_timelimit"] == "12.5");
    rc = -1;
    v = runServer(vfs, {"mp_toujane", "sd", "--scorelimit", "4", "--timelimit", "0", "--roundlimit", "6"},
                  {"scr_sd_scorelimit", "scr_sd_timelimit", "scr_sd_roundlimit"}, &rc);
    CHECK(rc == 0 && v["scr_sd_scorelimit"] == "4" && v["scr_sd_timelimit"] == "0" && v["scr_sd_roundlimit"] == "6");
    // The command line wins over +set (as over a server.cfg), and 0 means no limit.
    rc = -1;
    v = runServer(vfs, {"mp_toujane", "dm", "--scorelimit", "0", "+set", "scr_dm_scorelimit", "9"}, {"scr_dm_scorelimit"}, &rc);
    CHECK(rc == 0 && v["scr_dm_scorelimit"] == "0");
    // Without the options the configuration's defaults stay.
    rc = -1;
    v = runServer(vfs, {"mp_toujane", "tdm"}, {"scr_tdm_scorelimit", "scr_tdm_timelimit"}, &rc);
    CHECK(rc == 0 && v["scr_tdm_scorelimit"] == "300" && v["scr_tdm_timelimit"] == "30");
    // A map without a gametype runs dm (default_mp.cfg's "random" is no choice).
    rc = -1;
    v = runServer(vfs, {"mp_toujane"}, {"g_gametype"}, &rc);
    CHECK(rc == 0 && v["g_gametype"] == "dm");
    // --weapons reaches the console too (iw2_weaponclasses and the grenade cvars).
    rc = -1;
    v = runServer(vfs, {"mp_toujane", "tdm", "--weapons", "smg,grenades"}, {"iw2_weaponclasses", "scr_allow_fraggrenades"}, &rc);
    CHECK(rc == 0 && v["iw2_weaponclasses"] == "smg" && v["scr_allow_fraggrenades"] == "1");
}

// The game side: the scripts read the cvars that --scorelimit / --timelimit / --roundlimit set and publish them as the
// ui_<gametype>_* values of the match.
void limitsInGame(fs::Vfs& vfs) {
    for (const std::string gt : {"dm", "tdm", "sd", "ctf", "hq"}) {
        game::World w(vfs, "mp_toujane", true);
        netgame::DedicatedSettings s;
        s.scoreLimit = 33;
        s.timeLimit = 17;
        if (gt == "sd") s.roundLimit = 5;
        std::vector<std::pair<std::string, std::string>> cv;
        CHECK(netgame::serverLimitCvars(s, gt, cv).empty());
        for (const auto& [k, v] : cv) w.cvars[k] = v;
        w.mpStart(gt);
        CHECK(w.cvars["ui_" + gt + "_scorelimit"] == "33");
        CHECK(w.cvars["ui_" + gt + "_timelimit"] == "17");
        if (gt == "sd") CHECK(w.cvars["ui_sd_roundlimit"] == "5");
        CHECK(w.vm().errors().empty());
    }
}

} // namespace

int main() {
    weaponLists();
    panelLists();
    commandLine();
    limitCvars();
    if (const char* main = std::getenv("IW2_MAIN")) {
        fs::Vfs vfs;
        vfs.mountDirectory(main);
        weaponsInGame(vfs);
        botsObey(vfs, "mp_toujane", "bolt,sniper,grenades", {"bolt", "sniper"});
        botsObey(vfs, "mp_leningrad", "smg,shotgun", {"smg", "shotgun"});
        botsObey(vfs, "mp_carentan", "rifle,sniper", {"rifle", "sniper"});
        limitsInGame(vfs);
        limitsInServer(vfs);
    } else {
        std::cout << "IW2_MAIN not set, game part skipped\n";
    }
    if (failures) {
        std::cerr << failures << " checks failed\n";
        return 1;
    }
    std::cout << "serveroptions: ok\n";
    return 0;
}
