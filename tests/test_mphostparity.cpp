// The same scripted steps run twice on the server side: once for the listen host's own player (client 0 of a World with
// a local player) and once for a network client of a dedicated server (client 0, no local player, real usercmds through
// net::Server and netgame::WorldGame). After each step the server-side outcome of both is compared. Every difference
// is a bug or one of the network differences listed in `kJustified` (docs/spec/mp.md "Per-client player state").
// Headless on mp_toujane (dm). Needs IW2_MAIN.
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/mpgame.h"
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
using game::Entity;
using game::World;
using script::Value;

Value call(World& w, const std::string& fn, std::vector<Value> args, Value self) {
    script::Thread t;
    script::CallContext c{w.vm(), t, self, std::span<Value>(args)};
    return w.vm().builtin(fn)(c);
}

// One side of the comparison: a server World with the tested client and a way to run frames with its input.
struct Side {
    virtual ~Side() = default;
    virtual World& world() = 0;
    virtual int client() const = 0;
    // Runs `frames` server frames with these buttons (game::Button bits) held and the view pitched down by `pitch`.
    virtual void run(int frames, uint32_t buttons, float pitch) = 0;
    // Sound commands (s <index>) this client received since the last call; the host plays them itself.
    virtual std::vector<std::string> takeSounds() = 0;
    Entity* ent() { return world().mpClientEntity(client()); }
};

struct Host : Side {
    World w;
    explicit Host(fs::Vfs& vfs) : w(vfs, "mp_toujane", true) {
        w.vm().setLoopGuardIterations(200000u);
        w.cvars["mapname"] = "mp_toujane";
        w.mpStart("dm");
        const int c = w.mpConnect("alpha");
        w.mpBegin(c);
        w.frame();
        w.mpAutoJoin(c);
    }
    World& world() override { return w; }
    int client() const override { return 0; }
    void run(int frames, uint32_t buttons, float pitch) override {
        for (int i = 0; i < frames; ++i) {
            World::PlayerCommand pc;
            pc.buttons = buttons;
            pc.viewAngles = {pitch, 0, 0};
            pc.stance = (buttons & uint32_t(game::Button::Prone)) ? 2 : (buttons & uint32_t(game::Button::Crouch)) ? 1 : 0;
            w.setPlayerCommand(pc);
            w.frame();
        }
    }
    std::vector<std::string> takeSounds() override { return {}; }
};

struct Dedicated : Side {
    World w;
    netgame::WorldGame game;
    net::Server server;
    std::unique_ptr<net::GameClient> cl;
    std::vector<std::string> sounds;
    int64_t now = 1000;
    int num = -1;
    explicit Dedicated(fs::Vfs& vfs) : w(vfs, "mp_toujane", true), game(nullptr, "", "", 7, 8), server(game, 5) {
        w.vm().setLoopGuardIterations(200000u);
        w.cvars["mapname"] = "mp_toujane";
        w.mpSetDedicated();
        w.mpStart("dm");
        game.setWorld(&w, "mp_toujane", "dm", 7);
        server.serverId = 7;
        game.sendCommand = [this](int slot, const std::string& text) { server.sendServerCommand(slot, text); };
        server.info().mapname = "mp_toujane";
        server.info().gametype = "dm";
        if (!server.open(0)) return;
        cl = std::make_unique<net::GameClient>();
        cl->name = "alpha";
        net::NetAddress a;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(server.port()), a, 28960);
        cl->connect(a, now);
        for (int i = 0; i < 200 && cl->state() != net::ClientState::Active; ++i) run(1, 0, 0);
        if (cl->state() == net::ClientState::Active) num = cl->clientNum;
        if (num == 0) w.mpAutoJoin(0);
    }
    World& world() override { return w; }
    int client() const override { return num; }
    void run(int frames, uint32_t buttons, float pitch) override {
        for (int i = 0; i < frames; ++i) {
            cl->poll(now, 1);
            if (cl->state() >= net::ClientState::Primed) {
                net::UserCmd c;
                c.serverTime = int(now);
                c.buttons = int32_t(buttons);
                c.angles[0] = int32_t(pitch * 65536.0f / 360.0f) & 0xffff;
                cl->sendCommand(c, now);
            }
            for (const std::string& sc : cl->takeServerCommands())
                if (sc.rfind("s ", 0) == 0) { // the alias the client resolves from the sound configstrings ("" when unknown)
                    const auto it = cl->configstrings.find(netgame::kSoundConfigstrings - 1 + std::atoi(sc.c_str() + 2));
                    sounds.push_back(it == cl->configstrings.end() ? std::string() : it->second);
                }
            server.poll(now, 0);
            now += 50;
        }
    }
    std::vector<std::string> takeSounds() override {
        run(2, 0, 0); // the reliable command reaches the client
        auto out = sounds;
        sounds.clear();
        return out;
    }
};

// Server-side outcome of the tested client after a step.
struct Probe {
    std::map<std::string, std::string> f;
    static Probe of(Side& s) {
        Probe p;
        Entity* e = s.ent();
        if (!e) return p;
        World& w = s.world();
        p.f["sessionstate"] = e->obj->get("sessionstate").toString();
        p.f["health"] = e->obj->get("health").toString();
        std::string owned;
        for (const auto& o : w.inventory(*e).owned) owned += o + " ";
        p.f["owned"] = owned;
        const game::MpClient& c = w.mpState().clients[size_t(s.client())];
        p.f["damageTimer"] = std::to_string(c.damageTimerMs);
        p.f["spawnCount"] = std::to_string(c.spawnCount);
        return p;
    }
};

void compare(const std::string& step, const Probe& a, const Probe& b, const std::set<std::string>& justified = {}) {
    for (const auto& [k, v] : a.f) {
        const auto it = b.f.find(k);
        const std::string bv = it == b.f.end() ? "<none>" : it->second;
        if (v == bv || justified.count(k)) continue;
        std::cerr << "step " << step << ": " << k << " host '" << v << "' network '" << bv << "'\n";
        ++failures;
    }
}

// The steps, the same for both sides; per-step checks of the absolute outcome are inside.
struct Outcome {
    std::vector<Probe> probes;
    bool crouchTouched = false, standTouched = false;
    size_t sounds = 0, localSoundsLogged = 0;
    std::string soundAliases; // the aliases of the sound commands as the client resolves them
};

Outcome scenario(Side& s, const char* name) {
    Outcome out;
    World& w = s.world();
    s.run(40, 0, 0);
    Entity* p = s.ent();
    CHECK(p && p->obj->get("sessionstate").toString() == "playing");
    if (!p) return out;
    out.probes.push_back(Probe::of(s)); // spawned
    // A trigger_radius whose base is 55 above the feet: the crouched box (50) misses it, the standing box (70) touches it.
    s.run(30, uint32_t(game::Button::Crouch), 0);
    {
        const game::Vec3 o = p->obj->get("origin").asVector();
        Entity& t = w.debugSpawn("trigger_radius");
        t.obj->set("origin", Value(script::Vec3{o[0], o[1], o[2] + 55}));
        t.obj->set("radius", Value(32.0f));
        t.obj->set("height", Value(10.0f));
        int hits = 0;
        const script::Object* to = t.obj.get();
        w.vm().notifyHook = [&](const script::ObjectPtr& obj, const std::string& n) {
            if (obj.get() == to && n == "trigger") ++hits;
        };
        s.run(3, uint32_t(game::Button::Crouch), 0);
        out.crouchTouched = hits > 0;
        s.run(30, 0, 0);
        out.standTouched = hits > 0;
        w.vm().notifyHook = nullptr;
        t.obj->set("origin", Value(script::Vec3{o[0], o[1], o[2] - 5000}));
    }
    out.probes.push_back(Probe::of(s)); // crouched
    // Damage from the world, then playLocalSound on the client (the hit alert of _damagefeedback.gsc).
    call(w, "finishplayerdamage", {Value(), Value(), Value(int32_t(30)), Value(int32_t(0)), Value("MOD_PISTOL_BULLET"), Value("colt_mp"),
                                   Value(), Value(script::Vec3{1, 0, 0}), Value("torso_upper"), Value(int32_t(0))},
         Value(p->obj));
    out.probes.push_back(Probe::of(s)); // shot
    const size_t soundsBefore = w.mpState().localSounds.size();
    call(w, "playlocalsound", {Value("MP_hit_alert")}, Value(p->obj));
    out.localSoundsLogged = w.mpState().localSounds.size() - soundsBefore;
    const std::vector<std::string> played = s.takeSounds();
    out.sounds = played.size();
    for (const std::string& a : played) out.soundAliases += a;
    // Pick up a weapon item lying in view with a use press.
    std::string item;
    for (const char* wn : {"kar98k_mp", "m1garand_mp", "mosin_nagant_mp", "enfield_mp", "springfield_mp"})
        if (w.weapon(wn) && !w.inventory(*p).owned.count(wn)) { item = wn; break; }
    CHECK(!item.empty());
    s.run(10, 0, 30);
    const game::Vec3 o = p->obj->get("origin").asVector();
    w.debugDropItem(item, {o[0] + 30, o[1], o[2] + 8}, {0, 0, 0});
    for (int i = 0; i < 30 && !w.inventory(*p).owned.count(item); ++i) s.run(1, i % 4 < 2 ? uint32_t(game::Button::Activate) : 0, 30);
    s.run(20, 0, 0); // the damage timer runs out on both
    out.probes.push_back(Probe::of(s)); // picked up
    // Death and respawn.
    call(w, "finishplayerdamage", {Value(), Value(), Value(int32_t(1000)), Value(int32_t(0)), Value("MOD_SUICIDE"), Value("none"),
                                   Value(), Value(), Value("none"), Value(int32_t(0))},
         Value(p->obj));
    s.run(2, 0, 0);
    out.probes.push_back(Probe::of(s)); // dead
    for (int i = 0; i < 200 && s.ent()->obj->get("sessionstate").toString() != "playing"; ++i)
        s.run(1, i % 4 < 2 ? uint32_t(game::Button::Activate) : 0, 0);
    out.probes.push_back(Probe::of(s)); // respawned
    std::cout << name << ": " << out.probes.size() << " probes, sounds " << out.sounds << "\n";
    return out;
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
    Host host(vfs);
    Dedicated ded(vfs);
    CHECK(ded.client() == 0 && ded.w.mpLocalClient() == -1);
    if (ded.client() != 0) return 1;
    const Outcome h = scenario(host, "host");
    const Outcome n = scenario(ded, "network");
    const char* steps[] = {"spawn", "crouch", "shot", "pickup", "dead", "respawn"};
    CHECK(h.probes.size() == n.probes.size());
    for (size_t i = 0; i < h.probes.size() && i < n.probes.size(); ++i) compare(steps[i], h.probes[i], n.probes[i]);
    // playLocalSound: the host plays it itself; the network client gets one sound command (docs/spec/mp-hud.md).
    CHECK(h.localSoundsLogged == 0);
    // Touch triggers use the stance box for both.
    CHECK(!h.crouchTouched && h.standTouched);
    CHECK(!n.crouchTouched && n.standTouched);
    CHECK(n.localSoundsLogged == 1 && n.sounds == 1);
    // The alias was first registered by this playLocalSound: its configstring reached the client before the command.
    CHECK(n.soundAliases == "MP_hit_alert");
    for (const auto& e : host.w.vm().errors()) std::cerr << "host script error: " << e << "\n";
    for (const auto& e : ded.w.vm().errors()) std::cerr << "network script error: " << e << "\n";
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "mphostparity: ok\n";
    return failures ? 1 : 0;
}
