// The multiplayer World behind the network server core (docs/spec/net.md sections 8 and 11): network clients map to
// MP clients of the World, their user commands drive the shared movement (src/netgame/netmove.cpp), and every
// snapshot carries what the client front end needs to show the same as a local game: the other players and bots,
// script entities with models (grenades, flags, the bomb, bodies), health, weapon and ammo, stance, the client's HUD
// elements, objectives, obituaries (event entities), chat and prints (reliable server commands).
//
// Used by tools/iw2server (dedicated) and by iw2view as a listen server (the local player stays MP client 0).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/d3dbsp.h"
#include "game/mpgame.h"
#include "game/pmove.h"
#include "net/server.h"
#include "netgame/netcombat.h"
#include "script/value.h"

namespace iw2::game {
class World;
struct Entity;
struct MpObituary;
struct ArchivedFrame;
}

namespace iw2::netgame {

// Configstring layout of the 1.3 gamestate (docs/spec/net.md section 11): a name with index i (1-based, 0 = none)
// of a precache table is configstring base + i. The constants below are the configstring of index 1.
// Weapons are one configstring holding every weapon name separated by spaces; weapon number i is the i-th name.
constexpr int kWeaponListConfigstring = 7;
constexpr int kModelConfigstrings = 335;      // models: base 334, 255 names
constexpr int kStringConfigstrings = 1311;    // localized strings: base 1310, 255 names
constexpr int kMaterialConfigstrings = 1567;
constexpr int kSoundConfigstrings = 591;      // sounds: base 590, 255 names
constexpr int kSoundConfigstringCount = 255;
constexpr int kHintConfigstrings = 1278;      // hint strings: base 1278, index 0 used (observed), 32 names
constexpr int kHintConfigstringCount = 32;
constexpr int kDvarNameConfigstrings = 142;   // server-info / synced dvar names, their values at +96
constexpr int kDvarValueConfigstrings = 238;
constexpr int kDvarConfigstringCount = 96;
constexpr int kFogConfigstring = 12;
constexpr int kHeadIconConfigstrings = 31;    // head icons: base 30 (observed), then the talking icon
constexpr int kHeadIconConfigstringCount = 15;
constexpr int kAmbientConfigstring = 3;
constexpr int kEffectConfigstrings = 847;     // effects (loadfx): base 846, 63 names
constexpr int kEffectConfigstringCount = 63;  // materials (shaders): base 1566, 127 names
constexpr int kStatusIconConfigstrings = 23;  // status icons: base 22 (observed) TODO(spec): count
constexpr int kWeaponConfigstringCount = 127;
constexpr int kMaterialConfigstringCount = 127;
constexpr int kStringConfigstringCount = 255;
constexpr int kModelConfigstringCount = 255;
// Weapon number n (1-based) of the weapon list configstring; empty for 0 or an unknown number.
std::string weaponNameFromList(const std::map<int, std::string>& configstrings, int n);
// Every name of the weapon list configstring, weapon 1 first.
std::vector<std::string> weaponList(const std::map<int, std::string>& configstrings);
// The ammo (clip) name of a weapon (its weapon file's ammoName / clipName, else the weapon name).
std::string weaponAmmoName(game::World& w, const std::string& weapon, bool clip);
// The player-state ammo (clip) slot of a weapon: the position of its ammo (clip) name among the distinct ammo (clip)
// names of the weapon list in list order, 0-based (observed, docs/spec/net.md section 11); -1 when not listed.
int weaponAmmoSlot(game::World& w, const std::vector<std::string>& list, const std::string& weapon, bool clip);
// Shell shock names: configstring 0x48e + index (index 1..15; 1.3 Linux dedicated server 0x81162a2 compares the name
// against them), the player state's shock index selects one (docs/spec/mp.md "Per-client player state").
constexpr int kShockConfigstrings = 0x48e;

// Wire entity numbers (10 bits): players by MP client number, other entities from kFirstWireEntity, events last.
constexpr int kFirstWireEntity = 64;
constexpr int kFirstMapEntity = 72;  // the map's first entity after worldspawn (observed; 64..71 TODO(spec))
constexpr int kFirstEventEntity = 1008;  // 14 event slots, 1022 / 1023 stay reserved
constexpr int kEventEntityCount = 14;
// Entity types on the wire (docs/spec/net.md section 11): observed 1 player, 6 script model, 9 turret.
constexpr int kEtPlayer = 1;
constexpr int kEtGeneral = 6;  // script models (observed with the 1.3 server)
// Temporary event entities: eType = kEtEvents + event (observed: bullet impacts 192, the obituary 208 = 10 + 0xc6).
constexpr int kEtEvents = 10;
constexpr int kEtItem = 3;
constexpr int kEtCorpse = 2;   // a body of the body queue 64..71 (observed)
constexpr int kFirstBodyEntity = 64;     // a dropped weapon, typeIndex the weapon number (observed)
constexpr int kEtTurret = 9;
constexpr int kEtLoopFx = 8;   // playLoopedFx (observed)   // misc_turret / misc_mg42 (observed)
constexpr int kEventGrenadeExplode = 0xbc;  // event-name table order (docs/spec/net.md section 13)
constexpr int kEventRocketExplode = 0xbd;
constexpr int kEventObituary = 0xc6;      // the obituary event (docs/spec/mp-hud.md)
constexpr int kKillcamMissileKey = -(1 << 24); // wire keys of the killcam's archived missiles (minus the number)
constexpr int kEventLifeMs = 1000;        // an event entity stays in the snapshots this long
// Player-state pm_flags bits for the stance: 1 prone (docs/spec/vehicles.md), 2 crouch TODO(spec).
constexpr int kPmfProne = 1;
constexpr int kPmfCrouch = 2;
// Entity eFlags bits for another player's stance. TODO(spec): the 1.3 bits.
constexpr int kEfCrouch = 4;
constexpr int kEfProne = 8;
constexpr int kEfDead = 1;
// A body whose death animation is still playing; its think clears the bit when the animation ends (docs/spec/net.md section 11).
constexpr int kEfBodyNew = 0x80000;
constexpr int kEfHidden = 0x20;  // openiw2: a hidden script model (TODO(spec): the 1.3 server does not send those)
constexpr int kEfFiring = 0x40;
// The killcam / follow view (docs/spec/mp-hud.md "What the killcam client draws"): pm_flags bit kept from the spectator,
// eFlags bit the copied player state gets (0x80f6916).
constexpr int kPmfFollow = 0x100000;
constexpr int kPmfAds = 0x40; // aiming down the sights (the followed state's aim flag in a killcam)
constexpr int kEfKillcam = 0x400000;

// A growing name table in one configstring range; new names are announced to the clients.
class ConfigstringRange {
public:
    // list: every name goes into the one configstring base, separated by spaces (the weapon list).
    // lowerCase: names are kept in lower case (materials).
    ConfigstringRange(int base, int count, bool list = false, bool lowerCase = false)
        : base_(base), count_(count), list_(list), lower_(lowerCase) {}
    // The configstrings of the range (index -> text).
    void emit(std::map<int, std::string>& cs) const;
    // The 0-based index of name, adding it when there is room (-1 when the range is full or name is empty).
    int index(const std::string& name);
    const std::vector<std::string>& names() const { return names_; }
    int base() const { return base_; }
    void clear() { names_.clear(); }
    std::function<void(int configstring, const std::string& value)> onAdd;

private:
    int base_, count_;
    bool list_ = false;
    bool lower_ = false;
    std::vector<std::string> names_;
    std::string joined() const;
};

// HUD element <-> the 128-byte wire structure (field table of docs/spec/net.md section 7). Times are server times;
// the client shifts them by timeOffset (client world time - server time).
struct HudStrings {
    ConfigstringRange* materials = nullptr;
    ConfigstringRange* strings = nullptr;
};
void encodeHudElem(uint8_t* out, const script::Object& h, HudStrings& s);
script::ObjectPtr decodeHudElem(const uint8_t* in, const std::function<script::ObjectPtr()>& newObject,
                                const std::map<int, std::string>& configstrings, int64_t timeOffset);
// A HUD string as stored in a configstring: a localized reference is its key, as in the 1.3 gamestate; a plain text
// (an openiw2 extension, the original HUD only shows localized strings) is marked by a leading '"'.
constexpr char kPlainHudStringMark = '"';
std::string hudStringToConfig(const script::Value& v);
script::Value hudStringFromConfig(const std::string& s);

class WorldGame : public net::ServerGame {
public:
    WorldGame(game::World* w, std::string map, std::string gametype, int serverId, int maxClients);
    ~WorldGame() override;

    // A new map: the old world's clients are gone; network clients connect again through clientConnect.
    void setWorld(game::World* w, std::string map, std::string gametype, int serverId);
    std::string hostname = "openiw2";
    // network slot -> MP client
    const std::map<int, int>& slots() const { return mp_; }
    // A listen server's own front end runs the world's frames; runFrame then only forwards (World::frame not called).
    bool worldOwnedByHost = false;
    // Reliable server commands to one network slot (-1: all); set by the owner of the net::Server.
    std::function<void(int, const std::string&)> sendCommand;
    // The server's mean round trip of a network slot, for the scoreboard; set by the owner of the net::Server.
    std::function<int(int)> clientPing;
    // A map_restart of the world (sd rounds): the owner flips the server's snapshot flag bit (observed with the 1.3
    // server after a round restart, docs/spec/net.md section 11).
    std::function<void()> onRestart;

    std::map<int, std::string> configstrings() override;
    std::map<int, std::vector<uint8_t>> baselines() override;
    int clientNumber(int slot) override;
    bool clientConnect(int slot, const std::string& userinfo) override;
    void clientBegin(int slot) override;
    void clientCommand(int slot, const std::string& text) override;
    void clientThink(int slot, const net::UserCmd& cmd) override;
    void clientDisconnect(int slot) override;
    void runFrame(int frameMs) override;
    int serverTime() const override;
    net::SnapshotView snapshot(int slot) override;
    // The weapon side of a network slot (tests); nullptr when the slot has no client.
    const NetCombat* combat(int slot) const;
    // User-command weapon byte <-> weapon name: the weapon configstring index + 1, 0 none (docs/spec/net.md section 13).
    int weaponByte(const std::string& name) {
        if (name.empty() || name == "none") return 0;
        const int i = weapons_.index(name);
        return i < 0 || i > 126 ? 0 : i + 1;
    }
    // The wire weapon number of a weapon name (weaponByte; 0 none).
    int weaponNumber(const std::string& name) { return weaponByte(name); }

    // Forwards menus, prints, chat and obituaries that appeared since the last call (runFrame calls it).
    void forwardToClients();

private:
public:
    int stateClears = 0; // player states cleared outside a spawn (the NETTRACE reset case), for tests
private:
    struct Move {
        game::PlayerState ps;
        bool havePs = false;
        int spawnCount = 0;  // the MP client's spawn count the movement was restarted for
        int dropSpawn = -1;   // IW2_MP_DROP: the spawn count the client's items were last dropped for
        int placedSpawn = -1; // IW2_MP_PLACE: the spawn count the client was last placed for
        std::array<float, 3> lastOrigin{};
        int lastTime = 0;
        int lastServerTime = 0;  // the antilag time (0x80f5614)
        bool turretMoved = false; // on a map turret at the last think, or the turret code moved the client since
        float viewPitch = 0, viewYaw = 0;
        std::shared_ptr<NetCombat> combat = std::make_shared<NetCombat>();  // the weapon side (netcombat.h)
        // The client entity's event ring (docs/spec/net.md section 13): the new movement events of ps (footsteps,
        // jumps, landings) and the weapon events of the combat ring, in the order the command produced them.
        EventRing entityRing;
        int psEventsDone = 0;     // ps.eventSeq already copied
        int combatEventsDone = 0; // combat ring sequence already copied
        // The view set by the server (docs/spec/net.md section 10 "View angles"): delta_angles (ps+84) and the raw
        // angles of the last command, which SetClientViewAngle subtracts.
        int deltaAngles[3] = {0, 0, 0};
        int32_t lastCmdAngles[3] = {0, 0, 0};
        int viewAngleSetsSeen = 0;
    };
    struct Event {
        int number = 0;
        int time = 0;
        std::vector<uint8_t> es;
    };
    void announce(int index, const std::string& value);
    // The archived frame a killcam spectator is shown (frame nullptr when not in a killcam) and the followed MP client.
    struct KillcamFrame {
        const game::ArchivedFrame* frame = nullptr;
        int client = -1;
        int64_t number = -1;  // the frame's number in the archive
    };
    KillcamFrame killcamFrame(int self) const;
    void writeKillcamPlayerState(std::vector<uint8_t>& ps, const KillcamFrame& kc);
    void writePlayerEntity(std::vector<uint8_t>& es, int client, const game::Entity& e);
    void writeInventory(std::vector<uint8_t>& ps, game::Entity& e);
    int wireNumber(const game::Entity& e);
    void pushObituary(const game::MpObituary& ob);
    void pushExplosion(const game::MpState::Explosion& ex);
    Event explosionEvent(bool rocket, const std::string& weapon, const script::Vec3& at, const script::Vec3& dir, int surface);
    // A killcam spectator (MP client) -> the newest archived frame whose explosions it was sent, and those event entities.
    std::map<int, int64_t> killcamReplayed_;
    std::map<int, std::vector<Event>> killcamEvents_;

    game::World* w_;
    std::string map_, gametype_;
    int serverId_;
    int maxClients_;
    std::map<int, int> mp_;  // network slot -> MP client
    std::map<int, Move> moves_;
    bool seeding_ = false;
    int eventNumber();
    int freeNumber();
    int restartsSeen_ = 0;
    int levelStartMs_ = 0;  // configstring 13
    int wireNumber(int key, int mapIndex);
    std::map<int, int> sentOffhand_;  // network slot -> offhand weapon number last announced
    std::vector<assets::BspEntity> worldspawn_;
    size_t printsSent_ = 0, chatSent_ = 0, obituariesSeen_ = 0, explosionsSeen_ = 0, clientCommandsSent_ = 0, localSoundsSent_ = 0;
    ConfigstringRange weapons_{kWeaponListConfigstring, kWeaponConfigstringCount, true};
    ConfigstringRange materials_{kMaterialConfigstrings, kMaterialConfigstringCount, false, true};
    ConfigstringRange strings_{kStringConfigstrings, kStringConfigstringCount};
    ConfigstringRange models_{kModelConfigstrings, kModelConfigstringCount};
    ConfigstringRange effects_{kEffectConfigstrings, kEffectConfigstringCount};
    ConfigstringRange sounds_{kSoundConfigstrings, kSoundConfigstringCount};
    ConfigstringRange hints_{kHintConfigstrings, kHintConfigstringCount};
    ConfigstringRange headIcons_{kHeadIconConfigstrings, kHeadIconConfigstringCount};
    void syncNames();
    // Configstrings that follow the world's state (fog, ambient, dvars); re-announced when they change.
    std::map<int, std::string> dynamicConfigstrings() const;
    std::map<int, std::string> lastDynamic_;
    void registerWeaponNames(const std::string& weapon);
    std::map<int, int> wireOf_;       // world entity number -> wire number
    std::vector<bool> wireUsed_;
    std::vector<Event> events_;
    // The fire events of the players that are not network clients (the host, bots): MP client -> (shots seen, ring).
    std::map<int, std::pair<int, EventRing>> turretRings_; // map turrets (entity number): shots seen, fire events
    std::map<int, std::pair<int, EventRing>> otherRings_;  // map entities: origin and angles at the start (sent once moved)
};

} // namespace iw2::netgame
