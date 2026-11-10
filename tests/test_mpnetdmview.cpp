// End to end, headless (docs/spec/net.md, docs/spec/mp-hud.md): a network client kills a bot on a dm listen server and
// its front end (RemoteView into a client World) shows what the 1.3 client shows: the centre print without the teammate
// form (dm has no teams: every client state carries team 0, whatever the player model's side), plain-white kill-feed
// names, and the victim's dropped weapon drawn with its world model, falling to the ground. Needs IW2_MAIN.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/mphud.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
#include "netgame/netcombat.h"
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

// mp_carentan: its map entity list (over 1800 entries) is longer than the entity numbers the wire has.
constexpr const char* kMap = "mp_carentan";

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
game::Vec3 origin(Entity* e) { return e ? e->obj->get("origin").asVector() : game::Vec3{0, 0, 0}; }

int32_t angleShort(float deg) { return int32_t(std::lround(deg * 65536.0f / 360.0f)) & 0xffff; }

// A listen server (host player 0 and its World) with network clients.
struct Game {
    World host;
    netgame::WorldGame game;
    net::Server server;
    std::vector<std::unique_ptr<net::GameClient>> clients;
    std::vector<net::UserCmd> cmds;  // the command each client sends every frame (serverTime filled in)
    int64_t now = 1000;

    Game(fs::Vfs& vfs, const char* gt) : host(vfs, kMap, true), game(nullptr, "", "", 7, 8), server(game, 5) {
        host.vm().setLoopGuardIterations(200000u);
        host.cvars["mapname"] = kMap;
        host.mpStart(gt);
        const int local = host.mpConnect("host");
        host.mpBegin(local);
        host.frame();
        game.setWorld(&host, kMap, gt, 7);
        game.worldOwnedByHost = true;
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = kMap;
        server.info().gametype = gt;
    }
    bool open() { return server.open(0); }
    int connect(const std::string& name) {
        auto c = std::make_unique<net::GameClient>();
        c->name = name;
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        c->connect(a, now);
        clients.push_back(std::move(c));
        cmds.emplace_back();
        for (int i = 0; i < 200 && clients.back()->state() != net::ClientState::Active; ++i) frame();
        return clients.back()->state() == net::ClientState::Active ? clients.back()->clientNum : -1;
    }
    void frame() {
        for (size_t i = 0; i < clients.size(); ++i) {
            clients[i]->poll(now, 1);
            if (clients[i]->state() >= net::ClientState::Primed) {
                net::UserCmd c = cmds[i];
                c.serverTime = int(now);
                clients[i]->sendCommand(c, now);
            }
            (void)clients[i]->takeServerCommands();
        }
        server.poll(now, 0);
        host.frame();
        server.poll(now, 0);
        now += 50;
    }
    int slotOf(int client) const {
        for (const auto& [s, n] : game.slots())
            if (n == client) return s;
        return -1;
    }
    // Client i looks at the point (its view from the eye the server uses: origin + view height 60, standing).
    void aim(size_t i, const game::Vec3& at) {
        const game::Vec3 o = origin(host.mpClientEntity(clients[i]->clientNum));
        const float dx = at[0] - o[0], dy = at[1] - o[1], dz = at[2] - (o[2] + 60.0f);
        cmds[i].angles[1] = angleShort(std::atan2(dy, dx) * 180.0f / float(M_PI));
        cmds[i].angles[0] = angleShort(-std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0f / float(M_PI));
    }
    // Moves a player next to `target` where the line between them is clear (the movement restarts there).
    bool placeNear(Entity* p, Entity* target, float dist) {
        if (!p || !target) return false;
        const game::Vec3 t = origin(target);
        for (int k = 0; k < 16; ++k) {
            const float a = float(k) * float(M_PI) / 8.0f;
            const game::Vec3 c{t[0] + std::cos(a) * dist, t[1] + std::sin(a) * dist, t[2]};
            const auto tr = host.collision().trace({t[0], t[1], t[2] + 40}, {c[0], c[1], c[2] + 40}, {-16, -16, -16}, {16, 16, 16},
                                                   game::Contents::PlayerClipMask, target->number);
            if (tr.fraction < 1 || tr.startSolid) continue;
            p->obj->set("origin", Value(c));
            return true;
        }
        return false;
    }
    bool obituary(int victim, int attacker) const {
        for (const auto& ob : host.mpState().obituaryLog)
            if (ob.victim == victim && (attacker < 0 || ob.attacker == attacker)) return true;
        return false;
    }
};


int32_t csTeam(const net::ClientSnapshot& s, int client) {
    const auto it = s.clients.find(client);
    if (it == s.clients.end()) return -1;
    int32_t t = 0;
    std::memcpy(&t, it->second.data() + 4, 4);
    return t & 3;
}

int snapCount(const net::ClientSnapshot& s, int eType) {
    int n = 0;
    for (const auto& [num, es] : s.entities) {
        int32_t t = 0;
        std::memcpy(&t, es.data() + 4, 4);
        n += (t & 0xff) == eType;
    }
    return n;
}

void run(fs::Vfs& vfs) {
    Game g(vfs, "dm");
    g.host.mpAutoJoin(0);
    if (!g.open()) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    std::vector<int> bots;
    for (int k = 0; k < 4; ++k) {
        bots.push_back(g.host.mpAddBot("bot" + std::to_string(k + 1)));
        g.frame();
        g.host.mpBotSetCombat(bots.back(), false);
    }
    const int a = g.connect("alpha");
    CHECK(a > 0);
    if (a <= 0) return;
    g.host.mpAutoJoin(a);
    for (int i = 0; i < 100; ++i) g.frame();
    const netgame::NetCombat* ca = g.game.combat(g.slotOf(a));
    CHECK(ca && ca->weapon().equipped());
    if (!ca) return;

    // Alpha's front end: a remote World fed with alpha's snapshots.
    World view(vfs, kMap, true);
    view.mpStartRemote();
    netgame::RemoteView rv;
    std::vector<game::MpObituary> obits;
    std::vector<World::FxEvent> fx;
    // The heights of the dropped weapons as drawn in alpha's view, from their first sight (a snapshot follows every
    // server frame, so a weapon dropped at a kill is seen falling from the frame of the kill on).
    std::map<std::string, std::vector<float>> zs;
    auto show = [&]() {
        if (const net::ClientSnapshot* snap = g.clients[0]->latest()) {
            view.mpSetRemotePlayers(netgame::RemoteView::players(*snap, *snap, 1.0f, a, g.clients[0]->configstrings));
            rv.apply(view, *snap, g.clients[0]->configstrings, a, view.timeMs());
            view.frame();
            for (auto& o : view.takeMpObituaries()) obits.push_back(o);
            for (auto& f : view.takeFxEvents()) fx.push_back(f);
            for (const auto& e : view.entities())
                if (!e->obj->removed && e->classname == "script_model" && !e->obj->get("$hidden").truthy()) {
                    const std::string m = e->obj->get("model").toString();
                    if (m.rfind("xmodel/weapon_", 0) == 0) zs[m].push_back(origin(e.get())[2]);
                }
        }
    };
    // 1. Alpha kills a bot of its own side (the same body models' nation) and one of the other: in dm both are
    // team 0 in the client states, so the centre print names the victim without the teammate form and the kill-feed
    // names are white.
    const std::string alphaModel = field(g.host.mpClientEntity(a), "model");
    const auto side = [](const std::string& m) { return m.find("german") != std::string::npos ? 1 : 2; };
    int sameSide = -1, otherSide = -1;
    for (int b : bots)
        (side(field(g.host.mpClientEntity(b), "model")) == side(alphaModel) ? sameSide : otherSide) = b;
    std::cout << "alpha " << alphaModel << " same side bot " << sameSide << " other side bot " << otherSide << "\n";
    CHECK(sameSide >= 0);
    std::vector<int> victims;
    for (int bt : {sameSide, otherSide}) {
        if (bt < 0) continue;
        bool kill = false;
        for (int i = 0; i < 400 && !kill; ++i) {
            Entity* bot = g.host.mpClientEntity(bt);
            if (i % 20 == 0) g.placeNear(bot, g.host.mpClientEntity(a), 120);
            g.aim(0, {origin(bot)[0], origin(bot)[1], origin(bot)[2] + 40});
            g.cmds[0].buttons = int32_t((i / 2) % 2 == 0 ? game::Button::Attack : 0);
            g.frame();
            show();
            kill = g.obituary(bt, a);
        }
        CHECK(kill);
        victims.push_back(bt);
        if (const net::ClientSnapshot* snap = g.clients[0]->latest()) {
            CHECK(csTeam(*snap, a) == 0);
            CHECK(csTeam(*snap, bt) == 0);
        }
    }
    g.cmds[0].buttons = 0;
    for (int i = 0; i < 4; ++i) g.frame(), show();
    int centrePrints = 0;
    for (const auto& o : obits) {
        if (o.attacker != 0) continue;  // alpha is slot 0 of its own view
        const game::ObituaryLine l = view.mpObituaryLine(o);
        std::cout << "obituary victim " << o.victimName << " teams " << o.victimTeam << "/" << o.attackerTeam << " centre '"
                  << l.centerText << "'\n";
        CHECK(l.centerPrint == "CGAME_YOUKILLED");
        CHECK(!l.teammate);
        CHECK(l.centerName == o.victimName.substr(0, 31) + "^7" && !o.victimName.empty());
        CHECK(l.centerText.find("CGAME_TEAMMATE") == std::string::npos);
        const auto white = game::mpTeamColor(l.victimTeam, "", ""), white2 = game::mpTeamColor(l.attackerTeam, "", "");
        CHECK(white[0] == 1 && white[1] == 1 && white[2] == 1 && white2[0] == 1 && white2[2] == 1);
        ++centrePrints;
    }
    CHECK(centrePrints == int(victims.size()));
    CHECK(view.compassFriendlies().empty());  // no teammates on the compass in dm

    // 2. The dropped weapons: eType 3 in the snapshots, drawn in alpha's view with the weapon's world model at the
    // server's position, falling to the ground.
    for (int i = 0; i < 40; ++i) {
        g.frame();
        show();
    }
    const net::ClientSnapshot* snap = g.clients[0]->latest();
    CHECK(snap && snapCount(*snap, netgame::kEtItem) > 0);
    int serverItems = 0, matched = 0;
    for (const auto& e : g.host.entities()) {
        if (e->obj->removed || e->classname.rfind("weapon_", 0) != 0) continue;
        ++serverItems;
        const std::string m = field(e.get(), "model");
        const auto it = zs.find(m);
        std::cout << "server item " << e->classname << " " << m << " z " << origin(e.get())[2] << " view samples "
                  << (it == zs.end() ? 0 : it->second.size()) << "\n";
        if (it != zs.end() && std::fabs(it->second.back() - origin(e.get())[2]) < 1.0f) ++matched;
    }
    CHECK(serverItems > 0);
    CHECK(matched == serverItems);
    bool fell = false;
    for (const auto& [m, v] : zs) fell |= v.size() > 1 && v.front() > v.back() + 4.0f;
    CHECK(fell);

    // 3. Alpha's frag grenade: in flight in its own view (the projectile model), then the explosion's effects there.
    fx.clear();
    std::string projectile, explosionFx;
    for (const auto& wn : g.host.inventory(*g.host.mpClientEntity(a)).owned)
        if (const auto* d = g.host.weapon(wn)) {
            const auto c = d->find("offhandclass"), pm = d->find("projectilemodel");
            if (c != d->end() && c->second == "Frag Grenade" && pm != d->end()) {
                projectile = pm->second;
                if (const auto ef = d->find("projexplosioneffect"); ef != d->end()) explosionFx = ef->second;
            }
        }
    std::cout << "projectile " << projectile << "\n";
    CHECK(!projectile.empty());
    bool flying = false;
    size_t fxBefore = 0;
    int hostGrenades = 0;
    for (int i = 0; i < 120; ++i) {
        g.cmds[0].angles[0] = angleShort(-30.0f);
        g.cmds[0].buttons = int32_t(i < 4 ? game::Button::Frag : 0);
        g.frame();
        show();
        for (const auto& e : view.entities())
            if (!e->obj->removed && e->classname == "script_model" && e->obj->get("model").toString() == projectile) flying = true;
        if (!flying) fxBefore = fx.size();
        hostGrenades += std::count_if(g.host.entities().begin(), g.host.entities().end(),
                                      [](const auto& e) { return !e->obj->removed && e->classname == "grenade"; }) > 0;
    }
    std::cout << "frames with a grenade on the server " << hostGrenades << "\n";
    g.cmds[0].buttons = 0;
    std::cout << "grenade seen in flight " << flying << ", effects after it " << fx.size() - fxBefore << "\n";
    CHECK(flying);
    bool explosion = false;
    // The impact table's grenade explosion effect for the surface (and projExplosionEffect when the weapon has one).
    for (size_t k = fxBefore; k < fx.size(); ++k) {
        std::cout << "effect " << fx[k].path << "\n";
        explosion |= fx[k].path.find("grenade") != std::string::npos || (!explosionFx.empty() && fx[k].path == explosionFx);
    }
    CHECK(explosion);
    for (const auto& e : g.host.vm().errors()) std::cerr << "script error: " << e << "\n";
}
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    run(vfs);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpnetdmview_e2e: ok\n";
    return failures ? 1 : 0;
}
