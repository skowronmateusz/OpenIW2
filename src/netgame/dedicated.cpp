// The dedicated server loop shared by tools/iw2server and iw2view (docs/spec/dedicated.md); implemented from the spec.
#include "netgame/dedicated.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "audio/sound.h"
#include "fs/vfs.h"
#include "game/world.h"
#include "net/oob.h"
#include "net/server.h"
#include "net/svconsole.h"
#include "netgame/serverargs.h"
#include "netgame/worldgame.h"

namespace iw2::netgame {
namespace {

using iw2::game::World;
using iw2::net::SnapshotView;
using iw2::net::UserCmd;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

using iw2::netgame::WorldGame;

// The console host: owns the current World and switches maps (docs/spec/dedicated.md).
class Dedicated : public iw2::net::ConsoleHost {
public:
    Dedicated(iw2::fs::Vfs& vfs, int maxClients) : vfs_(vfs), maxClients_(maxClients) {}

    iw2::net::ServerConsole* console = nullptr;
    iw2::net::Server* server = nullptr;
    WorldGame* game = nullptr;
    int bots = 0;          // --bots: bots added on every map
    int botSkill = 3;
    DedicatedSettings limits; // scoreLimit / timeLimit / roundLimit of the command line
    bool quitRequested = false;

    World* world() { return world_.get(); }
    void setMaxClients(int n) { maxClients_ = n; }
    const std::string& map() const { return map_; }

    bool running() const override { return world_ != nullptr; }
    std::string mapName() const override { return map_; }

    bool loadMap(const std::string& map) override {
        if (!vfs_.exists("maps/mp/" + map + ".d3dbsp")) return false;
        std::string gametype = console->cvar("g_gametype");
        // default_mp.cfg sets g_gametype to "random", which stands for no choice: a map given without a gametype runs dm.
        if (gametype.empty() || iequals(gametype, "random")) gametype = "dm";
        if (!vfs_.exists("maps/mp/gametypes/" + gametype + ".gsc")) {
            std::printf("Can't find gametype %s\n", gametype.c_str());
            return false;
        }
        console->cvars["g_gametype"] = gametype;
        std::printf("------ Server Initialization ------\nServer: %s (%s)\n", map.c_str(), gametype.c_str());
        auto w = std::make_unique<World>(vfs_, map, true);
        w->print = [](const std::string& s) { std::cout << "[script] " << s << "\n"; };
        w->vm().setLoopGuardIterations(200000u);
        aliases_ = std::make_unique<iw2::audio::SoundAliases>();
        aliases_->load(vfs_, map, false, "all_mp");
        w->soundExists = [a = aliases_.get()](const std::string& n) { return a->exists(n); };
        // --scorelimit / --timelimit / --roundlimit: the limits of this map's gametype (the round limit only for sd).
        {
            DedicatedSettings l = limits;
            if (gametype != "sd") l.roundLimit = -1;
            std::vector<std::pair<std::string, std::string>> cv;
            if (serverLimitCvars(l, gametype, cv).empty())
                for (const auto& [k, v] : cv) console->cvars[k] = v;
        }
        for (const auto& [k, v] : console->cvars) w->cvars[k] = v;
        w->cvars["mapname"] = map;
        w->cvars["g_gametype"] = gametype;
        w->mpSetDedicated(); // client 0 is a network client or a bot like every other
        w->mpStart(gametype);
        errorsShown_ = 0;
        world_ = std::move(w);
        map_ = map;
        gametype_ = gametype;
        console->cvars["mapname"] = map;
        const int serverId = static_cast<int>((nowMs() + 1 + server->serverId) & 0xff);
        game->setWorld(world_.get(), map, gametype, serverId);
        game->hostname = hostname();
        server->info().mapname = map;
        server->info().gametype = gametype;
        server->info().hostname = hostname();
        server->newMap(serverId);
        // Network clients that were connected take a client of the new world again.
        for (int s = 0; s < server->maxClients; ++s) {
            const auto& c = server->client(s);
            if (c.state != iw2::net::ServerClientState::Free) game->clientConnect(s, c.userinfo);
        }
        world_->mpSetBotSkill(botSkill);
        for (int b = 0; b < bots; ++b)
            if (!addTestClient()) break;
        // The graph is made here, while the map loads, and not by the first bot of the match: generating takes tens of
        // seconds without a cached file, and in the running match every client would time out.
        if (bots > 0) world_->mpNav();
        return true;
    }
    void restartMap(bool fast) override {
        // TODO(spec): fast_restart keeps the loaded level in the original; openiw2 reloads in both cases.
        (void)fast;
        if (!map_.empty()) loadMap(map_);
    }

    std::vector<iw2::net::ConsoleClient> clients() override {
        std::vector<iw2::net::ConsoleClient> out;
        if (!world_) return out;
        std::map<int, int> slotOf;
        for (const auto& [slot, n] : game->slots()) slotOf[n] = slot;
        const auto& mp = world_->mpState();
        for (size_t n = 0; n < mp.clients.size(); ++n) {
            const auto& mc = mp.clients[n];
            if (mc.state == iw2::game::MpClientState::Free) continue;
            iw2::net::ConsoleClient c;
            c.num = static_cast<int>(n);
            c.name = mc.name;
            if (mc.ent) {
                const auto sc = mc.ent->obj->get("score");
                if (sc.isInt()) c.score = sc.asInt();
                else if (sc.isFloat()) c.score = static_cast<int>(sc.asFloat());
            }
            c.state = mc.state == iw2::game::MpClientState::Active ? iw2::net::ConsoleClient::State::Active
                                                                   : iw2::net::ConsoleClient::State::Connecting;
            auto it = slotOf.find(c.num);
            if (it != slotOf.end()) {
                const auto& sc = server->client(it->second);
                if (sc.state != iw2::net::ServerClientState::Active) c.state = iw2::net::ConsoleClient::State::Connecting;
                c.ping = sc.ping;
                c.lastMsgMs = static_cast<int>(nowMs() - sc.lastPacketMs);
                c.address = sc.address.toString();
                c.qport = sc.qport;
                c.rate = sc.rate;
                c.userinfo = sc.userinfo;
            } else {
                c.bot = true;
                c.address = "bot";
            }
            out.push_back(c);
        }
        return out;
    }
    int usedClients() {
        return static_cast<int>(clients().size());
    }
    void kick(int num, const std::string& reason) override {
        for (const auto& [slot, n] : game->slots())
            if (n == num) {
                server->dropClient(slot, reason);
                return;
            }
        if (world_ && world_->mpIsBot(num)) world_->mpKickBot(num);
    }
    void serverCommand(int num, const std::string& text) override {
        if (num < 0) {
            server->sendServerCommand(-1, text);
            std::printf("%s\n", text.c_str());
            return;
        }
        for (const auto& [slot, n] : game->slots())
            if (n == num) server->sendServerCommand(slot, text);
    }
    bool readFile(const std::string& name, std::string& out) override {
        auto data = vfs_.read(name);
        if (!data) return false;
        out.assign(data->begin(), data->end());
        return true;
    }
    void quit() override { quitRequested = true; }
    bool addTestClient() override {
        if (!world_ || usedClients() >= maxClients_) return false;
        const int n = world_->mpAddBot();
        if (n < 0) return false;
        world_->frame();  // one connect per frame (as iw2game --mp-bots)
        return true;
    }
    void cvarChanged(const std::string& name, const std::string& value) override {
        if (world_) world_->cvars[name] = value;
        if (iequals(name, "sv_hostname")) {
            if (game) game->hostname = value;
            if (server) server->info().hostname = value;
        } else if (iequals(name, "sv_privateClients") && server) {
            server->privateClients = std::atoi(value.c_str());
        } else if (iequals(name, "sv_privatePassword") && server) {
            server->privatePassword = value;
        }
    }

    // getinfo / getstatus with the live game (docs/spec/dedicated.md section 6).
    void fillStatus(iw2::net::ServerInfo& info) {
        info.players.clear();
        const auto list = clients();
        info.clients = static_cast<int>(list.size());
        info.maxClients = maxClients_ - server->privateClients;
        info.hostname = hostname();
        const std::string pw = console->cvar("g_password");
        info.password = !pw.empty() && !iequals(pw, "none");
        info.friendlyFire = std::atoi(console->cvar("scr_friendlyfire").c_str());
        info.killcam = std::atoi(console->cvar("scr_killcam").c_str());
        info.statusCvars.clear();
        const std::string si = console->serverInfo();
        auto tok = splitInfo(si);
        for (size_t i = 0; i + 1 < tok.size(); i += 2) info.statusCvars.emplace_back(tok[i], tok[i + 1]);
        for (const auto& c : list) info.players.push_back({c.score, c.ping, c.name});
    }
    // The game's connect check (g_password) and the slot count shared with bots.
    std::string connectRejection(const std::string& userinfo) {
        if (auto r = iw2::net::gamePasswordRejection(console->cvar("g_password"), userinfo); !r.empty()) return r;
        if (usedClients() >= maxClients_) return "EXE_SERVERISFULL";
        return {};
    }
    void showScriptErrors() {
        if (!world_) return;
        const auto& errors = world_->vm().errors();
        for (; errorsShown_ < errors.size(); ++errorsShown_) std::cerr << "script error: " << errors[errorsShown_] << "\n";
    }

    static bool iequals(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

private:
    std::string hostname() const {
        const std::string h = console->cvar("sv_hostname");
        return h.empty() ? "openiw2" : h;
    }
    static std::vector<std::string> splitInfo(const std::string& s) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < s.size() && s[i] == '\\') {
            size_t j = s.find('\\', i + 1);
            out.push_back(s.substr(i + 1, (j == std::string::npos ? s.size() : j) - i - 1));
            if (j == std::string::npos) break;
            i = j;
        }
        return out;
    }

    iw2::fs::Vfs& vfs_;
    int maxClients_;
    std::unique_ptr<World> world_;
    std::unique_ptr<iw2::audio::SoundAliases> aliases_;
    std::string map_, gametype_;
    size_t errorsShown_ = 0;
};

// Non-blocking reader of console lines from stdin; stops at end of file.
class StdinConsole {
public:
    bool enabled = true;
    std::vector<std::string> poll() {
        std::vector<std::string> lines;
        if (!enabled) return lines;
        pollfd p{0, POLLIN, 0};
        while (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            const ssize_t n = ::read(0, buf, sizeof buf);
            if (n <= 0) {
                enabled = false;
                break;
            }
            pending_.append(buf, static_cast<size_t>(n));
        }
        size_t nl;
        while ((nl = pending_.find('\n')) != std::string::npos) {
            lines.push_back(pending_.substr(0, nl));
            pending_.erase(0, nl + 1);
        }
        if (!enabled && !pending_.empty()) {
            lines.push_back(pending_);
            pending_.clear();
        }
        return lines;
    }

private:
    std::string pending_;
};


std::atomic<bool> gSignalQuit{false};
void onSignal(int) { gSignalQuit = true; }

} // namespace

bool menuStartIsDedicated(const std::string& dedicatedValue) { return std::atoi(dedicatedValue.c_str()) != 0; }

DedicatedSettings dedicatedFromMenu(const std::map<std::string, std::string>& menuCvars, const std::string& startCommand,
                                    int bots, int botSkill, int port) {
    DedicatedSettings s;
    size_t e = startCommand.find_last_not_of(" \t\r\n");
    const size_t b = startCommand.rfind(' ', e);
    if (e != std::string::npos) s.map = startCommand.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
    for (const auto& [k, v] : menuCvars) s.cvars.emplace_back(k, v);
    if (auto it = menuCvars.find("g_gametype"); it != menuCvars.end()) s.gametype = it->second;
    s.bots = bots;
    s.botSkill = botSkill;
    s.port = port;
    s.readStdin = false;
    return s;
}

int runDedicated(fs::Vfs& vfs, const DedicatedSettings& s) {
    // The console and its cvars exist before the first map; +set / +exec may change them.
    Dedicated host(vfs, 0);
    iw2::net::ServerConsole console(host);
    host.console = &console;
    console.cvars["sv_hostname"] = "openiw2";
    console.cvars["g_gametype"] = "dm";
    console.cvars["sv_maxclients"] = "8";
    console.cvars["net_port"] = std::to_string(iw2::net::kDefaultPort);
    console.cvars["gamename"] = "Call of Duty 2";
    console.cvars["protocol"] = std::to_string(iw2::net::kProtocolVersion);
    console.cvars["shortversion"] = iw2::net::kProtocolVersionString;
    for (const char* k : {"rcon_password", "g_password", "sv_privatePassword", "sv_mapRotation", "sv_mapRotationCurrent"})
        console.cvars[k] = "";
    // Server-info cvars with the 1.3 dedicated server's values (observed in its getstatus); openiw2 has no
    // authorisation, flood protection, rate or ping limits, PunkBuster or voice.
    for (const auto& [k, v] : {std::pair<const char*, const char*>{"sv_privateClients", "0"}, {"g_antilag", "1"},
                               {"scr_friendlyfire", "0"}, {"scr_killcam", "1"}, {"sv_allowAnonymous", "0"},
                               {"sv_floodProtect", "1"}, {"sv_maxPing", "0"}, {"sv_maxRate", "0"}, {"sv_minPing", "0"},
                               {"sv_punkbuster", "0"}, {"sv_pure", "0"}, {"sv_voice", "0"}})
        console.cvars[k] = v;
    // The 1.3 dedicated server runs the default configuration first (VAs in the 1.3 Linux dedicated server: 0x80a32d4
    // picks default_localize_mp.cfg when it exists, else default_mp.cfg; executed at start, 0x806203c), so its game
    // cvars (score and time limits, ...) are those of a 1.3 server (docs/spec/dedicated.md); the command line comes
    // after it.
    {
        std::string text;
        console.execute(host.readFile("default_localize_mp.cfg", text) ? "exec default_localize_mp.cfg"
                                                                       : "exec default_mp.cfg");
    }
    for (const auto& [k, v] : s.cvars) console.cvars[k] = v;
    if (!s.gametype.empty()) console.cvars["g_gametype"] = s.gametype;
    if (s.maxClients > 0) console.cvars["sv_maxclients"] = std::to_string(s.maxClients);
    if (s.port >= 0) console.cvars["net_port"] = std::to_string(s.port);

    // Start-up commands up to the first map command set cvars (+set, +exec); the map ones run after the socket opens.
    std::vector<std::string> before, after;
    {
        bool mapSeen = false;
        for (const std::string& line : s.startup) {
            auto tok = iw2::net::tokenize(line);
            if (tok.empty()) continue;
            const std::string c = tok[0];
            if (c == "map" || c == "devmap" || c == "map_rotate" || c == "map_restart" || c == "fast_restart")
                mapSeen = true;
            (mapSeen ? after : before).push_back(line);
        }
    }
    for (const auto& line : before) console.execute(line);
    // The command line wins over the configuration: the weapon list now, the limits of each map's gametype at its load.
    for (const auto& [k, v] : s.overrideCvars) console.cvars[k] = v;
    host.limits = s;
    {
        // A gametype known at this point (the command line or server.cfg) is checked now; with a rotation it is only
        // known when the map loads.
        std::vector<std::pair<std::string, std::string>> unused;
        const std::string gt = console.cvar("g_gametype");
        const auto& known = serverGametypes();
        if ((s.scoreLimit >= 0 || s.timeLimit >= 0 || s.roundLimit >= 0) && std::find(known.begin(), known.end(), gt) != known.end()) {
            const std::string err = serverLimitCvars(s, gt, unused);
            if (!err.empty()) {
                std::fprintf(stderr, "iw2server: error: %s\n", err.c_str());
                return 2;
            }
        }
    }

    const int maxClients = std::max(1, std::min(20, std::atoi(console.cvar("sv_maxclients").c_str())));  // World has 20 slots
    host.setMaxClients(maxClients);
    host.bots = s.bots;
    host.botSkill = s.botSkill;

    WorldGame game(nullptr, "", "", 0, maxClients);
    iw2::net::Server server(game, static_cast<uint32_t>(nowMs()));
    game.sendCommand = [&server](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
    game.clientPing = [&server](int slot) { return server.client(slot).ping; };
    game.onRestart = [&server] { server.snapFlags ^= 4; };
    server.maxClients = maxClients;
    server.checksumFeed = static_cast<int>(nowMs() * 2654435761u);
    server.privateClients = std::atoi(console.cvar("sv_privateClients").c_str());
    server.privatePassword = console.cvar("sv_privatePassword");
    host.server = &server;
    host.game = &game;
    server.oobHook = [&](const iw2::net::NetAddress& from, const std::string& line, int64_t now) {
        auto tok = iw2::net::tokenize(line);
        if (tok.empty() || tok[0] != "rcon") return false;
        auto replies = console.rcon(line, from.toString(), now);
        if (replies)
            for (const auto& r : *replies) server.sendOutOfBand(from, r);
        return true;
    };
    server.statusHook = [&](iw2::net::ServerInfo& info) { host.fillStatus(info); };
    server.connectFilter = [&](const std::string& userinfo) { return host.connectRejection(userinfo); };
    if (!server.open(static_cast<uint16_t>(std::atoi(console.cvar("net_port").c_str())))) {
        std::fprintf(stderr, "cannot bind UDP port %s\n", console.cvar("net_port").c_str());
        return 1;
    }
    if (!s.map.empty()) after.insert(after.begin(), "map " + s.map);
    for (const auto& line : after) console.execute(line);
    if (!host.running()) {
        std::fprintf(stderr, "no map loaded (give <map> or +map <name>)\n");
        return 1;
    }
    std::printf("iw2server: %s %s on UDP %u\n", host.map().c_str(), console.cvar("g_gametype").c_str(), server.port());
    std::fflush(stdout);
    if (s.onStarted) s.onStarted(server.port());

    StdinConsole in;
    in.enabled = s.readStdin;
    // SIGINT / SIGTERM (Ctrl+C, a plain kill, Quit in Activity Monitor) end the server like quit.
    gSignalQuit = false;
    auto oldInt = std::signal(SIGINT, onSignal), oldTerm = std::signal(SIGTERM, onSignal);
    const int64_t start = nowMs();
    while (!host.quitRequested && !gSignalQuit && (s.seconds <= 0 || nowMs() - start < s.seconds * 1000LL)) {
        server.poll(nowMs(), 5);
        for (const auto& line : in.poll()) console.execute(line);
        if (s.frontEnd)
            for (const auto& line : s.frontEnd()) console.execute(line);
        host.showScriptErrors();
        // A gametype that ended its round with exitLevel goes on with the rotation.
        if (host.world() && host.world()->mpState().exitRequested) console.mapRotate();
        std::fflush(stdout);
    }
    // quit / the end of --seconds: the clients learn that the server quit (docs/spec/net.md section 14).
    server.shutdown("EXE_SERVERQUIT");
    std::signal(SIGINT, oldInt);
    std::signal(SIGTERM, oldTerm);
    std::fflush(stdout);
    return 0;
}

} // namespace iw2::netgame
