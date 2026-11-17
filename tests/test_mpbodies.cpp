// Bodies and dropped weapons of network clients that kill each other (docs/spec/net.md section 4 "Gamestate" and section
// 11, docs/spec/mp-players.md "The body", docs/spec/mp.md "Dropped weapons"): iw2server runs as a child process (dm, no
// bots) and three scripted network clients play the client side of iw2view's network step (RemoteView: players, apply,
// placeTrajectories, playerEvents, followEvents, killcamView; a client World per gamestate). The server puts them next to
// each other (IW2_MP_PLACE) and each one strafes and shoots at the others, so every client dies, watches its killcam
// and respawns over and over. Twice a client leaves and joins again while bodies and weapons lie around, as players do:
// its gamestate is sent while the level runs. Checked on every client, from its own snapshots and its own World:
//   body.gone    a body number of the body queue that was in a live snapshot is missing from a later live one (a body
//                is only ever replaced, never removed, until the level changes);
//   item.gone    a dropped weapon that lay still left the live snapshots while no player stood within 64 units of it
//                and no new item took its slot (g_maxDroppedWeapons);
//   set.differs  two clients got a live snapshot of the same server time with other bodies or items;
//   draw.body / draw.item   a body / item of the newest live snapshot has no drawn entity in the client World (model,
//                place); draw.*.nomodel: nothing to draw it with;
//   killcam.*    the same checks for the first live snapshot after a killcam against the last one before it;
//   client.dropped  a client's connection ended other than by the test's own rejoin (the reason it got; the server's
//                last output lines follow the result of a failed run).
// Environment: IW2_MAIN (required), IW2_BODIES_SERVER (the iw2server binary, set by CMake), IW2_BODIES_SECONDS (default 60),
// IW2_BODIES_LAG=<wan|wifiburst> (client C through an in-process relay with latency, jitter and loss),
// IW2_BODIES_TRACE=1 (a line per change of a client's body / item set), IW2_BODIES_SERVERLOG=<file> (the server's output).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fs/vfs.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/oob.h"
#include "net/udp.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

extern char** environ;

namespace {

using namespace iw2;

int64_t gStart = 0;
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
const bool gTrace = std::getenv("IW2_BODIES_TRACE") != nullptr;

int32_t getI(const std::vector<uint8_t>& b, size_t off) {
    int32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
float getF(const std::vector<uint8_t>& b, size_t off) {
    float v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}

std::map<std::string, int> gViolations;
void violation(const std::string& id, const std::string& detail) {
    ++gViolations[id];
    if (gViolations[id] <= 10) std::printf("VIOLATION %s t=%.1f %s\n", id.c_str(), double(nowMs() - gStart) / 1000.0, detail.c_str());
}

struct ServerProcess {
    pid_t pid = -1;
    int out = -1, in = -1;
    std::string buffer;
    int port = 0;
    FILE* log = nullptr;
    std::vector<std::string> tail;
    bool start(const std::string& exe, const std::vector<std::string>& args) {
        int pout[2], pin[2];
        if (pipe(pout) != 0 || pipe(pin) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
        posix_spawn_file_actions_addclose(&fa, pin[1]);
        posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
        posix_spawn_file_actions_adddup2(&fa, pout[1], 2);
        posix_spawn_file_actions_addclose(&fa, pout[0]);
        std::vector<char*> argv{const_cast<char*>(exe.c_str())};
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(pout[1]);
        close(pin[0]);
        in = pin[1];
        if (rc != 0) return false;
        out = pout[0];
        fcntl(out, F_SETFL, fcntl(out, F_GETFL) | O_NONBLOCK);
        if (const char* l = std::getenv("IW2_BODIES_SERVERLOG")) log = std::fopen(l, "w");
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
            const std::string l = buffer.substr(0, p);
            buffer.erase(0, p + 1);
            if (log) std::fprintf(log, "%s\n", l.c_str());
            tail.push_back(l); // the newest lines, printed when the run fails
            if (tail.size() > 40) tail.erase(tail.begin());
            const auto at = l.find(" on UDP ");
            if (!port && l.rfind("iw2server: ", 0) == 0 && at != std::string::npos) port = std::atoi(l.c_str() + at + 8);
        }
    }
    bool alive() const { return pid > 0 && kill(pid, 0) == 0; }
    void stop() {
        if (pid <= 0) return;
        kill(pid, SIGTERM);
        for (int i = 0; i < 50 && waitpid(pid, nullptr, WNOHANG) != pid; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        pid = -1;
        if (log) std::fclose(log);
    }
};

// A delaying, lossy path for one client (as tests/test_mplag.cpp's relay, simplified: latency, jitter, loss, in order).
struct Relay {
    struct Pending {
        int64_t at;
        bool toServer;
        std::vector<uint8_t> data;
    };
    net::UdpSocket front, back;
    net::NetAddress server, client;
    bool haveClient = false;
    int baseMs = 0, jitterMs = 0;
    double loss = 0;
    std::mt19937 rng{777u};
    std::vector<Pending> queue;
    int64_t fifo[2] = {0, 0};
    uint16_t open(uint16_t serverPort) {
        server.ip = 0x7f000001;
        server.port = serverPort;
        front.open(0x7f000001, 0);
        back.open(0x7f000001, 0);
        return front.localPort();
    }
    double uni() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); }
    void enqueue(bool toServer, const uint8_t* d, int n, int64_t now) {
        if (uni() < loss) return;
        int64_t& f = fifo[toServer ? 1 : 0];
        const int64_t at = std::max(now + baseMs + int64_t((uni() * 2 - 1) * jitterMs), f);
        f = at;
        queue.push_back({at, toServer, std::vector<uint8_t>(d, d + n)});
    }
    void pump(int64_t now) {
        static uint8_t buf[0x20000];
        net::NetAddress from;
        for (int n; (n = front.receive(buf, sizeof buf, from, 0)) > 0;) {
            client = from;
            haveClient = true;
            enqueue(true, buf, n, now);
        }
        for (int n; (n = back.receive(buf, sizeof buf, from, 0)) > 0;)
            if (from == server) enqueue(false, buf, n, now);
        std::stable_sort(queue.begin(), queue.end(), [](const Pending& a, const Pending& b) { return a.at < b.at; });
        size_t k = 0;
        for (; k < queue.size() && queue[k].at <= now; ++k) {
            if (queue[k].toServer) back.sendTo(server, queue[k].data.data(), int(queue[k].data.size()));
            else if (haveClient) front.sendTo(client, queue[k].data.data(), int(queue[k].data.size()));
        }
        queue.erase(queue.begin(), queue.begin() + std::ptrdiff_t(k));
    }
};

// The corpses and items of every live snapshot, by server time and client: two clients that got a snapshot of the same
// server time see the same bodies and items (they are not per-client).
std::map<int, std::map<std::string, std::string>> gSets;

struct Seen {
    int client = -1, flip = 0, type = 0, weapon = 0;
    int still = 0; // live snapshots it has lain at this place without a moving trajectory
    float origin[3] = {0, 0, 0};
};

struct NetClient {
    std::string name, target;
    const fs::Vfs* vfs = nullptr;
    std::unique_ptr<net::GameClient> gc;
    std::unique_ptr<game::World> view;
    netgame::RemoteView rv;
    std::tuple<int, int> loadedGamestate{0, -1};
    std::vector<std::string> menuStack;
    std::string answeredMenu;
    float pitch = 0, yaw = 0;
    int64_t nextShotMs = 0, lastTraceMs = 0;
    bool firing = false;
    int lastMsg = -1;
    // Observations of the live snapshots (not following another client).
    std::set<int> bodiesSeen;           // body numbers seen in a live snapshot of this level
    std::map<int, Seen> lastLive;       // corpses and items of the last live snapshot
    std::map<int, Seen> lastLivePlayers; // player entities (and the own origin as -1) of the last live snapshot
    bool killcamSince = false;          // a following snapshot came after the last live one
    struct Gone {
        int number, weapon, atSnap;
        bool afterKillcam;
        std::string detail;
    };
    std::vector<Gone> goneItems;        // items that left the live snapshots, not yet explained
    int killcams = 0, deaths = 0, kills = 0, evictions = 0, absentBodies = 0, lostInFlight = 0, liveSnaps = 0, bodiesChecked = 0, itemsChecked = 0;
    bool dropReported = false;
    int drawnTime = 0;  // the time the client World's moving models were placed at in the last frame
    // checkDrawn's copy of each body's client and model ("" none), and the eFlags bit 2 of the bodies of the last snapshot.
    std::map<int, std::pair<int, std::string>> bodyModels;
    std::map<int, int> bodiesShown;
};

void respondMenu(NetClient& c, const std::string& response) {
    for (int k = 0; k < 64; ++k) {
        const auto cs = c.gc->configstrings.find(netgame::kMenuConfigstrings + k);
        if (cs != c.gc->configstrings.end() && !c.menuStack.empty() && cs->second == c.menuStack.back()) {
            c.gc->addReliableCommand("mr " + std::to_string(c.gc->serverId) + " " + std::to_string(k) + " " + response);
            return;
        }
    }
}

void loadLevel(NetClient& c) {
    const std::string map = net::infoValueForKey(c.gc->configstrings[0], "mapname");
    c.view = std::make_unique<game::World>(*c.vfs, map, true);
    netgame::setMantleAnims(c.view->mpMantleAnims());
    c.view->vm().setLoopGuardIterations(200000u);
    c.view->cvars["mapname"] = map;
    c.view->mpStartRemote();
    std::vector<std::string> menus;
    for (int i = netgame::kMenuConfigstrings; c.gc->configstrings.count(i); ++i) menus.push_back(c.gc->configstrings[i]);
    c.view->mpSetMenuNames(std::move(menus));
    c.rv = netgame::RemoteView{};
    c.menuStack.clear();
    c.bodiesSeen.clear();
    c.lastLive.clear();
    c.lastLivePlayers.clear();
}

void serverCommands(NetClient& c) {
    for (const std::string& sc : c.gc->takeServerCommands()) {
        const auto tok = net::tokenize(sc);
        if (tok.empty() || tok[0].size() != 1) continue;
        if (tok[0][0] == 't' && tok.size() >= 2) {
            const auto cs = c.gc->configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
            if (cs != c.gc->configstrings.end()) c.menuStack.push_back(cs->second);
        } else if (tok[0][0] == 'u') {
            if (!c.menuStack.empty()) c.menuStack.pop_back();
            c.answeredMenu.clear();
        } else if (tok[0][0] == 'v' && tok.size() >= 3 && c.view) {
            c.view->cvars[tok[1]] = tok[2];
        }
    }
    if (c.menuStack.empty() || c.menuStack.back() == c.answeredMenu) return;
    const std::string top = c.menuStack.back();
    c.answeredMenu = top;
    if (top.rfind("serverinfo_", 0) == 0) respondMenu(c, "close");
    else if (top.rfind("team_", 0) == 0) respondMenu(c, "autoassign");
    else if (top.rfind("weapon_", 0) == 0)
        respondMenu(c, top == "weapon_german" ? "kar98k_mp" : top == "weapon_russian" ? "mosin_nagant_mp" : top == "weapon_british" ? "enfield_mp" : "m1garand_mp");
}

std::string describe(const std::map<int, Seen>& m) {
    std::string s;
    for (const auto& [n, e] : m) s += " " + std::to_string(n) + (e.type == netgame::kEtCorpse ? "b" : "i") + std::to_string(e.type == netgame::kEtCorpse ? e.client : e.weapon) + (gTrace ? "@" + std::to_string(int(e.origin[2])) : std::string());
    return s;
}

// What the client World draws for an entity of the snapshot: a live entity with that model near that place.
bool drawn(game::World& w, const std::string& model, const float* o, float slack) {
    for (const auto& e : w.entities()) {
        if (e->obj->removed || e->isPlayer || e->obj->get("$hidden").truthy()) continue;
        if (e->obj->get("model").toString() != model) continue;
        const script::Value ov = e->obj->get("origin");
        if (!ov.isVector()) continue;
        const auto p = ov.asVector();
        if (std::fabs(p[0] - o[0]) + std::fabs(p[1] - o[1]) + std::fabs(p[2] - o[2]) <= slack) return true;
    }
    return false;
}

// The models drawn within `slack` of `o` (for a violation's message).
std::string modelsNear(game::World& w, const float* o, float slack) {
    std::string out;
    for (const auto& e : w.entities()) {
        if (e->obj->removed || e->isPlayer) continue;
        const script::Value ov = e->obj->get("origin");
        if (!ov.isVector()) continue;
        const auto p = ov.asVector();
        if (std::fabs(p[0] - o[0]) + std::fabs(p[1] - o[1]) + std::fabs(p[2] - o[2]) <= slack)
            out += " " + e->obj->get("model").toString() + (e->obj->get("$hidden").truthy() ? "(hidden)" : "");
    }
    return out.empty() ? std::string(" nothing") : out;
}

void observe(NetClient& c, const net::ClientSnapshot& s) {
    const bool follow = netgame::RemoteView::following(s);
    if (follow) {
        if (!c.killcamSince) ++c.killcams;
        c.killcamSince = true;
        return;
    }
    const netgame::PsFields ps = netgame::readPs(s.ps);
    // Only playing or dead clients see the live set (an intermission or a level change resets it anyway).
    std::map<int, Seen> now, players;
    for (const auto& [n, es] : s.entities) {
        const int type = getI(es, 4) & 0xff;
        Seen e;
        e.type = type;
        for (int k = 0; k < 3; ++k) e.origin[k] = getF(es, 24 + 4 * size_t(k));
        if (type == netgame::kEtPlayer) {
            players[n] = e;
            continue;
        }
        if (type != netgame::kEtCorpse && type != netgame::kEtItem) continue;
        e.client = getI(es, 144) & 63;
        e.flip = getI(es, 8) & 2;
        e.weapon = getI(es, 140);
        if (const auto prev = c.lastLive.find(n); prev != c.lastLive.end() && prev->second.type == type &&
                                                  !netgame::trajectoryMoves(netgame::readTrajectory(es, netgame::kPosTrajectory)) &&
                                                  std::memcmp(prev->second.origin, e.origin, sizeof e.origin) == 0)
            e.still = prev->second.still + 1;
        now[n] = e;
    }
    Seen self;
    for (int k = 0; k < 3; ++k) self.origin[k] = ps.origin[k];
    players[-1] = self;
    const std::string where = c.name + (c.killcamSince ? " (first live snapshot after a killcam)" : "") + " snap " + std::to_string(s.serverTime);
    for (const auto& [n, e] : now)
        if (e.type == netgame::kEtCorpse) c.bodiesSeen.insert(n);
    for (int n : c.bodiesSeen)
        if (!now.count(n))
            violation(c.killcamSince ? "killcam.body.gone" : "body.gone", where + ": body " + std::to_string(n) + " missing; before:" + describe(c.lastLive) + " now:" + describe(now));
    // An item that left the snapshots: a pickup when a player stood within 64 units of it; the slot taken by a new item
    // (g_maxDroppedWeapons: the item farthest from the playing clients is freed for it) when a new item shows up within
    // the next three live snapshots; anything else is a loss.
    bool newItem = false;
    for (const auto& [m, f] : now)
        if (f.type == netgame::kEtItem) {
            const auto before = c.lastLive.find(m);
            if (before == c.lastLive.end() || before->second.type != f.type || before->second.weapon != f.weapon) newItem = true; // a new one, maybe on a reused number
        }
    for (const auto& [n, e] : c.lastLive) {
        if (e.type != netgame::kEtItem) continue;
        const auto it = now.find(n);
        if (it != now.end() && it->second.type == netgame::kEtItem && it->second.weapon == e.weapon) continue;
        float nearest = 1e9f;
        for (const auto* set : {&c.lastLivePlayers, &players})
            for (const auto& [pn, p] : *set) {
                const float dx = p.origin[0] - e.origin[0], dy = p.origin[1] - e.origin[1], dz = p.origin[2] - e.origin[2];
                nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        if (nearest <= 64.0f) continue;
        // One still in flight or not yet at rest may be freed by the item step (a wall hit, a no-drop surface): only an
        // item that lay still for a second counts.
        if (e.still < 60) {
            ++c.lostInFlight;
            continue;
        }
        c.goneItems.push_back({n, e.weapon, c.liveSnaps, c.killcamSince,
                               where + ": item " + std::to_string(n) + " (weapon " + std::to_string(e.weapon) + ") gone, nearest player " + std::to_string(int(nearest)) +
                                   "; before:" + describe(c.lastLive) + " now:" + describe(now)});
    }
    if (newItem) {
        c.evictions += int(c.goneItems.size());
        c.goneItems.clear();
    }
    std::erase_if(c.goneItems, [&](const NetClient::Gone& g) {
        if (c.liveSnaps - g.atSnap < 3) return false;
        violation(g.afterKillcam ? "killcam.item.gone" : "item.gone", g.detail);
        return true;
    });
    {
        const std::string mine = describe(now);
        auto& at = gSets[s.serverTime];
        for (const auto& [other, set] : at)
            if (other != c.name && set != mine)
                violation("set.differs", "snap " + std::to_string(s.serverTime) + ": " + c.name + ":" + mine + " / " + other + ":" + set);
        at[c.name] = mine;
        while (gSets.size() > 400) gSets.erase(gSets.begin());
    }
    if (gTrace && describe(now) != describe(c.lastLive))
        std::printf("trace %s t=%d pm=%d z=%.1f set:%s\n", c.name.c_str(), s.serverTime, ps.pmType, double(ps.origin[2]), describe(now).c_str());
    c.lastLive = std::move(now);
    c.lastLivePlayers = std::move(players);
    c.killcamSince = false;
    ++c.liveSnaps;
}

// After the World's frame: every body and item of the newest live snapshot is drawn.
void checkDrawn(NetClient& c, const net::ClientSnapshot& s) {
    // The body's model (docs/spec/net.md section 11 "Entities", bodies): the client copies its client's body model when the
    // body is reset (new in the snapshot or its eFlags bit 2 changed), always while it has eFlags 0x80000, otherwise only
    // into a copy without a model or of another client; a killcam's snapshots included.
    std::map<int, int> present;
    for (const auto& [n, es] : s.entities) {
        if ((getI(es, 4) & 0xff) != netgame::kEtCorpse) continue;
        const int flags = getI(es, 8), client = getI(es, 144) & 63;
        present[n] = flags & 2;
        if (const auto p = c.bodiesShown.find(n); p != c.bodiesShown.end() && p->second == (flags & 2)) continue;
        auto& rec = c.bodyModels[n];
        if ((flags & netgame::kEfBodyNew) == 0 && !rec.second.empty() && rec.first == client) continue;
        std::string live;
        if (const auto cs = s.clients.find(client); cs != s.clients.end())
            if (const int mi = getI(cs->second, 8) & 0xff; mi > 0)
                if (const auto m = c.gc->configstrings.find(netgame::kModelConfigstrings + mi - 1); m != c.gc->configstrings.end()) live = m->second;
        rec = {client, live};
    }
    c.bodiesShown = std::move(present);
    if (netgame::RemoteView::following(s)) return;
    for (const auto& [n, es] : s.entities) {
        const int type = getI(es, 4) & 0xff;
        if (type != netgame::kEtCorpse && type != netgame::kEtItem) continue;
        std::string model;
        if (type == netgame::kEtCorpse) {
            // The body keeps the model its client had when it appeared (later spawns pick another character, the client may
            // leave); a body first seen when its client had no body model has nothing to draw it with; not counted.
            model = c.bodyModels[n].second;
            if (model.empty()) {
                ++c.absentBodies;
                continue;
            }
            ++c.bodiesChecked;
        } else {
            const std::string wn = netgame::weaponNameFromList(c.gc->configstrings, getI(es, 140));
            if (const auto* def = c.view->weapon(wn))
                if (const auto it = def->find("worldmodel"); it != def->end()) model = it->second;
            ++c.itemsChecked;
        }
        if (model.empty()) {
            violation(type == netgame::kEtCorpse ? "draw.body.nomodel" : "draw.item.nomodel", c.name + ": entity " + std::to_string(n) + " has no model to draw");
            continue;
        }
        // A falling body or item is drawn along its trajectory, a little behind the snapshot's time.
        float o[3] = {getF(es, 24), getF(es, 28), getF(es, 32)};
        if (const netgame::Trajectory tr = netgame::readTrajectory(es, netgame::kPosTrajectory); netgame::trajectoryMoves(tr))
            netgame::evaluateTrajectory(tr, s.serverTime, o);
        if (!drawn(*c.view, model, o, 64.0f))
            violation(type == netgame::kEtCorpse ? "draw.body" : "draw.item", c.name + " snap " + std::to_string(s.serverTime) + ": entity " + std::to_string(n) + " " + model +
                                                                                  " at " + std::to_string(int(o[0])) + "," + std::to_string(int(o[1])) + "," + std::to_string(int(o[2])) + " not drawn (drawn time " +
                                                                                  std::to_string(c.drawnTime) + "); drawn there:" + modelsNear(*c.view, o, 64.0f));
    }
}

void frame(NetClient& c, const std::map<std::string, game::Vec>& others, int64_t now) {
    const int64_t netNow = now - gStart;
    c.gc->poll(netNow, 0);
    if (c.gc->state() == net::ClientState::Disconnected) {
        // Only the test's own rejoin disconnects a client (and replaces it at once): any other end of the connection is a
        // fault, reported with the reason the server or the connection gave.
        if (!c.dropReported)
            violation("client.dropped", c.name + ": " + (c.gc->dropFromServer ? "dropped by the server: " : "connection lost: ") + c.gc->error);
        c.dropReported = true;
        return;
    }
    if (c.gc->state() >= net::ClientState::Primed &&
        (c.gc->gamestates != std::get<0>(c.loadedGamestate) || c.gc->serverId != std::get<1>(c.loadedGamestate))) {
        const bool newMap = c.gc->serverId != std::get<1>(c.loadedGamestate) || !c.view;
        c.loadedGamestate = {c.gc->gamestates, c.gc->serverId};
        if (newMap) loadLevel(c);
    }
    serverCommands(c);
    if (!c.view || c.gc->state() < net::ClientState::Primed) return;
    for (const auto& [n, val] : netgame::RemoteView::syncedDvars(c.gc->configstrings)) c.view->cvars[n] = val;
    // iw2view's order: the drawn players and the killcam view from the interpolated pair, then the newest snapshot.
    const net::ClientSnapshot *a = nullptr, *b = nullptr;
    float frac = 0;
    if (c.gc->interpolation(c.gc->renderTime(netNow), a, b, frac)) {
        c.view->mpSetRemotePlayers(netgame::RemoteView::players(*a, *b, frac, c.gc->clientNum, c.gc->configstrings));
        (void)c.rv.killcamView(*a, *b, frac, c.gc->configstrings, c.view->timeMs());
    }
    const net::ClientSnapshot* last = c.gc->latest();
    if (last) {
        c.rv.apply(*c.view, *last, c.gc->configstrings, c.gc->clientNum, c.view->timeMs(), nullptr);
        if (a && b) {
            const int trajTime = c.gc->renderTime(netNow) - netgame::RemoteView::trajectoryDelayMs(c.gc->snapshotIntervalMs());
            netgame::RemoteView::placeTrajectories(*c.view, c.gc->snapshotsFrom(trajTime), trajTime);
            c.drawnTime = trajTime;
        }
        c.rv.playerEvents(*c.view, *last, c.gc->configstrings, c.gc->clientNum);
        c.rv.followEvents(*c.view, *last, c.gc->configstrings, c.view->timeMs());
    }
    c.view->frame();
    for (const auto& ob : c.view->takeMpObituaries()) {
        if (ob.victimName == c.name) ++c.deaths;
        if (ob.attackerName == c.name && ob.victimName != c.name) ++c.kills;
    }
    (void)c.view->takeFxEvents();
    if (last && last->messageNum != c.lastMsg) {
        c.lastMsg = last->messageNum;
        observe(c, *last);
        checkDrawn(c, *last);
    }
    if (gTrace && last && (now - gStart) / 2000 != (c.lastTraceMs - gStart) / 2000) {
        const netgame::PsFields f = netgame::readPs(last->ps);
        std::printf("place %s t=%d pm=%d at %.1f %.1f %.1f vel z %.1f\n", c.name.c_str(), last->serverTime, f.pmType, double(f.origin[0]), double(f.origin[1]),
                    double(f.origin[2]), double(f.velocity[2]));
        c.lastTraceMs = now;
    }
    // Aim at the target (its body centre) and fire a single shot every 1.2-2 s.
    uint32_t buttons = 0;
    int weapon = 0;
    // They strafe while they fight (left and right every 0.7 s, a jump now and then), so deaths come with a velocity:
    // falling bodies and weapons thrown along.
    const int64_t phase = (now - gStart + int64_t(c.name.back()) * 233) / 700;
    const float right = phase % 2 ? 1.0f : -1.0f;
    if (phase % 5 == 0 && (now - gStart) % 700 < 40) buttons |= 0x400;
    if (last) {
        const netgame::PsFields f = netgame::readPs(last->ps);
        weapon = f.weapon;
        // The target alternates between the two others with each shot.
        const auto t = others.find(c.target);
        if (f.pmType == 0 && !netgame::RemoteView::following(*last) && t != others.end()) {
            const float dx = t->second[0] - f.origin[0], dy = t->second[1] - f.origin[1], dz = t->second[2] + 40.0f - (f.origin[2] + 60.0f);
            c.yaw = std::atan2(dy, dx) * 180.0f / float(M_PI);
            c.pitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0f / float(M_PI);
            if (now >= c.nextShotMs) {
                buttons |= 1;
                c.nextShotMs = now + 1200 + (std::hash<std::string>{}(c.name) + uint64_t(now)) % 800;
                for (const auto& [n, p] : others)
                    if (n != c.name && n != c.target) {
                        c.target = n;
                        break;
                    }
            }
        }
    }
    if (c.gc->state() >= net::ClientState::Primed) {
        if (!c.menuStack.empty()) buttons = 0;
        const net::UserCmd uv = netgame::makeUserCmd(c.gc->commandTime(netNow), buttons, 0.0f, right, c.pitch, c.yaw, weapon);
        c.gc->sendCommand(last ? netgame::viewCommand(uv, last->ps) : uv, netNow);
    }
}

} // namespace

int main() {
    const char* mainDir = std::getenv("IW2_MAIN");
    const char* serverExe = std::getenv("IW2_BODIES_SERVER");
    if (!mainDir || !serverExe) {
        std::cerr << "IW2_MAIN and IW2_BODIES_SERVER must be set\n";
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    gStart = nowMs();
    const double seconds = std::getenv("IW2_BODIES_SECONDS") ? std::atof(std::getenv("IW2_BODIES_SECONDS")) : 60.0;
    // Three clients a few steps apart next to one of mp_toujane's dm spawns (1474 1055 96).
    if (!std::getenv("IW2_MP_PLACE")) setenv("IW2_MP_PLACE", "bA=1474,1055,70;bB=1474,1135,70;bC=1434,1095,70", 1);
    // Every placement must hold a standing player: a client put into a wall cannot move and its dropped weapons start in the
    // solid, so the item step frees them in their first frame (docs/spec/mp.md "Dropped weapons") after they took the slots
    // of other weapons. 1404 1095 (used before) lies in the wall west of the spawn.
    {
        game::World w(vfs, "mp_toujane", true);
        const std::string list = std::getenv("IW2_MP_PLACE");
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            const std::string item = list.substr(at, end - at);
            at = end + 1;
            const size_t eq = item.find('=');
            float x = 0, y = 0, z = 0;
            if (std::sscanf(item.c_str() + (eq == std::string::npos ? 0 : eq + 1), "%f,%f,%f", &x, &y, &z) != 3) continue;
            const auto tr = w.collision().trace({x, y, z}, {x, y, z}, {-15, -15, 0}, {15, 15, 70}, game::Contents::PlayerClipMask, -1);
            if (tr.startSolid) {
                std::printf("placement %s is in solid (contents %x)\nmpbodies: FAILED\n", item.c_str(), tr.contents);
                return 1;
            }
        }
    }
    ServerProcess server;
    if (!server.start(serverExe, {mainDir, "mp_toujane", "dm", "--port", "0", "--bots", "0", "--maxclients", "8", "+set", "scr_dm_timelimit", "0",
                                  "+set", "scr_dm_scorelimit", "0", "+set", "scr_forcerespawn", "1"})) {
        std::cerr << "cannot start the server\n";
        return 1;
    }
    for (const int64_t t0 = nowMs(); !server.port && server.alive() && nowMs() - t0 < 120000;) {
        server.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!server.port) {
        std::cerr << "the server did not start\n";
        server.stop();
        return 1;
    }
    Relay relay;
    const char* lag = std::getenv("IW2_BODIES_LAG");
    if (lag) {
        const std::string p = lag;
        relay.baseMs = p == "wifiburst" ? 12 : 42;
        relay.jitterMs = p == "wifiburst" ? 15 : 10;
        relay.loss = p == "wifiburst" ? 0.01 : 0.015;
    }
    const uint16_t relayPort = lag ? relay.open(uint16_t(server.port)) : 0;
    std::vector<std::unique_ptr<NetClient>> clients;
    const char* names[3] = {"bA", "bB", "bC"};
    auto add = [&](int i) {
        auto c = std::make_unique<NetClient>();
        c->name = names[i];
        c->target = names[(i + 1) % 3];
        c->vfs = &vfs;
        c->gc = std::make_unique<net::GameClient>();
        c->gc->name = c->name;
        c->gc->connectTimeoutMs = 30000;
        net::NetAddress addr;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(i == 2 && lag ? relayPort : server.port), addr, 28960);
        c->gc->connect(addr, nowMs() - gStart);
        clients.push_back(std::move(c));
    };
    add(0);
    const int64_t end = nowMs() + int64_t(seconds * 1000) + 20000;
    int deathsAtRejoin = -1; // the deaths when the first client left and joined again
    while (nowMs() < end && server.alive()) {
        const int64_t now = nowMs();
        server.pump();
        if (lag) relay.pump(now);
        // The next client joins once the previous one has its level (one level load at a time).
        if (clients.size() < 3 && clients.back()->view && clients.back()->gc->state() == net::ClientState::Active) add(int(clients.size()));
        // Where everyone is, by name, from each client's newest snapshot (its own place from its player state).
        std::map<std::string, game::Vec> places;
        for (const auto& c : clients)
            if (const net::ClientSnapshot* s = c->gc->latest()) {
                const netgame::PsFields f = netgame::readPs(s->ps);
                if (f.pmType == 0 && !netgame::RemoteView::following(*s)) places[c->name] = {f.origin[0], f.origin[1], f.origin[2]};
            }
        // Each client reads the time right before its poll, as iw2view does: a poll given a time taken before the frames of
        // the other clients (a level load, a pause of this process) dates the waiting snapshots too early, and the client's
        // clock runs ahead of them for its window (seen 150 to 320 ms ahead, items drawn that far down their fall: draw.item).
        for (const auto& c : clients) frame(*c, places, nowMs());
        // A player who leaves and joins again while bodies and dropped weapons lie around (as players do in a real game):
        // the new connection's gamestate is sent while the level is running.
        static int rejoins = 0;
        int deathsSoFar = 0;
        for (const auto& c : clients) deathsSoFar += c->deaths;
        if (clients.size() == 3 && rejoins < 2 && deathsSoFar >= 4 + 6 * rejoins) {
            ++rejoins;
            if (rejoins == 1) deathsAtRejoin = deathsSoFar;
            const size_t i = size_t(rejoins % 3);
            std::printf("rejoin %s after %d deaths\n", clients[i]->name.c_str(), deathsSoFar);
            const int keepDeaths = clients[i]->deaths, keepKills = clients[i]->kills, keepKc = clients[i]->killcams;
            clients[i]->gc->disconnect();
            auto c = std::make_unique<NetClient>();
            c->name = clients[i]->name;
            c->target = clients[i]->target;
            c->vfs = &vfs;
            c->deaths = keepDeaths, c->kills = keepKills, c->killcams = keepKc;
            c->gc = std::make_unique<net::GameClient>();
            c->gc->name = c->name;
            c->gc->connectTimeoutMs = 30000;
            net::NetAddress addr;
            net::NetAddress::parse("127.0.0.1:" + std::to_string(i == 2 && lag ? relayPort : server.port), addr, 28960);
            c->gc->connect(addr, nowMs() - gStart);
            clients[i] = std::move(c);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    server.stop();
    int deaths = 0, killcams = 0;
    for (const auto& c : clients) {
        std::printf("%s: deaths %d kills %d killcams %d live snapshots %d bodies checked %d items checked %d items replaced %d freed in flight %d bodies of absent clients %d\n",
                    c->name.c_str(), c->deaths, c->kills, c->killcams, c->liveSnaps, c->bodiesChecked, c->itemsChecked, c->evictions, c->lostInFlight,
                    c->absentBodies);
        deaths += c->deaths;
        killcams += c->killcams;
    }
    // The scenario played: deaths with killcams, and deaths after a client joined again while bodies lay around.
    bool ok = clients.size() == 3 && deaths >= 6 && killcams >= 3 && deathsAtRejoin >= 0 && deaths >= deathsAtRejoin + 3;
    if (!ok) std::printf("the scenario did not play: %d deaths, %d killcams, %d deaths at the first rejoin\n", deaths, killcams, deathsAtRejoin);
    for (const auto& [id, n] : gViolations) {
        std::printf("%s: %d\n", id.c_str(), n);
        ok = false;
    }
    if (!ok) {
        std::printf("the server's last lines:\n");
        for (const auto& l : server.tail) std::printf("  %s\n", l.c_str());
    }
    std::printf(ok ? "mpbodies: OK\n" : "mpbodies: FAILED\n");
    return ok ? 0 : 1;
}
