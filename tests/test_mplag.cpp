// Movement through an impaired network path (docs/spec/net.md section 10 "Command redundancy", "Client time",
// "Prediction and interpolation"): iw2server runs as a child process, two scripted network clients reach it through a
// UDP relay in this process that delays every datagram (a base latency plus jitter, in order like a real path), loses
// some, reorders some and has stretches that lose everything, from a fixed seed. Client A walks back and forth; client
// B stands and watches A. Measured per profile:
//   - A's prediction against the server: the predicted origin after the command a snapshot acknowledges against the
//     snapshot's origin (a correction of the prediction);
//   - how far A's track falls back from the furthest point of the current leg: on the server (A's snapshots), as A
//     draws himself (the prediction with the error decay) and as B draws him (interpolated between snapshots).
// Default profiles: "wan" (40-45 ms each way, +-10 ms jitter, 1.5 % loss, 2 % reordered) and "wifiburst" (a LAN peer
// on Wi-Fi: 2-25 ms each way, +-15 ms jitter, 1 % loss and stretches of 80-180 ms that lose every datagram). A snap
// above kMaxSnap units fails the test. The server's NETTRACE counters (docs/debug-flags.md) are printed.
//
// Environment: IW2_MAIN (required), IW2_LAG_SERVER (the iw2server binary, set by CMake); docs/debug-flags.md lists the
// IW2_LAG_* options.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "assets/xanim.h"
#include "fs/vfs.h"
#include "game/playerweapon.h"
#include "game/pmove.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/oob.h"
#include "net/udp.h"
#include "netgame/netcombat.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

extern char** environ;

namespace {

using namespace iw2;

constexpr float kMaxSnap = 4.0f;

int64_t gStart = 0;
bool gFireOn = false; // IW2_LAG_BFIRE: B fires once the measurement runs
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// The server process

struct ServerProcess {
    pid_t pid = -1;
    int out = -1, in = -1;
    std::string buffer;
    std::vector<std::string> status;  // lines after a status command
    bool collect = false;
    std::map<std::string, int> trace;
    void command(const std::string& l) {
        const std::string t = l + "\n";
        if (in >= 0) (void)!write(in, t.data(), t.size());
    }
    int port = 0;
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
            if (collect) status.push_back(l);
            if (static FILE* log = std::getenv("IW2_LAG_SERVERLOG") ? std::fopen(std::getenv("IW2_LAG_SERVERLOG"), "w") : nullptr; log) {
                std::fprintf(log, "%s\n", l.c_str());
                std::fflush(log);
            }
            if (l.rfind("NETTRACE ", 0) == 0) {
                const std::string kind = l.substr(9, l.find(' ', 9) - 9);
                ++trace[kind];
                if (trace[kind] <= 5) std::printf("  server: %s\n", l.c_str());
            }
            const auto at = l.find(" on UDP ");
            if (!port && l.rfind("iw2server: ", 0) == 0 && at != std::string::npos) port = std::atoi(l.c_str() + at + 8);
        }
    }
    bool alive() const { return pid > 0 && kill(pid, 0) == 0; }
    void stop() {
        if (pid <= 0) return;
        kill(pid, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid, nullptr, WNOHANG) == pid) {
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
        if (out >= 0) close(out);
        if (in >= 0) close(in);
        out = in = -1;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The impaired path: one relay port per client; each client gets its own upstream socket towards the server.

struct Profile {
    std::string name;
    int baseMinMs, baseMaxMs;  // one-way latency range (a slow drift between the two)
    int jitterMs;              // uniform +- on top
    double loss, reorder;      // probabilities per datagram
    double blackout = 0;       // per datagram: a stretch of 80-180 ms in this direction loses everything
    bool hold = false;         // the stretch holds the datagrams back instead (a radio that stops sending for a while)
};

struct Relay {
    struct Pending {
        int64_t at;
        uint64_t order;
        bool toServer;
        size_t link;
        std::vector<uint8_t> data;
    };
    struct Link {
        net::UdpSocket front;  // the client talks to this one
        net::UdpSocket back;   // towards the server
        net::NetAddress client;
        bool haveClient = false;
    };
    Profile prof;
    std::mt19937 rng;
    net::NetAddress server;
    std::vector<std::unique_ptr<Link>> links;
    std::vector<Pending> queue;
    uint64_t order = 0;
    int64_t lastAt[2][8] = {};
    int64_t darkUntil[2] = {};
    int blackouts = 0;
    int sent = 0, lost = 0, reordered = 0;

    Relay(const Profile& p, uint32_t seed, uint16_t serverPort) : prof(p), rng(seed) {
        server.ip = 0x7f000001;
        server.port = serverPort;
    }
    uint16_t addLink() {
        auto l = std::make_unique<Link>();
        l->front.open(0x7f000001, 0);
        l->back.open(0x7f000001, 0);
        links.push_back(std::move(l));
        return links.back()->front.localPort();
    }
    double uni() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); }
    int delay(int64_t now) {
        // The base drifts slowly between its bounds (a path whose latency changes), the jitter is per datagram.
        const double phase = 0.5 + 0.5 * std::sin(double(now) / 3000.0);
        const double base = prof.baseMinMs + (prof.baseMaxMs - prof.baseMinMs) * phase;
        return std::max(0, int(std::lround(base + (uni() * 2 - 1) * prof.jitterMs)));
    }
    void enqueue(bool toServer, size_t link, const uint8_t* d, int n, int64_t now) {
        ++sent;
        int64_t& dark = darkUntil[toServer ? 1 : 0];
        if (now >= dark && uni() < prof.blackout) {
            dark = now + 80 + int64_t(uni() * 100);
            ++blackouts;
        }
        if ((now < dark && !prof.hold) || uni() < prof.loss) {
            ++lost;
            return;
        }
        // A path is a queue: the jitter moves whole stretches of datagrams, it does not shuffle them; a reordered
        // datagram is held behind the next ones.
        int64_t& fifo = lastAt[toServer ? 1 : 0][link & 7];
        int64_t at = std::max(now + delay(now), fifo);
        if (prof.hold) at = std::max(at, dark);
        if (uni() < prof.reorder) {
            at += 15 + int(uni() * 20);
            ++reordered;
        } else {
            fifo = at;
        }
        queue.push_back({at, order++, toServer, link, std::vector<uint8_t>(d, d + n)});
    }
    void pump(int64_t now) {
        static uint8_t buf[0x20000];
        for (size_t i = 0; i < links.size(); ++i) {
            Link& l = *links[i];
            net::NetAddress from;
            for (int n; (n = l.front.receive(buf, sizeof buf, from, 0)) > 0;) {
                l.client = from;
                l.haveClient = true;
                enqueue(true, i, buf, n, now);
            }
            for (int n; (n = l.back.receive(buf, sizeof buf, from, 0)) > 0;)
                if (from == server) enqueue(false, i, buf, n, now);
        }
        std::sort(queue.begin(), queue.end(), [](const Pending& a, const Pending& b) { return a.at != b.at ? a.at < b.at : a.order < b.order; });
        size_t k = 0;
        for (; k < queue.size() && queue[k].at <= now; ++k) {
            Pending& p = queue[k];
            Link& l = *links[p.link];
            if (p.toServer) l.back.sendTo(server, p.data.data(), int(p.data.size()));
            else if (l.haveClient) l.front.sendTo(l.client, p.data.data(), int(p.data.size()));
        }
        queue.erase(queue.begin(), queue.begin() + std::ptrdiff_t(k));
    }
};

// ---------------------------------------------------------------------------------------------------------------
// A scripted network client: menus answered, the view world for the collision and the weapon, the user command and
// the prediction as iw2view's frame does them.

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

struct NetClient {
    std::string name;
    const fs::Vfs* vfs = nullptr;
    std::unique_ptr<net::GameClient> gc;
    std::unique_ptr<game::World> view;
    netgame::RemoteView rv;
    game::PlayerWeapon pweapon;
    game::PlayerState ps;
    netgame::Predictor predictor;
    int predictorGamestate = -1;
    std::tuple<int, int> loadedGamestate{0, -1};
    std::vector<std::string> menuStack;
    std::string answeredMenu;
    std::map<std::string, std::string> cvars;
    std::map<int, game::Vec> predicted;  // command time -> predicted origin after it
    float yaw = 0;
    int lastSnapMsg = -1;
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
    c.view->vm().setLoopGuardIterations(200000u);
    c.view->cvars["mapname"] = map;
    c.view->mpStartRemote();
    std::vector<std::string> menus;
    for (int i = netgame::kMenuConfigstrings; c.gc->configstrings.count(i); ++i) menus.push_back(c.gc->configstrings[i]);
    c.view->mpSetMenuNames(std::move(menus));
    c.rv = netgame::RemoteView{};
    c.ps = game::PlayerState{};
    c.menuStack.clear();
    c.predicted.clear();
    game::World* w = c.view.get();
    c.pweapon = game::PlayerWeapon{};
    c.pweapon.lookup = [w](const std::string& n) { return w->weapon(n); };
    c.pweapon.mpAnimRates = true;
    const fs::Vfs* vfs = c.vfs;
    c.pweapon.animLength = [vfs](const std::string& n) {
        const assets::XAnim* an = n.empty() ? nullptr : viewAnim(*vfs, n);
        return an && !an->looping ? an->length() : 0.0f;
    };
}

// How far a track falls back from the furthest point it reached along the walk direction in the current leg (a
// rubber band, however many frames it is drawn over).
struct Retreat {
    int64_t leg = -1;
    float best = 0;
    float add(float along, int64_t legId) {
        if (legId != leg) {
            leg = legId;
            best = along;
            return 0;
        }
        best = std::max(best, along);
        return best - along;
    }
};

struct Stats {
    std::vector<float> predErr, selfBack, otherBack;
    int predChecks = 0;
    float walked = 0, lastX = 0;
    bool haveX = false;
    std::vector<float> serverBack;
    float maxOf(const std::vector<float>& v) const { return v.empty() ? 0.0f : *std::max_element(v.begin(), v.end()); }
    int over(const std::vector<float>& v, float lim) const { return int(std::count_if(v.begin(), v.end(), [&](float x) { return x > lim; })); }
};

struct Run {
    const fs::Vfs& vfs;
    std::vector<std::unique_ptr<NetClient>> clients;
    Stats st;
    int64_t walkStartMs = 0, dirChangeMs = 0;
    float walkDir = 1;
    int64_t aSpawnMs = 0;
    int aPm = -1;
    bool haveSelf = false, haveOther = false;
    game::Vec lastSelf{}, lastOther{};
    Retreat rSelf, rOther, rServer;

    explicit Run(const fs::Vfs& v) : vfs(v) {}

    void addClient(const std::string& name, uint16_t port) {
        auto c = std::make_unique<NetClient>();
        c->name = name;
        c->vfs = &vfs;
        c->gc = std::make_unique<net::GameClient>();
        c->gc->name = name;
        c->gc->connectTimeoutMs = 30000;  // the level load of the other client holds this one's frames
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(port), a, 28960);
        c->gc->connect(a, nowMs() - gStart);
        clients.push_back(std::move(c));
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
            } else if (tok[0][0] == 'v' && tok.size() >= 3) {
                c.cvars[tok[1]] = tok[2];
                if (c.view) c.view->cvars[tok[1]] = tok[2];
            }
        }
        if (c.menuStack.empty() || c.menuStack.back() == c.answeredMenu) return;
        const std::string top = c.menuStack.back();
        c.answeredMenu = top;
        if (top.rfind("serverinfo_", 0) == 0) respondMenu(c, "close");
        else if (top.rfind("team_", 0) == 0) respondMenu(c, "autoassign");
        else if (top.rfind("weapon_", 0) == 0) respondMenu(c, top == "weapon_german" ? "kar98k_mp" : top == "weapon_russian" ? "mosin_nagant_mp" : top == "weapon_british" ? "enfield_mp" : "m1garand_mp");
    }

    // One frame of client i; returns the remote player i == 1 draws for client 0 (if any).
    void frame(size_t i, float forward, bool measuring, int64_t now) {
        NetClient& c = *clients[i];
        const int64_t netNow = now - gStart;
        c.gc->poll(netNow, 0);
        if (c.gc->state() == net::ClientState::Disconnected) return;
        if (c.gc->state() >= net::ClientState::Primed &&
            (c.gc->gamestates != std::get<0>(c.loadedGamestate) || c.gc->serverId != std::get<1>(c.loadedGamestate))) {
            const bool newMap = c.gc->serverId != std::get<1>(c.loadedGamestate) || !c.view;
            c.loadedGamestate = {c.gc->gamestates, c.gc->serverId};
            if (newMap) loadLevel(c);
        }
        serverCommands(c);
        if (!c.view || c.gc->state() < net::ClientState::Primed) return;
        for (const auto& [n, val] : netgame::RemoteView::syncedDvars(c.gc->configstrings)) c.view->cvars[n] = val;
        const net::ClientSnapshot* last = c.gc->latest();
        if (last) c.rv.apply(*c.view, *last, c.gc->configstrings, c.gc->clientNum, c.view->timeMs(), nullptr);
        c.view->frame();
        (void)c.view->takeMpObituaries();
        (void)c.view->takeFxEvents();
        // The weapon in hand (only its move parameters matter here).
        if (c.view->player()) {
            const script::Value w = c.view->player()->obj->get("$weapon");
            const std::string name = w.isString() && w.asString() != "none" ? w.asString() : std::string();
            if (!name.empty() && c.pweapon.name() != name)
                if (const auto* def = c.view->weapon(name)) c.pweapon.equip(name, *def);
            game::PlayerWeapon::Input wi;
            wi.weapon = c.pweapon.name();
            c.pweapon.update(0.016f, wi);
        }
        const float fwd = c.menuStack.empty() ? forward : 0.0f;
        const int clientTime = c.gc->commandTime(netNow);
        int weaponByte = 0;
        for (int wi = 1; wi <= netgame::kWeaponConfigstringCount && !c.pweapon.name().empty(); ++wi)
            if (netgame::weaponNameFromList(c.gc->configstrings, wi) == c.pweapon.name()) {
                weaponByte = wi;
                break;
            }
        static const bool bFire = std::getenv("IW2_LAG_BFIRE") != nullptr;
        const uint32_t btn = bFire && gFireOn && i == 1 && (netNow / 150) % 2 ? 1u : 0u;
        c.gc->sendCommand(netgame::makeUserCmd(clientTime, btn, fwd, 0, 0, c.yaw, weaponByte), netNow);
        if (!last) return;
        const netgame::PsFields f = netgame::readPs(last->ps);
        if (c.predictorGamestate != c.gc->gamestates) {
            c.predictorGamestate = c.gc->gamestates;
            c.predictor.reset();
        }
        const netgame::WeaponMove wm = netgame::weaponMoveOf(c.pweapon);
        // As iw2view: the own player entity (and so its body clip) stands at the drawn origin, and the prediction skips it.
        const int self = std::getenv("IW2_LAG_NOPASS") ? -1 : c.view->player() ? c.view->player()->number : -1;
        c.ps = c.predictor.predict(c.view->collision(), f, c.gc->commandsAfter(f.commandTime), &wm, netNow, self);
        {
            const game::Vec d = c.predictor.drawOrigin(netNow);
            c.view->setPlayerOrigin({d[0], d[1], d[2]}, {0.0f, c.yaw, 0.0f});
        }
        if (const net::UserCmd* lc = c.gc->lastCommand()) c.predicted[lc->serverTime] = c.ps.origin;
        while (c.predicted.size() > 512) c.predicted.erase(c.predicted.begin());
        const bool newSnap = last->messageNum != c.lastSnapMsg;
        c.lastSnapMsg = last->messageNum;

        if (i == 0) {
            if (aPm != 0 && f.pmType == 0) aSpawnMs = now;
            aPm = f.pmType;
            if (newSnap && measuring && f.pmType == 0) {
                const auto it = c.predicted.find(f.commandTime);
                if (it != c.predicted.end()) {
                    const float dx = it->second[0] - f.origin[0], dy = it->second[1] - f.origin[1], dz = it->second[2] - f.origin[2];
                    const float e = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (e < 64.0f) st.predErr.push_back(e);
                    if (e > 2.0f && std::getenv("IW2_LAG_TRACE"))
                        std::printf("miss t=%lld cmd=%d along %.1f (predicted - server), vx %.0f\n", (long long)(now - gStart), f.commandTime, double(dx * walkDir), double(f.velocity[0]));
                    st.walked += std::fabs(f.origin[0] - st.lastX) < 64 ? std::fabs(f.origin[0] - st.lastX) : 0.0f;
                    if (now - dirChangeMs > 700) {
                        const float back = rServer.add(f.origin[0] * walkDir, dirChangeMs);
                        if (back > 0.01f) st.serverBack.push_back(back);
                    }
                    st.lastX = f.origin[0];
                    st.haveX = true;
                    ++st.predChecks;
                }
            }
            // The drawn origin frame to frame against the walk direction (yaw 0: +x; the walk sign flips).
            const game::Vec d = c.predictor.drawOrigin(netNow);
            if (haveSelf && measuring && now - dirChangeMs > 600) {
                const float back = rSelf.add(d[0] * walkDir, dirChangeMs);
                if (back > 0.01f) st.selfBack.push_back(back);
            }
            lastSelf = d;
            haveSelf = true;
        } else {
            const net::ClientSnapshot *a = nullptr, *b = nullptr;
            float frac = 0;
            if (!c.gc->interpolation(c.gc->renderTime(netNow), a, b, frac)) return;
            const int aNum = clients[0]->gc->clientNum;
            for (const auto& p : netgame::RemoteView::players(*a, *b, frac, c.gc->clientNum, c.gc->configstrings)) {
                if (p.client != aNum || p.dead || p.hidden) continue;
                const game::Vec o{p.origin[0], p.origin[1], p.origin[2]};
                // B draws A one interpolation delay later: A's direction change shows up ~ a round trip later.
                if (haveOther && measuring && now - dirChangeMs > 900) {
                    const float back = rOther.add(o[0] * walkDir, dirChangeMs);
                    if (back > 0.01f) st.otherBack.push_back(back);
                }
                lastOther = o;
                haveOther = true;
            }
        }
    }
};

bool runProfile(const fs::Vfs& vfs, const std::string& mainDir, const std::string& serverExe, const Profile& prof, double seconds) {
    ServerProcess server;
    setenv("IW2_NET_TRACE", "1", 1);
    if (!server.start(serverExe, {mainDir, std::getenv("IW2_LAG_MAP") ? std::getenv("IW2_LAG_MAP") : "mp_toujane", "dm", "--port", "0", "--bots", std::getenv("IW2_LAG_BOTS") ? std::getenv("IW2_LAG_BOTS") : "0", "--maxclients", "20", "+set", "scr_dm_timelimit", "0",
                                  "+set", "scr_dm_scorelimit", "0"})) {
        std::cerr << "cannot start the server\n";
        return false;
    }
    const int64_t t0 = nowMs();
    while (!server.port && server.alive() && nowMs() - t0 < 120000) {
        server.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!server.port) {
        std::cerr << "the server did not start\n";
        server.stop();
        return false;
    }
    Relay relay(prof, 12345u, uint16_t(server.port));
    Run run(vfs);
    run.addClient("lagA", relay.addLink());
    const uint16_t portB = relay.addLink();
    const int64_t deadline = nowMs() + 90000;
    bool ok = true;
    int64_t measureStart = 0, measureEnd = 0;
    while (nowMs() < deadline) {
        const int64_t now = nowMs();
        server.pump();
        relay.pump(now);
        // B joins once A is in (one level load at a time).
        if (run.clients.size() == 1 && run.clients[0]->view) run.addClient("lagB", portB);
        const bool playing = run.clients.size() == 2 && run.aPm == 0 && run.aSpawnMs && now - run.aSpawnMs > 1500 &&
                             run.clients[1]->gc->state() == net::ClientState::Active;
        if (playing && !measureStart) {
            measureStart = now;
            run.dirChangeMs = now;
            measureEnd = now + int64_t(seconds * 1000);
        }
        // A walks along x, turning back every 2.5 s (the forward move with the yaw flipped by 180).
        if (measureStart && now - run.dirChangeMs > 2500) {
            run.walkDir = -run.walkDir;
            run.clients[0]->yaw = run.walkDir > 0 ? 0.0f : 180.0f;
            run.dirChangeMs = now;
        }
        const bool measuring = measureStart && now < measureEnd;
        gFireOn = measuring;
        if (std::getenv("IW2_LAG_TRACE") && now / 1000 != (now - 8) / 1000)
            std::printf("t=%lld A state %d snaps %d gamestates %d\n", (long long)(now - t0), int(run.clients[0]->gc->state()), run.clients[0]->gc->snapshots, run.clients[0]->gc->gamestates);
        run.frame(0, measureStart ? 1.0f : 0.0f, measuring, now);
        relay.pump(nowMs());
        if (run.clients.size() == 2) {
            static const bool bWalk = std::getenv("IW2_LAG_BWALK") != nullptr;
            if (bWalk) run.clients[1]->yaw = float((now / 1500) % 4) * 90.0f;
            if (std::getenv("IW2_LAG_BAIM") && run.haveOther) { // B faces A (and walks at it with IW2_LAG_BWALK)
                const auto& me = run.clients[1]->ps.origin;
                run.clients[1]->yaw = std::atan2(run.lastOther[1] - me[1], run.lastOther[0] - me[0]) * 180.0f / float(M_PI);
            }
            run.frame(1, bWalk && measureStart ? 1.0f : 0.0f, measuring, now);
        }
        if (measureStart && now >= measureEnd) break;
        // The server's ping of both clients (status) half way through.
        if (measureStart && !server.collect && now > measureStart + int64_t(seconds * 500)) {
            server.collect = true;
            server.command("status");
        }
        if (!server.alive()) break;
        bool dropped = false;
        for (auto& c : run.clients) dropped |= c->gc->state() == net::ClientState::Disconnected;
        if (dropped) {
            std::cerr << "relay " << relay.sent << " queued " << relay.queue.size() << " port " << server.port << "; a client dropped: " << run.clients[0]->gc->error << " / " << run.clients[1]->gc->error << "\n";
            ok = false;
            break;
        }
        // About 125 frames a second, the relay pumped in between.
        // Frame times of a real client: 8 ms, sometimes a slow frame, rarely a hitch (IW2_LAG_HITCH=1).
        int frameMs = 8;
        if (std::getenv("IW2_LAG_HITCH")) {
            const double r = relay.uni();
            frameMs = r < 0.01 ? 150 : r < 0.08 ? 30 : 8 + int(relay.uni() * 10);
        } else if (const char* fm = std::getenv("IW2_LAG_FRAME")) {
            frameMs = std::atoi(fm);
        }
        const int64_t until = now + frameMs;
        while (nowMs() < until) {
            relay.pump(nowMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    server.pump();
    for (const auto& l : server.status) std::printf("  status: %s\n", l.c_str());
    for (auto& c : run.clients) c->gc->disconnect();
    for (int k = 0; k < 20; ++k) relay.pump(nowMs() + 1000);
    server.stop();
    if (!measureStart) {
        std::cerr << prof.name << ": never walked (server " << (server.alive() ? "running" : "gone") << ")\n";
        return false;
    }
    const Stats& s = run.st;
    std::printf("%s: A walked %.0f units on the server (%.0f units/s), server track step back max %.1f; %d blackouts\n", prof.name.c_str(),
                double(s.walked), double(s.walked / seconds), double(s.maxOf(s.serverBack)), relay.blackouts);
    for (const auto& [k, n] : server.trace) std::printf("  server %s: %d\n", k.c_str(), n);
    std::printf("%s: relay %d datagrams, %d lost, %d reordered; prediction checks %d, error max %.1f, over 1 unit %d, over %.0f %d; "
                "own view falls back max %.1f, over %.0f %d; other's view falls back max %.1f, over %.0f %d\n",
                prof.name.c_str(), relay.sent, relay.lost, relay.reordered, s.predChecks, double(s.maxOf(s.predErr)), s.over(s.predErr, 1.0f),
                double(kMaxSnap), s.over(s.predErr, kMaxSnap), double(s.maxOf(s.selfBack)), double(kMaxSnap), s.over(s.selfBack, kMaxSnap),
                double(s.maxOf(s.otherBack)), double(kMaxSnap), s.over(s.otherBack, kMaxSnap));
    if (s.predChecks < 50) {
        std::printf("FAIL %s: too few prediction checks\n", prof.name.c_str());
        ok = false;
    }
    if (s.maxOf(s.predErr) > kMaxSnap || s.maxOf(s.serverBack) > kMaxSnap || s.maxOf(s.selfBack) > kMaxSnap || s.maxOf(s.otherBack) > kMaxSnap) {
        std::printf("FAIL %s: a position snap over %.0f units\n", prof.name.c_str(), double(kMaxSnap));
        ok = false;
    }
    return ok;
}

} // namespace

int main() {
    const char* mainDir = std::getenv("IW2_MAIN");
    const char* serverExe = std::getenv("IW2_LAG_SERVER");
    if (!mainDir || !serverExe) {
        std::cerr << "IW2_MAIN and IW2_LAG_SERVER must be set\n";
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    gStart = nowMs();
    const double seconds = std::getenv("IW2_LAG_SECONDS") ? std::atof(std::getenv("IW2_LAG_SECONDS")) : 10.0;
    const std::vector<Profile> profiles = {{"loopback", 0, 0, 0, 0.0, 0.0}, {"wan", 40, 45, 10, 0.015, 0.02}, {"wifi", 1, 20, 10, 0.005, 0.0}, {"wifiburst", 2, 25, 15, 0.01, 0.0, 0.004}, {"wifistall", 2, 25, 15, 0.0, 0.0, 0.004, true}};
    const char* only = std::getenv("IW2_LAG_PROFILE");
    // IW2_LAG_RELAY=<server port>: only the relay with the profile (default wifiburst), for real clients; prints its port.
    if (const char* rp = std::getenv("IW2_LAG_RELAY")) {
        const std::string want = only ? only : "wifiburst";
        for (const Profile& p : profiles)
            if (p.name == want) {
                Relay relay(p, 12345u, uint16_t(std::atoi(rp)));
                std::printf("relay %s port %u\n", p.name.c_str(), unsigned(relay.addLink()));
                std::fflush(stdout);
                const int64_t end = nowMs() + int64_t(seconds * 1000);
                while (nowMs() < end) {
                    relay.pump(nowMs());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::printf("relay %d datagrams, %d lost, %d blackouts\n", relay.sent, relay.lost, relay.blackouts);
                return 0;
            }
        return 1;
    }
    bool ok = true;
    for (const Profile& p : profiles)
        if (only ? p.name == only : p.name == "wan" || p.name == "wifiburst") ok = runProfile(vfs, mainDir, serverExe, p, seconds) && ok;
    std::printf(ok ? "mplag: OK\n" : "mplag: FAILED\n");
    return ok ? 0 : 1;
}
