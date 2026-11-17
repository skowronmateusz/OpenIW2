// A network client that stands still is hit (docs/spec/mp-players.md "Animation rate from the movement", "Hit
// locations"): the server poses every client from its own player animation, with the rate the movement gives it, and the
// bullets of the others trace that pose. A dedicated World on mp_toujane (dm) with two bots and one network client over
// loopback, in simulated time:
//   - standing still: the legs play the stance's idle at a rate of 1 (an animation without a move speed), the posed upper
//     spine lies in the body's box, and a bullet line through it from each of eight sides hits the client;
//   - walking, then stopping: while the legs still play the walk (the rate goes down to its lower bound) every server
//     frame's pose is hit from the eight sides;
//   - a bot brought next to the standing client, facing it, sees it and shoots it: its health goes down and the
//     obituary names the bot.
// Needs IW2_MAIN (and IW2_NAV_CACHE for the bots' graph).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "net/server.h"
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
using game::Vec3;

Vec3 originOf(game::Entity* e) { return e && e->obj->get("origin").isVector() ? e->obj->get("origin").asVector() : Vec3{0, 0, 0}; }

int healthOf(game::Entity* e) {
    const script::Value v = e ? e->obj->get("health") : script::Value();
    return v.isInt() ? v.asInt() : v.isNumber() ? int(v.asFloat()) : -1;
}

// The posed upper spine of a client in the world (the bone the bots' decided hits aim at).
bool spineOf(game::World& w, game::Entity& e, Vec3& out) { return w.entityTagPosition(e.number, "j_spine4", out); }

// Bullet lines through `p` from eight sides, 48 units out: how many sides count (the world leaves the way in to `p`
// clear, as a spawn point may stand at a wall, and nothing else, a passing bot, stops the bullet first) and how many of
// them pass the client without hitting it.
std::pair<int, int> missesThrough(game::World& w, const Vec3& p, int num) {
    int sides = 0, misses = 0;
    for (int k = 0; k < 8; ++k) {
        const float a = float(k) * float(M_PI) / 4.0f;
        const Vec3 s{p[0] + std::cos(a) * 48.0f, p[1] + std::sin(a) * 48.0f, p[2]};
        const Vec3 e{p[0] - std::cos(a) * 48.0f, p[1] - std::sin(a) * 48.0f, p[2]};
        if (w.collision().trace(s, p, {0, 0, 0}, {0, 0, 0}, game::Contents::SightMaskNoCharacters, num).fraction < 1.0f) continue;
        const int hit = w.debugBulletLocation(s, e, nullptr).first;
        if (hit != num && hit != -1) continue;
        ++sides;
        misses += hit == -1;
    }
    return {sides, misses};
}

void run(const char* mainDir) {
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    game::World w(vfs, "mp_toujane", true);
    w.cvars["mapname"] = "mp_toujane";
    w.cvars["sv_maxclients"] = "4";
    w.cvars["scr_dm_timelimit"] = "0";
    w.cvars["scr_dm_scorelimit"] = "0";
    w.mpSetDedicated();
    w.mpStart("dm");
    (void)w.mpNav();
    netgame::WorldGame game(nullptr, "", "", 7, 4);
    game.setWorld(&w, "mp_toujane", "dm", 7);
    net::Server server(game, 5);
    server.maxClients = 4;
    server.serverId = 7;
    game.sendCommand = [&server](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
    game.clientPing = [&server](int slot) { return server.client(slot).ping; };
    server.info().mapname = "mp_toujane";
    server.info().gametype = "dm";
    CHECK(server.open(0));
    int bots[2] = {-1, -1};
    for (int& b : bots) {
        b = w.mpAddBot();
        w.frame();
        CHECK(b >= 0);
        if (b >= 0) w.mpBotSetCombat(b, false);  // nobody shoots until the last part
    }

    net::NetAddress to;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), to, 28960);
    int64_t now = 100000;
    net::GameClient client;
    client.name = "still";
    CHECK(client.connect(to, now));
    net::UserCmd cmd;
    int64_t nextCmd = now;
    auto step = [&](int ms) {  // server and client on one simulated clock; a command every 16 ms
        for (int i = 0; i < ms; i += 4) {
            now += 4;
            server.poll(now, 0);
            client.poll(now, 0);
            if (client.state() >= net::ClientState::Primed && now >= nextCmd) {
                net::UserCmd c = cmd;
                c.serverTime = client.commandTime(now);
                client.sendCommand(c, now);
                nextCmd = now + 16;
            }
        }
    };
    for (int i = 0; i < 1000 && client.state() != net::ClientState::Active && client.state() != net::ClientState::Disconnected; ++i) step(20);
    CHECK(client.state() == net::ClientState::Active);
    const int me = client.clientNum;
    w.mpAutoJoin(me);
    step(3000);  // spawned, standing still
    game::Entity* h = w.mpClientEntity(me);
    CHECK(h && h->obj->get("sessionstate").toString() == "playing");
    if (!h || !h->anim) return;
    const game::MpClient& cl = w.mpState().clients[size_t(me)];

    // Standing still: the idle has no move speed, so its rate stays 1; the pose is a standing body that bullets meet.
    {
        const int legs = cl.anim.legsNode;
        CHECK(legs > 0);
        if (legs <= 0) return;
        const std::string legsName = h->anim->tree().nodes()[size_t(legs)].name;
        const float rate = h->anim->nodeStates()[size_t(legs)].rate;
        std::printf("still: legs %s move speed %.1f rate %.2f\n", legsName.c_str(), double(w.mpAnimMoveSpeed(legs)), double(rate));
        CHECK(cl.anim.moveType == "idle");
        CHECK(w.mpAnimMoveSpeed(legs) == 0.0f && rate == 1.0f);
        Vec3 sp{};
        CHECK(spineOf(w, *h, sp));
        const Vec3 o = originOf(h);
        std::printf("still: origin (%.1f %.1f %.1f) j_spine4 (%.1f %.1f %.1f)\n", double(o[0]), double(o[1]), double(o[2]), double(sp[0]),
                    double(sp[1]), double(sp[2]));
        CHECK(sp[2] - o[2] > 30.0f && sp[2] - o[2] < 70.0f && std::hypot(sp[0] - o[0], sp[1] - o[1]) < 20.0f);
        for (int i = 0; i < 10; ++i) {  // over half a second of server frames
            const auto [sides, misses] = spineOf(w, *h, sp) ? missesThrough(w, sp, h->number) : std::pair<int, int>{0, 0};
            if (i == 0) std::printf("still: %d sides, %d misses\n", sides, misses);
            CHECK(sides >= 4 && misses == 0);
            step(50);
        }
    }

    // Walking, then stopping: the walk's rate follows the speed down to its lower bound while it still plays; every
    // frame's pose is hit.
    {
        cmd.forwardmove = 127;
        step(1500);
        cmd.forwardmove = 0;
        float minRate = 10.0f, maxRate = 0.0f;
        int frames = 0, moving = 0;
        bool allHit = true;
        for (int i = 0; i < 30; ++i) {
            step(50);
            const int legs = cl.anim.legsNode;
            if (legs <= 0) continue;
            const float rate = h->anim->nodeStates()[size_t(legs)].rate;
            if (w.mpAnimMoveSpeed(legs) > 0.0f) {
                ++moving;
                minRate = std::min(minRate, rate);
                maxRate = std::max(maxRate, rate);
            }
            Vec3 sp{};
            const auto [sides, misses] = spineOf(w, *h, sp) ? missesThrough(w, sp, h->number) : std::pair<int, int>{0, 0};
            const bool hit = sides >= 4 && misses == 0;
            allHit = allHit && hit;
            ++frames;
        }
        std::printf("walk and stop: %d frames, %d with a moving animation, rate %.2f..%.2f, all hit %d\n", frames, moving,
                    double(moving ? minRate : 0.0f), double(maxRate), int(allHit));
        CHECK(frames == 30 && allHit);
        CHECK(moving > 0 && minRate >= 0.1f);
        CHECK(cl.anim.moveType == "idle");
    }

    // A bot's shot: the bot is brought 200 units from the standing client where the line is clear, facing it, and
    // fights; its own sight, aim and bullet do the rest.
    {
        const int b = bots[0];
        game::Entity* be = w.mpClientEntity(b);
        CHECK(be && be->obj->get("sessionstate").toString() == "playing");
        if (!be) return;
        w.mpBotSetCombat(b, true);
        const int hp0 = healthOf(h);
        bool damaged = false, killed = false;
        for (int i = 0; i < 300 && !killed; ++i) {
            if (i % 40 == 0 && be->obj->get("sessionstate").toString() == "playing") {
                const Vec3 t = originOf(h);
                for (int k = 0; k < 16; ++k) {
                    const float a = float(k) * float(M_PI) / 8.0f;
                    const Vec3 c{t[0] + std::cos(a) * 200.0f, t[1] + std::sin(a) * 200.0f, t[2]};
                    const auto tr = w.collision().trace({t[0], t[1], t[2] + 40}, {c[0], c[1], c[2] + 40}, {-16, -16, -16}, {16, 16, 16},
                                                        game::Contents::PlayerClipMask, h->number);
                    if (tr.fraction < 1 || tr.startSolid) continue;
                    be->obj->set("origin", script::Value(c));
                    be->obj->set("angles", script::Value(Vec3{0.0f, std::atan2(t[1] - c[1], t[0] - c[0]) * 180.0f / float(M_PI), 0.0f}));
                    break;
                }
            }
            step(50);
            damaged |= healthOf(h) < hp0 || h->obj->get("sessionstate").toString() != "playing";
            for (const auto& ob : w.mpState().obituaryLog) killed |= ob.victim == me && ob.attacker == b;
        }
        std::printf("bot shot: health %d -> %d, killed by the bot %d; %s\n", hp0, healthOf(h), int(killed), w.mpBotDescribe(b).c_str());
        CHECK(damaged);
        CHECK(killed);
    }
    CHECK(w.vm().errors().empty());
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cerr << "IW2_MAIN not set\n";
        return 1;
    }
    run(main);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpstillhit: ok\n";
    return failures ? 1 : 0;
}
