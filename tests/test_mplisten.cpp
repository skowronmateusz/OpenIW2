// End to end, headless (docs/spec/net.md sections 11 and 12): a listen server (the host's World with its own player
// and bots behind net::Server and netgame::WorldGame) is found by the LAN browser, a network client connects and
// applies the snapshots to a remote World. It must see the bots, its health, weapon and ammo, HUD elements, an
// obituary in the kill feed and a chat line. Also the HUD element wire round trip. Needs IW2_MAIN.
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/lanbrowser.h"
#include "net/netfields.h"
#include "net/server.h"
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
using script::Value;

void hudRoundTrip() {
    script::VM vm([](const std::string&) { return std::nullopt; });
    auto h = vm.newObject("hudelem");
    h->set("x", Value(12.5f));
    h->set("y", Value(-40.0f));
    h->set("alignx", Value("center"));
    h->set("aligny", Value("bottom"));
    h->set("horzalign", Value("right"));
    h->set("vertalign", Value("fullscreen"));
    h->set("color", Value(script::Vec3{1, 0, 0}));
    h->set("alpha", Value(0.5f));
    h->set("fontscale", Value(1.4f));
    h->set("font", Value("objective"));
    h->set("sort", Value(3.0f));
    h->set("label", Value(script::LocalizedString{"MP_SCORE"}));
    h->set("$value", Value(42.0f));
    netgame::ConfigstringRange mats(netgame::kMaterialConfigstrings, 255), strs(netgame::kStringConfigstrings, 255);
    std::map<int, std::string> cs;
    strs.onAdd = mats.onAdd = [&](int i, const std::string& v) { cs[i] = v; };
    netgame::HudStrings hs{&mats, &strs};
    uint8_t buf[net::kHudElemSize];
    netgame::encodeHudElem(buf, *h, hs);
    auto d = netgame::decodeHudElem(buf, [&] { return vm.newObject("hudelem"); }, cs, 0);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->get("x").asFloat() == 12.5f && d->get("y").asFloat() == -40.0f);
    CHECK(d->get("alignx").toString() == "center" && d->get("aligny").toString() == "bottom");
    CHECK(d->get("horzalign").toString() == "right" && d->get("vertalign").toString() == "fullscreen");
    CHECK(d->get("font").toString() == "objective");
    CHECK(std::abs(d->get("alpha").asFloat() - 0.5f) < 0.01f && d->get("color").asVector()[1] == 0.0f);
    CHECK(d->get("$value").asFloat() == 42.0f);
    CHECK(std::holds_alternative<script::LocalizedString>(d->get("label").v));
    // A material element with a timer-less shader, and a text element.
    auto m = vm.newObject("hudelem");
    m->set("$shader", Value("white"));
    m->set("$width", Value(int32_t(64)));
    m->set("$height", Value(int32_t(8)));
    netgame::encodeHudElem(buf, *m, hs);
    d = netgame::decodeHudElem(buf, [&] { return vm.newObject("hudelem"); }, cs, 0);
    CHECK(d && d->get("$shader").toString() == "white" && d->get("$width").asInt() == 64);
    auto t = vm.newObject("hudelem");
    t->set("$text", Value("hello"));
    t->set("$fadestart", Value(1000.0f));
    t->set("$fadetime", Value(500.0f));
    t->set("$fadealpha", Value(1.0f));
    netgame::encodeHudElem(buf, *t, hs);
    d = netgame::decodeHudElem(buf, [&] { return vm.newObject("hudelem"); }, cs, 250);
    CHECK(d && d->get("$text").toString() == "hello" && d->get("$fadestart").asFloat() == 1250.0f);
}

} // namespace

int main() {
    hudRoundTrip();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(main);

    // The host: its own player (client 0) and two bots, dm on mp_toujane.
    game::World host(vfs, "mp_toujane", true);
    host.vm().setLoopGuardIterations(200000u);
    host.cvars["mapname"] = "mp_toujane";
    host.mpStart("dm");
    const int local = host.mpConnect("host");
    host.mpBegin(local);
    host.frame();
    host.mpAutoJoin(local);
    for (int i = 0; i < 2; ++i) {
        host.mpAddBot();
        host.frame();
    }
    netgame::WorldGame game(&host, "mp_toujane", "dm", 7, 8);
    game.worldOwnedByHost = true;  // the host's front end runs the frames
    net::Server server(game, 5);
    server.serverId = 7;
    game.sendCommand = [&](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
    server.info().mapname = "mp_toujane";
    server.info().gametype = "dm";
    server.info().hostname = "listen test";
    if (!server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        return 1;
    }
    int64_t now = 1000;
    auto hostFrame = [&] {
        host.frame();
        server.poll(now, 0);
        now += 50;
    };

    // LAN browser: the server answers getinfo (asked directly on its port as well as by broadcast).
    net::LanBrowser browser;
    net::NetAddress self;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), self, 28960);
    CHECK(browser.refresh(now, {self}));
    for (int i = 0; i < 40 && browser.servers().empty(); ++i) {
        server.poll(now, 5);
        browser.poll(now);
    }
    CHECK(!browser.servers().empty());
    if (browser.servers().empty()) return 1;
    CHECK(browser.column(0, 2) == "listen test");
    CHECK(browser.column(0, 3) == "mp_toujane");
    std::cout << "browser: " << browser.column(0, 2) << " " << browser.column(0, 3) << " " << browser.column(0, 4) << "\n";

    // The network client joins the first row.
    net::GameClient client;
    client.name = "remote";
    CHECK(client.connect(browser.servers()[0].address, now));
    for (int i = 0; i < 200 && client.state() < net::ClientState::Primed && client.state() != net::ClientState::Disconnected; ++i) {
        client.poll(now, 2);
        hostFrame();
    }
    CHECK(client.state() >= net::ClientState::Primed);
    const int me = client.clientNum;
    std::cout << "client number " << me << "\n";
    CHECK(me >= 3);  // host 0 and the bots come first

    game::World view(vfs, "mp_toujane", true);
    view.mpStartRemote();
    netgame::RemoteView rv;
    bool sawModels = false, sawBots = false, sawHud = false, sawHealth = false, sawWeapon = false, sawObit = false, sawChat = false;
    bool joined = false, killed = false, said = false;
    for (int i = 0; i < 400; ++i) {
        client.poll(now, 1);
        hostFrame();
        net::UserCmd c;
        c.serverTime = int(now);
        client.sendCommand(c, now);
        if (client.state() == net::ClientState::Active && !joined) {
            // The gametype's menus: autoassign a team (as the menu response would).
            if (const auto& slots = game.slots(); !slots.empty()) {
                host.mpAutoJoin(slots.begin()->second);
                joined = true;
            }
        }
        for (const std::string& sc : client.takeServerCommands()) {
            const auto tok = net::tokenize(sc);
            if (tok.size() >= 2 && tok[0] == "h" && tok[1].find("hello lan") != std::string::npos) sawChat = true;
        }
        const net::ClientSnapshot* snap = client.latest();
        if (!snap) continue;
        rv.apply(view, *snap, client.configstrings, me, view.timeMs());
        const auto players = netgame::RemoteView::players(*snap, *snap, 1.0f, me, client.configstrings);
        view.mpSetRemotePlayers(players);
        sawBots |= players.size() >= 3;
        // Bots and the host carry the body and head models of their client states (a puppet without them is invisible).
        for (const auto& p : players)
            if (!p.hidden && !p.dead && !p.model.empty() && !p.attachments.empty()) {
                const game::Entity* pe = view.mpClientEntity(p.client);
                sawModels |= pe && pe->obj->get("model").toString() == p.model && view.mpPlayerDrawn(*pe);
            }
        sawHud |= !view.hudElems().empty();
        if (view.player()) {
            const Value hp = view.player()->obj->get("health");
            sawHealth |= hp.isNumber() && hp.asInt() > 0;
            sawWeapon |= view.player()->obj->get("$weapon").isString() && !view.inventory(*view.player()).owned.empty();
        }
        for (const auto& ob : view.takeMpObituaries()) {
            std::cout << "obituary victim " << ob.victim << " attacker " << ob.attacker << " weapon " << ob.weapon << "\n";
            sawObit = true;
        }
        if (joined && i > 120 && !killed) {
            // A kill on the host: obituary(victim, attacker, weapon, mod) as the gametype's callback does.
            game::Entity* v = host.mpClientEntity(1);
            game::Entity* a = host.mpClientEntity(0);
            if (v && a) {
                std::vector<Value> args{Value(v->obj), Value(a->obj), Value("thompson_mp"), Value("MOD_PISTOL_BULLET")};
                script::Thread t;
                script::CallContext cc{host.vm(), t, Value(), std::span<Value>(args)};
                host.vm().builtin("obituary")(cc);
                killed = true;
            }
        }
        if (joined && i > 100 && !said) {
            client.addReliableCommand("say hello lan");
            said = true;
        }
        if (sawBots && sawHud && sawHealth && sawWeapon && sawObit && sawChat) break;
    }
    // The client states list every client (the host's own player included) for the scoreboard, by server client number.
    if (const net::ClientSnapshot* snap = client.latest()) {
        std::string hostName, selfName;
        int hostTeam = -1, selfTeam = -1;
        CHECK(netgame::RemoteView::clientInfo(*snap, 0, hostName, hostTeam));
        CHECK(netgame::RemoteView::clientInfo(*snap, me, selfName, selfTeam));
        CHECK(!hostName.empty() && hostName != "remote");
        CHECK(selfName == "remote");
        CHECK(selfTeam >= 0 && selfTeam <= 3 && hostTeam >= 0 && hostTeam <= 3); // dm: team 0
        std::string n;
        int t = 0;
        CHECK(!netgame::RemoteView::clientInfo(*snap, 63, n, t));
        CHECK(netgame::RemoteView::viewWeaponHidden(5) && netgame::RemoteView::viewWeaponHidden(4));
        CHECK(!netgame::RemoteView::viewWeaponHidden(0) && !netgame::RemoteView::viewWeaponHidden(1));
    }
    // The map entities the dm script deleted on the host (the objects of the other gametypes) are not drawn here: every
    // drawable map entity of the view is alive on the host, and at least one model of the map was hidden.
    {
        int hiddenMap = 0;
        for (const auto& e : view.entities()) {
            if (e->mapIndex <= 0 || e->isPlayer || e->obj->removed) continue;
            const Value md = e->obj->get("model");
            if (!md.isString() || md.asString().empty()) continue;
            if (e->obj->get("$hidden").truthy()) {
                ++hiddenMap;
                for (const auto& h : host.entities())
                    if (h->mapIndex == e->mapIndex && !h->obj->removed && !h->obj->get("$hidden").truthy() &&
                        h->classname.rfind("trigger_", 0) != 0) {
                        std::cerr << "hidden here, shown on the host: " << h->classname << " " << md.toString() << "\n";
                        CHECK(false);
                    }
                continue;
            }
            bool alive = false;
            for (const auto& h : host.entities())
                alive |= h->mapIndex == e->mapIndex && !h->obj->removed;
            if (!alive) std::cerr << "drawn but deleted on the host: " << e->classname << " " << md.toString() << "\n";
            CHECK(alive);
        }
        std::cout << "hidden map entities " << hiddenMap << "\n";
        CHECK(hiddenMap > 0);
    }
    CHECK(sawModels);
    std::cout << "bots " << sawBots << " hud " << sawHud << " health " << sawHealth << " weapon " << sawWeapon << " obituary "
              << sawObit << " chat " << sawChat << "\n";
    CHECK(sawBots);
    CHECK(sawHud);
    CHECK(sawHealth);
    CHECK(sawWeapon);
    CHECK(sawObit);
    CHECK(sawChat);
    client.disconnect();
    for (const auto& e : host.vm().errors()) std::cerr << "host script error: " << e << "\n";
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mplisten: ok\n";
    return failures ? 1 : 0;
}
