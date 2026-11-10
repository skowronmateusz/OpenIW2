// End to end, headless (docs/spec/net.md section 13): network players fight on a listen server. Two network clients
// and bots on mp_toujane (dm): a client shoots a bot dead with its usercmd buttons, a bot kills the other client, that
// client respawns with the use button and gets a weapon again; the other client sees the shooter's fire events in its
// snapshots. Then sd: a network client plants the bomb by holding use in a bomb zone, and a round restart (map_restart)
// keeps both clients connected and fighting. Needs IW2_MAIN.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/mpkillcam.h"
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

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
float health(Entity* e) {
    const Value v = e ? e->obj->get("health") : Value();
    return v.isNumber() ? v.asFloat() : -1;
}
game::Vec3 origin(Entity* e) { return e ? e->obj->get("origin").asVector() : game::Vec3{0, 0, 0}; }

int32_t angleShort(float deg) { return int32_t(std::lround(deg * 65536.0f / 360.0f)) & 0xffff; }

std::pair<int, std::string> hintOf(const net::GameClient& c);

// A listen server (host player 0 and its World) with network clients.
struct Game {
    World host;
    netgame::WorldGame game;
    net::Server server;
    std::vector<std::unique_ptr<net::GameClient>> clients;
    std::vector<net::UserCmd> cmds;  // the command each client sends every frame (serverTime filled in)
    int64_t now = 1000;

    Game(fs::Vfs& vfs, const char* gt) : host(vfs, "mp_toujane", true), game(nullptr, "", "", 7, 8), server(game, 5) {
        host.vm().setLoopGuardIterations(200000u);
        host.cvars["mapname"] = "mp_toujane";
        host.mpStart(gt);
        const int local = host.mpConnect("host");
        host.mpBegin(local);
        host.frame();
        game.setWorld(&host, "mp_toujane", gt, 7);
        game.worldOwnedByHost = true;
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = "mp_toujane";
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
    std::pair<int, std::string> hintOf(size_t i) const { return ::hintOf(*clients[i]); }
    bool obituary(int victim, int attacker) const {
        for (const auto& ob : host.mpState().obituaryLog)
            if (ob.victim == victim && (attacker < 0 || ob.attacker == attacker)) return true;
        return false;
    }
};

// The cursor hint of client i's newest snapshot: hintType (+0x598) and the hint string configstring (+0x59c).
std::pair<int, std::string> hintOf(const net::GameClient& c) {
    const net::ClientSnapshot* s = c.latest();
    if (!s || s->ps.size() < 1440) return {-1, ""};
    int32_t type = 0, idx = 0;
    std::memcpy(&type, s->ps.data() + 1432, 4);
    std::memcpy(&idx, s->ps.data() + 1436, 4);
    std::string text;
    if (idx >= 0) {
        auto it = c.configstrings.find(netgame::kHintConfigstrings + idx);
        if (it != c.configstrings.end()) text = it->second;
    }
    return {type & 0xff, text};
}

// A standing spot on the floor inside a bomb zone trigger (also for a zone the gametype removed).
bool zoneSpot(Game& g, Entity* zone, Entity* p, game::Vec3& spot) {
    game::Vec3 mn, mx;
    if (!g.host.submodelBounds(zone->obj->get("model").toString(), mn, mx)) return false;
    const Value ov = zone->obj->get("origin");
    const game::Vec3 zo = ov.isVector() ? ov.asVector() : game::Vec3{0, 0, 0};
    const float cx = zo[0] + (mn[0] + mx[0]) * 0.5f, cy = zo[1] + (mn[1] + mx[1]) * 0.5f;
    const auto down = g.host.collision().trace({cx, cy, zo[2] + mx[2]}, {cx, cy, zo[2] + mn[2] - 64}, {-15, -15, 0}, {15, 15, 70},
                                               game::Contents::PlayerClipMask, p->number);
    if (down.startSolid) return false;
    spot = {cx, cy, down.endPos[2] + 1};
    return true;
}

// A client's snapshot shows another player's fire event in that player's entity event ring.
bool sawFireEvent(const net::GameClient& c, int shooter) {
    const net::ClientSnapshot* s = c.latest();
    if (!s) return false;
    auto it = s->entities.find(shooter);
    if (it == s->entities.end()) return false;
    for (int i = 0; i < 4; ++i) {
        int32_t ev = 0;
        std::memcpy(&ev, it->second.data() + 168 + 4 * i, 4);
        if (ev == netgame::kEvFireWeapon || ev == netgame::kEvFireWeaponLastShot) return true;
    }
    return false;
}

void dm(fs::Vfs& vfs) {
    Game g(vfs, "dm");
    g.host.mpAutoJoin(0);
    if (!g.open()) {
        std::cerr << "cannot open a UDP port\n";
        ++failures;
        return;
    }
    const int bot1 = g.host.mpAddBot("bot1");
    g.frame();
    const int bot2 = g.host.mpAddBot("bot2");
    g.frame();
    g.host.mpBotSetCombat(bot1, false);
    g.host.mpBotSetCombat(bot2, false);
    const int a = g.connect("alpha");
    const int b = g.connect("bravo");
    CHECK(a > 0 && b > 0 && a != b);
    if (a <= 0 || b <= 0) return;
    // The serverinfo menu's limits come from the synced dvars of the gamestate (makeCvarServerInfo in dm.gsc).
    {
        const auto dv = netgame::RemoteView::syncedDvars(g.clients[0]->configstrings);
        const auto sl = dv.find("ui_dm_scorelimit"), tl = dv.find("ui_dm_timelimit");
        std::cout << "synced ui_dm_scorelimit '" << (sl == dv.end() ? "-" : sl->second) << "' ui_dm_timelimit '"
                  << (tl == dv.end() ? "-" : tl->second) << "'\n";
        CHECK(sl != dv.end() && !sl->second.empty());
        CHECK(tl != dv.end() && !tl->second.empty());
    }
    g.host.mpAutoJoin(a);
    g.host.mpAutoJoin(b);
    for (int i = 0; i < 100; ++i) g.frame();
    const netgame::NetCombat* ca = g.game.combat(g.slotOf(a));
    const netgame::NetCombat* cb = g.game.combat(g.slotOf(b));
    CHECK(ca && cb);
    if (!ca || !cb) return;
    CHECK(field(g.host.mpClientEntity(a), "sessionstate") == "playing");
    CHECK(ca->weapon().equipped());
    std::cout << "alpha " << a << " weapon " << ca->weapon().name() << " clip " << ca->weapon().clip() << "\n";

    // A weapon change by the command's weapon byte: alpha selects another owned weapon, the server puts it in hand and
    // keeps it (no switching back from the scripted $weapon), then alpha selects the first one again.
    {
        const std::string first = ca->weapon().name();
        std::string other;
        for (const std::string& wn : g.host.inventory(*g.host.mpClientEntity(a)).owned) {
            const auto* d = g.host.weapon(wn);
            const auto t = d ? d->find("weapontype") : decltype(d->end()){};
            if (wn != first && d && t != d->end() && t->second == "bullet") other = wn;
        }
        CHECK(!other.empty());
        std::cout << "switch " << first << " -> " << other << "\n";
        g.cmds[0].weapon = uint8_t(g.game.weaponByte(other));
        for (int i = 0; i < 60; ++i) g.frame();
        CHECK(ca->weapon().name() == other && field(g.host.mpClientEntity(a), "$weapon") == other);
        for (int i = 0; i < 40; ++i) g.frame();
        CHECK(ca->weapon().name() == other);
        g.cmds[0].weapon = uint8_t(g.game.weaponByte(first));
        for (int i = 0; i < 60; ++i) g.frame();
        CHECK(ca->weapon().name() == first);
    }

    // Bravo's front end: a remote World showing the snapshots; alpha's shots reach it as entity events.
    World view(vfs, "mp_toujane", true);
    view.mpStartRemote();
    netgame::RemoteView rv;
    int viewEffects = 0;
    // Alpha shoots bot1 dead: next to it, aiming at its chest, the attack button held.
    bool alphaKill = false, bravoSaw = false;
    for (int i = 0; i < 400 && !alphaKill; ++i) {
        Entity* bot = g.host.mpClientEntity(bot1);
        if (i % 20 == 0) g.placeNear(bot, g.host.mpClientEntity(a), 120);
        g.aim(0, {origin(bot)[0], origin(bot)[1], origin(bot)[2] + 40});
        g.cmds[0].buttons = int32_t((i / 2) % 2 == 0 ? game::Button::Attack : 0);  // semi-automatic weapons need presses
        g.frame();
        alphaKill = g.obituary(bot1, a);
        bravoSaw |= sawFireEvent(*g.clients[1], a);
        if (const net::ClientSnapshot* snap = g.clients[1]->latest()) {
            view.mpSetRemotePlayers(netgame::RemoteView::players(*snap, *snap, 1.0f, b));
            rv.playerEvents(view, *snap, g.clients[1]->configstrings, b);
            viewEffects += int(view.takeFxEvents().size() + view.takeTracerEvents().size());
        }
    }
    for (int i = 0; i < 4; ++i) {  // the last shots' snapshots
        g.frame();
        if (const net::ClientSnapshot* snap = g.clients[1]->latest()) {
            rv.playerEvents(view, *snap, g.clients[1]->configstrings, b);
            viewEffects += int(view.takeFxEvents().size() + view.takeTracerEvents().size());
        }
    }
    // The shooter's puppet in bravo's world holds alpha's weapon; the shots made impacts there (no damage: the server's).
    Entity* puppet = view.mpClientEntity(netgame::RemoteView::worldSlot(a, b));
    std::cout << "bravo view: puppet weapon " << field(puppet, "$weapon") << " effects " << viewEffects << "\n";
    CHECK(field(puppet, "$weapon") == ca->weapon().name());
    CHECK(viewEffects > 0);
    g.cmds[0].buttons = 0;
    std::cout << "alpha shots " << ca->shots() << " kill " << alphaKill << " bravo saw fire " << bravoSaw << "\n";
    CHECK(ca->shots() > 0);
    CHECK(alphaKill);
    CHECK(bravoSaw);

    // bot2 kills bravo.
    g.host.mpBotSetCombat(bot2, true);
    bool botKill = false;
    for (int i = 0; i < 1200 && !botKill; ++i) {
        if (i % 40 == 0 && field(g.host.mpClientEntity(b), "sessionstate") == "playing")
            g.placeNear(g.host.mpClientEntity(bot2), g.host.mpClientEntity(b), 150);
        g.frame();
        botKill = g.obituary(b, bot2);
    }
    // The eyes of bot2 in the archived frames around the kill (the killcam must replay them).
    std::vector<game::Vec3> killEyes;
    if (const game::MpArchive* ar = g.host.mpArchive())
        for (size_t k = ar->frames.size() > 4 ? ar->frames.size() - 4 : 0; k < ar->frames.size(); ++k)
            if (size_t(bot2) < ar->frames[k].clients.size() && ar->frames[k].clients[size_t(bot2)].active)
                killEyes.push_back(ar->frames[k].clients[size_t(bot2)].eye);
    // Bravo's own archived place in its last playing frames (the killcam shows its body there).
    std::vector<game::Vec3> victimSpots;
    if (const game::MpArchive* ar = g.host.mpArchive())
        for (size_t k = ar->frames.size() > 8 ? ar->frames.size() - 8 : 0; k < ar->frames.size(); ++k)
            if (size_t(b) < ar->frames[k].clients.size() && ar->frames[k].clients[size_t(b)].active) {
                const game::ArchivedFrame& fr = ar->frames[k];
                const game::ArchivedEntity* ae = fr.find(fr.clients[size_t(b)].entity);
                if (ae && ae->sessionstate == "playing") victimSpots.push_back(fr.clients[size_t(b)].origin);
            }
    for (int i = 0; i < 3; ++i) g.frame();
    std::cout << "bot kill " << botKill << "\n";
    CHECK(botKill);
    g.host.mpBotSetCombat(bot2, false);
    CHECK(!cb->weapon().equipped());  // dead: nothing in hand
    // Bravo's body in alpha's view (docs/spec/net.md section 11): the body model with its head and helmet, posed by the
    // death animation sent in legsAnim (not the bind pose).
    {
        World aview(vfs, "mp_toujane", true);
        aview.mpStartRemote();
        netgame::RemoteView arv;
        for (int i = 0; i < 3; ++i) g.frame();
        const net::ClientSnapshot* snap = g.clients[0]->latest();
        CHECK(snap != nullptr);
        if (snap) arv.apply(aview, *snap, g.clients[0]->configstrings, a, aview.timeMs());
        Entity* body = nullptr;
        for (const auto& e : aview.entities())
            if (e->classname == "script_model" && !e->obj->removed && e->obj->get("model").toString().rfind("xmodel/playerbody", 0) == 0)
                body = e.get();
        std::cout << "alpha view: body " << (body ? field(body, "model") : std::string("none")) << " attachments "
                  << (body ? body->attachments.size() : 0) << " anim " << (body && body->anim ? 1 : 0) << "\n";
        CHECK(body && !body->attachments.empty() && body->anim);
    }
    // Bravo's killcam over the network (docs/spec/mp-hud.md "Killcam"): the server sends bot2's archived player state
    // with the follow bit; bravo's camera is bot2's eye, and the replay reaches the frames of the kill.
    {
        int followSnaps = 0, killSeen = 0, ownSeen = 0, ownAtDeath = 0;
        float minDist = 1e9f;
        World kview(vfs, "mp_toujane", true);
        kview.mpStartRemote();
        netgame::RemoteView krv;
        std::string ownModel, ownWeapon;
        int kcShots = 0, kcEffects = 0, kcFireAnim = 0, kcAgeSteps = 0;
        int kcFirstItems = -1, kcLastItems = 0; // dropped weapons of the shown frames: the victim's drops only at its death
        // Bravo's body in the replay: none while its puppet is alive, then at the place of its death, in frames of the
        // archive even though the live body is gone by then (the queue of 8 reuses slots within a replay).
        int bodyWithPuppet = 0, bodyAfter = 0, bodyNew = 0, otherBodyNew = 0, otherBodies = 0;
        bool puppetGone = false;
        net::ClientSnapshot lastFollow;
        for (int i = 0; i < 260; ++i) {
            g.frame();
            const net::ClientSnapshot* s = g.clients[1]->latest();
            // In the replay this client's input drives no weapon of its own (the killcam view weapon is the followed one).
            if (s) CHECK(netgame::RemoteView::ownWeaponRuns(s) == !netgame::RemoteView::following(*s));
            if (s && netgame::RemoteView::following(*s)) CHECK(!netgame::RemoteView::ownWeaponRuns(s));
            if (!s || !netgame::RemoteView::following(*s)) {
                if (followSnaps > 0) {  // the killcam ended
                    // The newest snapshot is live, the drawn time still in the replay: the view stays on the last followed one.
                    float e0[3], a0[3], e1[3], a1[3];
                    CHECK(s && netgame::RemoteView::followView(lastFollow, *s, 0.5f, e1, a1));
                    netgame::RemoteView::followView(lastFollow, lastFollow, 1.0f, e0, a0);
                    CHECK(e0[0] == e1[0] && e0[1] == e1[1] && e0[2] == e1[2] && a0[1] == a1[1]);
                    const auto held = krv.killcamView(lastFollow, *s, 0.5f, g.clients[1]->configstrings, int64_t(i) * 50);
                    CHECK(held.active && !held.weapon.empty() && held.fireCount == kcShots);
                    CHECK(netgame::RemoteView::players(lastFollow, *s, 0.5f, b).size() == netgame::RemoteView::players(lastFollow, lastFollow, 0.5f, b).size());
                    break;
                }
                continue;
            }
            lastFollow = *s;
            if (++followSnaps == 1)
                for (const auto& e : g.host.entities())  // as a reuse of its slot would
                    if (e->classname == "playercorpse" && e->obj->get("$bodyclient").asInt() == b) e->obj->removed = true;
            for (const auto& [number, es] : s->entities) {
                int32_t type = 0, flags = 0, client = 0;
                std::memcpy(&type, es.data() + 4, 4);
                std::memcpy(&flags, es.data() + 8, 4);
                std::memcpy(&client, es.data() + 144, 4);
                if ((type & 0xff) != netgame::kEtCorpse) continue;
                const bool isNew = (flags & netgame::kEfBodyNew) != 0;
                if ((client & 63) != b) {
                    ++otherBodies;
                    // A body that really died within its death animation before the first replayed frame is still in its death animation
                    // there (the bots fight on, so that happens); only one older than that must not die again.
                    if (isNew && followSnaps == 1) {
                        const game::MpArchive* ar = g.host.mpArchive();
                        const Value at = g.host.mpClientEntity(b)->obj->get("archivetime");
                        int ms = int(std::trunc(at.asFloat() * 1000.0f));
                        const int64_t n = game::archiveLookup(*ar, ms, game::kMpServerFps);
                        if (const game::ArchivedFrame* f = ar->frame(n))
                            for (const game::ArchivedEntity& ae : f->entities)
                                if (ae.bodySlot >= 0 && ae.bodyClient == (client & 63) && f->timeMs >= ae.bodyThinkMs + 100)
                                    ++otherBodyNew;
                    }
                    continue;
                }
                if (s->entities.count(b)) ++bodyWithPuppet;
                else ++bodyAfter;
                bodyNew += isNew;
                float o[3];
                std::memcpy(o, es.data() + 24, 12);
                float best = 1e9f;
                for (const game::Vec3& v : victimSpots) best = std::min(best, std::hypot(o[0] - v[0], o[1] - v[1], o[2] - v[2]));
                CHECK(best < 1.0f);
            }
            puppetGone = puppetGone || (ownSeen > 0 && !s->entities.count(b));
            krv.apply(kview, *s, g.clients[1]->configstrings, b, kview.timeMs());
            // The killer's shots of the archived frames: the player state's event ring, played from its eye (impacts,
            // tracers) and timed for the view model's fire animation.
            {
                const int64_t nowMs = int64_t(i) * 50;
                krv.followEvents(kview, *s, g.clients[1]->configstrings, nowMs);
                kcEffects += int(kview.takeFxEvents().size() + kview.takeTracerEvents().size());
                const auto kv = krv.killcamView(*s, *s, 1.0f, g.clients[1]->configstrings, nowMs);
                CHECK(kv.active && !kv.weapon.empty());
                kcShots = kv.fireCount;
                if (kv.fireAgeMs == 0) {  // a shot of this snapshot starts the fire animation now
                    ++kcFireAnim;
                    // The age runs on the drawn frame's clock, not in server frames.
                    kcAgeSteps += krv.killcamView(*s, *s, 1.0f, g.clients[1]->configstrings, nowMs + 7).fireAgeMs == 7;
                }
            }
            {
                int items = 0;
                for (const auto& [num, es] : s->entities) { int t = 0; std::memcpy(&t, es.data() + 4, 4); items += t == netgame::kEtItem; }
                if (kcFirstItems < 0) kcFirstItems = items;
                kcLastItems = items;
            }
            float eye[3], ang[3];
            CHECK(netgame::RemoteView::followView(*s, *s, 1.0f, eye, ang));
            const netgame::PsFields f = netgame::readPs(s->ps);
            CHECK(f.pmType == 0);
            int32_t hp = 0, wn = 0;
            std::memcpy(&hp, s->ps.data() + 300, 4);
            std::memcpy(&wn, s->ps.data() + 212, 4);
            CHECK(hp > 0 && wn > 0);           // the killer's health and weapon
            CHECK(!s->entities.count(bot2));   // the followed bot is the camera, not an entity
            // Bravo's own body at its archived place (while it was playing then): an entity of the snapshot, a puppet
            // with its model and weapon in bravo's world.
            if (s->entities.count(b)) {
                ++ownSeen;
                kview.mpSetRemotePlayers(netgame::RemoteView::players(*s, *s, 1.0f, b, g.clients[1]->configstrings));
                krv.playerEvents(kview, *s, g.clients[1]->configstrings, b);
                if (Entity* own = kview.mpClientEntity(netgame::RemoteView::kOwnBodySlot)) {
                    ownModel = field(own, "model");
                    ownWeapon = field(own, "$weapon");
                    for (const game::Vec3& v : victimSpots)
                        if (std::hypot(origin(own)[0] - v[0], origin(own)[1] - v[1], origin(own)[2] - v[2]) < 1.0f) ++ownAtDeath;
                }
            }
            for (const game::Vec3& k : killEyes) {
                const float d = std::hypot(eye[0] - k[0], eye[1] - k[1], eye[2] - k[2]);
                minDist = std::min(minDist, d);
                if (d < 1.0f) ++killSeen;
            }
        }
        std::cout << "net killcam: " << followSnaps << " follow snapshots, kill frames seen " << killSeen << ", nearest eye "
                  << minDist << "\n";
        std::cout << "net killcam: own body in " << ownSeen << " snapshots, at its last places " << ownAtDeath << ", model "
                  << ownModel << " weapon " << ownWeapon << "\n";
        std::cout << "net killcam: killer shots " << kcShots << " effects " << kcEffects << " fire anims " << kcFireAnim << "\n";
        std::cout << "net killcam: own body with the puppet " << bodyWithPuppet << ", after it " << bodyAfter << ", new " << bodyNew
                  << ", other bodies " << otherBodies << " new at the start " << otherBodyNew << "\n";
        CHECK(kcFirstItems <= kcLastItems);
        CHECK(puppetGone);
        CHECK(bodyWithPuppet == 0);
        CHECK(bodyAfter >= 10);
        CHECK(bodyNew >= 1 && bodyNew <= bodyAfter);  // the flag lasts as long as the death animation, which may outlast the replay
        CHECK(otherBodyNew == 0);                    // a body older than the replay does not die again at its start
        CHECK(kcAgeSteps >= 1);
        CHECK(kcShots >= 1);
        CHECK(kcEffects >= 1);
        CHECK(kcFireAnim >= 1);
        CHECK(ownSeen >= 20);
        CHECK(ownAtDeath >= 1);
        CHECK(ownModel.rfind("xmodel/playerbody", 0) == 0);
        CHECK(!ownWeapon.empty());
        CHECK(followSnaps >= 60);  // about 7 s of 20 snapshots a second (the client's snapshot rate)
        CHECK(killSeen >= 1);
        // After the killcam bravo is no longer following.
        const net::ClientSnapshot* s = g.clients[1]->latest();
        CHECK(s && !netgame::RemoteView::following(*s));
        CHECK(netgame::RemoteView::ownWeaponRuns(s));
        CHECK(!netgame::RemoteView::ownWeaponRuns(nullptr));
    }
    CHECK(netgame::sayCommand(false, "hi there") == "say \x15hi there");
    CHECK(netgame::sayCommand(true, "go") == "say_team \x15go");

    // Bravo respawns with the use button and holds a weapon again.
    bool back = false;
    for (int i = 0; i < 300 && !back; ++i) {
        g.cmds[1].buttons = int32_t((i % 4) < 2 ? game::Button::Activate : 0);
        g.frame();
        back = field(g.host.mpClientEntity(b), "sessionstate") == "playing" && health(g.host.mpClientEntity(b)) > 0 &&
               cb->weapon().equipped();
    }
    g.cmds[1].buttons = 0;
    std::cout << "respawn " << back << "\n";
    CHECK(back);

    // A team change from the in-game team menu: the menu response of a network client (mr <serverId> <menu> <response>).
    {
        int teamMenu = -1;
        for (int i = netgame::kMenuConfigstrings; g.clients[0]->configstrings.count(i); ++i)
            if (g.clients[0]->configstrings[i].rfind("team_", 0) == 0) teamMenu = i - netgame::kMenuConfigstrings;
        CHECK(teamMenu >= 0);
        g.clients[0]->addReliableCommand("mr " + std::to_string(g.clients[0]->serverId) + " " + std::to_string(teamMenu) + " spectator");
        for (int i = 0; i < 20; ++i) g.frame();
        std::cout << "team change: alpha " << field(g.host.mpClientEntity(a), "sessionteam") << " "
                  << field(g.host.mpClientEntity(a), "sessionstate") << "\n";
        CHECK(field(g.host.mpClientEntity(a), "sessionteam") == "spectator");
        CHECK(field(g.host.mpClientEntity(a), "sessionstate") == "spectator");
        g.clients[0]->addReliableCommand("mr " + std::to_string(g.clients[0]->serverId) + " " + std::to_string(teamMenu) + " autoassign");
        for (int i = 0; i < 20; ++i) g.frame();
        // autoassign opens the side's weapon menu; the team is set when a weapon is chosen.
        const std::string wm = g.host.mpState().clients[size_t(a)].openMenu;
        std::cout << "team change: weapon menu " << wm << "\n";
        CHECK(wm.rfind("weapon_", 0) == 0);
        g.clients[0]->addReliableCommand("mr " + std::to_string(g.clients[0]->serverId) + " " + wm + " " +
                                         (wm == "weapon_german" ? "mp40_mp" : "thompson_mp"));
        for (int i = 0; i < 20; ++i) g.frame();
        std::cout << "team change: alpha " << field(g.host.mpClientEntity(a), "sessionteam") << "\n";
        CHECK(field(g.host.mpClientEntity(a), "sessionteam") != "spectator");
    }
    // A kick (rcon kick / clientkick drops the slot): the client is disconnected, and connects again.
    {
        const int slot = g.slotOf(b);
        CHECK(slot >= 0);
        g.server.dropClient(slot, "EXE_PLAYERKICKED");
        for (int i = 0; i < 10; ++i) g.frame();
        std::cout << "kick: bravo state " << int(g.clients[1]->state()) << " error '" << g.clients[1]->error << "'\n";
        CHECK(g.clients[1]->state() == net::ClientState::Disconnected);
        CHECK(g.clients[1]->error == "EXE_PLAYERKICKED");
        CHECK(g.slotOf(b) < 0);
        const int again = g.connect("bravo");
        std::cout << "kick: reconnected as " << again << "\n";
        CHECK(again > 0);
        if (again > 0) {
            g.host.mpAutoJoin(again);
            bool playing = false;
            for (int i = 0; i < 100 && !playing; ++i) {
                g.frame();
                playing = field(g.host.mpClientEntity(again), "sessionstate") == "playing";
            }
            CHECK(playing);
        }
    }
    for (const auto& e : g.host.vm().errors()) std::cerr << "dm script error: " << e << "\n";
}

void sd(fs::Vfs& vfs) {
    Game g(vfs, "sd");
    g.host.cvars["scr_sd_roundlength"] = "2";
    if (!g.open()) {
        ++failures;
        return;
    }
    const int a = g.connect("alpha");
    const int b = g.connect("bravo");
    CHECK(a > 0 && b > 0);
    if (a <= 0 || b <= 0) return;
    const std::string attackers = g.host.vm().game()->get("attackers").toString();
    const std::string defenders = attackers == "allies" ? "axis" : "allies";
    auto join = [&](int client, const std::string& team) {
        g.host.mpMenuResponse(client, g.host.vm().game()->get("menu_team").toString(), team);
        g.frame();
        const std::string menu = g.host.mpState().clients[size_t(client)].openMenu;
        g.host.mpMenuResponse(client, menu, menu == "weapon_german" ? "mp40_mp" : menu == "weapon_russian" ? "ppsh_mp" : "thompson_mp");
        g.frame();
    };
    join(0, defenders);
    join(a, attackers);
    join(b, defenders);
    // The first round is a reset round once both teams have players: map_restart with the clients connected.
    for (int i = 0; i < 400 && g.host.mpRestarts() == 0; ++i) g.frame();
    CHECK(g.host.mpRestarts() == 1);
    for (int i = 0; i < 60; ++i) g.frame();
    CHECK(field(g.host.mpClientEntity(a), "sessionstate") == "playing");
    CHECK(field(g.host.mpClientEntity(b), "sessionstate") == "playing");
    const netgame::NetCombat* ca = g.game.combat(g.slotOf(a));
    CHECK(ca && ca->weapon().equipped());

    // Alpha plants: in a bomb zone of the bomb mode (each zone in turn), the use button held for the plant time.
    std::vector<Entity*> zones;
    for (const auto& e : g.host.entities())
        if (!e->obj->removed && e->obj->get("targetname").toString() == "bombzone" && e->classname.rfind("trigger", 0) == 0)
            zones.push_back(e.get());
    CHECK(!zones.empty());
    bool planted = false;
    for (Entity* zone : zones) {
        game::Vec3 mn, mx;
        if (!g.host.submodelBounds(zone->obj->get("model").toString(), mn, mx)) continue;
        const game::Vec3 zo = origin(zone);
        // The floor under the zone's centre: a player box traced down from the top of the zone.
        const float cx = zo[0] + (mn[0] + mx[0]) * 0.5f, cy = zo[1] + (mn[1] + mx[1]) * 0.5f;
        const auto down = g.host.collision().trace({cx, cy, zo[2] + mx[2]}, {cx, cy, zo[2] + mn[2] - 64}, {-15, -15, 0}, {15, 15, 70},
                                                   game::Contents::PlayerClipMask, g.host.mpClientEntity(a)->number);
        if (down.startSolid) continue;
        const game::Vec3 spot{cx, cy, down.endPos[2] + 1};
        g.host.mpClientEntity(a)->obj->set("origin", Value(spot));
        for (int i = 0; i < 160 && !planted; ++i) {
            g.cmds[0].buttons = int32_t(i < 20 ? 0 : game::Button::Activate);  // land first: the plant needs isOnGround
            g.frame();
            if (i == 19) {  // standing in the zone: the server's use search gives the zone's hint (sd.gsc sets it)
                const auto h = g.hintOf(0);
                std::cout << "sd zone hint " << h.first << " '" << h.second << "'\n";
                CHECK(h.first == 2);
                CHECK(h.second == "PLATFORM_HOLD_TO_PLANT_EXPLOSIVES");
                // Bravo (a defender, the zone is the attackers') gets no hint anywhere near it.
                CHECK(g.hintOf(1).first == 0);
            }
            planted = g.host.vm().level()->get("bombplanted").truthy();
        }
        if (planted) break;
    }
    g.cmds[0].buttons = 0;
    std::cout << "planted " << planted << "\n";
    CHECK(planted);

    // The round ends (the defenders are killed) and restarts with both clients still connected and able to fire.
    g.host.mpDebugDamage(0, a, 500, "MOD_RIFLE_BULLET");
    g.host.mpDebugDamage(b, a, 500, "MOD_RIFLE_BULLET");
    const int restarts = g.host.mpRestarts();
    for (int i = 0; i < 600 && g.host.mpRestarts() == restarts; ++i) g.frame();
    CHECK(g.host.mpRestarts() == restarts + 1);
    for (int i = 0; i < 80; ++i) g.frame();
    CHECK(g.clients[0]->state() == net::ClientState::Active && g.clients[1]->state() == net::ClientState::Active);
    CHECK(field(g.host.mpClientEntity(a), "sessionstate") == "playing");
    CHECK(field(g.host.mpClientEntity(b), "sessionstate") == "playing");
    ca = g.game.combat(g.slotOf(a));
    const int before = ca ? ca->shots() : 0;
    for (int i = 0; i < 60; ++i) {
        g.cmds[0].buttons = int32_t((i / 2) % 2 == 0 ? game::Button::Attack : 0);
        g.frame();
    }
    g.cmds[0].buttons = 0;
    std::cout << "shots after restart " << (ca ? ca->shots() - before : -1) << "\n";
    CHECK(ca && ca->shots() > before);
    for (const auto& e : g.host.vm().errors()) std::cerr << "sd script error: " << e << "\n";
}

// Use hints of a network client in dm on the same map: no hint in a bomb zone (dm removes the sd triggers), a weapon
// on the ground gives its weapon hint (weapon number + 4, the client side shows the pickup prompt), and so does a
// turret seen from behind with its useHintString.
void dmHints(fs::Vfs& vfs) {
    Game g(vfs, "dm");
    g.host.mpAutoJoin(0);
    if (!g.open()) {
        ++failures;
        return;
    }
    const int a = g.connect("alpha");
    CHECK(a > 0);
    if (a <= 0) return;
    g.host.mpAutoJoin(a);
    for (int i = 0; i < 100; ++i) g.frame();
    Entity* pa = g.host.mpClientEntity(a);
    CHECK(field(pa, "sessionstate") == "playing");
    if (!pa) return;
    int zonesTried = 0;
    for (const auto& e : g.host.entities()) {
        if (e->obj->get("targetname").toString() != "bombzone" || e->classname.rfind("trigger", 0) != 0) continue;
        game::Vec3 spot;
        if (!zoneSpot(g, e.get(), pa, spot)) continue;
        pa->obj->set("origin", Value(spot));
        for (int i = 0; i < 25; ++i) g.frame();
        ++zonesTried;
        CHECK(g.hintOf(0).first == 0);
    }
    std::cout << "dm bomb zones tried " << zonesTried << "\n";
    CHECK(zonesTried > 0);

    // A weapon item alpha does not own, in front of it.
    std::string item;
    for (const char* w : {"kar98k_mp", "m1garand_mp", "mosin_nagant_mp", "enfield_mp", "springfield_mp"})
        if (g.host.weapon(w) && !g.host.inventory(*pa).owned.count(w)) { item = w; break; }
    CHECK(!item.empty());
    if (!item.empty()) {
        for (int i = 0; i < 10; ++i) g.frame();
        const game::Vec3 o = origin(pa);
        // The host's random spawn can be next to the spot, and it would touch the items first.
        g.host.mpClientEntity(0)->obj->set("origin", Value(game::Vec3{o[0] + 4000, o[1], o[2]}));
        g.host.debugDropItem(item, {o[0] + 30, o[1], o[2] + 8}, {0, 0, 0});  // falls in front of alpha
        bool seen = false;
        for (int i = 0; i < 20 && !seen; ++i) {
            g.aim(0, {o[0] + 30, o[1], o[2] + 2});
            g.frame();
            const auto h = g.hintOf(0);
            seen = h.first > 4 && netgame::weaponNameFromList(g.clients[0]->configstrings, h.first - 4) == item;
        }
        std::cout << "weapon item hint " << g.hintOf(0).first << "\n";
        CHECK(seen);
        // Alpha's front end shows it as a weapon hint with the pickup prompt.
        World view(vfs, "mp_toujane", true);
        view.mpStartRemote();
        const auto h = g.hintOf(0);
        view.mpSetRemoteUseHint(h.first, h.second, netgame::weaponNameFromList(g.clients[0]->configstrings, h.first - 4));
        view.frame();
        CHECK(view.playerUseHint().weapon == item);
        CHECK(!view.playerUseHint().text.empty());
        // One press of use takes the hinted item; a second press takes the dropped weapon back. The front end's world, fed by
        // RemoteView::apply from each newest snapshot, shows the prompt on every frame with a weapon hint for a weapon the player
        // does not hold: the icon and the text come and go together.
        {
            netgame::RemoteView srv;
            int shown = 0, bad = 0, swaps = 0;
            std::string held = field(pa, "$weapon"), start = held;
            const auto step = [&](bool press) {
                g.cmds[0].buttons = int32_t(press ? game::Button::Activate : 0);
                g.aim(0, {o[0] + 30, o[1], o[2] + 2});
                g.frame();
                const net::ClientSnapshot* snap = g.clients[0]->latest();
                if (!snap) return;
                srv.apply(view, *snap, g.clients[0]->configstrings, g.clients[0]->clientNum, view.timeMs());
                view.frame();
                const auto& uh = view.playerUseHint();
                if (!uh.weapon.empty()) {
                    ++shown;
                    int32_t psWeapon = 0;
                    std::memcpy(&psWeapon, snap->ps.data() + 212, 4);
                    const std::string cur = netgame::weaponNameFromList(g.clients[0]->configstrings, psWeapon);
                    if (uh.text.empty() && uh.weapon != cur) {
                        ++bad;
                        std::cout << "hint without prompt: weapon " << uh.weapon << " current " << cur << "\n";
                    }
                }
                if (!uh.text.empty() && uh.weapon.empty()) ++bad;  // a prompt without its icon
                const std::string now = field(pa, "$weapon");
                if (now != held) ++swaps, held = now;
            };
            for (int i = 0; i < 2; ++i) step(true);  // down for two server frames, then up
            for (int i = 0; i < 20; ++i) step(false);
            CHECK(g.host.inventory(*pa).owned.count(item) == 1);
            CHECK(held == item);
            for (int i = 0; i < 20; ++i) step(false);  // the hint is the dropped weapon now
            for (int i = 0; i < 2; ++i) step(true);
            for (int i = 0; i < 20; ++i) step(false);
            g.cmds[0].buttons = 0;
            std::cout << "use presses: swaps " << swaps << ", hint frames " << shown << ", without prompt " << bad << ", weapon " << start << " -> " << held << "\n";
            CHECK(swaps == 2);
            CHECK(held == start);
            CHECK(shown > 0);
            CHECK(bad == 0);
        }
        // Out of touch range: the press takes what the use search picked, as the hint promised.
        for (const auto& e : g.host.entities())
            if (!e->obj->removed && e->classname.rfind("weapon_", 0) == 0) e->obj->removed = true;
        std::string far;
        for (const char* w : {"m1garand_mp", "mosin_nagant_mp", "enfield_mp", "springfield_mp"})
            if (g.host.weapon(w) && !g.host.inventory(*pa).owned.count(w)) { far = w; break; }
        CHECK(!far.empty());
        g.host.debugDropItem(far, {o[0] + 40, o[1], o[2] + 8}, {0, 0, 0});
        for (int i = 0; i < 20; ++i) g.frame();  // it lands first: the hint of a falling item says nothing about the item at rest
        bool farSeen = false;
        for (int i = 0; i < 20 && !farSeen; ++i) {
            g.aim(0, {o[0] + 40, o[1], o[2] + 2});
            g.frame();
            const auto fh = g.hintOf(0);
            farSeen = fh.first > 4 && netgame::weaponNameFromList(g.clients[0]->configstrings, fh.first - 4) == far;
        }
        CHECK(farSeen);
        for (int i = 0; i < 20 && !g.host.inventory(*pa).owned.count(far); ++i) { // press only while hinted (a press needs the hint of a frame before)
            const auto fh = g.hintOf(0);
            const bool hinted = fh.first > 4 && netgame::weaponNameFromList(g.clients[0]->configstrings, fh.first - 4) == far;
            g.cmds[0].buttons = int32_t(hinted && (i % 2) == 0 ? game::Button::Activate : 0);
            g.aim(0, {o[0] + 40, o[1], o[2] + 2});
            g.frame();
        }
        g.cmds[0].buttons = 0;
        for (int i = 0; i < 3; ++i) g.frame();
        std::cout << "use press out of touch range: weapon " << field(pa, "$weapon") << "\n";
        CHECK(g.host.inventory(*pa).owned.count(far) == 1);
    }

    // A turret: alpha stands behind it (one of the spots around it whose direction is inside the arc).
    Entity* turret = nullptr;
    for (const auto& e : g.host.entities())
        if (!e->obj->removed && e->turret) { turret = e.get(); break; }
    std::cout << "turret " << (turret ? turret->classname : std::string("none")) << "\n";
    CHECK(turret);
    if (turret) {
        bool seen = false;
        const std::string tw = turret->turret->weapon;
        for (int k = 0; k < 16 && !seen; ++k) {
            if (!g.placeNear(pa, turret, 40)) break;
            const game::Vec3 t = origin(turret);
            const float ang = float(k) * float(M_PI) / 8.0f;
            pa->obj->set("origin", Value(game::Vec3{t[0] + std::cos(ang) * 40, t[1] + std::sin(ang) * 40, t[2]}));
            for (int i = 0; i < 8 && !seen; ++i) {
                g.aim(0, {t[0], t[1], t[2] + 40});
                g.frame();
                const auto h = g.hintOf(0);
                seen = h.first > 4 && netgame::weaponNameFromList(g.clients[0]->configstrings, h.first - 4) == tw;
                if (seen) {
                    std::cout << "turret hint " << h.first << " '" << h.second << "'\n";
                    const auto* d = g.host.weapon(tw);
                    const auto u = d ? d->find("usehintstring") : decltype(d->end()){};
                    if (d && u != d->end() && !u->second.empty()) CHECK(h.second == u->second);
                }
            }
        }
        CHECK(seen);
        // A press of use gets on the picked turret; on it the hint is the weapon's drop hint.
        const game::Vec3 tp = origin(turret);
        for (int i = 0; i < 40 && !g.host.mpClientOnTurret(a); ++i) {
            if (g.host.mpState().clients[size_t(a)].useEntity == turret->number) g.cmds[0].buttons = int32_t((i % 2) ? game::Button::Activate : 0);
            else g.cmds[0].buttons = 0;
            g.aim(0, {tp[0], tp[1], tp[2] + 40});
            g.frame();
        }
        g.cmds[0].buttons = 0;
        for (int i = 0; i < 4; ++i) g.frame();
        std::cout << "use entity " << g.host.mpState().clients[size_t(a)].useEntity << " turret " << turret->number << "\n";
        CHECK(g.host.mpClientOnTurret(a));
        const auto* d = g.host.weapon(tw);
        const auto dh = d ? d->find("drophintstring") : decltype(d->end()){};
        const auto h = g.hintOf(0);
        std::cout << "on turret " << g.host.mpClientOnTurret(a) << " hint " << h.first << " '" << h.second << "'\n";
        if (d && dh != d->end() && !dh->second.empty()) CHECK(h.first > 4 && h.second == dh->second);
        // The turret holds the client behind the gun while he turns, and puts him down on release: his movement
        // state goes on from there (no cleared player state outside a spawn).
        const int clears = g.game.stateClears;
        const game::Vec3 before = origin(g.host.mpClientEntity(a));
        for (int i = 0; i < 20; ++i) {
            g.cmds[0].buttons = 0;
            g.cmds[0].angles[1] = int32_t(std::lround((10.0f + float(i)) * 65536.0f / 360.0f)) & 0xffff;
            g.frame();
        }
        const game::Vec3 after = origin(g.host.mpClientEntity(a));
        std::cout << "turret turn moved " << std::fabs(after[0] - before[0]) + std::fabs(after[1] - before[1]) << " clears " << g.game.stateClears - clears << "\n";
        CHECK(g.game.stateClears == clears);
        for (int i = 0; i < 40 && g.host.mpClientOnTurret(a); ++i) {
            g.cmds[0].buttons = int32_t((i % 2) ? game::Button::Activate : 0);
            g.frame();
        }
        g.cmds[0].buttons = 0;
        for (int i = 0; i < 5; ++i) g.frame();
        CHECK(!g.host.mpClientOnTurret(a));
        CHECK(g.game.stateClears == clears);
    }
    for (const auto& e : g.host.vm().errors()) std::cerr << "dm hints script error: " << e << "\n";
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
    dm(vfs);
    sd(vfs);
    dmHints(vfs);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mpnetcombat: ok\n";
    return failures ? 1 : 0;
}
