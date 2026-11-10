// Multiplayer bots (docs/spec/mp-bots.md): test clients driven by a controller that produces player input for pmove.
// An OpenIW2 extension: the original's test clients (addtestclient) never move.
#pragma once

#include "game/navgen.h"
#include "assets/xanim.h"
#include "game/playerweapon.h"
#include "game/pmove.h"
#include "script/value.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace iw2::game {

// Difficulty preset (docs/spec/mp-bots.md, "Skill presets").
struct BotSkill {
    const char* name;
    int reactionMs;      // delay from first sight of an enemy to the first shot
    float turnRate;      // view turn limit, degrees per second
    float accuracy;      // multiplies the weapon's accuracy graph (the AI's actor accuracy)
    float missOffset;    // how far a missed shot passes the target (units)
    float fovCos;        // cosine of the half view cone for noticing a new enemy
    float sightDist;     // farthest enemy noticed
    int burst;           // shots before a pause
    int burstPauseMs;
    // Humanisation (docs/spec/mp-bots.md "Combat behaviour")
    int aimLagMs;        // the aim point follows the enemy with this lag
    int memoryMs;        // how long a lost enemy's last position is kept
    float hearDist;      // enemy weapon fire within this distance is heard
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
// Launch velocity of a grenade from `from` to land at `to` at `speed` under `gravity`; the low arc, else (or with `high`)
// the high one. False when out of reach.
bool botGrenadeVelocity(const std::array<float, 3>& from, const std::array<float, 3>& to, float speed, float gravity, bool high,
                        std::array<float, 3>& vel);

// Per-bot counters for tests and the debug overlay.
struct MpBotStats {
    int shots = 0, hits = 0, kills = 0, deaths = 0;
    int grenades = 0, grenadeHits = 0, smokes = 0;
    int pistolSwitches = 0, adsShots = 0, crouchFrames = 0, proneFrames = 0, heard = 0, cornerChecks = 0;
    int stuckEvents = 0; // 20 s without moving 64 units (not fighting, not on the objective)
    int longestStillMs = 0; // longest spell within 64 units while not fighting and not on the objective
    int trapped = 0;     // stuck twice at the same spot with no node in straight reach: the bot killed itself
    int offPathReplans = 0;  // off the path (the next node out of reach in height, or far and not in straight reach): planned again
    int offGraphReplans = 0; // 3 s off the graph (no node within 32 units, not fighting): planned again
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
    // Enemy
    int enemy = -1; // entity number
    bool enemyVisible = false;
    int64_t enemySeenMs = -100000, reactAtMs = 0;
    script::Vec3 enemyLastPos{};
    float strafe = 1;
    int64_t strafeFlipMs = 0;
    // Weapon
    std::string clipWeapon;
    int clip = 0;
    int64_t nextFireMs = 0; // burst pauses and the reaction (the weapon's own cadence is the weapon state's)
    int burstShots = 0;
    // The player's weapon state machine (docs/spec/weapons.md): fireTime, rechamber, reload, raise / drop, semi-auto,
    // with the inventory's clip and reserve; restarted on every spawn.
    PlayerWeapon weapon;
    bool weaponWired = false;
    bool firePrev = false;  // the fire input of the previous frame (a semi-automatic trigger is released between shots)
    bool reloadPrev = false;
    int weaponStatePrev = 0; // the weapon state of the previous frame (the offhand prepare event is its entry into 13)
    // A solved grenade throw: the bot holds the offhand button until the weapon state machine has started the throw
    // (pull-back, then the throw event), and the grenade leaves with this velocity at the throw event
    // (docs/spec/mp-bots.md "Grenades", docs/spec/mp-hud.md "The recorded view state").
    struct PendingThrow {
        bool valid = false;
        bool smoke = false;
        std::string weapon;
        std::array<float, 3> velocity{};
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
    int64_t adsSinceMs = -1;
    bool coverCrouch = false;
    int64_t coverCheckMs = 0;
    int64_t pistolSinceMs = -1, noEnemySinceMs = 0;
    int64_t nextGrenadeMs = 0, grenadeCheckMs = 0;
    bool smokeUsed = false;
    int64_t glanceUntilMs = 0, nextGlanceMs = 0;
    float glanceYaw = 0;
    std::array<float, 2> escapeDir{};
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
    std::vector<int> badStarts; // start nodes the bot got stuck on the way to here (cleared after 64 units) // the last time the bot was on the graph or fighting
};

struct BotSystem {
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

// Converts a world-space move direction to the forward / right inputs for the view yaw (degrees), scaled so that the
// larger of the two is 1 (exposed for tests).
std::array<float, 2> botMoveInput(const std::array<float, 2>& dir, float yawDeg);
// Turns `from` towards `to` (degrees, shortest way) by at most `maxStep`.
float botTurn(float from, float to, float maxStep);

} // namespace iw2::game
