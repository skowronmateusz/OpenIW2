// End to end over UDP (docs/spec/turrets.md "Multiplayer", docs/spec/net.md): a network client gets on the mp_carentan
// 30cal with the use button, its front end (RemoteView into a client World, the commands built by netgame::makeUserCmd
// as iw2view builds them) learns it is on the turret from the player state (eFlags 0x300 bits, viewLockMode,
// viewLockEntity), its prediction holds it behind the gun, the turret aim arrives in the turret's angles2, attack fires
// the turret and kills a second client in front of it with the turret weapon in the obituary, and a second use press
// gets it off. Run on a listen server (gunner = MP client 2) and on a dedicated-style server where the gunner is MP
// client 0, the client whose player state is the World's own. Needs IW2_MAIN.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
#include "net/netfields.h"
#include "netgame/netmove.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

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
using game::Entity;
using game::World;
using script::Value;

constexpr const char* kMap = "mp_carentan";

int32_t getI(const std::vector<uint8_t>& b, size_t off) {
    int32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}

game::Vec3 origin(const Entity* e) { return e ? e->obj->get("origin").asVector() : game::Vec3{0, 0, 0}; }

struct Client {
    std::unique_ptr<net::GameClient> net;
    uint32_t buttons = 0;
    float forward = 0, pitch = 0, yaw = 0;
};

struct Game {
    World host;
    netgame::WorldGame game;
    net::Server server;
    std::vector<Client> clients;
    int64_t now = 1000;

    Game(fs::Vfs& vfs, bool listen) : host(vfs, kMap, true), game(nullptr, "", "", 7, 8), server(game, 5) {
        host.vm().setLoopGuardIterations(200000u);
        host.cvars["mapname"] = kMap;
        if (!listen) host.mpSetDedicated(); // client 0 is a network client
        host.mpStart("dm");
        if (listen) { // a listen server's own player is MP client 0
            const int local = host.mpConnect("host");
            host.mpBegin(local);
            host.frame();
            host.mpAutoJoin(local);
        }
        game.setWorld(&host, kMap, "dm", 7);
        game.worldOwnedByHost = true;
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = kMap;
        server.info().gametype = "dm";
    }
    int connect(const std::string& name) {
        Client c;
        c.net = std::make_unique<net::GameClient>();
        c.net->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        c.net->connect(a, now);
        clients.push_back(std::move(c));
        for (int i = 0; i < 200 && clients.back().net->state() != net::ClientState::Active; ++i) frame();
        return clients.back().net->state() == net::ClientState::Active ? clients.back().net->clientNum : -1;
    }
    void frame() {
        for (Client& c : clients) {
            c.net->poll(now, 1);
            if (c.net->state() >= net::ClientState::Primed)
            {  // c.pitch / c.yaw are the view; the command carries the view minus delta_angles (docs/spec/net.md section 10)
                const net::UserCmd v = netgame::makeUserCmd(int(now), c.buttons, c.forward, 0.0f, c.pitch, c.yaw, 0);
                const net::ClientSnapshot* ls = c.net->latest();
                c.net->sendCommand(ls ? netgame::viewCommand(v, ls->ps) : v, now);
            }
            (void)c.net->takeServerCommands();
        }
        server.poll(now, 0);
        host.frame();
        server.poll(now, 0);
        now += 50;
    }
};

void run(fs::Vfs& vfs, bool listen) {
    const char* label = listen ? "listen" : "dedicated";
    Game g(vfs, listen);
    if (!g.server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    const int gunner = g.connect("gunner");
    const int victim = g.connect("victim");
    const int watcher = g.connect("watcher"); // stays out of the game (no team): another front end that sees the turret
    std::cout << label << ": gunner " << gunner << " victim " << victim << "\n";
    CHECK(gunner >= 0 && victim >= 0);
    if (gunner < 0 || victim < 0) return;
    CHECK(listen ? gunner > 0 : gunner == 0);
    g.host.mpAutoJoin(gunner);
    g.host.mpAutoJoin(victim);
    for (int i = 0; i < 40; ++i) g.frame();
    Entity* pg = g.host.mpClientEntity(gunner);
    Entity* pv = g.host.mpClientEntity(victim);
    CHECK(pg && pv && pg->obj->get("sessionstate").toString() == "playing");
    if (!pg || !pv) return;

    // The dm 30cal (weaponinfo 30cal_stand_mp), facing +y.
    Entity* t = nullptr;
    for (const auto& e : g.host.entities())
        if (e->turret && !e->obj->removed && e->turret->weapon == "30cal_stand_mp") t = e.get();
    CHECK(t != nullptr);
    if (!t) return;
    const game::Vec3 to = origin(t), ta = t->obj->get("angles").asVector();
    const float yawRad = ta[1] * float(M_PI) / 180.0f;
    const game::Vec3 fwd{std::cos(yawRad), std::sin(yawRad), 0};
    const game::Vec3 behind{to[0] - fwd[0] * 60.0f, to[1] - fwd[1] * 60.0f, to[2] - 22.0f};
    const game::Vec3 front{to[0] + fwd[0] * 110.0f, to[1] + fwd[1] * 110.0f, to[2] - 22.0f};

    // The gunner's front end.
    World view(vfs, kMap, true);
    view.mpStartRemote();
    netgame::RemoteView rv;
    Client& cg = g.clients[0];
    game::PlayerState predicted;
    netgame::PsFields psf;
    int flashes = 0;
    std::string flashFx;
    if (const auto* wd = view.weapon("30cal_stand_mp"))
        if (const auto it = wd->find("worldflasheffect"); it != wd->end()) flashFx = it->second;
    // The turret's sounds in the front end (the loop and its stop come from the server's entity state and events).
    struct Played { std::string alias; int handle; bool stopped = false; };
    std::vector<Played> played;
    view.playSound = [&](const std::string& alias, const game::Vec3*, float*, int) {
        played.push_back({alias, int(played.size()) + 1});
        return int(played.size());
    };
    view.stopSound = [&](int h) {
        if (h > 0 && size_t(h) <= played.size()) played[size_t(h - 1)].stopped = true;
    };
    auto count = [&](const std::string& alias) { return int(std::count_if(played.begin(), played.end(), [&](const Played& p) { return p.alias == alias; })); };
    auto looping = [&](const std::string& alias) {
        return std::any_of(played.begin(), played.end(), [&](const Played& p) { return p.alias == alias && !p.stopped; });
    };
    std::string loopPlr, stopPlr;
    if (const auto* wd = view.weapon("30cal_stand_mp")) {
        if (const auto it = wd->find("loopfiresoundplayer"); it != wd->end()) loopPlr = it->second;
        if (const auto it = wd->find("stopfiresoundplayer"); it != wd->end()) stopPlr = it->second;
    }
    // The own turret's fire is predicted on the client's commands (docs/spec/net.md section 10 "Predicted turret
    // fire"); predict() alone is a frame whose snapshots have not come yet.
    netgame::PredictedTurret pt;
    auto predict = [&]() {
        if (const net::UserCmd* lc = cg.net->lastCommand()) netgame::RemoteView::predictTurret(view, pt, lc->serverTime, (lc->buttons & 1) != 0, cg.net->latest());
        view.frame();
        for (const auto& f : view.takeFxEvents())
            if (f.path == flashFx && f.tag == "tag_flash") ++flashes;
    };
    // The watcher's front end: the turret's loop and its stop come from the server's entity state and event ring.
    World wview(vfs, kMap, true);
    wview.mpStartRemote();
    netgame::RemoteView wrv;
    std::vector<Played> wplayed;
    wview.playSound = [&](const std::string& alias, const game::Vec3*, float*, int) {
        wplayed.push_back({alias, int(wplayed.size()) + 1});
        return int(wplayed.size());
    };
    wview.stopSound = [&](int h) {
        if (h > 0 && size_t(h) <= wplayed.size()) wplayed[size_t(h - 1)].stopped = true;
    };
    auto wcount = [&](const std::string& alias) { return int(std::count_if(wplayed.begin(), wplayed.end(), [&](const Played& p) { return p.alias == alias; })); };
    auto wlooping = [&](const std::string& alias) {
        return std::any_of(wplayed.begin(), wplayed.end(), [&](const Played& p) { return p.alias == alias && !p.stopped; });
    };
    std::string loopSnd, stopSnd;
    if (const auto* wd = view.weapon("30cal_stand_mp")) {
        if (const auto it = wd->find("loopfiresound"); it != wd->end()) loopSnd = it->second;
        if (const auto it = wd->find("stopfiresound"); it != wd->end()) stopSnd = it->second;
    }
    Client& cw = g.clients[2];
    int wflashes = 0;
    std::vector<net::ClientSnapshot> wsnaps, gsnaps; // the last few snapshots of the watcher and the gunner
    auto wframe = [&]() {
        wview.frame();
        for (const auto& f : wview.takeFxEvents())
            if (f.path == flashFx && f.tag == "tag_flash") ++wflashes;
    };
    auto showWatcher = [&]() {
        if (const net::ClientSnapshot* snap = cw.net->latest()) {
            wrv.apply(wview, *snap, cw.net->configstrings, watcher, wview.timeMs());
            wrv.turretEvents(wview, *snap, cw.net->configstrings);
            wframe();
            wsnaps.push_back(*snap);
            if (wsnaps.size() > 4) wsnaps.erase(wsnaps.begin());
        }
        if (const net::ClientSnapshot* snap = cg.net->latest()) {
            gsnaps.push_back(*snap);
            if (gsnaps.size() > 4) gsnaps.erase(gsnaps.begin());
        }
    };
    auto show = [&]() {
        showWatcher();
        if (const net::ClientSnapshot* snap = cg.net->latest()) {
            rv.apply(view, *snap, cg.net->configstrings, gunner, view.timeMs());
            if (const net::UserCmd* lc = cg.net->lastCommand()) netgame::RemoteView::predictTurret(view, pt, lc->serverTime, (lc->buttons & 1) != 0, cg.net->latest());
            rv.turretEvents(view, *snap, cg.net->configstrings, &pt);
            psf = netgame::readPs(snap->ps);
            predicted = netgame::predictPlayer(view.collision(), psf, cg.net->commandsAfter(psf.commandTime), &predicted);
            view.frame();
            for (const auto& f : view.takeFxEvents())
                if (f.path == flashFx && f.tag == "tag_flash") ++flashes;
        }
    };

    pg->obj->set("origin", Value(behind));
    cg.yaw = ta[1];
    for (int i = 0; i < 20; ++i) {
        g.frame();
        show();
    }
    CHECK(!view.playerOnTurret());
    std::cout << label << ": use entity " << g.host.mpState().clients[size_t(gunner)].useEntity << " turret " << t->number << "\n";
    // F: on the turret on the server, and the front end follows from the player state.
    cg.buttons = game::Button::Activate;
    for (int i = 0; i < 2; ++i) g.frame();
    cg.buttons = 0;
    for (int i = 0; i < 10; ++i) {
        g.frame();
        show();
    }
    CHECK(g.host.mpClientOnTurret(gunner));
    std::cout << label << ": ps eFlags " << std::hex << psf.eFlags << std::dec << " viewLockMode " << psf.viewLockMode
              << " viewLockEntity " << psf.viewLockEntity << "\n";
    CHECK((psf.eFlags & 0x300) != 0);
    CHECK(psf.viewLockMode == 1);
    CHECK(psf.viewLockEntity >= 64 && psf.viewLockEntity < 1022);
    CHECK(view.playerOnTurret());
    CHECK(view.playerTurretWeapon() == "30cal_stand_mp");
    // The prediction keeps the server's place behind the gun, whatever the commands' move.
    cg.forward = 1.0f;
    for (int i = 0; i < 6; ++i) {
        g.frame();
        show();
    }
    const game::Vec3 held = origin(pg);
    CHECK(std::fabs(predicted.origin[0] - held[0]) < 0.5f && std::fabs(predicted.origin[1] - held[1]) < 0.5f);
    cg.forward = 0.0f;
    // Turning the view turns the turret; the front end's copy of it gets the aim from the snapshot.
    cg.yaw = ta[1] + 10.0f;
    Entity* vt = nullptr;
    for (const auto& e : view.entities())
        if (e->turret && e->mapIndex == t->mapIndex) vt = e.get();
    CHECK(vt != nullptr);
    for (int i = 0; i < 6; ++i) {
        g.frame();
        show();
    }
    std::cout << label << ": server turret yaw " << t->turret->yaw << " client " << (vt ? vt->turret->yaw : -999.0f) << "\n";
    CHECK(std::fabs(t->turret->yaw - 10.0f) < 0.5f);
    CHECK(vt && std::fabs(vt->turret->yaw - t->turret->yaw) < 0.5f);
    cg.yaw = ta[1];
    // Attack: the turret fires and kills the victim in front of it; the obituary names the turret weapon.
    const int flashes0 = flashes, shots0 = t->turret->fireCount;
    cg.buttons = game::Button::Attack;
    const size_t obits = g.host.mpState().obituaryLog.size();
    bool killed = false;
    bool lockFired = false;
    for (int f = 0; f < 20 * 6 && !killed; ++f) {
        if (pv->obj->get("sessionstate").toString() == "playing") pv->obj->set("origin", Value(front));
        g.frame();
        show();
        lockFired = lockFired || psf.viewLockMode == 2;
        const auto& log = g.host.mpState().obituaryLog;
        for (size_t i = obits; i < log.size(); ++i)
            if (log[i].victim == victim) {
                std::cout << label << ": kill weapon " << log[i].weapon << " attacker " << log[i].attacker << "\n";
                CHECK(log[i].attacker == gunner);
                CHECK(log[i].weapon == "30cal_stand_mp" || (log[i].modByte && log[i].mod == 8));
                killed = true;
            }
    }
    CHECK(killed);
    CHECK(lockFired);
    // Every shot of the server is drawn once: predicted and matched by its event, not played a second time.
    std::cout << label << ": flashes " << flashes - flashes0 << " for " << t->turret->fireCount - shots0 << " server shots, "
              << pt.pending() << " predicted shots unmatched\n";
    CHECK(flashes - flashes0 == t->turret->fireCount - shots0);
    CHECK(pt.pending() <= 1);
    // A stretch without snapshots while attack is held (a bursty path): the front end's world runs on, the fire loop
    // goes on and no stop sound plays; the loop is the own turret's player variant.
    // The drawn pair stepping back (a clock reset) and forward again replays no event: no flash, no stop sound, and
    // the gunner's predicted shots keep their matches.
    {
        CHECK(wsnaps.size() == 4 && gsnaps.size() == 4);
        const int wf0 = wflashes, f0 = flashes, pending0 = pt.pending(), wstops0 = wcount(stopSnd), stops0 = count(stopPlr);
        if (wsnaps.size() == 4 && gsnaps.size() == 4) {
            wrv.turretEvents(wview, wsnaps[0], cw.net->configstrings);
            wrv.turretEvents(wview, wsnaps[3], cw.net->configstrings);
            wframe();
            rv.turretEvents(view, gsnaps[0], cg.net->configstrings, &pt);
            rv.turretEvents(view, gsnaps[3], cg.net->configstrings, &pt);
            view.frame();
            for (const auto& f : view.takeFxEvents())
                if (f.path == flashFx && f.tag == "tag_flash") ++flashes;
        }
        std::cout << label << ": a step back and forward: watcher flashes " << wflashes - wf0 << ", gunner flashes " << flashes - f0
                  << ", predicted shots unmatched " << pending0 << " -> " << pt.pending() << "\n";
        CHECK(wflashes == wf0 && flashes == f0 && pt.pending() == pending0);
        CHECK(wcount(stopSnd) == wstops0 && count(stopPlr) == stops0);
    }
    const int stopsHeld = count(stopPlr), wstopsHeld = wcount(stopSnd);
    const bool wloopFiring = wlooping(loopSnd);
    for (int i = 0; i < 6; ++i) {
        g.frame();
        view.frame();
        wframe();
    }
    for (int i = 0; i < 4; ++i) {
        g.frame();
        show();
    }
    std::cout << label << ": loop " << loopPlr << " playing " << looping(loopPlr) << ", stop sounds while held " << count(stopPlr) - stopsHeld << "\n";
    CHECK(!loopPlr.empty() && !stopPlr.empty());
    CHECK(looping(loopPlr));
    CHECK(count(stopPlr) == stopsHeld);
    std::cout << label << ": watcher: loop " << loopSnd << " while firing " << wloopFiring << ", after the gap " << wlooping(loopSnd)
              << ", stop sounds while held " << wcount(stopSnd) - wstopsHeld << "\n";
    CHECK(!loopSnd.empty() && !stopSnd.empty() && loopSnd != loopPlr);
    CHECK(wloopFiring && wlooping(loopSnd));
    CHECK(wcount(stopSnd) == wstopsHeld);
    CHECK(wcount(loopPlr) == 0);
    // Attack released: the server's stop sound event plays once and the loop ends.
    cg.buttons = 0;
    for (int i = 0; i < 8; ++i) {
        g.frame();
        show();
    }
    std::cout << label << ": after the release: stop sounds " << count(stopPlr) - stopsHeld << ", loop playing " << looping(loopPlr) << "\n";
    CHECK(count(stopPlr) == stopsHeld + 1);
    CHECK(!looping(loopPlr));
    std::cout << label << ": watcher after the release: stop sounds " << wcount(stopSnd) - wstopsHeld << ", loop playing " << wlooping(loopSnd) << "\n";
    CHECK(wcount(stopSnd) == wstopsHeld + 1);
    CHECK(!wlooping(loopSnd));
    // A press whose snapshots have not come yet: the predicted shot's flash and the fire loop start in that frame; the
    // server's shots that follow are matched, not drawn twice.
    {
        const int f0 = flashes, s0 = t->turret->fireCount;
        cg.buttons = game::Button::Attack;
        g.frame();
        predict();
        std::cout << label << ": press before its snapshot: " << flashes - f0 << " flash, loop " << looping(loopPlr) << "\n";
        CHECK(flashes - f0 == 1);
        CHECK(looping(loopPlr));
        for (int i = 0; i < 5; ++i) {
            g.frame();
            show();
        }
        cg.buttons = 0;
        for (int i = 0; i < 6; ++i) {
            g.frame();
            show();
        }
        std::cout << label << ": that burst: " << flashes - f0 << " flashes for " << t->turret->fireCount - s0 << " server shots\n";
        CHECK(flashes - f0 == t->turret->fireCount - s0);
        CHECK(!looping(loopPlr));
    }
    // A player state of pm_type 4 or more (the intermission's 5), or one that follows another client (a killcam of a
    // killer on a turret), predicts no turret shot and ends a predicted fire loop with its stop sound (PM_Weapon's
    // early-out; no prediction while following, docs/spec/turrets.md "Multiplayer").
    for (const bool followCase : {false, true}) {
        cg.buttons = game::Button::Attack;
        g.frame();
        predict(); // a shot predicted at once, the loop runs
        CHECK(looping(loopPlr));
        const int f0 = flashes, stops0 = count(stopPlr);
        for (int i = 0; i < 4; ++i) {
            g.frame();
            net::ClientSnapshot blocked = *cg.net->latest();
            if (followCase) {
                const int32_t flags = getI(blocked.ps, 12) | 0x400000;
                std::memcpy(blocked.ps.data() + 12, &flags, 4);
            } else {
                netgame::PsFields f = netgame::readPs(blocked.ps);
                f.pmType = 5;
                netgame::writePs(blocked.ps, f);
            }
            if (const net::UserCmd* lc = cg.net->lastCommand()) netgame::RemoteView::predictTurret(view, pt, lc->serverTime, true, &blocked);
            view.frame();
            for (const auto& f : view.takeFxEvents())
                if (f.path == flashFx && f.tag == "tag_flash") ++flashes;
        }
        std::cout << label << (followCase ? ": following" : ": pm_type 5") << " on the turret: " << flashes - f0 << " predicted flashes, stop sounds "
                  << count(stopPlr) - stops0 << ", loop " << looping(loopPlr) << "\n";
        CHECK(flashes == f0);
        CHECK(count(stopPlr) == stops0 + 1);
        CHECK(!looping(loopPlr));
        CHECK(pt.pending() == 0);
        cg.buttons = 0;
        for (int i = 0; i < 10; ++i) {
            g.frame();
            show();
        }
    }
    // The front end plays the turret's shots (EV_FIRE_WEAPON_MG42 on the turret): the muzzle flash at tag_flash.
    std::cout << label << ": flashes " << flashes << " shots " << t->turret->fireCount << "\n";
    CHECK(!flashFx.empty());
    CHECK(flashes > 0);
    // F again: off the turret, on the server and in the front end.
    cg.buttons = 0;
    for (int i = 0; i < 2; ++i) g.frame();
    cg.buttons = game::Button::Activate;
    for (int i = 0; i < 2; ++i) g.frame();
    cg.buttons = 0;
    for (int i = 0; i < 10; ++i) {
        g.frame();
        show();
    }
    CHECK(!g.host.mpClientOnTurret(gunner));
    CHECK(!t->turret->inUse);
    CHECK((psf.eFlags & 0x300) == 0 && psf.viewLockEntity == 1023);
    CHECK(!view.playerOnTurret());
    // On the gun with attack held when the map ends (dm.gsc endMap -> spawnIntermission): the intermission spawn takes
    // the gunner off (docs/spec/turrets.md "Multiplayer"); his snapshot says so, the turret stops, the front end fires
    // nothing more.
    pg->obj->set("origin", Value(behind));
    cg.yaw = ta[1];
    for (int i = 0; i < 4; ++i) {
        g.frame();
        show();
    }
    cg.buttons = game::Button::Activate;
    for (int i = 0; i < 2; ++i) g.frame();
    cg.buttons = game::Button::Attack;
    for (int i = 0; i < 8; ++i) {
        g.frame();
        show();
    }
    CHECK(g.host.mpClientOnTurret(gunner) && view.playerOnTurret());
    g.host.vm().start("maps/mp/gametypes/dm", "endMap", {}, Value(g.host.vm().level()));
    for (int i = 0; i < 6; ++i) {
        g.frame();
        show();
    }
    const int endFlashes = flashes, endShots = t->turret->fireCount;
    for (int i = 0; i < 20; ++i) {
        g.frame();
        show();
    }
    std::cout << label << ": intermission with attack held: pm_type " << psf.pmType << ", eFlags 0x" << std::hex << psf.eFlags << std::dec
              << ", lock entity " << psf.viewLockEntity << ", " << flashes - endFlashes << " flashes, " << t->turret->fireCount - endShots << " shots\n";
    CHECK(pg->obj->get("sessionstate").toString() == "intermission");
    CHECK(psf.pmType == 5 && (psf.eFlags & 0x300) == 0 && psf.viewLockEntity == 1023);
    CHECK(!g.host.mpClientOnTurret(gunner) && !t->turret->inUse && !view.playerOnTurret());
    CHECK(flashes == endFlashes && t->turret->fireCount == endShots);
    CHECK(!looping(loopPlr));
    CHECK(g.host.vm().errors().empty());
}

} // namespace

int main() {
    // Intermission (pm_type 5, docs/spec/mp-spectate.md "Intermission"): the view input is ignored and a network client takes
    // the server's view angles from the player state; other movement types keep the client's own look.
    {
        CHECK(netgame::viewInputIgnored(5));
        for (int t : {0, 1, 2, 3, 4, 6, 7}) CHECK(!netgame::viewInputIgnored(t));
        netgame::PsFields f;
        f.pmType = 5;
        f.viewangles[0] = 10.0f, f.viewangles[1] = 135.0f;
        std::vector<uint8_t> ps(size_t(net::kPlayerStateSize), 0);
        netgame::writePs(ps, f);
        const netgame::PsFields r = netgame::readPs(ps);
        CHECK(netgame::viewInputIgnored(r.pmType) && r.viewangles[0] == 10.0f && r.viewangles[1] == 135.0f);
    }
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    run(vfs, true);
    run(vfs, false);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
