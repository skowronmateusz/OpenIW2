// The entity events of a player's weapon state changes (docs/spec/net.md section 13 "Entity events"): reload, rechamber
// (the bolt of a bolt-action rifle), raise and put away go into the player's event ring, so other players and the killcam
// hear them. Needs IW2_MAIN.
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "fs/vfs.h"
#include "game/clientinput.h"
#include "game/mpkillcam.h"
#include "game/playerweapon.h"
#include "game/world.h"
#include "netgame/netcombat.h"

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
using game::PlayerWeapon;
using script::Value;

std::vector<int> stateEvents(PlayerWeapon& w) {
    std::vector<int> out;
    for (const auto& e : w.takeEvents())
        if (e.kind == PlayerWeapon::Event::StateEvent) out.push_back(e.code);
    return out;
}
bool has(const std::vector<int>& v, int code) {
    for (int c : v)
        if (c == code) return true;
    return false;
}
std::vector<int> ringEvents(const game::MpEventRing& r, int fromSeq) {
    std::vector<int> out;
    for (int s = fromSeq; s != r.sequence; s = (s + 1) & 0xff) out.push_back(r.events[size_t(s & 3)]);
    return out;
}

} // namespace

int main() {
    const char* mainDir = std::getenv("IW2_MAIN");
    if (!mainDir) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);

    // The weapon itself: a kar98k raise (0x9a), the shot's bolt (0xa1), the reload (0x96 / 0x97), the drop (0x9b).
    {
        game::World w(vfs, "mp_toujane", true);
        PlayerWeapon pw;
        pw.lookup = [&](const std::string& n) { return w.weapon(n); };
        const auto* kar = w.weapon("kar98k_mp");
        CHECK(kar != nullptr);
        if (kar) {
            pw.equip("kar98k_mp", *kar);
            CHECK(has(stateEvents(pw), 0x9a));
            PlayerWeapon::Input in;
            in.weapon = "kar98k_mp";
            for (int i = 0; i < 40; ++i) pw.update(0.05f, in); // raised
            (void)pw.takeEvents();
            in.fire = true;
            std::vector<int> ev;
            for (int i = 0; i < 20; ++i) {
                pw.update(0.05f, in);
                for (int c : stateEvents(pw)) ev.push_back(c);
            }
            CHECK(has(ev, 0xa1)); // the bolt after the shot
            in.fire = false;
            for (int i = 0; i < 40; ++i) pw.update(0.05f, in);
            (void)pw.takeEvents();
            in.reload = true;
            pw.update(0.05f, in);
            in.reload = false;
            ev = stateEvents(pw);
            for (int i = 0; i < 10; ++i) {
                pw.update(0.05f, in);
                for (int c : stateEvents(pw)) ev.push_back(c);
            }
            CHECK(has(ev, 0x96) || has(ev, 0x97));
            for (int i = 0; i < 80; ++i) pw.update(0.05f, in);
            (void)pw.takeEvents();
            in.weapon = "luger_mp";
            pw.update(0.05f, in);
            CHECK(has(stateEvents(pw), 0x9b));
        }
    }

    // The server's weapon step of a network client puts them into its ring and the client's archived ring (the killcam).
    {
        game::World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int slot = w.mpConnect("rifleman");
        w.mpBegin(slot);
        w.frame();
        w.mpMenuResponse(slot, "team_britishgerman", "axis");
        w.frame();
        w.mpMenuResponse(slot, w.mpState().clients[size_t(slot)].openMenu, "kar98k_mp");
        for (int i = 0; i < 4; ++i) w.frame();
        game::Entity* e = w.mpClientEntity(slot);
        CHECK(e && e->obj->get("sessionstate").toString() == "playing");
        CHECK(e && e->obj->get("$weapon").toString() == "kar98k_mp");
        netgame::NetCombat nc;
        nc.weaponIndex = [](const std::string& n) { return n.empty() ? 0 : 1; };
        game::PlayerState ps;
        if (e) {
            const auto o = e->obj->get("origin").asVector();
            ps.origin = {o[0], o[1], o[2]};
        }
        ps.viewHeight = 60;
        const game::MpEventRing* ring = w.mpClientEvents(slot);
        const int seq0 = ring ? ring->sequence : 0;
        std::vector<int> wire;
        int wireSeq = nc.ring().sequence;
        net::UserCmd cmd{};
        int t = 0;
        auto step = [&](int buttons) {
            cmd.buttons = buttons;
            cmd.serverTime = (t += 50);
            nc.think(w, slot, cmd, ps, 50, "kar98k_mp");
            const netgame::EventRing& r = nc.ring();
            for (; wireSeq != r.sequence; wireSeq = (wireSeq + 1) & 0xff) wire.push_back(r.events[size_t(wireSeq & 3)]);
        };
        for (int i = 0; i < 30; ++i) step(0);
        for (int i = 0; i < 4; ++i) step(int(game::Button::Attack));
        for (int i = 0; i < 30; ++i) step(0);
        CHECK(has(wire, netgame::kEvFireWeapon) || has(wire, netgame::kEvFireWeaponLastShot));
        CHECK(has(wire, netgame::kEvRechamberWeapon));
        if (ring) {
            const std::vector<int> archived = ringEvents(*ring, seq0);
            CHECK(has(archived, game::mpev::kRechamber));
        }
    }

    // The listening side: another player's bolt at its position (the plain alias), the followed client's as the view's.
    {
        game::World mp(vfs, "mp_toujane", true);
        mp.mpStartRemote();
        game::World::RemotePlayer rp;
        rp.client = 3;
        rp.name = "far";
        rp.origin = {500, 0, 0};
        mp.mpSetRemotePlayers({rp});
        std::vector<std::pair<std::string, bool>> sounds;
        mp.playSound = [&](const std::string& a, const game::Vec3* pos, float*, int) {
            sounds.emplace_back(a, pos != nullptr);
            return 0;
        };
        auto played = [&](const char* alias, bool positional) {
            for (const auto& [a, p] : sounds)
                if (a == alias && p == positional) return true;
            return false;
        };
        mp.mpRemotePlayerEvent(3, 0xa1, "kar98k_mp", 0.0f);
        CHECK(played("weap_kar98k_rechamber_npc", true));
        sounds.clear();
        mp.mpRemotePlayerEvent(3, 0x98, "kar98k_sniper_mp", 0.0f);
        mp.mpRemotePlayerEvent(3, 0x99, "kar98k_sniper_mp", 0.0f);
        CHECK(played("weap_kar98k_sniper_reload_start_npc", true) && played("weap_kar98k_sniper_reload_end_npc", true));
        sounds.clear();
        mp.mpRemotePlayerEvent(3, 0x9a, "bar_mp", 0.0f);
        mp.mpRemotePlayerEvent(3, 0x9b, "bar_mp", 0.0f);
        CHECK(played("weap_bar_raise", true) && played("weap_bar_putaway", true));
        sounds.clear();
        mp.mpRemoteFollowEvent(0xa1, "kar98k_mp");
        mp.mpRemoteFollowEvent(0x96, "kar98k_mp");
        CHECK(played("weap_kar98k_rechamber", false) && played("weap_kar98k_reload", false)); // the player variants

        // Every hand weapon of the multiplayer set, as the killcam replays it: emptied and reloaded, its state events
        // (reload, reload start / end, rechamber, raise, put away) give the followed client's sounds.
        const char* const kWeapons[] = {"thompson_mp", "greasegun_mp", "m1carbine_mp", "m1garand_mp", "bar_mp", "springfield_mp",
                                        "shotgun_mp", "colt_mp", "mp40_mp", "mp44_mp", "kar98k_mp", "kar98k_sniper_mp",
                                        "g43_mp", "luger_mp", "sten_mp", "bren_mp", "enfield_mp", "enfield_scope_mp",
                                        "webley_mp", "ppsh_mp", "pps42_mp", "mosin_nagant_mp", "mosin_nagant_sniper_mp",
                                        "svt40_mp", "tt30_mp"};
        for (const char* wn : kWeapons) {
            const auto* def = mp.weapon(wn);
            CHECK(def != nullptr);
            if (!def) continue;
            auto field = [&](const char* k) {
                const auto it = def->find(k);
                return it == def->end() ? std::string() : it->second;
            };
            PlayerWeapon pw;
            pw.lookup = [&](const std::string& n) { return mp.weapon(n); };
            pw.equip(wn, *def);
            PlayerWeapon::Input in;
            in.weapon = wn;
            std::vector<int> codes;
            auto run = [&](int frames, bool tapFire, bool reload) {
                for (int i = 0; i < frames; ++i) {
                    in.fire = tapFire && (i % 4) < 2;
                    in.reload = reload && i == 0;
                    pw.update(0.05f, in);
                    for (int c : stateEvents(pw)) codes.push_back(c);
                }
            };
            run(40, false, false);
            run(400, true, false); // the clip emptied, shot by shot
            in.fire = false;
            run(200, false, true); // reloaded
            const bool reloaded = has(codes, 0x96) || has(codes, 0x97) || has(codes, 0x98);
            if (!reloaded) std::cerr << wn << ": no reload event\n";
            CHECK(reloaded);
            if (!field("rechamberSound").empty() && !has(codes, 0xa1)) {
                std::cerr << wn << ": no rechamber event\n";
                CHECK(false);
            }
            // Each event of the replay plays its weapon's sound when the weapon has one.
            for (int c : codes) {
                // The own-view chains (player variant first) of the followed client.
                std::vector<const char*> chain;
                if (c == 0x96) chain = {"reloadSoundPlayer", "reloadEmptySoundPlayer", "reloadSound", "reloadEmptySound"};
                else if (c == 0x97) chain = {"reloadEmptySoundPlayer", "reloadSoundPlayer", "reloadEmptySound", "reloadSound"};
                else if (c == 0x98) chain = {"reloadStartSoundPlayer", "reloadStartSound"};
                else if (c == 0x99) chain = {"reloadEndSoundPlayer", "reloadEndSound"};
                else if (c == 0xa1) chain = {"rechamberSoundPlayer", "rechamberSound"};
                else if (c == 0x9a) chain = {"raiseSound"};
                else chain = {"putawaySound"};
                std::string want;
                for (const char* k : chain)
                    if (want.empty()) want = field(k);
                if (want.empty()) continue;
                sounds.clear();
                mp.mpRemoteFollowEvent(c, wn);
                if (!played(want.c_str(), false)) {
                    std::cerr << wn << ": event 0x" << std::hex << c << std::dec << " did not play " << want << "\n";
                    CHECK(false);
                }
            }
        }
    }
    // The weapon change sound (docs/spec/weapons.md "Raise and put-away sounds"): a weapon without raiseSound /
    // putawaySound raises with weap_raise and puts away with weap_putaway, in the own view, for another player and for
    // the followed client; a weapon that names its own (the BAR) keeps them.
    {
        game::World mp(vfs, "mp_toujane", true);
        PlayerWeapon pw;
        pw.lookup = [&](const std::string& n) { return mp.weapon(n); };
        const auto* mp40 = mp.weapon("mp40_mp");
        CHECK(mp40 != nullptr);
        if (mp40) {
            pw.equip("mp40_mp", *mp40);
            CHECK(pw.takeSounds() == std::vector<std::string>{"weap_raise"});
            PlayerWeapon::Input in;
            in.weapon = "mp40_mp";
            for (int i = 0; i < 40; ++i) pw.update(0.05f, in);
            (void)pw.takeSounds();
            in.weapon = "bar_mp";
            pw.update(0.05f, in);
            const auto drop = pw.takeSounds();
            CHECK(!drop.empty() && drop.front() == "weap_putaway");
            for (int i = 0; i < 40; ++i) pw.update(0.05f, in);
            bool barRaise = false;
            for (const std::string& a : pw.takeSounds()) barRaise = barRaise || a == "weap_bar_raise";
            CHECK(pw.name() == "bar_mp" && barRaise);
        }
        mp.mpStartRemote();
        game::World::RemotePlayer rp;
        rp.client = 3;
        rp.name = "other";
        rp.origin = {500, 0, 0};
        mp.mpSetRemotePlayers({rp});
        std::vector<std::pair<std::string, bool>> sounds;
        mp.playSound = [&](const std::string& a, const game::Vec3* pos, float*, int) {
            sounds.emplace_back(a, pos != nullptr);
            return 0;
        };
        mp.mpRemotePlayerEvent(3, 0x9a, "mp40_mp", 0.0f);
        mp.mpRemotePlayerEvent(3, 0x9b, "mp40_mp", 0.0f);
        CHECK(sounds.size() == 2 && sounds[0] == std::make_pair(std::string("weap_raise"), true) &&
              sounds[1] == std::make_pair(std::string("weap_putaway"), true));
        sounds.clear();
        mp.mpRemoteFollowEvent(0x9a, "thompson_mp");
        CHECK(sounds.size() == 1 && sounds[0] == std::make_pair(std::string("weap_raise"), false));
    }
    std::cout << (failures ? "mpweaponevents: FAILED\n" : "mpweaponevents: OK\n");
    return failures ? 1 : 0;
}
