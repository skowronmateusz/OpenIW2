#include "netgame/serverargs.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "game/mpbots.h"

namespace iw2::netgame {
namespace {

bool parseInt(const std::string& opt, const std::string& text, long lo, long hi, long& out, std::string& err) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != '\0' || errno == ERANGE) {
        err = opt + " needs a whole number, got '" + text + "'";
        return false;
    }
    if (v < lo || v > hi) {
        err = opt + " must be " + std::to_string(lo) + ".." + std::to_string(hi) + ", got " + text;
        return false;
    }
    out = v;
    return true;
}

bool parseMinutes(const std::string& opt, const std::string& text, double lo, double hi, double& out, std::string& err) {
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(text.c_str(), &end);
    if (text.empty() || end == text.c_str() || *end != '\0' || errno == ERANGE || v != v) {
        err = opt + " needs a number, got '" + text + "'";
        return false;
    }
    if (v < lo || v > hi) {
        err = opt + " must be " + std::to_string(int(lo)) + ".." + std::to_string(int(hi)) + ", got " + text;
        return false;
    }
    out = v;
    return true;
}

std::string minutesText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
} // namespace

const std::vector<std::string>& serverGametypes() {
    static const std::vector<std::string> g{"dm", "tdm", "sd", "ctf", "hq"};
    return g;
}

bool parseServerArgs(const std::vector<std::string>& args, ServerArgs& out) {
    DedicatedSettings& s = out.settings;
    s.botSkill = 3;
    size_t i = 0;
    if (args.empty()) {
        out.error = "missing <main-dir>";
        return false;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        out.help = true;
        return true;
    }
    out.mainDir = args[i++];
    if (!out.mainDir.empty() && (out.mainDir[0] == '-' || out.mainDir[0] == '+')) {
        out.error = "the first argument must be the main folder, got '" + out.mainDir + "'";
        return false;
    }
    const auto plain = [&](size_t k) { return k < args.size() && !args[k].empty() && args[k][0] != '-' && args[k][0] != '+'; };
    if (plain(i)) s.map = args[i++];
    if (!s.map.empty() && plain(i)) {
        s.gametype = args[i++];
        const auto& gts = serverGametypes();
        if (std::find(gts.begin(), gts.end(), s.gametype) == gts.end()) {
            std::string list;
            for (const auto& g : gts) list += (list.empty() ? "" : ", ") + g;
            out.error = "unknown gametype '" + s.gametype + "' (valid: " + list + ")";
            return false;
        }
    }
    std::string startup;  // +commands
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (!a.empty() && a[0] == '+') {
            startup += "\n" + a.substr(1);
            continue;
        }
        if (!startup.empty()) {  // an argument of the last +command
            startup += a.find_first_of(" \t;") != std::string::npos ? " \"" + a + "\"" : " " + a;
            continue;
        }
        const auto value = [&](const char* opt, std::string& v) {
            if (i + 1 >= args.size()) {
                out.error = std::string(opt) + " needs a value";
                return false;
            }
            v = args[++i];
            return true;
        };
        std::string v;
        long n = 0;
        if (a == "--port") {
            if (!value("--port", v) || !parseInt("--port", v, 0, 65535, n, out.error)) return false;
            s.port = int(n);
        } else if (a == "--maxclients") {
            if (!value("--maxclients", v) || !parseInt("--maxclients", v, 1, 20, n, out.error)) return false;
            s.maxClients = int(n);
        } else if (a == "--seconds") {
            if (!value("--seconds", v) || !parseInt("--seconds", v, 0, 31536000, n, out.error)) return false;
            s.seconds = int(n);
        } else if (a == "--bots") {
            if (!value("--bots", v) || !parseInt("--bots", v, 0, 19, n, out.error)) return false;
            s.bots = int(n);
        } else if (a == "--bot-skill") {
            if (!value("--bot-skill", v) || !parseInt("--bot-skill", v, 0, 3, n, out.error)) return false;
            s.botSkill = int(n);
        } else if (a == "--scorelimit") {
            if (!value("--scorelimit", v) || !parseInt("--scorelimit", v, 0, 100000, n, out.error)) return false;
            s.scoreLimit = int(n);
        } else if (a == "--timelimit") {
            double m = 0;
            if (!value("--timelimit", v) || !parseMinutes("--timelimit", v, 0, 1440, m, out.error)) return false;
            s.timeLimit = m;
        } else if (a == "--roundlimit") {
            if (!value("--roundlimit", v) || !parseInt("--roundlimit", v, 0, 1000, n, out.error)) return false;
            s.roundLimit = int(n);
        } else if (a == "--weapons") {
            if (!value("--weapons", v)) return false;
            game::MpWeaponSelection sel;
            const std::string err = game::mpParseWeaponList(v, sel);
            if (!err.empty()) {
                out.error = "--weapons: " + err;
                return false;
            }
            s.overrideCvars.emplace_back("iw2_weaponclasses", sel.classesCvar);
            s.overrideCvars.emplace_back("scr_allow_fraggrenades", sel.grenades ? "1" : "0");
            s.overrideCvars.emplace_back("scr_allow_smokegrenades", sel.grenades ? "1" : "0");
        } else if (a == "--no-console") {
            s.readStdin = false;
        } else if (a == "--cvar") {
            if (!value("--cvar", v)) return false;
            const size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) {
                out.error = "--cvar needs name=value, got '" + v + "'";
                return false;
            }
            s.cvars.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else {
            out.error = "unknown option " + a;
            return false;
        }
    }
    if (s.roundLimit >= 0 && !s.gametype.empty() && s.gametype != "sd") {
        out.error = "--roundlimit only applies to the gametype sd, not " + s.gametype;
        return false;
    }
    for (size_t p = 0, q; p <= startup.size(); p = q + 1) {
        q = startup.find('\n', p);
        if (q == std::string::npos) q = startup.size();
        if (q > p) s.startup.push_back(startup.substr(p, q - p));
    }
    return true;
}

std::string serverLimitCvars(const DedicatedSettings& s, const std::string& gametype,
                             std::vector<std::pair<std::string, std::string>>& out) {
    const auto& gts = serverGametypes();
    const bool known = std::find(gts.begin(), gts.end(), gametype) != gts.end();
    if ((s.scoreLimit >= 0 || s.timeLimit >= 0 || s.roundLimit >= 0) && !known)
        return "unknown gametype '" + gametype + "'";
    if (s.roundLimit >= 0 && gametype != "sd") return "--roundlimit only applies to the gametype sd, not " + gametype;
    if (s.scoreLimit >= 0) out.emplace_back("scr_" + gametype + "_scorelimit", std::to_string(s.scoreLimit));
    if (s.timeLimit >= 0) out.emplace_back("scr_" + gametype + "_timelimit", minutesText(s.timeLimit));
    if (s.roundLimit >= 0) out.emplace_back("scr_sd_roundlimit", std::to_string(s.roundLimit));
    return {};
}

std::string serverUsage() {
    return
        "usage: iw2server <main-dir> [<map> [<gametype>]] [options] [+command args ...]\n"
        "\n"
        "  <main-dir>           folder with the game's .iwd files\n"
        "  <map>                first map, e.g. mp_toujane (without it +map_rotate must load one)\n"
        "  <gametype>           dm, tdm, sd, ctf or hq (default: g_gametype of the configuration, else dm)\n"
        "\n"
        "options:\n"
        "  --port N             UDP port 0..65535 (default 28960; 0 picks a free one)\n"
        "  --maxclients N       player slots 1..20 including bots (default 8)\n"
        "  --bots N             add N bots on every map, 0..19 (default 0)\n"
        "  --bot-skill N        bot skill 0..3: recruit, regular, hardened, veteran (default 3)\n"
        "  --weapons LIST       allowed weapon classes, comma separated: rifle, bolt, sniper, smg, mg, shotgun,\n"
        "                       grenades, or all (default: all). At least one weapon class is needed and every\n"
        "                       nation must keep a weapon, e.g. --weapons bolt,sniper,grenades\n"
        "  --scorelimit N       score limit of the gametype, 0..100000 (0: none)\n"
        "  --timelimit MIN      time limit in minutes, 0..1440 (0: none)\n"
        "  --roundlimit N       rounds per map, 0..1000 (sd only; 0: none)\n"
        "  --cvar name=value    set a cvar before the first map\n"
        "  --seconds N          stop after N seconds (default: run until quit or Ctrl+C)\n"
        "  --no-console         do not read commands from stdin\n"
        "  --help               this text\n"
        "  +command args       console commands run at start-up in order, e.g. +exec server.cfg +map_rotate\n"
        "\n"
        "Options on the command line win over server.cfg. See docs/dedicated-server.md.\n";
}

} // namespace iw2::netgame
