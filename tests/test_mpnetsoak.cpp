// Network soak with invariants (docs/spec/net.md, docs/spec/mp.md): starts iw2server as a child process (port 0,
// bots, short time limits, a rotation over several maps and gametypes) and connects scripted network clients that run
// the client side of iw2view's network play step by step: the server commands (`t` / `u` menus answered with `mr`,
// `v` client cvars), a remote World per gamestate (World::mpStartRemote) fed by RemoteView (players, apply,
// placeTrajectories, playerEvents, followEvents, killcamView), the client weapon (game::PlayerWeapon with the view
// world's ammunition, the weapon select and the network lag rule of iw2view's syncWeapon), the user command
// (netgame::makeUserCmd with the weapon byte of the weapon list) and the prediction (netgame::Predictor from the
// newest snapshot, reset per gamestate, weaponMoveOf of the client weapon).
//
// Each life of the first client runs a scenario: walk straight, jumps, crouch / prone / stand, bursts to an empty
// magazine and a reload, weapon change to the other slot and back, a frag grenade, use, then it waits for its death
// (the bots fight) and watches the killcam. Invariants are checked every frame and reported as
//   <id> map=<map> gt=<gametype> life=<n> t=<seconds since the run start> <detail>
// The ids listed in the known-violations file (one per line, `#` comments) are reported as KNOWN and do not fail the
// test; any other id fails it. Whoever fixes a known violation removes its line. A known id that did not occur is a
// warning only. The report goes to stdout and to the report file.
//
// The second client leaves and joins again every 25 seconds, outside the first client's measured steps; hud.duplicate
// reports two live HUD elements of a client with one position and content (the per-client set of an earlier connection
// next to the new one).
//
// Environment: IW2_MAIN (required), IW2_SOAK_SERVER (the iw2server binary, set by CMake), IW2_SOAK_KNOWN (the
// known-violations file), IW2_SOAK_REPORT (the report file), IW2_SOAK_ROTATION (sv_mapRotation, default three maps),
// IW2_SOAK_MINUTES (time limit of each map in minutes, default 1.25), IW2_SOAK_MAPS (maps to play before the end,
// default the rotation's length), IW2_SOAK_BOTS (default 3). docs/debug-flags.md lists them.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "assets/xanim.h"
#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/mphud.h"
#include "game/playerweapon.h"
#include "game/pmove.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/oob.h"
#include "netgame/netcombat.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

extern char** environ;

namespace {

using namespace iw2;
using Clock = std::chrono::steady_clock;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
int64_t gStart = 0;
double runSeconds() { return double(nowMs() - gStart) / 1000.0; }

int32_t getI(const std::vector<uint8_t>& b, size_t off) {
    int32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// Report

struct Violation {
    std::string id, map, gametype, detail;
    int life = 0;
    double t = 0;
};
struct Report {
    std::vector<Violation> all;
    std::map<std::string, int> perId;
    std::string map, gametype;
    int life = 0;
    std::vector<std::string> notes;  // measurements
    void add(const std::string& id, const std::string& detail) {
        // At most 5 lines per id and map in the report; every occurrence is counted.
        const int n = ++perId[id + "@" + map + "/" + gametype];
        if (n <= 5) all.push_back({id, map, gametype, detail, life, runSeconds()});
        else if (n == 6) all.push_back({id, map, gametype, "(further occurrences on this map only counted)", life, runSeconds()});
        std::cout << "VIOLATION " << id << " map=" << map << " gt=" << gametype << " life=" << life << " t="
                  << runSeconds() << " " << detail << "\n";
    }
    void note(const std::string& s) {
        notes.push_back(s);
        std::cout << "note " << s << "\n";
    }
};
Report gReport;

// ---------------------------------------------------------------------------------------------------------------
// The server child process

struct ServerProcess {
    pid_t pid = -1;
    int in = -1, out = -1;
    std::string buffer;
    std::vector<std::string> lines;  // complete lines not taken yet
    std::ofstream log;
    int port = 0;

    bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& logPath) {
        int pin[2], pout[2];
        if (pipe(pin) != 0 || pipe(pout) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
        posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
        posix_spawn_file_actions_adddup2(&fa, pout[1], 2);
        posix_spawn_file_actions_addclose(&fa, pin[1]);
        posix_spawn_file_actions_addclose(&fa, pout[0]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(pin[0]);
        close(pout[1]);
        if (rc != 0) return false;
        in = pin[1];
        out = pout[0];
        fcntl(out, F_SETFL, fcntl(out, F_GETFL) | O_NONBLOCK);
        log.open(logPath);
        return true;
    }
    void pump() {
        char buf[8192];
        for (;;) {
            const ssize_t n = read(out, buf, sizeof buf);
            if (n <= 0) break;
            buffer.append(buf, size_t(n));
        }
        size_t p;
        while ((p = buffer.find('\n')) != std::string::npos) {
            std::string l = buffer.substr(0, p);
            buffer.erase(0, p + 1);
            if (log) log << l << "\n";
            lines.push_back(std::move(l));
        }
    }
    std::vector<std::string> take() {
        pump();
        return std::exchange(lines, {});
    }
    void command(const std::string& line) {
        const std::string l = line + "\n";
        if (in >= 0) (void)!write(in, l.data(), l.size());
    }
    bool alive() const { return pid > 0 && kill(pid, 0) == 0; }
    long rssKb() const {
        const std::string cmd = "ps -o rss= -p " + std::to_string(pid);
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return -1;
        long v = -1;
        if (std::fscanf(f, "%ld", &v) != 1) v = -1;
        pclose(f);
        return v;
    }
    void stop() {
        if (pid <= 0) return;
        command("quit");
        for (int i = 0; i < 50; ++i) {
            int st = 0;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                pid = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            pid = -1;
        }
        pump();
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        in = out = -1;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// A scripted network client (iw2view's client side of the network game)

constexpr uint32_t kFollowBit = 0x100000;  // pm_flags of a followed player state (killcam)

struct Scenario {
    // What the script holds this frame.
    float forward = 0, right = 0;
    uint32_t buttons = 0;
    std::string select;  // a weapon select change ("" none)
};

struct NetClient {
    std::string name;
    const fs::Vfs* vfs = nullptr;
    std::unique_ptr<net::GameClient> gc;
    std::unique_ptr<game::World> view;
    netgame::RemoteView rv;
    game::PlayerWeapon pweapon;
    game::PlayerState ps;
    netgame::Predictor predictor;
    netgame::PredictedWeapon predictedWeapon;  // the weapon, counters and shots against the acknowledged player state
    int predictorGamestate = -1;  // the gamestate the prediction belongs to
    std::string weaponSelect, weaponWritten, prevSelect;
    int64_t netSelectUntilMs = 0;
    std::tuple<int, int> loadedGamestate{0, -1};
    std::string map, gametype;
    std::vector<std::string> menuStack;
    std::string menuLog;  // the newest menu steps, for the report
    std::map<std::string, std::string> cvars;
    int lastSnapMsg = -1;
    int offhandCommands = 0;  // `C`: the offhand announcements of this connection
    int64_t lastFrameMs = 0;
    float pitch = 0, yaw = 0;
    std::map<int, game::Vec> predicted;  // command serverTime -> predicted origin after it
    // Statistics for the invariants.
    int predictedJumps = 0, predictedLands = 0;
    int64_t prevJumpMs = -100000;
    int prevLandSeq = 0;
    int lastEventSeq = 0;
    int playedEvents = 0;  // events iw2view would play this frame from the predicted ps
    std::map<int, int> playedEventKinds;
};

std::string menuTop(const NetClient& c) { return c.menuStack.empty() ? std::string() : c.menuStack.back(); }

void respondMenu(NetClient& c, const std::string& response) {
    for (int k = 0; k < 64; ++k) {
        const auto cs = c.gc->configstrings.find(netgame::kMenuConfigstrings + k);
        if (cs != c.gc->configstrings.end() && !c.menuStack.empty() && cs->second == c.menuStack.back()) {
            c.gc->addReliableCommand("mr " + std::to_string(c.gc->serverId) + " " + std::to_string(k) + " " + response);
            c.menuLog += " answer " + response + "@" + std::to_string(int(runSeconds()));
            if (c.menuLog.size() > 240) c.menuLog.erase(0, c.menuLog.size() - 240);
            return;
        }
    }
}

std::string chooseWeapon(NetClient& c, const std::string& menu) {
    static const char* american[] = {"thompson", "m1garand", "m1carbine", "bar", "greasegun", "springfield", "shotgun"};
    static const char* british[] = {"sten", "enfield", "bren", "m1garand", "thompson", "enfieldsniper", "shotgun"};
    static const char* russian[] = {"ppsh", "pps42", "mosin_nagant", "svt40", "mosin_nagant_sniper", "shotgun"};
    static const char* german[] = {"mp40", "mp44", "kar98k", "g43", "kar98k_sniper", "shotgun"};
    std::vector<const char*> side;
    if (menu == "weapon_german") side.assign(std::begin(german), std::end(german));
    else if (menu == "weapon_russian") side.assign(std::begin(russian), std::end(russian));
    else if (menu == "weapon_british") side.assign(std::begin(british), std::end(british));
    else side.assign(std::begin(american), std::end(american));
    auto allowKey = [](std::string w) {
        if (w == "mosin_nagant") return std::string("nagant");
        if (w == "mosin_nagant_sniper") return std::string("nagantsniper");
        if (w == "kar98k_sniper") return std::string("kar98ksniper");
        return w;
    };
    for (const char* w : side)
        if (c.cvars["ui_allow_" + allowKey(w)] == "1") return std::string(w) + "_mp";
    return std::string(side.front()) + "_mp";
}

const assets::XAnim* viewAnim(const fs::Vfs& vfs, const std::string& name) {
    static std::map<std::string, std::optional<assets::XAnim>> cache;
    auto [it, inserted] = cache.try_emplace(lower(name));
    if (inserted) {
        try {
            if (const auto d = vfs.read("xanim/" + lower(name))) it->second = assets::parseXAnim(*d);
        } catch (const std::exception&) {
        }
    }
    return it->second ? &*it->second : nullptr;
}

std::string defField(game::World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(key);
    return it == d->end() ? std::string() : it->second;
}

// The client's own weapon as iw2view keeps it (syncWeapon and the weapon update of its frame).
void wireWeapon(NetClient& c) {
    game::World* w = c.view.get();
    c.pweapon = game::PlayerWeapon{};
    c.pweapon.lookup = [w](const std::string& n) { return w->weapon(n); };
    c.pweapon.mpAnimRates = true;
    const fs::Vfs* vfs = c.vfs;
    c.pweapon.animLength = [vfs](const std::string& n) {
        const assets::XAnim* an = n.empty() ? nullptr : viewAnim(*vfs, n);
        return an && !an->looping ? an->length() : 0.0f;
    };
    c.pweapon.findOffhand = [w](const std::string& cls) { return w->playerOffhandFor(cls); };
    c.pweapon.binocularDef = [w]() { return w->binocularWeapon(); };
    c.pweapon.readAmmo = [w](const std::string& n) -> std::optional<std::pair<int, int>> {
        if (!w->player()) return std::nullopt;
        game::Inventory& inv = w->inventory(*w->player());
        const std::string k = lower(n);
        if (!inv.owned.count(k)) return std::nullopt;
        return std::make_pair(inv.clip[defField(*w, k, "clipname")], inv.ammo[defField(*w, k, "ammoname")]);
    };
    c.pweapon.writeAmmo = [w](const std::string& n, int clip, int stock) {
        if (!w->player()) return;
        game::Inventory& inv = w->inventory(*w->player());
        const std::string k = lower(n);
        if (!inv.owned.count(k)) return;
        inv.clip[defField(*w, k, "clipname")] = clip;
        inv.ammo[defField(*w, k, "ammoname")] = stock;
    };
}

void loadLevel(NetClient& c) {
    c.map = net::infoValueForKey(c.gc->configstrings[0], "mapname");
    c.gametype = net::infoValueForKey(c.gc->configstrings[0], "g_gametype");
    c.view = std::make_unique<game::World>(*c.vfs, c.map, true);
    c.view->vm().setLoopGuardIterations(200000u);
    c.view->cvars["mapname"] = c.map;
    c.view->mpStartRemote();
    std::vector<std::string> menus;
    for (int i = netgame::kMenuConfigstrings; c.gc->configstrings.count(i); ++i) menus.push_back(c.gc->configstrings[i]);
    c.view->mpSetMenuNames(std::move(menus));
    c.rv = netgame::RemoteView{};
    c.ps = game::PlayerState{};
    c.predictedWeapon.clear();
    c.weaponSelect.clear();
    c.weaponWritten.clear();
    c.prevSelect.clear();
    c.menuStack.clear();  // a level load closes the open menus; the new level's scripts open theirs
    c.predicted.clear();
    c.lastEventSeq = 0;
    c.prevLandSeq = 0;
    c.prevJumpMs = -100000;
    wireWeapon(c);
}

// ---------------------------------------------------------------------------------------------------------------
// Per-client observations the invariants use.

struct Observer {
    // server-side truth from the newest snapshot
    int lastSnapTime = 0;
    int lastCommandTime = 0;
    int serverWeapon = -1;
    int serverClip = -1;
    int pmType = -1;
    int health = 0;
    int64_t lastDamageMs = -100000;  // the server's health fell (a hit also pushes the player: not predictable)
    int teleportBit = -1;            // the player state's teleport bit (eFlags 2): it toggles with every spawn
    int restartBit = -1;             // the snapshot flag 4: it flips with every map_restart (an sd round restart places the players again)
    int64_t lastTeleportMs = -100000; // one of them changed: the server placed the player without a command
    bool following = false;
    int64_t lastSelectMs = -100000;  // the scenario changed the weapon select
    int64_t lastReloadMs = -100000;  // the scenario pressed reload (or the magazine ran empty: automatic reload)
    int64_t lastSpawnMs = -100000;
    int64_t lastOutOfAmmoMs = -100000;
    int64_t lastDeathMs = -100000;
    int64_t lastMenuStepMs = -100000;
    std::string answeredMenu;
    int hudClip = -1;
    std::string hudWeapon;
    // snapshot timing
    std::vector<int> snapGaps;
    int64_t lastSnapArrival = 0;
};

struct Life {
    int64_t spawnMs = 0;
    int number = 0;
    // walk
    std::vector<float> walkSpeeds;  // server velocity xy in the walk window
    std::vector<float> walkSpeedsPredicted;
    float walkScale = 1;
    // jumps
    int jumpPresses = 0;
    float jumpBaseZ = 0, jumpApex = -1e9f;
    std::vector<float> apexes;
    int openJump = -1;  // the jump being measured
    int predictedJumpsAtStart = 0, landsAtStart = 0, eventsAtStart = 0, landsAtSpawn = 0;
    std::string pressDetail, grenadeDetail;
    float freeHeight = 44;  // over the head at the first press (a ceiling ends the jump early)
    // fire
    int shotsClient = 0;
    int clipDrops = 0;
    int clipAtFireStart = -1;
    int serverWeaponChanges = 0;
    int hudWeaponChanges = 0;
    bool reloadSeen = false;
    bool grenadeSeen = false;
    int grenadeClipNow = -1;
    std::set<int> grenadeKinds;
    std::vector<float> grenadeSteps;
    // stance
    std::map<int, int> stanceSeen;
    bool scoreboardAnswered = false;
    std::set<int> once;  // one-shot steps done
    bool emptied = false;  // the server's magazine reached 0 in the fire step
};

// The killcam begins two seconds and a frame after the death (the gametype waits 2 s first, tdm.gsc:491-495; the soak's
// notes measure 2093 .. 2139 ms at the client). A level that ends before that shows none.
constexpr int64_t kKillcamStartMs = 2200;
struct DeathWatch {
    bool active = false;
    int64_t atMs = 0;
    int killer = -1;
    bool shotKill = true;  // by a weapon's shot: a grenade or a melee kill has no fire event to replay
    std::string cause;
    bool sawFollow = false, sawKillcamActive = false, sawOwnBody = false, sawKiller = false;
    int kcShots = 0;
    int kcClient = -1;
    std::set<int> kcClients;     // every followed client seen (world slots)
    int kcBadView = 0;           // killcam frames whose recorded view state is out of range
    int kcOffhandFrames = 0, kcAimFrames = 0, kcMovingFrames = 0; // what the recorded view state carried (notes)
    std::string kcBadDetail;
    int gamestate = -1;          // the client's gamestate count at the death
    bool cut = false;            // the level ended during the watch (intermission, a new gamestate, the end of the run)
    int64_t cutMs = -1;          // when it ended, after the death
    int64_t kcStartMs = -1;      // when the killcam began, after the death
    bool kcComplete = false;     // the killcam ran to its end before the level ended
    bool done = false;
};

// ---------------------------------------------------------------------------------------------------------------

struct Soak {
    const fs::Vfs& vfs;
    ServerProcess server;
    std::vector<std::unique_ptr<NetClient>> clients;
    std::vector<Observer> obs;
    Life life;
    DeathWatch death;
    int lives = 0;
    std::map<std::string, std::vector<float>> walkByMap, apexByMap;  // per map/gametype (A)
    std::vector<std::pair<double, long>> rss;
    int mapsPlayed = 0;
    int rejoins = 0;
    std::string currentMapKey;
    int expectedBots = 3;
    int64_t lastStatusMs = 0;
    bool statusPending = false;
    std::vector<std::string> statusLines;
    // smoothness of other players (as drawn) per client 0
    std::map<int, std::vector<std::array<float, 3>>> drawn;
    int stallFrames = 0, movingFrames = 0;
    int scoreRequests = 0, scoreAnswers = 0;
    int predChecks = 0, predBad = 0;
    // The same checks within 500 ms after a hit (the server pushes the player, which the client cannot predict) and
    // the largest difference there and elsewhere (differences of a teleport size left out).
    int predAfterDamage = 0;
    float predMaxAfterDamage = 0, predMaxOther = 0;
    // frame timing
    std::vector<int> frameDurations;

    explicit Soak(const fs::Vfs& v) : vfs(v) {}

    NetClient& addClient(const std::string& name, uint16_t port) {
        auto c = std::make_unique<NetClient>();
        c->name = name;
        c->vfs = &vfs;
        c->gc = std::make_unique<net::GameClient>();
        c->gc->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(port), a, 28960);
        c->gc->connect(a, nowMs() - gStart);
        clients.push_back(std::move(c));
        obs.emplace_back();
        return *clients.back();
    }

    // The client leaves and joins again as a new connection, as iw2view does after a disconnect (a new GameClient and
    // RemoteView): it usually gets its old slot back.
    void rejoin(size_t i, uint16_t port) {
        const std::string name = clients[i]->name;
        // Every connection is told its offhand weapon, also when the slot's last client had the same one.
        NetClient& leaving = *clients[i];
        if (leaving.view && leaving.view->player() && !leaving.view->inventory(*leaving.view->player()).offhand.empty() &&
            leaving.offhandCommands == 0)
            gReport.add("offhand.notannounced", name + " holds an offhand weapon no `C` command named");
        clients[i]->gc->disconnect();
        auto old = std::move(clients[i]);
        clients.erase(clients.begin() + std::ptrdiff_t(i));
        obs.erase(obs.begin() + std::ptrdiff_t(i));
        addClient(name, port);
        std::rotate(clients.begin() + std::ptrdiff_t(i), clients.end() - 1, clients.end());
        std::rotate(obs.begin() + std::ptrdiff_t(i), obs.end() - 1, obs.end());
        ++rejoins;
    }

    // No two live HUD elements of a client are the same element: same place and same text, label or material, values
    // and alpha aside (the copy of an earlier connection has the old score). A shared place alone is legitimate: hq.gsc
    // keeps its respawn timer and its stay-dead text at one spot (hq.gsc:2318, :2355), showing one by alpha.
    void checkHud(NetClient& c) {
        std::set<std::string> seen;
        for (const auto& h : c.view->hudElems()) {
            std::string key = h->get("horzalign").toString() + "/" + h->get("vertalign").toString() + " " + h->get("x").toString() + "," +
                              h->get("y").toString();
            for (const char* field : {"label", "$text", "$shader"})
                if (const script::Value v = h->get(field); v.isDefined()) key += std::string(" ") + field + "=" + v.toString();
            if (!seen.insert(key).second) gReport.add("hud.duplicate", c.name + ": two elements at " + key);
        }
    }

    // The serverinfo / team / weapon menus: exactly one script menu open at each step.
    void menus(size_t i) {
        NetClient& c = *clients[i];
        const std::string top = menuTop(c);
        if (top.empty()) return;
        const int64_t now = nowMs();
        // One answer per opened menu: the commands are reliable, and a second one while a slow server has not answered
        // yet acts on the script's next step twice (the next menu opened twice). The repeat is for a map change only.
        if (top == obs[i].answeredMenu && now - obs[i].lastMenuStepMs < 5000) return;
        obs[i].answeredMenu = top;
        auto stepCheck = [&](const char* step) {
            if (c.menuStack.size() != 1) {
                std::string s;
                for (const auto& m : c.menuStack) s += m + " ";
                gReport.add("menu.stack", c.name + " " + step + ": " + std::to_string(c.menuStack.size()) + " menus open: " + s + "; " + c.menuLog);
            }
        };
        if (top.rfind("serverinfo_", 0) == 0) {
            stepCheck("serverinfo");
            respondMenu(c, "close");
            obs[i].lastMenuStepMs = now;
        } else if (top.rfind("team_", 0) == 0) {
            stepCheck("team");
            respondMenu(c, "autoassign");
            obs[i].lastMenuStepMs = now;
        } else if (top.rfind("weapon_", 0) == 0) {
            stepCheck("weapon");
            respondMenu(c, chooseWeapon(c, top));
            obs[i].lastMenuStepMs = now;
        }
    }

    void serverCommands(size_t i) {
        NetClient& c = *clients[i];
        for (const std::string& sc : c.gc->takeServerCommands()) {
            const auto tok = net::tokenize(sc);
            if (tok.empty() || tok[0].size() != 1) continue;
            const char letter = tok[0][0];
            if (letter == 't' && tok.size() >= 2) {
                const auto cs = c.gc->configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
                const std::string m = cs == c.gc->configstrings.end() ? "?" : cs->second;
                // The same menu opened again on top of itself: one menu on the screen (MenuStack keeps one), reported.
                c.menuLog += " open " + m + "@" + std::to_string(int(runSeconds()));
                if (!c.menuStack.empty() && c.menuStack.back() == m) gReport.add("menu.duplicate", c.name + ": " + m + " opened twice; " + c.menuLog);
                else c.menuStack.push_back(m);
            } else if (letter == 'u') {
                if (!c.menuStack.empty()) c.menuStack.pop_back();
                obs[i].answeredMenu.clear();
            } else if (letter == 'v' && tok.size() >= 3) {
                c.cvars[tok[1]] = tok[2];
                if (c.view) c.view->cvars[tok[1]] = tok[2];
            } else if (letter == 'C') {
                ++c.offhandCommands;
            } else if (letter == 'b') {
                if (i == 0) ++scoreAnswers;
            }
        }
    }

    // One client frame: what iw2view's frame does for a network client, with the scenario's input.
    void clientFrame(size_t i, const Scenario& sc, int64_t now) {
        NetClient& c = *clients[i];
        Observer& o = obs[i];
        // The client's clock starts with the process, as iw2view's SDL ticks (netNowMs) do.
        const int64_t netNow = now - gStart;
        c.gc->poll(netNow, 0);
        if (c.gc->state() == net::ClientState::Disconnected) return;
        // A new gamestate for another server id: the client loads the map it names.
        if (c.gc->state() >= net::ClientState::Primed &&
            (c.gc->gamestates != std::get<0>(c.loadedGamestate) || c.gc->serverId != std::get<1>(c.loadedGamestate))) {
            const bool newMap = c.gc->serverId != std::get<1>(c.loadedGamestate) || !c.view;
            c.loadedGamestate = {c.gc->gamestates, c.gc->serverId};
            if (newMap) {
                loadLevel(c);
                if (i == 0) onNewMap(c);
            }
        }
        serverCommands(i);
        if (!c.view || c.gc->state() < net::ClientState::Primed) return;
        for (const auto& [n, val] : netgame::RemoteView::syncedDvars(c.gc->configstrings)) c.view->cvars[n] = val;
        menus(i);
        const net::ClientSnapshot *a = nullptr, *b = nullptr;
        float frac = 0;
        bool haveInterp = c.gc->interpolation(c.gc->renderTime(netNow), a, b, frac);
        game::World::MpKillcamView kc;
        std::vector<game::World::RemotePlayer> players;
        if (haveInterp) {
            players = netgame::RemoteView::players(*a, *b, frac, c.gc->clientNum, c.gc->configstrings);
            c.view->mpSetRemotePlayers(players);
            kc = c.rv.killcamView(*a, *b, frac, c.gc->configstrings, c.view->timeMs());
        }
        const net::ClientSnapshot* last = c.gc->latest();
        bool newSnap = false;
        if (last) {
            c.rv.apply(*c.view, *last, c.gc->configstrings, c.gc->clientNum, c.view->timeMs(), &c.predictedWeapon.ammo);
            if (a && b) netgame::RemoteView::placeTrajectories(*c.view, *a, *b, c.gc->renderTime(netNow));
            c.rv.playerEvents(*c.view, *last, c.gc->configstrings, c.gc->clientNum);
            c.rv.followEvents(*c.view, *last, c.gc->configstrings, c.view->timeMs());
            newSnap = last->messageNum != c.lastSnapMsg;
            c.lastSnapMsg = last->messageNum;
            if (newSnap) checkHud(c);
        }
        c.view->frame();
        std::vector<game::MpObituary> obits = c.view->takeMpObituaries();
        (void)c.view->takeFxEvents();

        // The client weapon (syncWeapon + the weapon update of iw2view's frame).
        const float dt = c.lastFrameMs ? float(now - c.lastFrameMs) / 1000.0f : 0.016f;
        c.lastFrameMs = now;
        int shots = 0;
        const bool dead = !last || netgame::readPs(last->ps).pmType != 0;
        if (c.view->player()) {
            game::Entity& pl = *c.view->player();
            if (pl.obj->get("$weapontaken").truthy()) {
                pl.obj->set("$weapontaken", script::Value());
                c.pweapon.weaponTaken();
            }
            const script::Value w = pl.obj->get("$weapon");
            const std::string name = w.isString() && !w.asString().empty() && w.asString() != "none" ? w.asString() : std::string();
            const bool netLag = !c.weaponSelect.empty() && name != c.weaponSelect && now < c.netSelectUntilMs;
            if (name == c.weaponSelect) c.netSelectUntilMs = 0;
            if (!netLag && game::takeScriptedWeapon(name, c.weaponWritten)) c.weaponSelect = name;
            if (!c.pweapon.equipped() && !c.pweapon.holstered())
                if (const auto* def = c.view->weapon(c.weaponSelect)) c.pweapon.equip(c.weaponSelect, *def);
            if (!sc.select.empty()) c.weaponSelect = sc.select;
            if (c.weaponSelect != c.prevSelect) {
                if (!c.prevSelect.empty()) c.netSelectUntilMs = now + 2000;
                c.prevSelect = c.weaponSelect;
            }
            game::PlayerWeapon::Input wi;
            // The weapon sees what the command carries: an open menu withholds the buttons from the command (below), so
            // a client weapon that saw them would fire shots the server never gets.
            const uint32_t heldButtons = c.menuStack.empty() ? sc.buttons : 0u;
            wi.fire = (heldButtons & game::Button::Attack) != 0;
            wi.ads = c.ps.ads;
            static std::map<size_t, uint32_t> prevButtons;
            wi.reload = (heldButtons & game::Button::Reload) && !(prevButtons[i] & game::Button::Reload);
            prevButtons[i] = heldButtons;
            wi.melee = (heldButtons & game::Button::Melee) != 0;
            wi.frag = (heldButtons & game::Button::Frag) != 0;
            wi.smoke = (heldButtons & game::Button::Smoke) != 0;
            wi.weapon = c.weaponSelect;
            wi.holster = game::moveLowersWeapon(c.ps, false);
            wi.proneMoving = c.ps.stance == game::Stance::Prone && (sc.forward != 0 || sc.right != 0);
            wi.use = (heldButtons & (game::Button::Activate | game::Button::UseReload)) != 0;
            wi.dead = dead;
            // The weapon against the acknowledged player state, then the weapon step of this frame's command (sent below;
            // its time is the client clock, kept rising by sendCommand), as iw2view (docs/spec/net.md section 10).
            game::Inventory& inv = c.view->inventory(pl);
            const float timerBefore = c.pweapon.timer();
            const int stateBefore = c.pweapon.stateNumber();
            bool rebuilt = false;
            if (newSnap && last && !netgame::RemoteView::following(*last)) {
                const netgame::PsFields sf = netgame::readPs(last->ps);
                rebuilt = c.predictedWeapon.snapshot(sf, netgame::weaponNameFromList(c.gc->configstrings, sf.weapon), c.pweapon, inv.clip, inv.ammo);
                if (i == 0 && rebuilt && std::getenv("IW2_SOAK_SHOTTRACE"))
                    std::printf("CLIT rebuild snapCt=%d snapWt=%d snapState=%d seq=%d state %d -> %d timer %.4f -> %.4f clip=%d\n", sf.commandTime, sf.weaponTime,
                                sf.weaponState, sf.eventSequence, stateBefore, c.pweapon.stateNumber(), timerBefore, c.pweapon.timer(), c.pweapon.clip());
            }
            if (const net::UserCmd* lc = c.gc->lastCommand())
                shots = c.predictedWeapon.command(std::max(c.gc->commandTime(netNow), lc->serverTime), wi, c.pweapon, inv.clip, inv.ammo);
            else
                shots = c.pweapon.update(std::min(dt, 0.1f), wi);  // no command yet: nothing to predict against
            if (i == 0 && std::getenv("IW2_SOAK_SHOTTRACE")) std::printf("CLIT net=%lld ct=%d last=%d dt=%.4f fire=%d shots=%d step=%d state=%d clip=%d menus=%zu dead=%d world=[%s] select=[%s] written=[%s] hand=[%s] hol=%d tm=%.4f\n", (long long)netNow, c.gc->commandTime(netNow), c.gc->lastCommand() ? c.gc->lastCommand()->serverTime : 0, dt, wi.fire?1:0, shots, c.predictedWeapon.lastStepShots(), c.pweapon.stateNumber(), c.pweapon.clip(), c.menuStack.size(), dead?1:0, name.c_str(), c.weaponSelect.c_str(), c.weaponWritten.c_str(), c.pweapon.name().c_str(), c.pweapon.holstered()?1:0, c.pweapon.timer());
            if (game::writesHeldWeapon(c.pweapon, c.weaponSelect)) {
                pl.obj->set("$weapon", script::Value(c.pweapon.name()));
                c.weaponWritten = c.pweapon.name();
            }
            for (const auto& e : c.pweapon.takeEvents()) {
                if (e.kind == game::PlayerWeapon::Event::OutOfAmmo) {
                    const std::string next = c.view->outOfAmmoSwitch(c.view->inventory(pl), e.weapon);
                    if (!next.empty()) c.weaponSelect = next;
                    o.lastOutOfAmmoMs = now;
                }
            }
        }
        // The user command and the prediction.
        if (c.gc->state() >= net::ClientState::Primed) {
            int weaponByte = 0;
            for (int wi = 1; wi <= netgame::kWeaponConfigstringCount && !c.weaponSelect.empty(); ++wi)
                if (netgame::weaponNameFromList(c.gc->configstrings, wi) == c.weaponSelect) {
                    weaponByte = wi;
                    break;
                }
            const uint32_t buttons = c.menuStack.empty() ? sc.buttons : 0u;
            const float fwd = c.menuStack.empty() ? sc.forward : 0.0f, rgt = c.menuStack.empty() ? sc.right : 0.0f;
            const int clientTime = c.gc->commandTime(netNow);
            const net::UserCmd uc = netgame::makeUserCmd(clientTime, buttons, fwd, rgt, c.pitch, c.yaw, weaponByte);
            c.gc->sendCommand(uc, netNow);
            // The command's time is the client clock (sendCommand only keeps it rising): a command time far ahead of
            // the clock makes every command a 1 ms step on the server (slow walk, slow jumps).
            if (const net::UserCmd* lc = c.gc->lastCommand(); lc && c.gc->state() == net::ClientState::Active && lc->serverTime - clientTime > 100)
                gReport.add("net.cmdclock", c.name + ": command time " + std::to_string(lc->serverTime) + " ahead of the client clock " +
                                                std::to_string(clientTime) + " by " + std::to_string(lc->serverTime - clientTime) + " ms");
            if (last) {
                const netgame::PsFields f = netgame::readPs(last->ps);
                const netgame::WeaponMove wm = netgame::weaponMoveOf(c.pweapon);
                // A new gamestate: the prediction and the handled event sequence start afresh (as iw2view).
                if (c.predictorGamestate != c.gc->gamestates) {
                    c.predictorGamestate = c.gc->gamestates;
                    c.predictor.reset();
                    c.predictedWeapon.clear();
                    c.lastEventSeq = 0;
                }
                c.ps = c.predictor.predict(c.view->collision(), f,
                                           netgame::RemoteView::following(*last) ? std::vector<net::UserCmd>{}
                                                                                 : c.gc->commandsAfter(f.commandTime),
                                           &wm, netNow);
                if (netgame::viewInputIgnored(f.pmType)) c.pitch = f.viewangles[0], c.yaw = f.viewangles[1];
                if (const net::UserCmd* lc = c.gc->lastCommand()) c.predicted[lc->serverTime] = c.ps.origin;
                while (c.predicted.size() > 256) c.predicted.erase(c.predicted.begin());
            }
            // The player-state events iw2view plays from the predicted state this frame.
            c.playedEvents = 0;
            if (c.ps.eventSeq - c.lastEventSeq > 4) c.lastEventSeq = c.ps.eventSeq - 4;
            for (; c.lastEventSeq < c.ps.eventSeq; ++c.lastEventSeq) {
                ++c.playedEvents;
                ++c.playedEventKinds[c.ps.events[c.lastEventSeq & 3]];
            }
            // A jump start: the predicted time of the last jump changes (an explosion or a step can also lift the player).
            if (c.ps.lastJumpMs != c.prevJumpMs) ++c.predictedJumps;
            c.prevJumpMs = c.ps.lastJumpMs;
            if (c.ps.landSeq != c.prevLandSeq) ++c.predictedLands;
            c.prevLandSeq = c.ps.landSeq;
        }
        if (i == 0) observeA(c, o, last, newSnap, shots, obits, kc, players, now, dead);
        else observeOther(c, last, newSnap);
    }

    void onNewMap(NetClient& c) {
        if (!currentMapKey.empty()) ++mapsPlayed;
        gReport.map = c.map;
        gReport.gametype = c.gametype;
        currentMapKey = c.map + "/" + c.gametype;
        std::cout << "--- map " << currentMapKey << " at t=" << runSeconds() << "\n";
        life = Life{};
        death = DeathWatch{};
        drawn.clear();
        for (auto& ob : obs) {
            ob.serverWeapon = -1;
            ob.serverClip = -1;
            ob.pmType = -1;
            ob.lastSpawnMs = nowMs();
        }
    }

    // Entities the client shows against the snapshot (map entities with a model).
    void checkEntities(NetClient& c, const net::ClientSnapshot& snap) {
        std::set<int> wires;
        for (const auto& [n, es] : snap.entities) wires.insert(n);
        int stale = 0, invisible = 0;
        std::string staleList, invisibleList;
        for (const auto& e : c.view->entities()) {
            if (e->mapIndex <= 0 || e->isPlayer || e->obj->removed) continue;
            const script::Value md = e->obj->get("model");
            if (!md.isString() || md.asString().empty()) continue;
            const int wire = netgame::kFirstMapEntity + e->mapIndex - 1;
            const bool shown = !e->obj->get("$hidden").truthy();
            const auto it = snap.entities.find(wire);
            if (shown && it == snap.entities.end() && wire < 1022) {
                ++stale;
                if (staleList.size() < 160) staleList += e->classname + ":" + md.asString() + " ";
            }
            if (!shown && it != snap.entities.end()) {
                const int type = getI(it->second, 4) & 0xff;
                const bool hiddenFlag = (getI(it->second, 8) & netgame::kEfHidden) != 0;
                if ((type == netgame::kEtGeneral || type == netgame::kEtTurret) && !hiddenFlag) {
                    ++invisible;
                    if (invisibleList.size() < 160) invisibleList += e->classname + ":" + md.asString() + " ";
                }
            }
        }
        if (stale) gReport.add("entity.stale", std::to_string(stale) + " map entities shown that the snapshot lacks: " + staleList);
        if (invisible) gReport.add("entity.invisible", std::to_string(invisible) + " snapshot entities hidden in the view: " + invisibleList);
    }

    void observeOther(NetClient& c, const net::ClientSnapshot* last, bool newSnap) {
        (void)c;
        (void)last;
        (void)newSnap;
    }

    void observeA(NetClient& c, Observer& o, const net::ClientSnapshot* last, bool newSnap, int shots,
                  const std::vector<game::MpObituary>& obits, const game::World::MpKillcamView& kc,
                  const std::vector<game::World::RemotePlayer>& players, int64_t now, bool dead) {
        if (!last) return;
        const netgame::PsFields f = netgame::readPs(last->ps);
        const int pmFlags = getI(last->ps, 12);
        const bool following = (uint32_t(pmFlags) & kFollowBit) != 0;
        const int health = getI(last->ps, 300);
        if (!netgame::RemoteView::following(*last)) {
            if (o.health > 0 && health < o.health) o.lastDamageMs = now;
            o.health = health;
            const int bit = getI(last->ps, 160) & 2;
            if (o.teleportBit >= 0 && bit != o.teleportBit) o.lastTeleportMs = now;
            o.teleportBit = bit;
            const int restart = last->snapFlags & 4;
            if (o.restartBit >= 0 && restart != o.restartBit) o.lastTeleportMs = now;
            o.restartBit = restart;
        }
        const int wn = getI(last->ps, 212);
        // Clip of the current weapon on the server (its clip slot in the player state, +836) and as the HUD shows it
        // (the view world's inventory after the client weapon's update).
        int clip = -1, hudClip = -1;
        if (wn > 0 && !following) {
            const std::string wname = netgame::weaponNameFromList(c.gc->configstrings, wn);
            const int slot = netgame::weaponAmmoSlot(*c.view, netgame::weaponList(c.gc->configstrings), wname, true);
            if (slot >= 0 && slot < 64) clip = getI(last->ps, size_t(836 + 4 * slot));
            if (c.view->player()) {
                const game::Inventory& inv = c.view->inventory(*c.view->player());
                const auto it = inv.clip.find(defField(*c.view, c.pweapon.name(), "clipname"));
                if (it != inv.clip.end()) hudClip = it->second;
            }
        }
        if (std::getenv("IW2_SOAK_TRACE") && life.spawnMs) {
            const float pv = std::sqrt(c.ps.velocity[0] * c.ps.velocity[0] + c.ps.velocity[1] * c.ps.velocity[1]);
            std::printf("trace t=%.3f life=%d cmdt=%d pm=%d z=%.1f vz=%.1f vxy=%.1f pz=%.1f pvxy=%.1f jumping=%d stance=%d weapon=%d clip=%d hud=%s/%d events=%d ws=%d cs=%d btn=%x off=%s\n",
                        double(now - life.spawnMs) / 1000.0, life.number, f.commandTime, f.pmType, f.origin[2], f.velocity[2],
                        std::sqrt(f.velocity[0] * f.velocity[0] + f.velocity[1] * f.velocity[1]), c.ps.origin[2], pv, c.ps.jumping ? 1 : 0,
                        netgame::RemoteView::stance(*last), wn, clip, c.pweapon.name().c_str(), hudClip, c.playedEvents, getI(last->ps, 216) & 31, c.pweapon.stateNumber(),
                        c.gc->lastCommand() ? unsigned(c.gc->lastCommand()->buttons) : 0u, c.view->player() ? c.view->inventory(*c.view->player()).offhand.c_str() : "-");
        }
        if (newSnap) {
            if (o.lastSnapArrival) o.snapGaps.push_back(int(last->serverTime - o.lastSnapTime));
            o.lastSnapArrival = now;
            // Prediction: the predicted origin after the command the snapshot acknowledges.
            const auto it = c.predicted.find(f.commandTime);
            if (it != c.predicted.end() && f.pmType == 0 && o.pmType == 0 && !following && now - o.lastSpawnMs > 1500 &&
                now - o.lastTeleportMs > 1500) {
                const float dx = it->second[0] - f.origin[0], dy = it->second[1] - f.origin[1], dz = it->second[2] - f.origin[2];
                const float err = std::sqrt(dx * dx + dy * dy + dz * dz);
                ++predChecks;
                if (err < 64.0f) {
                    if (now - o.lastDamageMs < 500) ++predAfterDamage, predMaxAfterDamage = std::max(predMaxAfterDamage, err);
                    else predMaxOther = std::max(predMaxOther, err);
                }
                // Larger: a teleport, not a miss. Within 500 ms after a hit: the server pushed the player (the hit's knockback is
                // added to its velocity before the next command), which no client can predict; the note at the end of the
                // run has the sizes (about 2 units). A difference over 16 units there is still reported.
                if (err > (now - o.lastDamageMs < 500 ? 16.0f : 2.0f) && err < 64.0f) {
                    ++predBad;
                    char b[200];
                    std::snprintf(b, sizeof b, "predicted vs server origin at command %d: %.1f units (%lld ms after the last damage)", f.commandTime, err,
                                  (long long)(now - o.lastDamageMs));
                    gReport.add("predict.error", b);
                }
            }
            o.lastSnapTime = last->serverTime;
            o.lastCommandTime = f.commandTime;
            checkEntities(c, *last);
        }
        // Spawn / death transitions.
        const bool alive = f.pmType == 0 && health > 0 && !following;
        if (o.pmType != 0 && f.pmType == 0 && !following) {
            o.lastSpawnMs = now;
            if (death.active && !death.done) finishDeath();
            life = Life{};
            life.spawnMs = now;
            life.number = ++lives;
            life.landsAtSpawn = c.predictedLands;
            gReport.life = lives;
            o.serverWeapon = -1;
            o.serverClip = -1;
            o.hudClip = -1;
            o.hudWeapon.clear();
            // Walk where the floor is open: the yaw with the longest free player-sized line (up to 400 units).
            float bestYaw = c.yaw, bestFrac = -1;
            for (int k = 0; k < 16; ++k) {
                const float yr = float(k) * float(M_PI) / 8.0f;
                const game::Vec3 from{f.origin[0], f.origin[1], f.origin[2] + 20}, to{from[0] + std::cos(yr) * 400, from[1] + std::sin(yr) * 400, from[2]};
                const auto tr = c.view->collision().trace(from, to, {-15, -15, 0}, {15, 15, 50}, game::Contents::PlayerClipMask, -1);
                const float fr = tr.startSolid ? 0.0f : tr.fraction;
                if (fr > bestFrac) bestFrac = fr, bestYaw = float(k) * 22.5f;
            }
            c.yaw = bestYaw;
            c.pitch = 0;
        }
        if (o.pmType == 0 && f.pmType != 0) o.lastDeathMs = now;
        o.pmType = f.pmType;
        for (const auto& ob : obits)
            if (ob.victim == 0 && !death.active) {
                death = DeathWatch{};
                death.active = true;
                death.atMs = now;
                death.killer = ob.attacker;
                death.shotKill = !ob.modByte && ob.weapon.find("grenade") == std::string::npos;
                death.cause = ob.modByte ? "mod " + std::to_string(ob.mod) : ob.weapon;
                death.gamestate = int(c.gc->gamestates);
            }
        if (death.active && !death.done) {
            if (!death.cut && (f.pmType == 5 || int(c.gc->gamestates) != death.gamestate)) {
                death.cut = true;
                death.cutMs = now - death.atMs;
            }
            if (following) death.sawFollow = true;
            if (death.sawKillcamActive && !kc.active && !death.cut) death.kcComplete = true;
            if (kc.active) {
                if (!death.sawKillcamActive) death.kcStartMs = now - death.atMs;
                death.sawKillcamActive = true;
                death.kcShots = std::max(death.kcShots, kc.fireCount);
                // The followed client: the copied player state's clientNum (docs/spec/mp-hud.md "The recorded view state"),
                // in world slots as the obituary's attacker.
                death.kcClient = netgame::RemoteView::worldSlot(kc.client, c.gc->clientNum);
                death.kcClients.insert(death.kcClient);
                if (death.kcClient == death.killer) death.sawKiller = true;
                for (const auto& p : players)
                    if (p.client == netgame::RemoteView::kOwnBodySlot) death.sawOwnBody = true;
                // The recorded view state the killcam draws from: a weapon in hand, a known state and animation, an aim
                // fraction and a stance height in range.
                const game::MpViewState& vs = kc.view;
                const bool heightOk = vs.viewHeightTarget == 60 || vs.viewHeightTarget == 40 || vs.viewHeightTarget == 11;
                // A followed client that was dead in the shown frame (it respawned shortly before the kill, the replay starts
                // earlier) has nothing in hand, as its player state had then.
                const bool ok = (!kc.weapon.empty() || kc.health <= 0) && !vs.animKey.empty() && vs.weaponState >= 0 && vs.weaponState <= 0x16 &&
                                vs.adsFrac >= 0.0f && vs.adsFrac <= 1.0f && heightOk && vs.animMs >= 0;
                if (!ok && death.kcBadView++ == 0) {
                    std::ostringstream d;
                    d << "weapon '" << kc.weapon << "' state " << vs.weaponState << " anim '" << vs.animKey << "'@" << vs.animMs << " ads " << vs.adsFrac
                      << " height " << vs.viewHeightTarget << " health " << kc.health;
                    death.kcBadDetail = d.str();
                }
                if (vs.weaponState >= 12 && vs.weaponState <= 16) ++death.kcOffhandFrames;
                if (vs.adsFrac > 0) ++death.kcAimFrames;
                if (std::fabs(vs.velocity[0]) + std::fabs(vs.velocity[1]) > 1.0f) ++death.kcMovingFrames;
            }
            if (now - death.atMs > 12000) finishDeath();
        }
        if (!alive) return;

        // Weapon: no change of the server's current weapon the client did not ask for.
        if (wn != o.serverWeapon) {
            if (o.serverWeapon > 0 && wn > 0) {
                ++life.serverWeaponChanges;
                // A round restart spawns the live players again with their loadout (the placement seen in the snapshot flags).
                const bool asked = now - o.lastSelectMs < 3000 || now - o.lastOutOfAmmoMs < 3000 || now - o.lastSpawnMs < 2000 ||
                                   now - o.lastTeleportMs < 2000;
                if (!asked)
                    gReport.add("weapon.selfchange", "server weapon " + netgame::weaponNameFromList(c.gc->configstrings, o.serverWeapon) +
                                                         " -> " + netgame::weaponNameFromList(c.gc->configstrings, wn) + " without a select");
            }
            o.serverWeapon = wn;
            o.serverClip = -1;
        }
        // HUD weapon (what iw2view draws): the client weapon's name.
        const std::string hudW = c.pweapon.name();
        if (hudW != o.hudWeapon) {
            if (!o.hudWeapon.empty() && !hudW.empty()) ++life.hudWeaponChanges;
            o.hudWeapon = hudW;
            o.hudClip = -1;
        }
        // Magazine: the server's rounds fall one per shot and never rise without a reload.
        if (clip >= 0) {
            if (o.serverClip >= 0 && clip > o.serverClip && now - o.lastReloadMs > 5000 && now - o.lastSpawnMs > 2000 &&
                now - o.lastSelectMs > 3000)
                gReport.add("ammo.refill", "clip " + std::to_string(o.serverClip) + " -> " + std::to_string(clip) + " without a reload");
            if (o.serverClip >= 0 && clip < o.serverClip) life.clipDrops += o.serverClip - clip;
            if (clip == 0) {
                o.lastReloadMs = now;  // an empty magazine reloads by itself
                life.emptied = true;
            }
            if (o.serverClip >= 0 && clip > o.serverClip) life.reloadSeen = true;
            o.serverClip = clip;
        }
        // The HUD's magazine: rises only with a reload, a weapon change or a spawn.
        if (hudClip >= 0) {
            if (o.hudClip >= 0 && hudClip > o.hudClip && now - o.lastReloadMs > 5000 && now - o.lastSpawnMs > 2000 &&
                now - o.lastSelectMs > 3000)
                gReport.add("ammo.hudrefill", "HUD clip " + std::to_string(o.hudClip) + " -> " + std::to_string(hudClip) +
                                                  " without a reload (server clip " + std::to_string(clip) + ", commandTime " + std::to_string(f.commandTime) + ", newest command " +
                                                  std::to_string(c.gc->lastCommand() ? c.gc->lastCommand()->serverTime : 0) + ", since select " +
                                                  std::to_string(now - o.lastSelectMs) + ", since reload " + std::to_string(now - o.lastReloadMs) + ")");
            o.hudClip = hudClip;
        }
        life.shotsClient += shots;
        (void)dead;
    }

    void finishDeath() {
        if (!death.active || death.done) return;
        death.done = true;
        std::ostringstream s;
        s << "killer " << death.killer << " follow " << death.sawFollow << " killcam " << death.sawKillcamActive << " shots "
          << death.kcShots << " own body " << death.sawOwnBody << " followed client " << death.kcClient << " killer followed "
          << death.sawKiller << " view frames offhand " << death.kcOffhandFrames << " aiming " << death.kcAimFrames << " moving "
          << death.kcMovingFrames << " killcam after " << death.kcStartMs << " ms";
        if (death.cut) s << " (cut by the level's end after " << death.cutMs << " ms)";
        gReport.note("death " + gReport.map + "/" + gReport.gametype + " life " + std::to_string(gReport.life) + ": " + s.str());
        if (death.killer < 0 || death.killer == 0) return;  // suicide, world or a fall: no killcam
        if (gReport.gametype == "sd") return;            // TODO(spec): sd killcam after the round
        // The killcam begins about two seconds after the death (the notes carry the measured time): a level that ends
        // before that shows none.
        const bool endedFirst = death.cut && death.cutMs < kKillcamStartMs;
        if (!death.sawKillcamActive) {
            if (!endedFirst) gReport.add("killcam.missing", s.str());
        } else {
            // A replay the level's end cut short has not reached the killing shot yet.
            if (death.kcShots <= 0 && death.shotKill && (!death.cut || death.kcComplete)) gReport.add("killcam.noshots", s.str() + " killed by " + death.cause);
            if (!death.sawOwnBody) gReport.add("killcam.novictim", s.str());
            // The killcam follows the killer the obituary named, and only it (the followed player state's clientNum).
            if (!death.sawKiller || death.kcClients.size() != 1) gReport.add("killcam.wrongkiller", s.str());
            if (death.kcBadView > 0) gReport.add("killcam.viewstate", s.str() + " bad frames " + std::to_string(death.kcBadView) + ": " + death.kcBadDetail);
        }
    }

    // The scenario of client A for this frame (time since its spawn).
    Scenario scenarioA(NetClient& c, Observer& o, int64_t now) {
        Scenario sc;
        const net::ClientSnapshot* last = c.gc->latest();
        if (!last || !c.view) return sc;
        const netgame::PsFields f = netgame::readPs(last->ps);
        const bool following = (uint32_t(getI(last->ps, 12)) & kFollowBit) != 0;
        if (f.pmType != 0 || following || life.spawnMs == 0) return sc;
        const double t = double(now - life.spawnMs) / 1000.0;
        auto phase = [&](double a, double b) { return t >= a && t < b; };
        // A step that runs once, in the first frame at or after `at` (and before `at` + 2 s).
        auto once = [&](int id, double at) { return t >= at && t < at + 2.0 && life.once.insert(id).second; };
        const float vx = f.velocity[0], vy = f.velocity[1];
        const float speed = std::sqrt(vx * vx + vy * vy);
        // walk straight (keep the yaw of the spawn; a wall stops the measure)
        if (phase(0.5, 2.6)) sc.forward = 1.0f;
        if (phase(1.0, 2.5) && std::fabs(f.velocity[2]) < 20.0f) {
            life.walkSpeeds.push_back(speed);
            const float pv = std::sqrt(c.ps.velocity[0] * c.ps.velocity[0] + c.ps.velocity[1] * c.ps.velocity[1]);
            life.walkSpeedsPredicted.push_back(pv);
            life.walkScale = c.pweapon.equipped() ? c.pweapon.number("moveSpeedScale") : 1.0f;
        }
        if (once(1, 2.6) && !life.walkSpeeds.empty()) evaluateWalk(c);
        // three jumps on the spot
        for (int k = 0; k < 3; ++k) {
            const double j = 3.0 + 1.2 * k;
            if (phase(j, j + 0.1)) {
                sc.buttons |= game::Button::Jump;
                if (life.openJump != k && !life.once.count(100 + k)) {
                    life.once.insert(100 + k);
                    life.openJump = k;
                    life.jumpBaseZ = f.origin[2];
                    life.jumpApex = f.origin[2];
                    ++life.jumpPresses;
                    const auto up = c.view->collision().trace({f.origin[0], f.origin[1], f.origin[2]}, {f.origin[0], f.origin[1], f.origin[2] + 44},
                                                              {-15, -15, 0}, {15, 15, 70}, game::Contents::PlayerClipMask, -1);
                    char d[200];
                    std::snprintf(d, sizeof d, " press: predicted jump flag %d pm_time %d on ground %d, vz %.1f, landings since spawn %d, free height over the head %.0f",
                                  c.ps.jumping ? 1 : 0, c.ps.pmTimeMs, c.ps.onGround ? 1 : 0, f.velocity[2], c.predictedLands - life.landsAtSpawn, up.fraction * 44);
                    life.pressDetail += d;
                    if (k == 0) life.freeHeight = up.fraction * 44;
                    if (k == 0) {
                        life.predictedJumpsAtStart = c.predictedJumps;
                        life.landsAtStart = c.predictedLands;
                        life.eventsAtStart = c.playedEventKinds[0];
                    }
                }
            }
            if (phase(j + 0.1, j + 1.15)) life.jumpApex = std::max(life.jumpApex, f.origin[2]);
            if (t >= j + 1.15 && life.openJump == k) {
                life.openJump = -1;
                life.apexes.push_back(life.jumpApex - life.jumpBaseZ);
            }
        }
        if (once(2, 6.7)) evaluateJumps(c);
        // crouch, prone, stand
        if (phase(7.0, 8.0)) sc.buttons |= game::Button::Crouch;
        if (phase(8.0, 9.4)) sc.buttons |= game::Button::Prone;
        if (phase(7.7, 8.0)) ++life.stanceSeen[netgame::RemoteView::stance(*last) == 1 ? 1 : 100 + netgame::RemoteView::stance(*last)];
        if (phase(9.1, 9.4)) ++life.stanceSeen[netgame::RemoteView::stance(*last) == 2 ? 2 : 200 + netgame::RemoteView::stance(*last)];
        if (once(3, 10.6)) evaluateStance(*last);
        // bursts to an empty magazine, then a reload
        if (phase(11.0, 19.5) && !life.emptied && o.serverClip != 0) {
            if (life.clipAtFireStart < 0) {
                life.clipAtFireStart = o.serverClip;
                life.shotsClient = 0;
                life.clipDrops = 0;
            }
            if (int((t - 11.0) / 0.1) % 4 != 3) sc.buttons |= game::Button::Attack;  // bursts of 0.3 s
            c.pitch = 10.0f;
        }
        if (once(4, 19.5)) evaluateFire(c);
        if (phase(19.6, 19.8)) {
            sc.buttons |= game::Button::Reload;
            o.lastReloadMs = now;
        }
        // weapon change to the other slot and back
        const bool firstSelect = once(5, 21.0);
        if (firstSelect || once(6, 25.0)) {
            if (c.view->player()) {
                const game::Inventory& inv = c.view->inventory(*c.view->player());
                const int cur = inv.slot[2] == c.weaponSelect ? 2 : 1;
                const std::string& other = inv.slot[cur == 1 ? 2 : 1];
                if (!other.empty()) {
                    sc.select = other;
                    o.lastSelectMs = now;
                    if (firstSelect) {
                        life.serverWeaponChanges = 0;
                        life.hudWeaponChanges = 0;
                    }
                }
            }
        }
        if (once(7, 28.9)) evaluateWeaponChanges();
        // a frag grenade
        if (phase(29.0, 29.4)) {
            sc.buttons |= game::Button::Frag;
            c.pitch = -20.0f;
        }
        if (once(10, 29.0) && c.view->player()) {
            const game::Inventory& inv = c.view->inventory(*c.view->player());
            const std::string name = inv.offhand;
            const int slot = name.empty() ? -1 : netgame::weaponAmmoSlot(*c.view, netgame::weaponList(c.gc->configstrings), name, true);
            life.grenadeDetail = "offhand '" + name + "' server clip " + std::to_string(slot >= 0 && slot < 64 ? getI(last->ps, size_t(836 + 4 * slot)) : -1) +
                                 " weapon state " + std::to_string(getI(last->ps, 216) & 31) + " client state " + std::to_string(c.pweapon.stateNumber()) +
                                 " menus " + std::to_string(c.menuStack.size()) + " stance " + std::to_string(netgame::RemoteView::stance(*last));
        }
        if (phase(29.0, 33.0)) {
            if (c.view->player()) {
                const std::string name = c.view->inventory(*c.view->player()).offhand;
                const int slot = name.empty() ? -1 : netgame::weaponAmmoSlot(*c.view, netgame::weaponList(c.gc->configstrings), name, true);
                life.grenadeClipNow = slot >= 0 && slot < 64 ? getI(last->ps, size_t(836 + 4 * slot)) : -1;
            }
            for (const auto& [n, es] : last->entities)
                if ((getI(es, 4) & 0xff) == netgame::kEtGeneral) life.grenadeKinds.insert(getI(es, 12));
            watchGrenade(c, *last);
        }
        if (once(8, 33.0)) evaluateGrenade(c);
        // use and the scoreboard
        if (phase(33.5, 33.7)) sc.buttons |= game::Button::Activate;
        if (once(9, 34.0)) {
            c.gc->addReliableCommand("score");
            ++scoreRequests;
        }
        if (t > 34.0) c.pitch = 0;
        return sc;
    }

    void evaluateWalk(NetClient& c) {
        // The walk ends where something stops it: the free line at the spawn is checked against the map only, and a
        // player (a bot) or a script brush in the way stops the walk dead (observed: mp_toujane dm, 199.5 to 0 in one
        // snapshot, server and prediction alike, command times going on). Only the samples before such a stop count.
        size_t stop = life.walkSpeeds.size();
        float top = 0;
        for (size_t i = 0; i < life.walkSpeeds.size(); ++i) {
            if (top > 100.0f && life.walkSpeeds[i] < top * 0.5f) {
                stop = i;
                break;
            }
            top = std::max(top, life.walkSpeeds[i]);
        }
        if (stop < 3) {
            gReport.note(currentMapKey + " life " + std::to_string(life.number) + ": walk stopped by an obstacle, not measured");
            return;
        }
        life.walkSpeeds.resize(stop);
        if (life.walkSpeedsPredicted.size() > stop) life.walkSpeedsPredicted.resize(stop);
        std::vector<float> s = life.walkSpeeds;
        std::sort(s.begin(), s.end());
        const float med = s[s.size() / 2];
        std::vector<float> p = life.walkSpeedsPredicted;
        std::sort(p.begin(), p.end());
        const float pmed = p.empty() ? 0 : p[p.size() / 2];
        const float expected = float(game::multiplayerMoveParams().speed) * (life.walkScale > 0 ? life.walkScale : 1.0f);
        char b[200];
        std::snprintf(b, sizeof b, "walk speed server %.1f predicted %.1f expected %.1f (weapon %s scale %.2f)", med, pmed, expected,
                      c.pweapon.name().c_str(), life.walkScale);
        gReport.note(currentMapKey + " life " + std::to_string(life.number) + ": " + b);
        walkByMap[currentMapKey].push_back(med);
        if (med > 20.0f && std::fabs(med - expected) > expected * 0.08f) gReport.add("move.speed", b);
        if (med > 20.0f && std::fabs(med - pmed) > med * 0.05f) gReport.add("move.speedpredict", b);
    }

    void evaluateJumps(NetClient& c) {
        if (life.apexes.empty()) return;
        // The height of a jump: v0 = sqrt(2 g h) with jump_height 39 and g 800 (multiplayerMoveParams), the apex 39.
        const game::MoveParams mp = game::multiplayerMoveParams();
        std::string all;
        float best = 0;
        for (float a : life.apexes) {
            all += std::to_string(int(std::lround(a))) + " ";
            best = std::max(best, a);
        }
        const int jumps = c.predictedJumps - life.predictedJumpsAtStart;
        const int lands = c.predictedLands - life.landsAtStart;
        char b[700];
        std::snprintf(b, sizeof b, "apex heights %s(expected %.0f), predicted jump starts %d, landings %d for %d presses;%s", all.c_str(),
                      mp.jumpHeight, jumps, lands, life.jumpPresses, life.pressDetail.c_str());
        gReport.note(currentMapKey + " life " + std::to_string(life.number) + ": " + b);
        apexByMap[currentMapKey].push_back(best);
        // The first jump of the three is the clean one (later ones may land in the slowdown).
        const float first = life.apexes.front();
        if (first > 2.0f && std::fabs(first - std::min(mp.jumpHeight, life.freeHeight)) > 4.0f) gReport.add("move.jumpheight", b);
        if (jumps != life.jumpPresses) gReport.add("event.jump", b);
        if (lands != life.jumpPresses) gReport.add("event.land", b);
    }

    void evaluateStance(const net::ClientSnapshot&) {
        int crouchOk = life.stanceSeen[1], crouchBad = 0, proneOk = life.stanceSeen[2], proneBad = 0;
        for (const auto& [k, n] : life.stanceSeen) {
            if (k >= 100 && k < 200) crouchBad += n;
            if (k >= 200) proneBad += n;
        }
        if (crouchOk == 0 && crouchBad > 0) gReport.add("move.stance", "crouch not on the server (" + std::to_string(crouchBad) + " frames)");
        if (proneOk == 0 && proneBad > 0) gReport.add("move.stance", "prone not on the server (" + std::to_string(proneBad) + " frames)");
    }

    void evaluateFire(NetClient& c) {
        char b[220];
        std::snprintf(b, sizeof b, "weapon %s: clip at start %d, server clip drops %d, client shots %d, clip now %d",
                      c.pweapon.name().c_str(), life.clipAtFireStart, life.clipDrops, life.shotsClient, obs[0].serverClip);
        gReport.note(currentMapKey + " life " + std::to_string(life.number) + ": " + b);
        if (life.clipAtFireStart <= 0) return;
        if (life.clipDrops != life.shotsClient) gReport.add("event.fire", b);
        if (!life.emptied) gReport.add("ammo.notempty", b);
    }

    void evaluateWeaponChanges() {
        char b[160];
        std::snprintf(b, sizeof b, "two selects: server weapon changes %d, client weapon changes %d", life.serverWeaponChanges,
                      life.hudWeaponChanges);
        gReport.note(currentMapKey + " life " + std::to_string(life.number) + ": " + b);
        if (life.serverWeaponChanges != 2) gReport.add("weapon.changecount", b);
        if (life.hudWeaponChanges != 2) gReport.add("weapon.clientchanges", b);
    }

    void watchGrenade(NetClient& c, const net::ClientSnapshot& snap) {
        // A grenade in flight on the client: a script model moving along its trajectory; its drawn steps per frame.
        for (const auto& [n, es] : snap.entities) {
            const int type = getI(es, 4) & 0xff;
            const int trType = getI(es, 12);
            if (type == netgame::kEtGeneral && trType == 5) life.grenadeSeen = true;
        }
        static std::map<const game::Entity*, std::array<float, 3>> prev;
        for (const auto& e : c.view->entities()) {
            if (e->obj->removed || e->classname != "script_model" || !e->obj->get("$nettraj").truthy()) continue;
            const script::Vec3 o = e->obj->get("origin").asVector();
            const auto it = prev.find(e.get());
            if (it != prev.end()) {
                const float dx = o[0] - it->second[0], dy = o[1] - it->second[1], dz = o[2] - it->second[2];
                life.grenadeSteps.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            prev[e.get()] = {o[0], o[1], o[2]};
        }
    }

    void evaluateGrenade(NetClient& c) {
        int still = 0, moving = 0;
        for (size_t k = 1; k + 1 < life.grenadeSteps.size(); ++k) {
            if (life.grenadeSteps[k - 1] > 2.0f && life.grenadeSteps[k + 1] > 2.0f) {
                ++moving;
                if (life.grenadeSteps[k] < 0.05f) ++still;
            }
        }
        char b[460];
        std::snprintf(b, sizeof b, "grenade seen %d, drawn steps %zu, frames without motion in flight %d of %d; %s, server clip after %d, trajectory kinds %zu, origin %.0f %.0f %.0f yaw %.0f", life.grenadeSeen ? 1 : 0,
                      life.grenadeSteps.size(), still, moving, life.grenadeDetail.c_str(), life.grenadeClipNow, life.grenadeKinds.size(), c.ps.origin[0], c.ps.origin[1], c.ps.origin[2], c.yaw);
        gReport.note(currentMapKey + " life " + std::to_string(life.number) + ": " + b);
        if (!life.grenadeSeen) gReport.add("grenade.missing", b);
        if (moving > 4 && still * 5 > moving) gReport.add("grenade.stutter", b);
    }

    // Smoothness of the other players as drawn by client 0: a frame without motion between two moving frames.
    void watchPlayers(NetClient& c) {
        if (!c.view) return;
        const net::ClientSnapshot *a = nullptr, *b = nullptr;
        float frac = 0;
        if (!c.gc->interpolation(c.gc->renderTime(nowMs() - gStart), a, b, frac)) return;
        for (const auto& p : netgame::RemoteView::players(*a, *b, frac, c.gc->clientNum, c.gc->configstrings)) {
            if (p.dead || p.hidden) continue;
            auto& v = drawn[p.client];
            v.push_back({p.origin[0], p.origin[1], p.origin[2]});
            if (v.size() > 3) v.erase(v.begin());
            if (v.size() == 3) {
                auto d = [](const std::array<float, 3>& x, const std::array<float, 3>& y) {
                    return std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]));
                };
                const float d1 = d(v[0], v[1]), d2 = d(v[1], v[2]);
                if (d1 + d2 > 3.0f && d1 + d2 < 40.0f) {
                    ++movingFrames;
                    if (d2 < 0.01f && d1 > 1.5f) ++stallFrames;
                }
            }
        }
    }

    void requestStatus() {
        statusLines.clear();
        statusPending = true;
        server.command("status");
        lastStatusMs = nowMs();
    }
    void serverOutput() {
        for (const std::string& l : server.take()) {
            if (statusPending) statusLines.push_back(l);
            if (l.find("script error") != std::string::npos || l.find("Script error") != std::string::npos ||
                l.find("ERROR") != std::string::npos)
                gReport.add("server.scripterror", l.substr(0, 200));
        }
        if (statusPending && nowMs() - lastStatusMs > 1500) {
            statusPending = false;
            int players = 0;
            bool inTable = false;
            for (const auto& l : statusLines) {
                if (l.rfind("---", 0) == 0) {
                    inTable = true;
                    continue;
                }
                if (inTable) {
                    if (l.empty() || l.rfind("map:", 0) == 0) {
                        inTable = false;
                        continue;
                    }
                    if (std::isdigit(static_cast<unsigned char>(l.substr(l.find_first_not_of(' ') == std::string::npos ? 0 : l.find_first_not_of(' '))[0])))
                        ++players;
                }
            }
            int connected = 0;
            for (const auto& c : clients) connected += c->gc->state() == net::ClientState::Active;
            if (!statusLines.empty() && connected == int(clients.size()) && players != expectedBots + connected)
                gReport.add("server.clientcount", "status lists " + std::to_string(players) + " clients, expected " +
                                                      std::to_string(expectedBots) + " bots + " + std::to_string(connected) + " connected");
        }
    }
};

std::set<std::string> readKnown(const char* path) {
    std::set<std::string> ids;
    if (!path) return ids;
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l)) {
        const auto h = l.find('#');
        if (h != std::string::npos) l = l.substr(0, h);
        l.erase(0, l.find_first_not_of(" \t"));
        if (const auto e = l.find_last_not_of(" \t\r"); e != std::string::npos) l = l.substr(0, e + 1);
        else l.clear();
        if (!l.empty()) ids.insert(l);
    }
    return ids;
}

int countRotation(const std::string& rot) {
    int n = 0;
    std::istringstream s(rot);
    std::string w;
    while (s >> w) n += w == "map";
    return n;
}

// The map / gametype pairs of a rotation, the start pair first.
std::set<std::pair<std::string, std::string>> rotationPairs(const std::string& rot, const std::string& startMap, const std::string& startGametype) {
    std::set<std::pair<std::string, std::string>> pairs{{startMap, startGametype}};
    std::istringstream s(rot);
    std::string w, gametype = startGametype, value;
    while (s >> w && s >> value) {
        if (w == "gametype") gametype = value;
        else if (w == "map") pairs.insert({value, gametype});
    }
    return pairs;
}

// A map without a cached navigation graph holds a map change up for tens of seconds, longer than a connected client
// waits. Short servers, one per pair and all at once, leave the graphs in the cache; the server writes them while the
// map loads, so with a warm cache this costs one map load. Bounded: a server still running after 10 minutes is killed.
bool warmNavCache(const std::string& exe, const std::string& mainDir, const std::string& rotation, const std::string& logPath) {
    std::vector<std::unique_ptr<ServerProcess>> warm;
    int n = 0;
    for (const auto& [map, gametype] : rotationPairs(rotation, "mp_carentan", "tdm")) {
        auto p = std::make_unique<ServerProcess>();
        if (!p->start(exe, {mainDir, map, gametype, "--port", "0", "--bots", "1", "--seconds", "1"}, logPath + ".warm" + std::to_string(n++))) return false;
        warm.push_back(std::move(p));
    }
    const int64_t deadline = nowMs() + 600000;
    bool ok = true;
    for (auto& p : warm) {
        for (;;) {
            p->take();
            int status = 0;
            if (waitpid(p->pid, &status, WNOHANG) == p->pid) {
                p->pid = -1;
                ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
                break;
            }
            if (nowMs() > deadline) {
                ok = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        p->stop();
    }
    return ok;
}

int run(fs::Vfs& vfs, const std::string& mainDir) {
    const char* serverExe = std::getenv("IW2_SOAK_SERVER");
    if (!serverExe) {
        std::cerr << "IW2_SOAK_SERVER not set (the iw2server binary)\n";
        return 1;
    }
    const std::string rotation = std::getenv("IW2_SOAK_ROTATION") ? std::getenv("IW2_SOAK_ROTATION")
                                                                   : "gametype sd map mp_toujane gametype hq map mp_carentan gametype dm map mp_toujane";
    const std::string minutes = std::getenv("IW2_SOAK_MINUTES") ? std::getenv("IW2_SOAK_MINUTES") : "1.25";
    const int bots = std::getenv("IW2_SOAK_BOTS") ? std::atoi(std::getenv("IW2_SOAK_BOTS")) : 3;
    const int mapsWanted = std::getenv("IW2_SOAK_MAPS") ? std::atoi(std::getenv("IW2_SOAK_MAPS")) : countRotation(rotation) + 1;
    const std::string reportPath = std::getenv("IW2_SOAK_REPORT") ? std::getenv("IW2_SOAK_REPORT") : "mpnetsoak_report.txt";
    const std::set<std::string> known = readKnown(std::getenv("IW2_SOAK_KNOWN"));

    if (!warmNavCache(serverExe, mainDir, rotation, reportPath)) {
        std::cerr << "the navigation graphs of the rotation could not be made\n";
        return 1;
    }
    Soak soak(vfs);
    soak.expectedBots = bots;
    std::vector<std::string> args = {mainDir, "mp_carentan", "tdm", "--port", "0", "--bots", std::to_string(bots), "--bot-skill", "3",
                                     "--maxclients", "12", "+set", "sv_mapRotation", rotation};
    for (const char* gt : {"dm", "tdm", "ctf", "hq"}) {
        args.push_back("+set");
        args.push_back(std::string("scr_") + gt + "_timelimit");
        args.push_back(minutes);
    }
    for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{{"scr_sd_roundlength", minutes},
                                                                              {"scr_sd_graceperiod", "5"},
                                                                              {"scr_sd_roundlimit", "2"},
                                                                              {"scr_sd_timelimit", "0"},
                                                                              {"scr_killcam", "1"}}) {
        args.push_back("+set");
        args.push_back(k);
        args.push_back(v);
    }
    if (!soak.server.start(serverExe, args, reportPath + ".server.log")) {
        std::cerr << "cannot start " << serverExe << "\n";
        return 1;
    }
    gStart = nowMs();
    gReport.map = "mp_carentan";
    gReport.gametype = "tdm";
    // The bound port from the start line.
    for (int i = 0; i < 1200 && !soak.server.port; ++i) {
        for (const auto& l : soak.server.take()) {
            const auto p = l.find(" on UDP ");
            if (l.rfind("iw2server: ", 0) == 0 && p != std::string::npos) soak.server.port = std::atoi(l.c_str() + p + 8);
        }
        if (!soak.server.alive()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!soak.server.port) {
        std::cerr << "server did not start\n";
        soak.server.stop();
        return 1;
    }
    std::cout << "server on UDP " << soak.server.port << ", rotation: " << rotation << ", time limit " << minutes << " min\n";
    soak.addClient("soakA", uint16_t(soak.server.port));
    soak.addClient("soakB", uint16_t(soak.server.port));

    const int64_t deadline = gStart + int64_t(mapsWanted) * int64_t(std::atof(minutes.c_str()) * 60000.0 + 45000.0) + 60000;
    int64_t lastRss = 0, lastFrame = nowMs();
    int64_t lastProgress = 0, lastRejoin = nowMs();
    while (nowMs() < deadline && soak.mapsPlayed < mapsWanted) {
        const int64_t now = nowMs();
        soak.frameDurations.push_back(int(now - lastFrame));
        lastFrame = now;
        Scenario sa = soak.scenarioA(*soak.clients[0], soak.obs[0], now);
        soak.clientFrame(0, sa, now);
        Scenario sb;  // B walks in circles
        sb.forward = 0.6f;
        soak.clients[1]->yaw += 1.0f;
        soak.clientFrame(1, sb, now);
        soak.watchPlayers(*soak.clients[0]);
        soak.serverOutput();
        // Not in sd: a player leaving can end the round, which cuts the first client's scenario short.
        // Nor while the first client measures: the new connection's level load holds this thread for about 300 ms, a
        // frame gap that swallows a 100 ms jump press and shifts a shot (seen as event.jump, event.fire).
        const double lifeTime = soak.life.spawnMs ? double(now - soak.life.spawnMs) / 1000.0 : 1000.0;
        const bool measuring = soak.obs[0].pmType == 0 && lifeTime < 35.0;
        if (now - lastRejoin > 25000 && !measuring && gReport.gametype != "sd" && soak.clients[1]->gc->state() == net::ClientState::Active) {
            lastRejoin = now;
            soak.rejoin(1, uint16_t(soak.server.port));
        }
        if (!soak.server.alive()) {
            gReport.add("server.exit", "the server process ended");
            break;
        }
        for (auto& c : soak.clients)
            if (c->gc->state() == net::ClientState::Disconnected) {
                gReport.add("client.dropped", c->name + ": " + c->gc->error);
                c->gc->state();
            }
        if (now - soak.lastStatusMs > 10000 && !soak.statusPending && soak.clients[0]->gc->state() == net::ClientState::Active &&
            soak.clients[1]->gc->state() == net::ClientState::Active && soak.obs[0].pmType == 0)
            soak.requestStatus();
        if (now - lastRss > 5000) {
            lastRss = now;
            soak.rss.push_back({runSeconds(), soak.server.rssKb()});
        }
        if (now - lastProgress > 15000) {
            lastProgress = now;
            std::cout << "t=" << int(runSeconds()) << " map " << soak.currentMapKey << " life " << soak.lives << " rss "
                      << (soak.rss.empty() ? 0 : soak.rss.back().second) << " kB\n";
        }
        bool dropped = false;
        for (auto& c : soak.clients) dropped |= c->gc->state() == net::ClientState::Disconnected;
        if (dropped) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (soak.death.active && !soak.death.done && !soak.death.cut) {  // the run ends here
        soak.death.cut = true;
        soak.death.cutMs = nowMs() - soak.death.atMs;
    }
    soak.finishDeath();
    if (soak.mapsPlayed < mapsWanted) gReport.add("rotation.stalled", "played " + std::to_string(soak.mapsPlayed) + " of " + std::to_string(mapsWanted) + " map changes");

    // Run-wide invariants.
    // Walk speed and jump height the same on every map (round 2 and later included).
    {
        std::vector<float> walks, apexes;
        for (const auto& [k, v] : soak.walkByMap) walks.insert(walks.end(), v.begin(), v.end());
        for (const auto& [k, v] : soak.apexByMap) apexes.insert(apexes.end(), v.begin(), v.end());
        std::ostringstream s;
        for (const auto& [k, v] : soak.walkByMap) {
            s << k << ":";
            for (float x : v) s << " " << int(std::lround(x));
            s << "; ";
        }
        gReport.note("walk speeds by map: " + s.str());
    }
    if (soak.movingFrames > 0) {
        char b[160];
        std::snprintf(b, sizeof b, "other players drawn: %d moving frames, %d frames without motion between moving ones", soak.movingFrames,
                      soak.stallFrames);
        gReport.note(b);
        if (soak.stallFrames * 10 > soak.movingFrames) gReport.add("motion.stall", b);
    }
    gReport.note("rejoins of the second client: " + std::to_string(soak.rejoins));
    if (soak.rejoins == 0) gReport.add("rejoin.none", "the second client never rejoined");
    {
        char b[200];
        std::snprintf(b, sizeof b, "prediction checks %d, over 2 units %d; within 500 ms after a hit %d checks, largest difference %.2f units, elsewhere %.2f units",
                      soak.predChecks, soak.predBad, soak.predAfterDamage, double(soak.predMaxAfterDamage), double(soak.predMaxOther));
        gReport.note(b);
    }
    if (soak.scoreRequests > 0 && soak.scoreAnswers == 0) gReport.add("scoreboard.noanswer", std::to_string(soak.scoreRequests) + " score requests, no answer");
    // Snapshot cadence (the server's frame time as the client sees it).
    {
        std::vector<int> g = soak.obs[0].snapGaps;
        if (!g.empty()) {
            std::vector<int> s = g;
            std::sort(s.begin(), s.end());
            const int med = s[s.size() / 2], p99 = s[std::min(s.size() - 1, s.size() * 99 / 100)];
            const size_t half = g.size() / 2;
            double m1 = 0, m2 = 0;
            for (size_t k = 0; k < g.size(); ++k) (k < half ? m1 : m2) += g[k] > 0 && g[k] < 1000 ? g[k] : 0;
            char b[200];
            std::snprintf(b, sizeof b, "snapshot server-time gap median %d ms, p99 %d ms, mean first half %.1f, second half %.1f", med, p99,
                          half ? m1 / double(half) : 0.0, g.size() > half ? m2 / double(g.size() - half) : 0.0);
            gReport.note(b);
            if (med > 60) gReport.add("server.frametime", b);
            if (half > 100 && m2 / double(g.size() - half) > 1.5 * (m1 / double(half)) + 5) gReport.add("server.frametimedrift", b);
        }
    }
    // Server memory: no steady growth (the last third against the first third after the first map loaded).
    if (soak.rss.size() >= 6) {
        const size_t n = soak.rss.size();
        long first = 0, lastV = 0;
        for (size_t k = 1; k < 1 + n / 3; ++k) first = std::max(first, soak.rss[k].second);
        for (size_t k = n - n / 3; k < n; ++k) lastV = std::max(lastV, soak.rss[k].second);
        char b[160];
        std::snprintf(b, sizeof b, "server RSS early max %ld kB, late max %ld kB", first, lastV);
        gReport.note(b);
        if (first > 0 && lastV > first * 3 / 2 + 100000) gReport.add("server.rss", b);
    }
    // ESC and the in-game menu are client UI (ui::MenuStack in iw2view); not covered here.
    gReport.note("not covered: ESC / in-game menu (client UI stack), turret and bomb use (use pressed only), hints");

    soak.clients[0]->gc->disconnect();
    soak.clients[1]->gc->disconnect();
    soak.server.stop();
    soak.serverOutput();

    // The report.
    std::map<std::string, int> count;
    std::map<std::string, std::vector<const Violation*>> byId;
    for (const auto& v : gReport.all) byId[v.id].push_back(&v);
    for (const auto& [k, n] : gReport.perId) count[k.substr(0, k.find('@'))] += n;
    std::ostringstream r;
    r << "mpnetsoak report: rotation '" << rotation << "', time limit " << minutes << " min, " << bots << " bots, " << soak.mapsPlayed
      << " map changes, " << soak.lives << " lives, " << int(runSeconds()) << " s\n";
    int unknown = 0;
    for (const auto& [id, n] : count) {
        const bool isKnown = known.count(id) != 0;
        if (!isKnown) ++unknown;
        r << (isKnown ? "KNOWN   " : "NEW     ") << id << " x" << n << "\n";
        for (const Violation* v : byId[id])
            r << "        map=" << v->map << " gt=" << v->gametype << " life=" << v->life << " t=" << int(v->t) << " " << v->detail << "\n";
    }
    for (const auto& id : known)
        if (!count.count(id)) r << "FIXED?  " << id << " (listed as known, not seen in this run: remove it when fixed)\n";
    r << "measurements:\n";
    for (const auto& n : gReport.notes) r << "  " << n << "\n";
    r << (unknown ? "mpnetsoak: FAILED (" + std::to_string(unknown) + " new violation kinds)\n" : "mpnetsoak: ok\n");
    std::cout << "\n" << r.str();
    std::ofstream(reportPath) << r.str();
    std::cout << "report written to " << reportPath << "\n";
    return unknown ? 1 : 0;
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    return run(vfs, main);
}
