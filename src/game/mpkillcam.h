// Multiplayer state archive and killcam (docs/spec/mp-hud.md "Killcam"), implemented from the spec.
// The server archives every frame; a client with `archivetime` > 0 sees the frame archivetime ago (the killcam).
#pragma once

#include "game/animtree.h"
#include "script/value.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace iw2::game {

// One entity of an archived frame: what the front end needs to draw it.
struct ArchivedEntity {
    int number = -1;
    script::Vec3 origin{}, angles{};
    std::string model;
    std::vector<std::pair<std::string, std::string>> attachments; // model, tag
    const AnimTree* tree = nullptr;                                // the animation tree (nullptr: no animation)
    std::vector<std::pair<int, AnimNodeState>> anim;               // the tree nodes with weight (sparse)
    bool hidden = false;
    std::string sessionstate;  // clients only
    std::string weaponModel;   // clients only: the world model in the hands
    // A missile (a grenade, a smoke grenade, a rocket): its trajectories as the entity state carries them (docs/spec/net.md
    // section 11 "Trajectories"), so a network killcam can send it with them. missile 0: none, 1 grenade, 2 rocket.
    int missile = 0;
    int trType = 0;
    int64_t trTime = 0;
    script::Vec3 trBase{}, trDelta{};
    int aType = 0;   // the angle trajectory: 0 none, 2 linear
    int64_t aTime = 0;
    script::Vec3 aBase{}, aDelta{};
    // A body of the body queue (cloneplayer): what its entity state carries, so a network killcam sends the bodies of the
    // shown frame. bodySlot -1: not a body.
    int bodySlot = -1, bodyClient = 0, bodyAnim = -1, bodyFlip = 0;
    int64_t bodyBornMs = 0; // the level time of its cloneplayer
    std::string itemWeapon; // a dropped weapon item: its weapon ("" otherwise); the network killcam sends the items of the frame
    int64_t bodyThinkMs = 0; // the level time its death animation ends (eFlags 0x80000 clears)
};

// The recorded first-person view state of a client (docs/spec/mp-hud.md "The recorded view state"): filled every
// frame by whoever moves the client (the bot controller, the network server, the host's front end), archived with
// the frame, and the only thing the killcam's view weapon, camera, scope overlay and crosshair read, locally and over
// the network (the network killcam carries it in the followed player state).
struct MpViewState {
    std::string weapon;      // the player state's weapon (the HUD's; "" none)
    std::string viewWeapon;  // the weapon whose file the view model uses: the offhand grenade in the offhand states, else weapon
    int weaponState = 0;     // PM_Weapon state number (0 ready, 1 raising, 2 dropping, 3 firing, 5 reloading, 12..16 offhand, ...)
    std::string animKey;     // weapon-file key of the view animation ("idleAnim", "fireAnim", "holdFireAnim", ...; "" idle)
    int animMs = 0;          // the animation's own elapsed time in ms (its playback rate applied)
    float adsFrac = 0;       // aim fraction 0..1
    bool adsHeld = false;    // the aim is held (pm_flags 0x40): the fraction rises
    int bobCycle = 0;        // ps+8, 0..255
    script::Vec3 velocity{};
    int viewHeightTarget = 60; // 60 standing, 40 crouched, 11 prone
    float viewHeight = 60;
    float lean = 0;
    float aimSpread = 255; // ps+0x5b0, 0..255 (the crosshair's spread and alpha)
    bool operator==(const MpViewState&) const = default;
};

// The weapon-file keys of the view animations in wire order: `weaponAnim` (+1488) of a killcam player state carries the
// index (0 = idle; docs/spec/mp-hud.md "The recorded view state").
inline constexpr const char* const kViewAnimKeys[] = {
    /* 0*/ "idleAnim",
    /* 1*/ "emptyIdleAnim",
    /* 2*/ "fireAnim",
    /* 3*/ "lastShotAnim",
    /* 4*/ "rechamberAnim",
    /* 5*/ "meleeAnim",
    /* 6*/ "reloadAnim",
    /* 7*/ "reloadEmptyAnim",
    /* 8*/ "reloadStartAnim",
    /* 9*/ "reloadEndAnim",
    /*10*/ "raiseAnim",
    /*11*/ "dropAnim",
    /*12*/ "altRaiseAnim",
    /*13*/ "altDropAnim",
    /*14*/ "quickRaiseAnim",
    /*15*/ "quickDropAnim",
    /*16*/ "firstRaiseAnim",
    /*17*/ "holdFireAnim",
    /*18*/ "holdAnim",
    /*19*/ "adsUpAnim",
    /*20*/ "adsDownAnim",
    /*21*/ "detonateAnim",
    /*22*/ "idleCookAnim"};
inline constexpr int kViewAnimKeyCount = int(sizeof(kViewAnimKeys) / sizeof(kViewAnimKeys[0]));
int viewAnimKeyIndex(const std::string& key); // 0 for the idle and for an unknown key
const char* viewAnimKeyName(int index);       // "idleAnim" for an index out of range

// Entity event numbers a player's event ring carries (the event-name table of the 1.3 Linux dedicated server at
// 0x8183da0, docs/spec/net.md section 13).
namespace mpev {
constexpr int kReload = 0x96, kReloadFromEmpty = 0x97, kRaiseWeapon = 0x9a, kPutawayWeapon = 0x9b, kFireWeapon = 0x9e,
              kFireWeaponLastShot = 0xa0, kRechamber = 0xa1, kMeleeSwipe = 0xa3, kFireMelee = 0xa4, kPrepOffhand = 0xa5,
              kUseOffhand = 0xa6;
}

// A client's entity event ring (eventSequence and the newest four events with their weapon parameter, as the player
// and entity states carry them). The parameter is kept by weapon name; the network layer sends its weapon index.
struct MpEventRing {
    int sequence = 0;
    std::array<int, 4> events{};
    std::array<std::string, 4> weapons{};
    void push(int event, const std::string& weapon) {
        events[size_t(sequence & 3)] = event;
        weapons[size_t(sequence & 3)] = weapon;
        sequence = (sequence + 1) & 0xff;
    }
    bool operator==(const MpEventRing&) const = default;
};

// One client of an archived frame (its player state as the killcam shows it).
struct ArchivedClient {
    bool active = false;
    int entity = -1;
    script::Vec3 origin{};     // the entity origin (the antilag rewind)
    script::Vec3 eye{};        // view origin
    script::Vec3 viewAngles{}; // pitch, yaw, roll
    std::string weapon;        // weapon name
    std::string hands;         // setviewmodel hands ("" = the weapon's handModel)
    int team = 0;              // client team number (0 none, 1 axis, 2 allies, 3 spectator)
    int fireCount = 0;         // shots fired so far (a change from the previous frame is a shot in this frame)
    bool lastShot = false;     // the newest shot emptied the clip
    int clip = 0, stock = 0;   // the weapon's loaded and stock rounds
    int health = 0, maxHealth = 0;
    int shockIndex = 0;        // the shell shock of the player state (precache index, 0 none), its start and duration
    int64_t shockStartMs = 0;
    int shockDurationMs = 0;
    // The damage feedback fields of the player state (damageEvent, damageYaw, damagePitch, damageCount) and its spawn count
    // (stats[5]): the copy of the followed client's player state carries them, so the replay shows his red flashes and kicks.
    int damageEvent = 0, damageYaw = 0, damagePitch = 0, damageCount = 0, spawnCount = 0;
    MpViewState view;          // the recorded view state (the weapon field above is the same as view.weapon)
    MpEventRing ring;          // the client's event ring at the end of the frame
};

// A client event of an archived frame: what a shot made the client show (fire sound, impact effects, tracer). The
// original sends the entity events of the archived snapshot again, so the killcam shows and plays them.
struct ArchivedEvent {
    enum Kind { Sound, Fx, Tracer, Explosion } kind = Sound;
    std::string name;       // sound alias (Sound), effect path (Fx) or the missile's weapon (Explosion)
    std::string playerName; // Sound: the alias the shooter itself hears (firesoundplayer / lastshotsoundplayer), "" = name
    script::Vec3 a{}, b{};  // Sound: origin; Fx: origin, forward; Tracer: from, to; Explosion: point, direction
    int shooter = -1;       // Sound: the shooting client (-1 none)
    int owner = -1;         // Sound: the slot owner (entity number)
    bool rocket = false;    // Explosion: a rocket's (else a grenade's)
    int surface = 0;        // Explosion: the surface type under the point
};

struct ArchivedFrame {
    int64_t timeMs = 0;
    std::vector<ArchivedEntity> entities; // in entity number order
    std::vector<ArchivedClient> clients;  // per client slot
    std::vector<ArchivedEvent> events;    // the client events raised during the frame
    const ArchivedEntity* find(int number) const;
};

// The view state drawn between two records (the frame or snapshot before, `prev`, and the shown one, `cur`) at the
// render frame's fraction `frac` (0 prev, 1 cur), `intervalMs` apart (docs/spec/mp-hud.md "Interpolation of the
// followed view"): the discrete fields are cur's; the bob cycle (mod 256), velocity, view height, lean and aim fraction
// are interpolated as CG_InterpolatePlayerState (0x4de7c0) does; the animation time runs from prev's to cur's when both
// play the same animation, else from cur's start within the interval (cur.animMs - (1 - frac) * intervalMs, not below 0).
MpViewState lerpMpViewState(const MpViewState& prev, const MpViewState& cur, float frac, int intervalMs);

// One line describing a view state (tests, traces).
std::string mpViewStateText(const MpViewState& v);

class PlayerWeapon;
struct PlayerState;
// The view state of a client from its weapon state machine and movement state (the three movers sample it this way).
MpViewState mpViewStateOf(const PlayerWeapon& weapon, const PlayerState& ps);

// sv_fps of the port's server (default 20, docs/spec/mp-hud.md "The archive").
constexpr int kMpServerFps = 20;

// The archive (setarchive, 0x8093312): 1200 frames are kept (the frame index ring, 0x80990a5).
struct MpArchive {
    static constexpr int kFrames = 1200;
    bool enabled = false;
    int64_t count = 0;                 // frames archived so far (the next frame number)
    std::deque<ArchivedFrame> frames;  // frames count - frames.size() .. count - 1
    std::vector<ArchivedEvent> pending; // events of the running frame (moved into it when it is archived)
    int64_t replayed = -1;              // the local killcam: the newest archived frame whose events were replayed
    const ArchivedFrame* frame(int64_t n) const {
        const int64_t first = count - int64_t(frames.size());
        return n < first || n >= count ? nullptr : &frames[size_t(n - first)];
    }
};

// The lookup of 0x8099c04: the frame `ms` milliseconds back at `fps` server frames per second. A start older than the
// kept frames (or before frame 0) is clamped and `ms` rewritten to the clamped span; the first archived frame from
// the start on is returned (its number), -1 with `ms` = 0 when there is none. Returns -1 without touching `ms` when
// the archive is off or `ms` <= 0.
int64_t archiveLookup(const MpArchive& a, int& ms, int fps);

// The frames whose events the killcam replays when the shown frame moved from `replayed` to `shown`: (replayed, shown],
// only `shown` itself when nothing was replayed yet or the view jumped back (a new killcam), none when shown < 0.
std::pair<int64_t, int64_t> killcamReplayRange(int64_t replayed, int64_t shown);

// Age in ms (archived server time) of the newest shot of client `client` at frame `n`: the newest frame m <= n whose
// fireCount differs from frame m - 1, searched back `maxFrames` frames; -1 when there is none.
int killcamFireAge(const MpArchive& a, int64_t n, int client, int maxFrames = 40);

} // namespace iw2::game
