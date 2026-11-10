#include "fs/gamedir.h"
// iw2game multiplayer mode: an offline listen server (docs/spec/mp.md) run headless.
//
//   iw2game <main-dir> mp/<map> <gametype> [seconds] [options]
//     --cvar name=value          set a cvar before the game starts
//     --mp-clients N             local clients to connect (default 1; client 0 is the local player)
//     --mp-spectate              do not join: the clients stay spectators after "begin"
//     --mp-menu t:c:menu:resp    at t seconds, client c sends the menu response (menu, response)
//     --mp-kill t:victim:attacker  at t seconds, 1000 damage to client victim from client attacker (-1 = the world)
//     --mp-use t:c               at t seconds, client c presses use for one frame (respawn)
//     --mp-disconnect t:c        at t seconds, client c disconnects
//     --mp-bots N                add N bots (addtestclient with the bot controller, docs/spec/mp-bots.md) after the clients
//     --mp-bot-skill S           bot skill 0 recruit .. 3 veteran (default 1)
//     --mp-classes LIST          allowed weapon classes (rifle, bolt, sniper, smg, mg, shotgun), e.g. rifle,smg (sets iw2_weaponclasses -> scr_allow_*)
// Prints the clients (state, team, score, deaths, health, origin), the game log and the script errors.

#include "audio/sound.h"
#include "game/world.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct Timed {
    float t;
    std::vector<std::string> parts;
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

} // namespace

int runMultiplayer(int argc, char** argv) {
    const std::string mapArg = argv[2];
    const std::string map = mapArg.substr(3); // "mp/<map>"
    if (argc < 4 || argv[3][0] == '-') {
        std::cerr << "usage: iw2game <main-dir> mp/<map> <gametype> [seconds] [options]\n";
        return 2;
    }
    const std::string gametype = argv[3];
    float seconds = 10.0f;
    int clients = 1;
    int bots = 0;
    int botSkill = 1;
    bool join = true;
    std::vector<std::pair<std::string, std::string>> cvars;
    std::vector<Timed> events; // kind in parts[0]
    int i = 4;
    if (i < argc && argv[i][0] != '-') seconds = std::stof(argv[i++]);
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto timed = [&](const char* kind) {
            auto p = split(argv[++i], ':');
            Timed t{std::stof(p[0]), {kind}};
            t.parts.insert(t.parts.end(), p.begin() + 1, p.end());
            events.push_back(t);
        };
        if (a == "--cvar" && i + 1 < argc) {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) cvars.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (a == "--mp-clients" && i + 1 < argc) {
            clients = std::atoi(argv[++i]);
        } else if (a == "--mp-bots" && i + 1 < argc) {
            bots = std::atoi(argv[++i]);
        } else if (a == "--mp-bot-skill" && i + 1 < argc) {
            botSkill = std::atoi(argv[++i]);
        } else if (a == "--mp-classes" && i + 1 < argc) {
            cvars.push_back({"iw2_weaponclasses", argv[++i]});
        } else if (a == "--mp-spectate") {
            join = false;
        } else if (a == "--mp-menu" && i + 1 < argc) {
            timed("menu");
        } else if (a == "--mp-kill" && i + 1 < argc) {
            timed("kill");
        } else if (a == "--mp-use" && i + 1 < argc) {
            timed("use");
        } else if (a == "--mp-disconnect" && i + 1 < argc) {
            timed("disconnect");
        } else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }

    iw2::fs::Vfs vfs;
    if (const std::string e = iw2::fs::mainDirError(argv[1]); !e.empty()) {
        std::fprintf(stderr, "iw2game: error: %s\n", e.c_str());
        return 1;
    }
    vfs.mountDirectory(argv[1]);
    iw2::game::World world(vfs, map, true);
    world.print = [](const std::string& s) { std::cout << "[script] " << s << "\n"; };
    world.vm().setLoopGuardIterations(200000u);
    iw2::audio::SoundAliases aliases;
    aliases.load(vfs, map, false, "all_mp"); // the bare map name selects the MP aliases (loadspec)
    world.soundExists = [&](const std::string& n) { return aliases.exists(n); };
    for (const auto& [k, v] : cvars) world.cvars[k] = v;
    world.mpStart(gametype);

    for (int c = 0; c < clients; ++c) {
        const int n = world.mpConnect(c == 0 ? "player" : "client" + std::to_string(c));
        if (n < 0) break;
        world.mpBegin(n);
        world.frame(); // one client per frame: _menus.gsc:49 re-arms its "connecting" wait only after handling one
    }
    if (join)
        for (int c = 0; c < clients; ++c) world.mpAutoJoin(c);
    world.mpSetBotSkill(botSkill);
    for (int b = 0; b < bots; ++b) {
        if (world.mpAddBot() < 0) break;
        world.frame(); // one connect per frame, as for the clients above
    }

    const int frames = int(seconds * 20.0f);
    std::vector<int> releaseUse;
    for (int f = 0; f < frames; ++f) {
        const float t = float(world.timeMs()) / 1000.0f;
        for (int c : releaseUse) world.mpSetButtons(c, 0);
        releaseUse.clear();
        for (auto& e : events) {
            if (e.t < 0 || t < e.t) continue;
            e.t = -1;
            const std::string& kind = e.parts[0];
            if (kind == "menu" && e.parts.size() >= 4) world.mpMenuResponse(std::atoi(e.parts[1].c_str()), e.parts[2], e.parts[3]);
            if (kind == "kill" && e.parts.size() >= 3)
                world.mpDebugDamage(std::atoi(e.parts[1].c_str()), std::atoi(e.parts[2].c_str()), 1000, "MOD_RIFLE_BULLET");
            if (kind == "use" && e.parts.size() >= 2) {
                const int c = std::atoi(e.parts[1].c_str());
                if (c == 0) {
                    iw2::game::World::PlayerCommand cmd;
                    cmd.buttons = 0x8;
                    world.setPlayerCommand(cmd);
                }
                world.mpSetButtons(c, 0x8);
                releaseUse.push_back(c);
            }
            if (kind == "disconnect" && e.parts.size() >= 2) world.mpDisconnect(std::atoi(e.parts[1].c_str()));
        }
        world.frame();
        if (!releaseUse.empty()) world.setPlayerCommand({});
    }

    const auto& st = world.mpState();
    std::cout << "mp " << map << " " << gametype << ": simulated " << seconds << " s\n";
    std::cout << "team score allies " << world.mpTeamScore("allies") << " axis " << world.mpTeamScore("axis") << " restarts "
              << world.mpRestarts() << "\n";
    for (size_t c = 0; c < st.clients.size(); ++c) {
        const auto& cl = st.clients[c];
        if (cl.state == iw2::game::MpClientState::Free || !cl.ent) continue;
        const auto& o = cl.ent->obj;
        const iw2::script::Value pers = o->get("pers");
        std::string team = "-", weapon = "-";
        if (pers.isArray())
            for (const auto& [k, v] : std::get<iw2::script::ArrayPtr>(pers.v)->items) {
                if (k.s == "team") team = v.toString();
                if (k.s == "weapon") weapon = v.toString();
            }
        const auto org = o->get("origin").asVector();
        std::printf("client %zu %s: %s team %s score %d deaths %d health %d origin %.0f %.0f %.0f menu %s weapon %s\n", c, cl.name.c_str(),
                    o->get("sessionstate").toString().c_str(), team.c_str(), o->get("score").asInt(), o->get("deaths").asInt(),
                    o->get("health").isNumber() ? o->get("health").asInt() : 0, org[0], org[1], org[2],
                    cl.openMenu.empty() ? "-" : cl.openMenu.c_str(), weapon.c_str());
        if (world.mpIsBot(int(c))) std::cout << "  " << world.mpBotDescribe(int(c)) << "\n";
    }
    for (const auto& l : st.log) std::cout << "log " << l << (l.empty() || l.back() != '\n' ? "\n" : "");
    if (st.exitRequested) std::cout << "exitLevel requested\n";
    std::map<std::string, int> errs;
    for (const auto& e : world.vm().errors()) ++errs[e];
    for (const auto& [e, n] : errs) std::cout << "  error x" << n << ": " << e << "\n";
    for (const auto& [f, n] : world.missingBuiltins()) std::cout << "  stub x" << n << ": " << f << "\n";
    std::cout << "runtime errors " << world.vm().errors().size() << "\n";
    return world.vm().errors().empty() ? 0 : 1;
}
