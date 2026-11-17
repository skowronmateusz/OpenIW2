// Multiplayer bots (docs/spec/mp-bots.md): test clients driven by a controller that produces player input for pmove.
// An OpenIW2 extension: the original's test clients (addtestclient) never move.
#pragma once

#include "game/navgen.h"
#include "assets/xanim.h"
#include "game/playerweapon.h"
#include "game/pmove.h"
#include "game/viewweapon.h"
#include "script/value.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace iw2::game {

// OpenIW2 limit on bots (the original has none and 64 client slots): at most this many bots on a server at once. World::mpAddBot
// refuses the next one, which covers the console, the scripts, --bots, the menu and the listen server.
constexpr int kMaxBots = 31;

// Difficulty preset (docs/spec/mp-bots.md, "Skill presets").
struct BotSkill {
    const char* name;
    int reactionMs;      // delay from first sight of an enemy to the first shot
    float turnRate;      // view turn limit, degrees per second
    float aimNoiseDeg;   // the hand's aim error: how far the aimed point wanders around the target, degrees
    float aimOmega;      // stiffness of the view's spring toward the aimed point (rad/s): how fast the view gets there
    float aimDamping;    // its damping ratio: below 1 the view overshoots and settles
    float aimLead;       // how much of a moving target's lag the bot makes up by leading (0..1)
    float aimTolerance;  // fires when the view is within this many target half widths of the aimed point
    int noticeMs;        // time to notice an enemy in the middle of the view close by (botNoticeMs)
    float sightDist;     // farthest enemy noticed
    int burst;           // shots before a pause
    int burstPauseMs;
    // Humanisation (docs/spec/mp-bots.md "Combat behaviour")
    int aimLagMs;        // the aim point follows the enemy with this lag
    int memoryMs;        // how long a lost enemy's last position is kept
    float hearDist;      // enemy weapon fire within this distance is heard
    float stepDist;      // an enemy's running steps within this distance are heard
    float grenadeChance; // chance to throw at a grenade opportunity
    bool cornerChecks;   // glances into open directions while roaming
};

const BotSkill& botSkill(int skill); // clamped to 0..3

struct Entity;

// A goal set through the API (World::mpBotSetGoal*, GSC botsetgoal): a point or an entity to reach within `radius`,
// optionally holding use there (a use trigger such as an SD bomb zone).
struct BotGoal {
    bool active = false;
    script::Vec3 pos{};
    int entity = -1;
    float radius = 64;
    bool use = false;
};

// What the bot goes for this frame (docs/spec/mp-bots.md, "Objectives").
struct BotObjective {
    bool valid = false;
    script::Vec3 pos{};
    float radius = 64;        // reached within this flat distance (and 72 units of height)
    bool use = false;         // hold use while touching useTarget
    Entity* useTarget = nullptr;
    const char* what = "";
};

// Menu weapons and their server cvars (docs/spec/mp-bots.md, "Weapon classes").
const std::vector<std::string>& mpWeaponClassNames();     // rifle, bolt, sniper, smg, mg, shotgun (the primary classes)
std::vector<std::string> mpMenuWeapons(const std::string& menu); // the weapons of weapon_<nation>
std::string mpWeaponAllowCvar(const std::string& weapon);  // scr_allow_* of a menu weapon
std::string mpMenuWeaponClass(const std::string& weapon);  // class of a menu weapon from the table ("" when not a menu weapon)
const std::vector<std::string>& mpWeaponNations();         // american, british, russian, german (weapon_<nation> menus)
std::string mpMenuPrimaryClasses(const std::string& nation); // the classes a nation's menu offers, comma separated

// A validated weapon list (the --weapons option, the menu panel): the primary classes allowed and whether grenades are.
struct MpWeaponSelection {
    std::vector<std::string> primaries; // canonical order
    bool grenades = true;
    std::string classesCvar;            // value for iw2_weaponclasses: the primaries, comma separated
};
// Parses a list of rifle, bolt, sniper, smg, mg, shotgun, grenades and all (comma or space separated, any case). Returns an
// empty string and fills `out`, or the error text: an unknown name, no primary class (grenades alone, none, empty), or a
// list that leaves a nation's weapon menu without a weapon.
std::string mpParseWeaponList(const std::string& text, MpWeaponSelection& out);

// Pure combat decisions (docs/spec/mp-bots.md "Combat behaviour"; src/game/mpbotcombat.cpp, exposed for tests).
struct BotStanceIn {
    std::string weaponClass; // rifle, smg, mg, sniper, shotgun, pistol
    float dist = 0;          // to the enemy
    bool fighting = false;   // a visible enemy and combat on
    bool moving = false;     // walking a path / objective or climbing
    bool coverCrouch = false; // crouching hides from the enemy while standing sees it
    bool holdFire = false;   // reloading or pausing between bursts
};
Stance botChooseStance(const BotStanceIn& in);
bool botWantAds(const std::string& weaponClass, float dist);
struct BotBurst { int shots; int pauseMs; };
BotBurst botBurst(const std::string& weaponClass, float dist, int skillBurst, int skillPauseMs);
// What the bot does with the weapons of its two slots (docs/spec/mp-bots.md "Ammunition and weapon choice").
struct BotWeaponIn {
    std::string heldClass, otherClass; // weapon classes (mpWeaponClass); otherClass "" = no second weapon
    int heldClip = 0, heldReserve = 0, heldClipSize = 1;
    int otherClip = 0, otherReserve = 0;
    float heldReloadTime = 2.0f; // seconds for a reload of the empty held weapon
    bool fighting = false;       // an enemy in sight
    float enemyDist = 1e9f;
    bool enemyKnown = false;     // an enemy remembered or suspected
    int quietMs = 0;             // since an enemy was last in sight
    bool heldPreferred = true;   // the held weapon is the one the bot wants to carry in its hands
    int sinceSwitchMs = 100000;  // since the bot last changed weapon
};
enum class BotWeaponAction { None, Reload, Switch };
BotWeaponAction botWeaponAction(const BotWeaponIn& in);
// The melee blow: an enemy in sight within reach (the blow's 64 units from the eye), the view on it.
bool botWantMelee(float eyeDist, float aimErrDeg, const std::string& weaponClass, int clip, bool busy);

// A weapon lying on the ground (docs/spec/mp-bots.md "Weapons from the ground"): does the bot want it, and how.
struct BotItemIn {
    std::string itemClass;   // class of the item's weapon ("" or "grenade": never wanted)
    bool owned = false;      // the bot has this weapon in a slot: walking over the item gives its ammunition
    int ownedReserve = 0, ownedMaxAmmo = 0;
    int ownedRounds = 0;     // clip + reserve of that weapon
    bool freeSlot = false;   // an empty weapon slot
    std::string slotClass[2]; // the classes of the two slot weapons ("" empty slot)
    int slotRounds[2] = {0, 0};
    int slotClipSize[2] = {1, 1};
};
struct BotItemWant {
    float desire = 0; // 0 not wanted .. 1; the bot leaves its way for an item within desire * 700 units
    bool use = false; // taken with the use button (a new weapon); false: by walking over it (ammunition)
    int replace = -1; // the slot (0 / 1) whose weapon the new one replaces, -1 none (a free slot)
};
BotItemWant botItemWant(const BotItemIn& in);

// A place the bot leaves its way for (docs/spec/mp-bots.md "Detours"): a weapon on the ground, the place of its last
// kill, a turret, cover.
struct BotDetour {
    enum Kind { None, Item, KillSpot, Turret, Cover } kind = None;
    int entity = -1;
    script::Vec3 pos{};
    float radius = 24;
    bool use = false;     // press use when the use search offers the entity
    std::string replace;  // Item: the slot weapon to hold when pressing use ("" any)
    int64_t sinceMs = 0, untilMs = 0; // given up at untilMs
    int turret = -1;      // Turret: index into BotSystem::turrets
};

// A mounted machine gun of the map as the bots know it (docs/spec/mp-bots.md "Turrets").
struct BotTurret {
    int entity = -1;
    script::Vec3 origin{};
    float centreYaw = 0, halfArc = 0; // the middle of its arcs (world yaw) and half their width, degrees
    int node = -1;                    // the graph node to use it from (-1: no bot can use it)
    script::Vec3 usePos{};
    int coverage = 0;                 // sampled graph nodes inside the arcs with a clear line from the gun
    int failures = 0;                 // bots that stood there and were not offered the turret
};
// Whether the point lies inside the turret's arcs (widened by `margin` degrees).
bool botTurretCovers(const BotTurret& t, const std::array<float, 3>& p, float margin);
// When a bot gets off a turret.
struct BotTurretIn {
    bool flanked = false;      // hit from a direction outside the arcs
    int health = 100;
    int enemyOutsideArcMs = 0; // an enemy in sight where the gun cannot turn to, for this long
    int quietMs = 0;           // since the bot got on or last saw an enemy
    int holdMs = 15000;        // how long it stays without seeing anybody
    int onMs = 0;              // since the bot got on
    int maxMs = 1000000;       // how long it stays at most
};
enum class BotTurretLeave { Stay, Flanked, Hurt, OutOfArc, Quiet };
BotTurretLeave botTurretLeave(const BotTurretIn& in);

// When a bot breaks off a fight for cover (docs/spec/mp-bots.md "Cover").
struct BotRetreatIn {
    int skill = 1;
    float aggression = 1; // the bot's trait (0.85..1.2)
    int health = 100;
    int hurtMs = 100000;  // since the last hit by an enemy
    bool enemyKnown = false;
    bool emptyClip = false; // the held weapon's clip is empty and it has a reserve (a reload is due or running)
    float enemyDist = 1e9f;
};
bool botWantRetreat(const BotRetreatIn& in);

// The view angles of a player's throw that lands at `to`: a grenade leaves along the view at `speed` with `speedUp` added
// upwards (docs/spec/grenades.md). The low arc, else (or with `high`) the high one; false when out of reach. `vel` is the
// velocity the grenade leaves with.
bool botGrenadeAngles(const std::array<float, 3>& from, const std::array<float, 3>& to, float speed, float speedUp, float gravity, bool high,
                      float& yawDeg, float& pitchDeg, std::array<float, 3>& vel);

// What a bot hears or feels (docs/spec/mp-bots.md "Hearing"); the order is the urgency: a later cue replaces a pending
// one only when it is at least as urgent.
enum class BotCue : int { Steps, Reload, Gunfire, GrenadeLanded, Explosion, NearMiss, Hit, Count };
inline int botCueUrgency(BotCue c) {
    switch (c) {
    case BotCue::Steps:
    case BotCue::Reload: return 1;     // quiet: not followed while another enemy is known
    case BotCue::Hit: return 3;        // turns the bot even in a fight
    default: return 2;
    }
}

// Per-bot counters for tests and the debug overlay.
struct MpBotStats {
    // shots: hand weapon shots (a shotgun blast is one); hits: those of them that hit an enemy (any pellet, any pass).
    int shots = 0, hits = 0, kills = 0, deaths = 0;
    // The same by the weapon class (rifle, bolt, sniper, smg, mg, shotgun, pistol: botClassIndex) and by the distance
    // to the enemy fought (under 500, 500..1000, over 1000 units).
    int classShots[7] = {}, classHits[7] = {};
    int bandShots[3] = {}, bandHits[3] = {};
    int grenades = 0, grenadeHits = 0, smokes = 0;
    int pistolSwitches = 0, adsShots = 0, crouchFrames = 0, proneFrames = 0, heard = 0, cornerChecks = 0;
    int stuckEvents = 0; // 20 s without moving 64 units (not fighting, not on the objective)
    int longestStillMs = 0; // longest spell within 64 units while not fighting and not on the objective
    int trapped = 0;     // stuck twice at the same spot with no node in straight reach: the bot killed itself
    int offPathReplans = 0;  // off the path (the next node out of reach in height, or far and not in straight reach): planned again
    int offGraphReplans = 0; // 3 s off the graph (no node within 32 units, not fighting): planned again
    // Measurement of the humanlike behaviour (docs/spec/mp-bots.md "Measurement"), in 50 ms frames or counts.
    int aliveFrames = 0;        // frames playing
    int defencelessFrames = 0;  // an enemy in sight and nothing to fight with: the held weapon's clip empty and the weapon
                                // neither reloading nor being changed, no melee and no turret
    int emptyFrames = 0;        // every slot weapon without a round (clip and reserve)
    int reloads = 0;            // reloads begun
    int weaponSwitches = 0;     // changes of the held weapon the bot chose
    int weaponPickups = 0, ammoPickups = 0; // weapons taken with use, ammunition taken by walking over an item
    int melees = 0, meleeHits = 0;
    int turretMounts = 0, turretFrames = 0, turretShots = 0, turretHits = 0, turretKills = 0;
    int acquired = 0;           // enemies noticed by sight
    int acquiredOutsideView = 0; // of those, outside the view the bot had then (its field of view at the current zoom)
    int retreats = 0;           // spells of falling back to cover when hurt
    int giveWays = 0;           // steps out of a team mate's way
    int heardCue[int(BotCue::Count)] = {}; // cues that made an enemy known, by kind
    int grenadeFlees = 0;       // runs from a live grenade that landed close by
};

// The bot's view (docs/spec/mp-bots.md "Perception"): the field of view of a player's screen at the current zoom.
// `fov43` is the horizontal field of view for a 4:3 picture (cg_fov 80, at the sights the weapon's adsZoomFov, adsFov of
// viewweapon.h); the picture is taken as 16:9, so the half angles are atan(tan(fov43 / 2) * 4 / 3) across and
// atan(tan(fov43 / 2) * 3 / 4) up and down.
struct BotView {
    float halfYawDeg = 0, halfPitchDeg = 0;
};
BotView botViewAngles(float fov43);
// The index of a weapon class in MpBotStats::classShots (rifle 0, bolt 1, sniper 2, smg 3, mg 4, shotgun 5, pistol 6; -1 other).
int botClassIndex(const std::string& weaponClass);
// The yaw and pitch (degrees) of `dir` relative to the view angles: {to the left +, down +} as view angles turn.
std::array<float, 2> botViewOffset(const std::array<float, 3>& dir, float viewYawDeg, float viewPitchDeg);
// Whether a direction lies inside the view.
bool botInView(const BotView& v, const std::array<float, 3>& dir, float viewYawDeg, float viewPitchDeg);
// How long an enemy in sight takes to be noticed (docs/spec/mp-bots.md "Perception").
struct BotNoticeIn {
    int baseMs = 300;   // the skill's notice time
    float ecc = 0;      // where in the view: 0 the middle .. 1 the edge
    float dist = 0;
    float fov43 = 80;   // the view's field of view (a zoomed view brings the target closer)
    bool targetMoving = false, targetCrouched = false, targetProne = false;
    bool targetFired = false; // a shot in the last 300 ms (the muzzle flash)
    bool alerted = false;     // the bot expects this enemy (known, heard or hit by it)
};
float botNoticeMs(const BotNoticeIn& in);
// The place a bot believes a sound came from: the source turned around the listener by `yawErrDeg` and its distance
// scaled by `distScale`.
std::array<float, 3> botHeardPosition(const std::array<float, 3>& listener, const std::array<float, 3>& source, float yawErrDeg,
                                      float distScale);
// The view's motion toward the wanted angles (docs/spec/mp-bots.md "Aim"): a spring of stiffness omega (rad/s) and
// damping ratio `damping`, the turn rate limited to maxRate (degrees/s), advanced by dt seconds.
struct BotAim {
    float yaw = 0, pitch = 0;            // the angles the bot steers (the view without the offsets below)
    float yawVel = 0, pitchVel = 0;      // degrees per second
    float noiseYaw = 0, noisePitch = 0;  // the hand's wandering error, degrees
    float offYaw = 0, offPitch = 0;      // what the view has on top of the steered angles: the error and the view kick
    int64_t onTargetSinceMs = 0;         // since the bot follows this target
};
void botAimStep(BotAim& a, float& yaw, float& pitch, float wantYaw, float wantPitch, float omega, float damping, float maxRate, float dt);
// The factor on the skill's aim error for the bot's state.
struct BotAimNoiseIn {
    bool moving = false;
    Stance stance = Stance::Stand;
    float adsFrac = 0;
    int hurtMs = 100000;   // since the last hit
    float turnRate = 0;    // the view's speed, degrees/s
    int onTargetMs = 0;    // on this target for
};
float botAimNoiseScale(const BotAimNoiseIn& in);
// How close (degrees) the view must be to the aimed point for the bot to pull the trigger: 1.3 half widths of a body at
// that distance (at least half a degree) times the skill's factor, half again as much for an automatic weapon up close.
float botFireTolerance(float dist, float skillFactor, bool sprayClose);

// A bot's own traits: every bot of a skill differs a little (factors around 1, drawn when the bot is added).
struct BotTrait {
    float reaction = 1; // reaction and notice times
    float aim = 1;      // aim error
    float aggression = 1; // how far it pushes, how low its health may fall before it retreats
};

// One spell of a bot that wanted to walk and stayed within 48 units for over a second (docs/spec/mp-bots.md "Stuck watch").
struct BotStall {
    int client = -1;
    std::array<float, 3> pos{};
    int ms = 0;
    // A second into the spell:
    int from = -1, to = -1;  // the nodes of the path's current link (-1: none)
    int flags = 0;           // that link's flags
    float fromDist = -1;     // flat distance to its start node
    bool blocked = false;    // the standing box does not get 12 units along the walk direction
    bool sampled = false;
    float travelled = 0;     // units walked during the spell (far more than 48: running to and fro)
    bool blockedByPlayer = false; // another playing client stood within 48 units when the spell ended
    std::string what;             // what the bot was walking to (its objective or detour, "" roaming)
};

struct MpBot {
    int client = -1;
    int skill = 1;
    PlayerState ps;
    bool psValid = false;
    int spawnCount = 0;   // the client's spawn count the movement state was restarted for
    float yaw = 0, pitch = 0;
    // Navigation
    std::vector<int> path;
    size_t pathIdx = 0;
    BotGoal goal;
    bool goalReached = false;
    std::string role = "auto";    // auto, attack, defend, hunt
    std::string weaponClass;      // preferred weapon class ("" = bot_weaponclass cvar, else any)
    std::string objective;        // what the last objective was (describe / tests)
    script::Vec3 planGoal{};      // the point the current path was planned to
    int64_t replanMs = 0;
    int64_t planMs = 0;           // when the last path was planned
    bool holdingUse = false;
    bool combat = true;
    bool jumpLatch = false;
    int eventsDone = 0; // player-state events already handed to the game (fall damage)
    script::Vec3 progressPos{};
    int64_t progressMs = 0;
    int stuck = 0;
    bool wantJump = false;
    int64_t traverseSinceMs = -1; // on a ladder / mantle link of the path since (navgen), -1 when not
    script::Vec3 traverseProgressPos{}; // where the traversal last moved the bot 8 units, and when
    int64_t traverseProgressMs = 0;
    // Enemy
    int enemy = -1; // entity number
    bool enemyVisible = false;
    int64_t enemySeenMs = -100000, reactAtMs = 0;
    script::Vec3 enemyLastPos{};  // where the bot believes the enemy (seen: its place; heard or hit by: a guess)
    script::Vec3 enemyVel{};      // the seen enemy's velocity
    int enemyParts = 0;           // the parts of the seen enemy with a clear line: 1 head, 2 chest, 4 legs
    bool enemyHeardOnly = false;  // the enemy is known from a sound or a hit, not seen yet
    std::vector<float> notice;    // per client: how far the bot is in noticing it (1 = noticed)
    std::vector<int64_t> noticeSeenMs;
    struct Alert {                // a sound or a hit the bot becomes aware of at atMs
        bool valid = false;
        int entity = -1;
        script::Vec3 pos{};
        int64_t atMs = 0;
        BotCue kind = BotCue::Steps;
    } alert;
    // A live frag grenade that landed close by (heard): the bot runs from it from dangerFromMs until it goes off.
    int dangerEnt = -1;
    int64_t dangerFromMs = 0, dangerUntilMs = 0;
    int64_t hurtMs = -100000;     // the last hit by an enemy
    int focusEnt = -1;            // the attacker a hit turned the bot to
    int64_t focusUntilMs = 0;
    BotTrait trait;
    float strafe = 1;
    int64_t strafeFlipMs = 0;
    // Weapon
    std::string clipWeapon;
    int clip = 0;
    int64_t nextFireMs = 0; // burst pauses and the reaction (the weapon's own cadence is the weapon state's)
    int burstShots = 0;
    bool firingShot = false; // within mpBotFireShot: a damage to an enemy marks the shot as a hit
    bool shotHit = false;
    // The player's weapon state machine (docs/spec/weapons.md): fireTime, rechamber, reload, raise / drop, semi-auto,
    // with the inventory's clip and reserve; restarted on every spawn.
    PlayerWeapon weapon;
    bool weaponWired = false;
    bool firePrev = false;  // the fire input of the previous frame (a semi-automatic trigger is released between shots)
    bool reloadPrev = false;
    bool meleePrev = false;
    int weaponStatePrev = 0; // the weapon state of the previous frame (the offhand prepare event is its entry into 13)
    // A solved grenade throw: the bot holds the offhand button until the weapon state machine has started the throw
    // (pull-back, then the throw event), and the grenade leaves with this velocity at the throw event
    // (docs/spec/mp-bots.md "Grenades", docs/spec/mp-hud.md "The recorded view state").
    struct PendingThrow {
        bool valid = false;
        bool smoke = false;
        std::string weapon;
        std::array<float, 3> velocity{};
        float yaw = 0, pitch = 0; // the view angles of the throw
        int64_t sinceMs = 0;
    } throwPending;
    // Lifecycle
    int joinStep = 0;
    int64_t joinAtMs = 0;
    int64_t deadSinceMs = -1;
    bool usePressed = false;
    int kills = 0, shots = 0;
    // Combat (docs/spec/mp-bots.md "Combat behaviour")
    MpBotStats stats;
    script::Vec3 aimPos{};
    bool aimValid = false;
    BotAim aim;
    RecoilKick kick;                 // the view kick of the bot's shots (docs/spec/weapons.md "Recoil")
    bool kickActive = false;
    int kickFrames = 0;
    script::Vec3 aimBoneOffset{};    // the aimed body part relative to the enemy's origin, from its pose
    int aimBoneEnt = -1, aimBonePart = 0;
    int64_t adsSinceMs = -1;
    bool forceAds = false; // test / debug: the bot holds its aim whenever it holds a weapon (as IW2_MP_BOT_ADS for all bots)
    bool coverCrouch = false;
    int64_t coverCheckMs = 0;
    int64_t pistolSinceMs = -1, noEnemySinceMs = 0;
    int64_t weaponSwitchMs = -100000; // the last weapon change the bot chose
    bool wantReload = false;          // botWeaponAction asked for a reload this frame
    std::string spawnWeapon;          // the weapon the bot chose in the menu (the one it prefers to hold)
    uint32_t buttons = 0;             // this frame's usercmd buttons (use 0x8, attack 0x1 on a turret)
    bool usePrev = false;             // use was pressed in the last frame's command (the next press needs a release)
    // Detours (docs/spec/mp-bots.md "Detours")
    BotDetour detour;
    bool noNewDetour = false;         // this frame no detour is begun (on a traversal)
    int64_t detourCheckMs = 0;
    // Places whose items the bot leaves alone for a while (a weapon it just laid down, an item it did not get to).
    std::vector<std::pair<script::Vec3, int64_t>> ignoreItems;
    script::Vec3 killPos{};           // where the bot's last kill fell (its weapon lies there)
    int64_t killMs = -100000;
    // Cover (docs/spec/mp-bots.md "Cover")
    int64_t coverCooldownMs = 0, coverSearchMs = 0, coverLeaveMs = 0;
    bool inCover = false;
    // Turrets (docs/spec/mp-bots.md "Turrets")
    bool onTurret = false;
    int64_t turretCheckMs = 0, turretCooldownMs = 0, turretSinceMs = 0;
    std::vector<int64_t> turretIgnoreUntilMs; // per turret: left alone until then (just left, or not reached)
    int turretHoldMs = 15000, turretShotBase = 0;
    bool turretFlanked = false;
    int64_t turretOutSinceMs = -1, turretBurstUntilMs = 0, turretPauseUntilMs = 0;
    int64_t nextGrenadeMs = 0, grenadeCheckMs = 0;
    bool smokeUsed = false;
    int64_t glanceUntilMs = 0, nextGlanceMs = 0;
    float glanceYaw = 0;
    float scanYaw = 0;            // where a standing bot looks (it looks around)
    int64_t scanNextMs = 0;
    std::array<float, 2> escapeDir{};
    std::array<float, 2> lastMoveDir{};  // the walk direction of the last frame (world, 0 0 standing): what a team mate reads
    std::array<float, 2> giveWayDir{};   // stepping out of a team mate's way (docs/spec/mp-bots.md "Giving way") until then
    int64_t giveWayUntilMs = 0;
    int64_t escapeUntilMs = 0;
    bool escapeCrouch = false;
    script::Vec3 anchorPos{};
    int noReachSpells = 0; // stuck spells in a row with no node in straight reach (reset on the graph)
    int64_t stillSinceMs = 0;
    int64_t anchorMs = 0;
    int64_t offGraphSinceMs = 0;
    script::Vec3 stallPos{};  // the spell being measured (BotStall)
    int64_t stallSinceMs = -1;
    float stallTravelled = 0;
    BotStall stallSample;
    // Measurement (mpBotMeasure)
    bool wasDefenceless = false;
    std::string lastSlots;
    int lastRounds = 0;
    std::vector<int> badStarts; // start nodes the bot got stuck on the way to here (cleared after 64 units) // the last time the bot was on the graph or fighting
};

// A weapon item on the ground as every bot may see it (refreshed twice a second).
struct BotItem {
    int entity = -1;
    script::Vec3 pos{};
    std::string weapon;
};

// The fields of a weapon file the bots read every frame, looked up once per weapon (docs/spec/mp-bots.md "Cost limits").
struct BotWeaponInfo {
    std::string cls;                 // weapon class (World::mpWeaponClass)
    std::string clipName, ammoName;
    int clipSize = 1;
    float reloadTime = 2.0f;         // of an empty weapon
    bool semi = false;               // one shot per trigger pull (semiAuto or boltAction)
    bool scope = false;              // an aim overlay (adsOverlayShader)
    float adsZoomFov = 80, adsZoomInFrac = 0, adsZoomOutFrac = 0, adsSpread = 0;
    float moveSpeedScale = 0;        // 0: none given
    HipSpread hip;
};

// What the bots' perception reads of every client, gathered once per server frame (docs/spec/mp-bots.md "Cost limits").
struct BotClientInfo {
    Entity* ent = nullptr;
    bool playing = false;     // active, alive, session state playing
    int team = 0;             // 0 none, 1 axis, 2 allies
    script::Vec3 origin{}, eye{};
    float speed = 0;          // flat speed
    int viewHeight = 60;      // 60 standing, 40 crouched, 11 prone
    bool ads = false;
};

struct BotSystem {
    std::vector<BotClientInfo> clients; // this frame's
    std::map<std::string, BotWeaponInfo> weaponInfo;
    std::vector<BotItem> items;
    int64_t itemsMs = -100000;
    std::vector<BotTurret> turrets;
    bool turretsBuilt = false;
    std::vector<std::pair<script::Vec3, int64_t>> smokes; // smoke grenades that went off: place and time
    std::optional<NavGraph> nav;
    // The PLAYER_MANTLE animations for the bots' pmove and the graph's mantle links (docs/spec/pmove.md "Mantle").
    std::array<std::optional<assets::XAnim>, 11> mantleAnims;
    MantleAnims mantle;
    bool mantleLoaded = false;
    std::vector<MpBot> bots;
    int skill = 1;
    int counter = 0;
    // Cost limits (docs/spec/mp-bots.md "Cost limits")
    int plansThisFrame = 0, maxPlansInAFrame = 0;
    std::map<uint64_t, int> linkFailures; // ladder / mantle / leap links that failed, by (from << 32 | to)
    int linksRemoved = 0;
    int64_t frameCount = 0;
    std::vector<BotStall> stalls; // finished spells, at most 65536
    int64_t walkMs = 0;           // time all bots together wanted to walk (not fighting, not on a traversal)
    double lastFrameUs = 0, maxFrameUs = 0, sumFrameUs = 0;
};

// Whether a team mate presses into the bot (docs/spec/mp-bots.md "Giving way"): it stands within 44 units (flat, heights
// within 60) and wants to walk towards the bot (its move direction within 60 degrees of the line to the bot). `toMe` is
// from the mate to the bot.
bool botPushedBy(const std::array<float, 2>& toMe, float dz, const std::array<float, 2>& mateMove);

// Converts a world-space move direction to the forward / right inputs for the view yaw (degrees), scaled so that the
// larger of the two is 1 (exposed for tests).
std::array<float, 2> botMoveInput(const std::array<float, 2>& dir, float yawDeg);
// Turns `from` towards `to` (degrees, shortest way) by at most `maxStep`.
float botTurn(float from, float to, float maxStep);

} // namespace iw2::game
