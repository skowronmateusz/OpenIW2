// The camera height of a network client around a landing (docs/spec/net.md section 10, "Prediction and interpolation"):
// a client joins the dedicated server on mp_toujane and jumps on the spot at about 120 frames per second. After each
// landing the drawn eye height (predicted origin, correction, view height, bob and landing dip) must change no faster
// than the dip's own slope. The same frames evaluated on the 50 ms server frame time must break that bound, so the test
// shows it can see the steps. Also checked: one landing per jump and no prediction correction.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/collision.h"
#include "game/viewweapon.h"
#include "net/gameclient.h"
#include "net/msg.h"
#include "net/oob.h"
#include "netgame/dedicated.h"
#include "netgame/netmove.h"

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

// One view clock: the dip as iw2view computes it, and the largest camera step per frame relative to the allowed one.
struct View {
    bool network;
    game::ViewImpulses impulses;
    float lastCam = 0;
    float worstRatio = 0, worstStep = 0;
    void land(const game::PlayerState& ps, int64_t t, const game::MoveParams& mp) {
        impulses.land(ps.landHeight, ps.landDamage, t, mp.fallDamageMinHeight, mp.fallDamageMaxHeight);
    }
    void frame(float eye, int64_t t, bool measured, int dtMs) {
        const float cam = eye + impulses.landOffset(t);
        if (measured) {
            const float step = std::fabs(cam - lastCam);
            // A jump's dip is about 8 units in 150 ms; a 1 ms rounding of the drawn time and the bob stay in the margin.
            worstRatio = std::max(worstRatio, step / (0.08f * float(dtMs) + 0.2f));
            worstStep = std::max(worstStep, step);
        }
        lastCam = cam;
    }
};

} // namespace

int main() {
    const char* mainDir = std::getenv("IW2_MAIN");
    if (!mainDir) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    if (!vfs.exists("maps/mp/mp_toujane.d3dbsp")) {
        std::cout << "mp_toujane missing, skipped\n";
        return 0;
    }
    const auto bytes = vfs.read("maps/mp/mp_toujane.d3dbsp");
    const assets::D3dBsp bsp(*bytes);
    const game::CollisionWorld world(bsp);

    Console console;
    std::atomic<int> port{0};
    netgame::DedicatedSettings s;
    s.map = "mp_toujane";
    s.gametype = "dm";
    s.port = 0;
    s.seconds = 120;  // a deadline only: the test quits it
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
    const game::MoveParams mp = game::multiplayerMoveParams();
    View drawn{true, {}}, serverFrames{false, {}};
    int64_t worldTime = 0, last = 0, landedAt = -1;
    double gameClock = 0;
    int lastLand = -1, gamestates = 0, landings = 0, jumps = 0;
    float worstMiss = 0;
    bool wasPressed = false;
    const int64_t start = nowMs();
    while (nowMs() - start < 14000) {
        const int64_t now = nowMs();
        client.poll(now, 1);
        for (const std::string& sc : client.takeServerCommands()) {
            const auto tok = net::tokenize(sc);
            if (tok.size() >= 2 && tok[0] == "t") {
                const auto cs = client.configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
                const std::string name = cs == client.configstrings.end() ? "" : cs->second;
                const std::string resp = name.rfind("team_", 0) == 0 ? "autoassign"
                                         : name == "weapon_german" ? "mp40_mp"
                                         : name.rfind("weapon_", 0) == 0 ? "thompson_mp" : "close";
                client.addReliableCommand("mr " + std::to_string(client.serverId) + " " + tok[1] + " " + resp);
            }
        }
        if (client.gamestates != gamestates) {
            gamestates = client.gamestates;
            predictor.reset();
            lastLand = -1;
        }
        if (client.state() < net::ClientState::Primed || now - last < 8) continue;
        const int dtMs = last ? int(now - last) : 0;
        last = now;
        gameClock += double(dtMs) * 0.001;
        while (gameClock >= double(game::kServerFrameSeconds)) {
            worldTime += game::kServerFrameMs;
            gameClock -= double(game::kServerFrameSeconds);
        }
        const float lerpFrac = game::serverFrameLerp(float(gameClock));
        const int t = int(now - start);
        const bool pressed = t > 4000 && t % 1500 < 100;
        if (pressed && !wasPressed) ++jumps;
        wasPressed = pressed;
        client.sendCommand(netgame::makeUserCmd(client.commandTime(now), pressed ? uint32_t(game::Button::Jump) : 0u, 0, 0, 0, 0, 0), now);
        const net::ClientSnapshot* snap = client.latest();
        if (!snap) continue;
        const netgame::PsFields f = netgame::readPs(snap->ps);
        if (f.pmType != 0) continue;
        const game::PlayerState& ps = predictor.predict(world, f, client.commandsAfter(f.commandTime), nullptr, now);
        worstMiss = std::max(worstMiss, predictor.lastMiss());
        const int64_t drawnTime = game::clientRenderTimeMs(worldTime, lerpFrac);
        const int64_t frameTime = worldTime;
        const bool landedNow = lastLand >= 0 && ps.landSeq != lastLand;
        if (landedNow) {
            ++landings;
            landedAt = now;
            drawn.land(ps, drawnTime, mp);
            serverFrames.land(ps, frameTime, mp);
        }
        lastLand = ps.landSeq;
        const game::Vec o = predictor.drawOrigin(now);
        const float xy = std::hypot(ps.velocity[0], ps.velocity[1]);
        const float eye = o[2] + ps.viewHeight + game::viewBob(ps.bobCycle, xy, ps.viewHeightTarget, 0, 0)[0];
        // From the first frame after the contact (the contact frame still has the fall in it) to the end of the dip.
        const bool measured = landedAt >= 0 && !landedNow && now - landedAt <= 450;
        drawn.frame(eye, drawnTime, measured, dtMs);
        serverFrames.frame(eye, frameTime, measured, dtMs);
    }
    console.push("quit");
    server.join();

    std::cout << "jumps " << jumps << " landings " << landings << " worst miss " << worstMiss << "; worst camera step "
              << drawn.worstStep << " on the drawn time (ratio to the bound " << drawn.worstRatio << "), "
              << serverFrames.worstStep << " on the server frame time (ratio " << serverFrames.worstRatio << ")\n";
    CHECK(landings >= 5);
    CHECK(std::abs(landings - jumps) <= 1);  // the last jump may still be in the air at the end
    CHECK(worstMiss < 0.1f);
    CHECK(drawn.worstRatio <= 1.0f);
    CHECK(serverFrames.worstRatio > 1.0f);
    return failures ? 1 : 0;
}
