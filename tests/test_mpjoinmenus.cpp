// End to end, headless (docs/spec/net.md section 10, docs/spec/mp.md "Server to client"): a network client joins a
// dedicated-style server (no local player) running dm on mp_carentan with 15 bots and walks the gametype's script
// menus as a player would: serverinfo -> team -> weapon -> playing. The client keeps the menu stack the server's
// `t` (open) and `u` (close) commands build; after each step exactly one script menu must be open, and none once the
// player spawned. Run for every weapon class and for all classes. Needs IW2_MAIN.
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
#include "netgame/netmove.h"
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

void run(fs::Vfs& vfs, const std::string& classes) {
    std::cout << "--- weapon classes " << (classes.empty() ? "all" : classes) << "\n";
    game::World host(vfs, "mp_carentan", true);
    host.vm().setLoopGuardIterations(200000u);
    host.cvars["mapname"] = "mp_carentan";
    if (!classes.empty()) host.cvars["iw2_weaponclasses"] = classes;
    host.mpStart("dm");
    host.mpSetBotSkill(1);
    for (int i = 0; i < 15; ++i) {
        CHECK(host.mpAddBot() >= 0);
        host.frame();
    }
    netgame::WorldGame game(&host, "mp_carentan", "dm", 9, 20);
    net::Server server(game, 5);
    server.serverId = 9;
    server.maxClients = 20;
    game.sendCommand = [&](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
    if (!server.open(0)) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    int64_t now = 1000;
    net::GameClient client;
    client.name = "joiner";
    net::NetAddress to;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), to, 28960);
    CHECK(client.connect(to, now));

    std::vector<std::string> stack;          // the client's open script menus, bottom first
    std::map<std::string, std::string> cvars; // setClientCvar values the client got
    std::string chosen;
    int step = 0; // 0 wait serverinfo, 1 wait team, 2 wait weapon, 3 wait spawn, 4 done
    for (int i = 0; i < 1200 && step < 4; ++i) {
        client.poll(now, 1);
        server.poll(now, 0);
        now += 50;
        if (client.state() == net::ClientState::Active) {
            net::UserCmd c;
            c.serverTime = int(now);
            client.sendCommand(c, now);
        }
        for (const std::string& sc : client.takeServerCommands()) {
            const auto tok = net::tokenize(sc);
            if (tok.empty()) continue;
            if (tok[0] == "t" && tok.size() >= 2) {
                const auto cs = client.configstrings.find(netgame::kMenuConfigstrings + std::atoi(tok[1].c_str()));
                stack.push_back(cs == client.configstrings.end() ? "?" : cs->second);
            } else if (tok[0] == "u") {
                if (!stack.empty()) stack.pop_back();
            } else if (tok[0] == "v" && tok.size() >= 3) {
                cvars[tok[1]] = tok[2];
            }
        }
        auto respond = [&](const std::string& response) {
            for (int k = 0; k < 32; ++k) {
                const auto cs = client.configstrings.find(netgame::kMenuConfigstrings + k);
                if (cs != client.configstrings.end() && !stack.empty() && cs->second == stack.back())
                    client.addReliableCommand("mr " + std::to_string(client.serverId) + " " + std::to_string(k) + " " + response);
            }
        };
        if (stack.empty()) continue;
        const std::string& top = stack.back();
        if (step == 0 && top.rfind("serverinfo_", 0) == 0) {
            CHECK(stack.size() == 1);
            respond("close");
            step = 1;
        } else if (step == 1 && top.rfind("team_", 0) == 0) {
            std::cout << "after serverinfo: " << stack.size() << " menu(s), top " << top << "\n";
            CHECK(stack.size() == 1);
            respond("autoassign");
            step = 2;
        } else if (step == 2 && top.rfind("weapon_", 0) == 0) {
            std::cout << "after team: " << stack.size() << " menu(s), top " << top << "\n";
            CHECK(stack.size() == 1);
            // The first weapon of the side the server allows (its ui_allow_* cvars).
            static const char* american[] = {"m1carbine", "m1garand", "thompson", "bar", "springfield", "greasegun", "shotgun"};
            static const char* british[] = {"enfield", "sten", "bren", "enfieldsniper", "m1garand", "thompson", "shotgun"};
            static const char* russian[] = {"mosin_nagant", "svt40", "ppsh", "pps42", "mosin_nagant_sniper", "shotgun"};
            static const char* german[] = {"kar98k", "g43", "mp40", "mp44", "kar98k_sniper", "shotgun"};
            std::vector<const char*> side;
            if (top == "weapon_german") side.assign(std::begin(german), std::end(german));
            else if (top == "weapon_russian") side.assign(std::begin(russian), std::end(russian));
            else if (top == "weapon_british") side.assign(std::begin(british), std::end(british));
            else side.assign(std::begin(american), std::end(american));
            auto allowKey = [](std::string w) {
                if (w == "mosin_nagant") return std::string("nagant");
                if (w == "mosin_nagant_sniper") return std::string("nagantsniper");
                if (w == "kar98k_sniper") return std::string("kar98ksniper");
                return w;
            };
            for (const char* w : side)
                if (cvars["ui_allow_" + allowKey(w)] == "1") {
                    chosen = std::string(w) + "_mp";
                    break;
                }
            CHECK(!chosen.empty());
            std::cout << "weapon " << chosen << "\n";
            respond(chosen);
            step = 3;
        }
        if (step == 3) {
            const auto& slots = game.slots();
            game::Entity* pe = slots.empty() ? nullptr : host.mpClientEntity(slots.begin()->second);
            if (pe && pe->obj->get("sessionstate").toString() == "playing") step = 4;
        }
    }
    // A few more frames so the last close reaches the client.
    for (int i = 0; i < 20; ++i) {
        client.poll(now, 1);
        server.poll(now, 0);
        now += 50;
        for (const std::string& sc : client.takeServerCommands()) {
            const auto tok = net::tokenize(sc);
            if (!tok.empty() && tok[0] == "u" && !stack.empty()) stack.pop_back();
            if (!tok.empty() && tok[0] == "t") stack.push_back(tok.size() > 1 ? tok[1] : "?");
        }
    }
    std::cout << "step " << step << ", menus left " << stack.size() << "\n";
    CHECK(step == 4);
    CHECK(stack.empty());
    if (step == 4) {
        game::Entity* pe = host.mpClientEntity(game.slots().begin()->second);
        CHECK(pe && pe->obj->get("$weapon").toString() == chosen);
    }
    client.disconnect();
    for (const auto& e : host.vm().errors()) std::cerr << "script error: " << e << "\n";
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
    for (const char* c : {"rifle", "smg", "mg", "sniper", "shotgun", ""}) run(vfs, c);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpjoinmenus: ok\n";
    return failures ? 1 : 0;
}
