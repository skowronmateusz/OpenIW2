// Implemented from the spec (docs/spec/dedicated.md).
#include "net/svconsole.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "net/oob.h"

namespace iw2::net {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ieq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::string fmt(const char* f, int v) {
    char b[32];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

// Splits a command buffer into commands at newlines and ';' outside double quotes.
std::vector<std::string> splitCommands(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') quoted = !quoted;
        if (!quoted && c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;  // comment to the end of the line
            out.push_back(cur);
            cur.clear();
            continue;
        }
        if ((c == '\n' || c == '\r') || (c == ';' && !quoted)) {
            out.push_back(cur);
            cur.clear();
            quoted = false;
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

// The text after the first token (the command name), leading whitespace removed.
std::string argsAfterFirst(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && static_cast<unsigned char>(line[i]) <= ' ') ++i;
    if (i < line.size() && line[i] == '"') {
        ++i;
        while (i < line.size() && line[i] != '"') ++i;
        if (i < line.size()) ++i;
    } else {
        while (i < line.size() && static_cast<unsigned char>(line[i]) > ' ') ++i;
    }
    while (i < line.size() && static_cast<unsigned char>(line[i]) <= ' ') ++i;
    std::string r = line.substr(i);
    while (!r.empty() && static_cast<unsigned char>(r.back()) <= ' ') r.pop_back();
    return r;
}

std::string joinFrom(const std::vector<std::string>& t, size_t first) {
    std::string r;
    for (size_t i = first; i < t.size(); ++i) {
        if (i > first) r += ' ';
        r += t[i];
    }
    return r;
}

// Server-info cvars reported by serverinfo / getstatus. TODO(spec): the original selects them by a cvar flag.
// Listed sorted by name ignoring case, as the 1.3 server prints them (observed byte for byte against getstatus and
// rcon serverinfo of the 1.3 Linux dedicated server).
const char* const kServerInfoCvars[] = {"fs_game",         "g_antilag",         "g_gametype",       "gamename",
                                        "mapname",         "protocol",          "scr_friendlyfire", "scr_killcam",
                                        "shortversion",    "sv_allowAnonymous", "sv_floodProtect",  "sv_hostname",
                                        "sv_maxclients",   "sv_maxPing",        "sv_maxRate",       "sv_minPing",
                                        "sv_privateClients", "sv_punkbuster",   "sv_pure",          "sv_voice"};

} // namespace

bool CaseLess::operator()(const std::string& a, const std::string& b) const { return lower(a) < lower(b); }

std::string stripColors(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '^' && i + 1 < s.size() && s[i + 1] != '^' && s[i + 1] >= '0' && s[i + 1] <= '9') {
            ++i;
            continue;
        }
        r += s[i];
    }
    return r;
}

std::string formatStatus(const std::string& mapName, const std::vector<ConsoleClient>& clients) {
    std::string s = "map: " + mapName + "\n";
    s += "num score ping guid   name            lastmsg address               qport rate\n";
    s += "--- ----- ---- ------ --------------- ------- --------------------- ----- -----\n";
    for (const ConsoleClient& c : clients) {
        s += fmt("%3i ", c.num);
        s += fmt("%5i ", c.score);
        if (c.state == ConsoleClient::State::Connecting) s += "CNCT ";
        else if (c.state == ConsoleClient::State::Zombie) s += "ZMBI ";
        else s += fmt("%4i ", std::min(c.ping, 9999));
        s += fmt("%6i ", 0);  // GUID: no authorisation
        s += c.name + "^7";
        const int pad = 16 - static_cast<int>(stripColors(c.name).size());
        int j = 0;
        do {
            s += ' ';
        } while (++j < pad);
        s += fmt("%7i ", c.lastMsgMs);
        s += c.address;
        const int apad = 22 - static_cast<int>(c.address.size());
        j = 0;
        do {
            s += ' ';
        } while (++j < apad);
        s += fmt("%5i", c.qport);
        s += fmt(" %5i", c.rate);
        s += "\n";
    }
    s += "\n";
    return s;
}

RotationStep rotateMap(std::string& current, const std::string& rotation) {
    RotationStep r;
    r.log = "map_rotate...\n\n";
    r.log += "\"sv_mapRotation\" is:\"" + rotation + "\"\n\n";
    r.log += "\"sv_mapRotationCurrent\" is:\"" + current + "\"\n\n";
    if (current.empty()) current = rotation;
    auto next = [&]() -> std::optional<std::string> {
        auto tok = tokenize(current);
        if (tok.empty()) {
            current.clear();
            return std::nullopt;
        }
        // The rest after the first token.
        current = argsAfterFirst(current);
        return tok[0];
    };
    auto tok = next();
    if (!tok) {
        current = rotation;
        tok = next();
    }
    for (;;) {
        if (!tok) {
            r.log += "No map specified in sv_mapRotation - forcing map_restart.\n";
            return r;
        }
        if (ieq(*tok, "gametype")) {
            auto g = next();
            if (!g) {
                r.log += "No gametype specified after 'gametype' keyword in sv_mapRotation - forcing map_restart.\n";
                return r;
            }
            r.log += "Setting g_gametype: " + *g + ".\n";
            r.gametype = *g;
        } else if (ieq(*tok, "map")) {
            auto m = next();
            if (!m) {
                r.log += "No map specified after 'map' keyword in sv_mapRotation - forcing map_restart.\n";
                return r;
            }
            r.log += "Setting map: " + *m + ".\n";
            r.map = *m;
            return r;
        } else {
            r.log += "Unknown keyword '" + *tok + "' in sv_mapRotation.\n";
        }
        tok = next();
    }
}

ServerConsole::ServerConsole(ConsoleHost& host) : host_(host) {
    print = [](const std::string& s) { std::cout << s << std::flush; };
}

std::string ServerConsole::cvar(const std::string& name) const {
    auto it = cvars.find(name);
    return it == cvars.end() ? std::string() : it->second;
}

void ServerConsole::setCvar(const std::string& name, const std::string& value) {
    cvars[name] = value;
    host_.cvarChanged(name, value);
}

std::string ServerConsole::serverInfo() const {
    std::string info;
    for (const char* k : kServerInfoCvars) {
        auto it = cvars.find(k);
        if (it != cvars.end() && !it->second.empty()) infoSetValueForKey(info, k, it->second);
    }
    return info;
}

void ServerConsole::execute(const std::string& text) {
    for (const std::string& line : splitCommands(text)) {
        auto tok = tokenize(line);
        if (tok.empty()) continue;
        command(tok, argsAfterFirst(line));
    }
}

std::string ServerConsole::capture(const std::string& text) {
    std::string buf;
    auto saved = print;
    print = [&](const std::string& s) { buf += s; };
    execute(text);
    print = saved;
    return buf;
}

void ServerConsole::mapRotate() {
    std::string current = cvar("sv_mapRotationCurrent");
    RotationStep r = rotateMap(current, cvar("sv_mapRotation"));
    out(r.log);
    setCvar("sv_mapRotationCurrent", current);
    if (r.gametype) setCvar("g_gametype", *r.gametype);
    if (r.map && host_.loadMap(*r.map)) return;
    // The forced map_restart is the console command, which needs a running server.
    if (!host_.running()) out("Server is not running.\n");
    else host_.restartMap(false);
}

void ServerConsole::command(const std::vector<std::string>& a, const std::string& argsText) {
    const std::string cmd = lower(a[0]);
    const bool running = host_.running();
    if (cmd == "set" || cmd == "seta" || cmd == "sets") {
        if (a.size() < 3) {
            out("USAGE: " + cmd + " <variable> <value>\n");
            return;
        }
        setCvar(a[1], joinFrom(a, 2));
    } else if (cmd == "exec") {
        if (a.size() != 2) {
            out("exec <filename> : execute a script file\n");
            return;
        }
        std::string name = a[1];
        if (name.find('.') == std::string::npos) name += ".cfg";
        std::string text;
        if (execDepth_ > 16 || !host_.readFile(name, text)) {
            out("couldn't exec " + name + "\n");
            return;
        }
        out("execing " + name + "\n");
        ++execDepth_;
        execute(text);
        --execDepth_;
    } else if (cmd == "quit") {
        host_.quit();
    } else if (cmd == "map" || cmd == "devmap") {
        if (a.size() < 2) return;
        if (!host_.loadMap(a[1])) out("Can't find map maps/mp/" + a[1] + ".d3dbsp\n");
    } else if (cmd == "map_rotate") {
        mapRotate();  // no running check (dedicated.md section 2): "+exec server.cfg +map_rotate" starts a server
    } else if (cmd == "heartbeat") {
        // Forces the next heartbeat in the original; this port has no master server (dedicated.md section 1).
    } else if (cmd == "map_restart" || cmd == "fast_restart" || cmd == "status" ||
               cmd == "kick" || cmd == "onlykick" || cmd == "tempbanuser" || cmd == "clientkick" ||
               cmd == "tempbanclient" || cmd == "banuser" || cmd == "banclient" || cmd == "say" || cmd == "tell" ||
               cmd == "serverinfo" || cmd == "dumpuser") {
        if (!running) {
            out("Server is not running.\n");
            return;
        }
        if (cmd == "map_restart" || cmd == "fast_restart") {
            host_.restartMap(cmd == "fast_restart");
        } else if (cmd == "status") {
            out(formatStatus(host_.mapName(), host_.clients()));
        } else if (cmd == "serverinfo") {
            out("Server info settings:\n");
            for (const char* k : kServerInfoCvars) {
                auto it = cvars.find(k);
                if (it == cvars.end() || it->second.empty()) continue;
                std::string key = k;
                key.resize(std::max<size_t>(key.size(), 20), ' ');
                out(key + it->second + "\n");
            }
        } else if (cmd == "dumpuser") {
            // dumpuser <name>: the client's userinfo, one key per line padded to 20 characters (observed).
            if (a.size() != 2) {
                out("Usage: info <userid>\n");
                return;
            }
            for (const ConsoleClient& cl : host_.clients()) {
                if (lower(stripColors(cl.name)) != lower(stripColors(a[1]))) continue;
                out("userinfo\n--------\n");
                size_t i = cl.userinfo.size() && cl.userinfo[0] == '\\' ? 1 : 0;
                while (i < cl.userinfo.size()) {
                    size_t ke = cl.userinfo.find('\\', i);
                    if (ke == std::string::npos) break;
                    size_t ve = cl.userinfo.find('\\', ke + 1);
                    std::string key = cl.userinfo.substr(i, ke - i);
                    key.resize(std::max<size_t>(key.size(), 20), ' ');
                    out(key + cl.userinfo.substr(ke + 1, ve == std::string::npos ? std::string::npos : ve - ke - 1) + "\n");
                    if (ve == std::string::npos) break;
                    i = ve + 1;
                }
                return;
            }
        } else if (cmd == "kick" || cmd == "onlykick" || cmd == "tempbanuser" || cmd == "banuser") {
            // Guid-based bans need a guid; this port has none (no authorisation), so kick, onlykick and tempBanUser are
            // the same and banUser refuses (dedicated.md section 1).
            const bool ban = cmd == "banuser";
            if (a.size() != 2) {
                if (ban) out("Usage: banUser <player name>\n");
                else out("Usage: " + a[0] + " <player name>\n" + a[0] + " all = kick everyone\n");
                return;
            }
            const auto list = host_.clients();
            if (!ban && ieq(a[1], "all")) {
                for (const auto& c : list) host_.kick(c.num, "EXE_PLAYERKICKED");
                return;
            }
            for (const auto& c : list) {
                // The raw name, then the name without colour codes, against the argument as typed (0x808baf8).
                if (ieq(c.name, a[1]) || ieq(stripColors(c.name), a[1])) {
                    if (ban) out("Can't ban user, GUID is 0\n");
                    else host_.kick(c.num, "EXE_PLAYERKICKED");
                    return;
                }
            }
            out("Player " + a[1] + " is not on the server\n");
        } else if (cmd == "clientkick" || cmd == "tempbanclient" || cmd == "banclient") {
            const bool ban = cmd == "banclient";
            if (a.size() != 2) {
                if (ban) out("Usage: banClient <client number>\n");
                else out("Usage: " + a[0] + " <client number>\n");
                return;
            }
            // The slot rules of 0x808bc10.
            if (a[1].empty() || a[1].find_first_not_of("0123456789") != std::string::npos) {
                out("Bad slot number: " + a[1] + "\n");
                return;
            }
            const int n = std::atoi(a[1].c_str());
            const std::string maxc = cvar("sv_maxclients");
            if (!maxc.empty() && n >= std::atoi(maxc.c_str())) {
                out("Bad client slot: " + std::to_string(n) + "\n");
                return;
            }
            for (const auto& c : host_.clients()) {
                if (c.num == n) {
                    if (ban) out("Can't ban user, GUID is 0\n");
                    else host_.kick(n, "EXE_PLAYERKICKED");
                    return;
                }
            }
            out("Client " + std::to_string(n) + " is not active\n");
        } else if (cmd == "say" || cmd == "tell") {
            const bool tell = cmd == "tell";
            if (a.size() < (tell ? 3u : 2u)) return;
            std::string text = tell ? argsAfterFirst(argsText) : argsText;
            if (!text.empty() && text[0] == '"') {
                text = text.substr(1);
                if (!text.empty()) text.pop_back();
            }
            const std::string msg = "h \"console: " + text + "\"";
            if (!tell) {
                host_.serverCommand(-1, msg);
                return;
            }
            const int n = std::atoi(a[1].c_str());
            for (const auto& c : host_.clients())
                if (c.num == n && c.state == ConsoleClient::State::Active) host_.serverCommand(n, msg);
        }
    } else if (cmd == "debugdamage" && std::getenv("IW2_DEBUG_DAMAGE")) {
        // Debug only (docs/debug-flags.md): debugdamage <victim> <attacker> <amount>, attacker -1 = the world.
        if (a.size() != 4 || !running) return;
        auto clientNum = [&](const std::string& t) { // a client number or a player name
            for (const ConsoleClient& c : host_.clients())
                if (ieq(c.name, t) || ieq(stripColors(c.name), t)) return c.num;
            return std::atoi(t.c_str());
        };
        if (!host_.debugDamage(clientNum(a[1]), a[2] == "-1" ? -1 : clientNum(a[2]), std::atoi(a[3].c_str()))) out("debugdamage: no such client\n");
        else out("debugdamage " + a[1] + " " + a[2] + " " + a[3] + "\n");
    } else if (cmd == "addtestclient") {
        if (!running) {
            out("Server is not running.\n");
            return;
        }
        const int count = a.size() >= 2 ? std::max(1, std::atoi(a[1].c_str())) : 1;
        for (int i = 0; i < count; ++i)
            if (!host_.addTestClient()) break;
    } else if (a.size() == 1 && cvars.count(a[0])) {
        out("\"" + a[0] + "\" is:\"" + cvar(a[0]) + "^7\"\n");
    } else if (a.size() >= 2 && cvars.count(a[0])) {
        setCvar(a[0], joinFrom(a, 1));  // a known cvar name followed by a value sets it
    } else if (cmd == "unbanuser") {
        // Removes a name from ban.txt in the original; this port writes no ban file, so there is nothing to remove.
        if (a.size() != 2) out("Usage: unban <client name>\n");
    }
    // Anything else (for example the bind and unbindall lines of default_mp.cfg) is ignored without a message: the 1.3
    // dedicated server's command executor (0x80608a6) falls through to the dvar and game-command checks and prints
    // nothing (docs/spec/dedicated.md).

}

std::optional<std::vector<std::string>> ServerConsole::rcon(const std::string& line, const std::string& from,
                                                             int64_t nowMs) {
    if (haveRcon_ && nowMs - lastRconMs_ <= 499) return std::nullopt;
    haveRcon_ = true;
    lastRconMs_ = nowMs;
    const auto tok = tokenize(line);
    const std::string password = tok.size() > 1 ? tok[1] : std::string();
    const std::string rconPassword = cvar("rcon_password");
    const bool valid = !rconPassword.empty() && password == rconPassword;
    std::cout << (valid ? "Rcon from " : "Bad rcon from ") << from << ":\n" << line << "\n" << std::flush;

    std::string buf;
    auto saved = print;
    print = [&](const std::string& s) { buf += s; };
    if (rconPassword.empty()) {
        out("The server must set 'rcon_password' for clients to use 'rcon'.\n");
    } else if (!valid) {
        if (!password.empty()) out("Invalid password.\n");
        else out("You must log in with 'rcon login <password>' before using 'rcon'.\n");
    } else {
        // Tokens 3..n joined with spaces; a token that is empty or holds a control or space character is quoted again
        // (docs/spec/dedicated.md section 4). `;` is quoted too, because this console splits on it, and the original does not.
        std::string cmdline;
        for (size_t i = 2; i < tok.size(); ++i) {
            if (i > 2) cmdline += ' ';
            bool space = tok[i].empty() || tok[i].find(';') != std::string::npos;
            for (char ch : tok[i]) space |= static_cast<unsigned char>(ch) <= ' ';
            cmdline += space ? "\"" + tok[i] + "\"" : tok[i];
            if (cmdline.size() > 1024) break;
        }
        if (cmdline.size() > 1024) cmdline.resize(1024);
        execute(cmdline);
    }
    print = saved;
    // The original collects up to 0x1fff0 bytes and sends one packet (docs/spec/dedicated.md section 4); openiw2 stays
    // below the usual path MTU with several packets.
    std::vector<std::string> replies;
    constexpr size_t kChunk = 1200;
    for (size_t i = 0; i < buf.size(); i += kChunk) replies.push_back("print\n" + buf.substr(i, kChunk));
    if (replies.empty()) replies.push_back("print\n");
    return replies;
}

} // namespace iw2::net
