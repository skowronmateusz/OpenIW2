// Network client prediction against the dedicated server (docs/spec/net.md section 10, "Prediction and interpolation"): a client joins
// tools/iw2server's loop (runDedicated) on mp_toujane, runs and jumps with its own clock, and predicts itself with
// netgame::Predictor. Checked: the prediction for every acknowledged command matches the server (no correction while
// running, jumping, landing and jumping again right after landing), each jump's event is handled exactly once, and the
// walk speed and jump height are the same after a map_restart and after a new map (round 2) as in round 1, and in
// every round a weapon change changes the weapon once, a clip fires down by one per shot to 0 and is reloaded, and
// the weapon stays in hand for 5 s after a single shot.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <mutex>
#include <thread>

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/collision.h"
#include "net/gameclient.h"
#include "net/msg.h"
#include "net/oob.h"
#include "netgame/dedicated.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

using namespace iw2;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Console {
    std::mutex m;
    std::deque<std::string> lines;
    void push(const std::string& s) {
        std::lock_guard<std::mutex> l(m);
        lines.push_back(s);
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> l(m);
        std::vector<std::string> out(lines.begin(), lines.end());
        lines.clear();
        return out;
    }
};

struct Round {
    float speed = 0;       // highest horizontal speed of the server's player state while running
    float jumpRise = 0;    // highest rise above the take-off of the jumps
    int jumps = 0;         // jump commands that left the ground (predicted)
    int jumpEvents = 0;    // jump events handled (0x46.. / 0x5b)
    int landings = 0;
    float worstMiss = 0;   // largest prediction correction
    int compared = 0;      // acknowledged commands compared
};

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    if (!vfs.exists("maps/mp/mp_toujane.d3dbsp")) {
        std::cout << "mp_toujane missing, skipped\n";
        return 0;
    }
    // The client's own collision world (iw2view builds the same from the map).
    const auto bytes = vfs.read("maps/mp/mp_toujane.d3dbsp");
    const assets::D3dBsp bsp(*bytes);
    const game::CollisionWorld world(bsp);

    Console console;
    std::atomic<int> port{0};
    netgame::DedicatedSettings s;
    s.map = "mp_toujane";
    s.gametype = "dm";
    s.port = 0;
    s.seconds = 600; // a deadline only: the test quits it
    s.readStdin = false;
    s.frontEnd = [&] { return console.take(); };
    s.onStarted = [&](int p) { port = p; };
    std::thread server([&] { netgame::runDedicated(vfs, s); });
    for (int64_t end = nowMs() + 20000; nowMs() < end && port == 0;) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(port != 0);
    if (port == 0) {
        console.push("quit");
        server.join();
        return 1;
    }

    net::GameClient client;
    client.name = "alpha";
    net::NetAddress addr;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(port.load()), addr, 28960);
    client.connect(addr, nowMs());

    netgame::Predictor predictor;
    std::vector<std::string> menus;
    int lastEventSeq = 0;
    int lastGamestates = 0;

    // The move parameters of the weapon in hand, as the front ends predict with them: the server's move runs at the
    // weapon's moveSpeedScale (the run is 5% faster with this weapon), so a prediction without it misses by the
    // difference over the commands the server has not acknowledged yet, and that depends on how late the snapshots are.
    std::map<int, float> speedScales;
    auto weaponMove = [&](int weapon) {
        netgame::WeaponMove wm;
        auto it = speedScales.find(weapon);
        if (it == speedScales.end()) {
            float scale = 0;
            const std::string name = netgame::weaponNameFromList(client.configstrings, weapon);
            if (const auto d = name.empty() ? std::nullopt : vfs.read("weapons/mp/" + name)) {
                const std::string text(d->begin(), d->end());
                const std::string key = "\\moveSpeedScale\\";
                if (const size_t at = text.find(key); at != std::string::npos) scale = float(std::atof(text.c_str() + at + key.size()));
            }
            if (!name.empty()) it = speedScales.emplace(weapon, scale).first; // the name list may not be complete yet: ask again
            else return wm;
        }
        wm.params.weaponSpeedScale = it->second;
        return wm;
    };

    // Runs the client for ms with the given command pattern; returns once done. pattern(t) -> buttons, forward.
    int cmdWeapon = 0;              // the weapon byte of the commands (the client's weapon select)
    uint32_t extraButtons = 0;      // buttons added to the pattern's
    std::function<void(const std::vector<uint8_t>&)> observe; // sees every new snapshot's player state
    int observedTime = -1;
    int worstAhead = 0; // the most a command time was ahead of the client clock (slow motion on the server)
    auto run = [&](int ms, Round* r, auto pattern) {
        const int64_t start = nowMs();
        int64_t lastSend = 0;
        bool wasOnGround = true;
        float takeoff = 0, top = -1e9f;
        while (nowMs() - start < ms) {
            const int64_t now = nowMs();
            client.poll(now, 1);
            for (const std::string& sc : client.takeServerCommands()) {
                const auto tok = net::tokenize(sc);
                if (tok.size() >= 2 && tok[0] == "t") {
                    const auto cs = client.configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
                    const std::string name = cs == client.configstrings.end() ? "" : cs->second;
                    std::string resp = name.rfind("team_", 0) == 0 ? "autoassign"
                                       : name == "weapon_german" ? "mp40_mp"
                                       : name.rfind("weapon_", 0) == 0 ? "thompson_mp" : "close";
                    client.addReliableCommand("mr " + std::to_string(client.serverId) + " " + tok[1] + " " + resp);
                }
            }
            if (client.gamestates != lastGamestates) {
                lastGamestates = client.gamestates;
                predictor.reset(); // a new level: nothing of the old one carries over
                speedScales.clear();
                lastEventSeq = 0;
            }
            if (client.state() < net::ClientState::Primed || now - lastSend < 16) continue;
            lastSend = now;
            const auto [buttons, forward] = pattern(int(now - start));
            client.sendCommand(netgame::makeUserCmd(client.commandTime(now), buttons | extraButtons, forward, 0, 0, 0, cmdWeapon), now);
            if (const net::UserCmd* lc = client.lastCommand(); lc && client.state() == net::ClientState::Active)
                worstAhead = std::max(worstAhead, lc->serverTime - client.commandTime(now));
            const net::ClientSnapshot* snap = client.latest();
            if (!snap) continue;
            if (observe && snap->serverTime != observedTime) {
                observedTime = snap->serverTime;
                observe(snap->ps);
            }
            const netgame::PsFields f = netgame::readPs(snap->ps);
            if (f.pmType != 0) continue;
            const netgame::WeaponMove wm = weaponMove(f.weapon);
            const game::PlayerState& ps =
                predictor.predict(world, f, client.commandsAfter(f.commandTime), &wm, now);
            if (!r) {
                lastEventSeq = ps.eventSeq;
                continue;
            }
            if (predictor.lastMiss() > 0) {
                ++r->compared;
                r->worstMiss = std::max(r->worstMiss, predictor.lastMiss());
            }
            if (ps.eventSeq - lastEventSeq > 4) lastEventSeq = ps.eventSeq - 4;
            for (; lastEventSeq < ps.eventSeq; ++lastEventSeq) {
                const int ev = ps.events[lastEventSeq & 3];
                // The jump events are 0x46 + the surface of the ground left (0x47..0x5c, 0x5b on a ladder: pmove.cpp); the landings
                // 0x5d..0x73 come right after them, and the sd spawn the round restart picks decides the ground's surface.
                if (ev > 0x46 && ev <= 0x5c) ++r->jumpEvents;
            }
            if (wasOnGround && !ps.onGround && ps.velocity[2] > 100) {
                ++r->jumps;
                takeoff = ps.origin[2];
            }
            if (!wasOnGround && ps.onGround) ++r->landings;
            if (!ps.onGround) top = std::max(top, ps.origin[2]);
            r->jumpRise = std::max(r->jumpRise, top - takeoff);
            wasOnGround = ps.onGround;
            r->speed = std::max(r->speed, std::hypot(f.velocity[0], f.velocity[1])); // the server's run speed
        }
    };
    auto idle = [](int) { return std::pair<uint32_t, float>{0u, 0.0f}; };
    auto runOnly = [](int) { return std::pair<uint32_t, float>{0u, 1.0f}; };
    // Jump every 900 ms (the landing slowdown is still running at the next jump), standing still.
    auto jumps = [](int t) { return std::pair<uint32_t, float>{(t % 900) < 100 ? uint32_t(game::Button::Jump) : 0u, 0.0f}; };

    // Weapons (the same rounds): the current weapon (ps+212), the two weapon slots (ps+1364 bytes 1 and 2) and the
    // clips (ps+836, by clip slot).
    auto psInt = [](const std::vector<uint8_t>& ps, int off) {
        int32_t v = 0;
        if (ps.size() >= size_t(off + 4)) std::memcpy(&v, ps.data() + off, 4);
        return v;
    };
    auto weaponChecks = [&](const char* name) {
        const net::ClientSnapshot* snap = client.latest();
        CHECK(snap != nullptr);
        if (!snap) return;
        const int current = psInt(snap->ps, 212), slots = psInt(snap->ps, 1364);
        const int other = ((slots >> 8) & 0xff) == current ? (slots >> 16) & 0xff : (slots >> 8) & 0xff;
        CHECK(current > 0 && other > 0 && other != current);
        // A weapon change (the command's weapon byte, as the slot key sends it): exactly one change of ps+212.
        cmdWeapon = current;
        run(300, nullptr, idle);
        int changes = 0, last = current;
        observe = [&](const std::vector<uint8_t>& ps) {
            const int w = psInt(ps, 212);
            if (w != last) ++changes;
            last = w;
        };
        cmdWeapon = other;
        run(2500, nullptr, idle);
        std::cout << name << ": weapon " << current << " -> " << last << " in " << changes << " change(s)\n";
        CHECK(changes == 1 && last == other);
        // Fire the clip out: each snapshot's clip of the weapon in hand goes down (by the shots since the last one)
        // to 0, a reload fills it again, and the weapon stays in hand.
        std::vector<int32_t> before(64);
        for (int i = 0; i < 64; ++i) before[size_t(i)] = psInt(client.latest()->ps, 836 + 4 * i);
        int clipSlot = -1, prevClip = -1, minClip = 1 << 30, rises = 0, ups = 0, weaponMoves = 0;
        bool reloaded = false;
        observe = [&](const std::vector<uint8_t>& ps) {
            if (psInt(ps, 212) != other) ++weaponMoves;
            if (clipSlot < 0)
                for (int i = 0; i < 64; ++i)
                    if (psInt(ps, 836 + 4 * i) < before[size_t(i)]) clipSlot = i;
            if (clipSlot < 0) return;
            const int clip = psInt(ps, 836 + 4 * clipSlot);
            if (prevClip >= 0 && clip > prevClip) {
                if (prevClip == 0) reloaded = true;
                else ++ups; // a clip that grows before it is empty: the ammo "comes back"
            }
            if (prevClip >= 0 && clip > prevClip) ++rises;
            minClip = std::min(minClip, clip);
            prevClip = clip;
        };
        // Separate presses (a semi-automatic weapon fires once per press).
        for (int k = 0; k < 60 && minClip != 0; ++k) {
            extraButtons = game::Button::Attack;
            run(150, nullptr, idle);
            extraButtons = 0;
            run(150, nullptr, idle);
        }
        run(200, nullptr, idle);
        if (!reloaded) {
            extraButtons = game::Button::Reload;
            run(100, nullptr, idle);
            extraButtons = 0;
        }
        run(4000, nullptr, idle);
        std::cout << name << ": clip slot " << clipSlot << " lowest " << minClip << " reloaded " << reloaded
                  << " early rises " << ups << " weapon moves " << weaponMoves << "\n";
        CHECK(clipSlot >= 0 && minClip == 0 && reloaded && ups == 0 && weaponMoves == 0);
        // One shot, then 5 s: the weapon stays.
        observe = [&](const std::vector<uint8_t>& ps) {
            if (psInt(ps, 212) != other) ++weaponMoves;
        };
        extraButtons = game::Button::Attack;
        run(60, nullptr, idle);
        extraButtons = 0;
        run(5000, nullptr, idle);
        std::cout << name << ": after one shot, weapon moves " << weaponMoves << "\n";
        CHECK(weaponMoves == 0);
        observe = nullptr;
        (void)rises;
    };
    auto measureRound = [&](const char* name) {
        Round speedR, jumpR;
        run(2500, nullptr, idle);  // join, spawn, settle
        run(1500, &speedR, runOnly);
        run(300, nullptr, idle);
        run(4000, &jumpR, jumps);
        std::cout << name << ": speed " << speedR.speed << " jumps " << jumpR.jumps << " jump events " << jumpR.jumpEvents
                  << " landings " << jumpR.landings << " rise " << jumpR.jumpRise << " worst miss "
                  << std::max(speedR.worstMiss, jumpR.worstMiss) << " (" << speedR.compared + jumpR.compared << " corrections)\n";
        CHECK(jumpR.jumps >= 3);
        weaponChecks(name);
        CHECK(jumpR.jumpEvents == jumpR.jumps);  // the jump event exactly once per jump
        CHECK(std::max(speedR.worstMiss, jumpR.worstMiss) < 0.5f);
        return std::make_pair(speedR, jumpR);
    };

    const auto r1 = measureRound("round 1");
    console.push("map_restart");
    const auto r2 = measureRound("after map_restart");
    console.push("map mp_toujane");
    const auto r3 = measureRound("after map");
    for (const auto* r : {&r2, &r3}) {
        CHECK(r1.first.speed > 100 && std::fabs(r->first.speed - r1.first.speed) < 0.1f * r1.first.speed);
        CHECK(r1.second.jumpRise > 30 && std::fabs(r->second.jumpRise - r1.second.jumpRise) < 3.0f);
    }

    // Search and destroy rounds: a round restart rebuilds the level in place (no new gamestate, the server's time goes
    // on; docs/spec/net.md section 10 "Client clock after a gamestate"). With a 15 s round and no grace period the
    // round of a lone player ends by time and the next one starts in place.
    console.push("set scr_sd_graceperiod 0");
    console.push("set scr_sd_roundlength 0.25");
    console.push("set g_gametype sd");
    console.push("map mp_toujane");
    run(3000, nullptr, idle);
    const int gamestatesSd = client.gamestates;
    // Two restarts: the first round is short; the one after the second restart is long enough to measure (the
    // gametype reads the round length again at each restart).
    bool restarted = false;
    for (int n = 0; n < 2; ++n) {
        const std::string startTime = client.configstrings[13]; // the level's start time, set again by a round restart
        restarted = false;
        for (int k = 0; k < 60 && !restarted; ++k) {
            run(1000, nullptr, idle);
            restarted = client.configstrings[13] != startTime;
        }
        console.push("set scr_sd_roundlength 3");
    }
    std::cout << "sd round restart " << restarted << ", gamestates " << client.gamestates - gamestatesSd << "\n";
    CHECK(restarted && client.gamestates == gamestatesSd);
    worstAhead = 0;
    const auto r4 = measureRound("after sd round restart");
    std::cout << "after sd round restart: command time ahead of the clock by " << worstAhead << " ms at most\n";
    CHECK(worstAhead <= 60);
    CHECK(r4.first.speed > 100); // the sd spawns differ (a wall can end the run early); the clock check above is the slow-motion one
    CHECK(std::fabs(r4.second.jumpRise - r1.second.jumpRise) < 3.0f);

    console.push("quit");
    server.join();
    return failures ? 1 : 0;
}
