// Soft-lock diagnostics for iw2game --report (docs/debug-flags.md): level scripts that wait for an AI group to clear
// (waittill_aigroupcleared, getAIGroupCount polls) while a spawner of the group can no longer spawn, and per-spawner spawn statistics.
// Debug tooling only: it reads the world and the VM and never changes them.
#pragma once

#include "game/world.h"

#include <map>
#include <string>
#include <vector>

namespace iw2::game {

// How a spawner with a positive count can still be started.
enum class SpawnerLife {
    Driven,  // a live thread runs on the spawner and calls dospawn / stalingradspawn (flood_think and the like)
    Pending, // no driver yet, but a trigger that targets it is still waited on, or another live thread names it
    Dead,    // nothing alive can ever spawn from it
};

struct SpawnerLiveness {
    int driverThreads = 0;       // threads whose frame has the spawner as self and calls dospawn / stalingradspawn
    int triggerWaiters = 0;      // threads waiting for "trigger" on an entity whose target is the spawner's targetname
    int referencingThreads = 0;  // other live threads that name the spawner's targetname / script_noteworthy as a string
};
SpawnerLife classifySpawner(const SpawnerLiveness& l);
// A group is a soft-lock when its spawnercount is positive and no spawner that still has a count can be started.
bool groupIsSoftlock(int spawnercount, const std::vector<SpawnerLife>& spawnersWithCount);

struct SpawnerReport {
    int number = 0;
    std::string targetname, noteworthy, aigroup;
    script::Vec3 origin{0, 0, 0};
    int count = 0, initialCount = 0, spawned = 0;
    std::map<std::string, int> refusals;
    std::string lastRefusal;
    int64_t lastRefusalMs = -1, lastSpawnMs = -1;
    bool flood = false;
    bool antispawn = false; // an antispawn trigger names this spawner's targetname: the script stops the flood on purpose when the player passes it
    SpawnerLiveness live;
    SpawnerLife life = SpawnerLife::Dead;
    std::string namedBy; // wait site of the first thread that names the spawner
    bool attempted = false; // dospawn was called at least once
};

struct AiGroupReport {
    std::string name;
    int spawnercount = 0, aicount = 0;
    std::vector<std::string> waitSites;   // "file:line function" of the threads waiting on the group
    std::vector<size_t> spawners;         // indexes into SoftlockReport::spawners: group spawners with count > 0
    int deadSpawners = 0;
    bool waited = false, softlock = false;
};

// A map-script thread waiting for "spawned" on a spawner that still has a count but that nothing alive can start.
struct SpawnWait {
    std::string targetname, site;
    script::Vec3 origin{0, 0, 0};
    int count = 0, waiters = 0;
    std::map<std::string, int> refusals;
};

struct LevelWait {
    std::string event, site;
    int count = 0;
    int otherRefs = 0;   // other live threads that name the event as a string
    bool freedObject = false;
};

struct SoftlockReport {
    std::vector<AiGroupReport> groups;
    std::vector<SpawnerReport> spawners;  // every spawner with a spawn attempt, plus every one in a group with a positive count
    std::vector<size_t> shortSpawners;    // flood spawners with count left that spawned fewer than their count and have no live driver
    int shortAntispawn = 0;               // like shortSpawners, but an antispawn trigger can stop the flood by design (not in shortSpawners)
    int shortLive = 0;                    // the same, with a live driver (still retrying / waiting for its AI to die)
    std::vector<SpawnWait> deadSpawnWaits; // "spawned" waits (outside maps/_spawner.gsc and maps/_load.gsc) on spawners that are Dead
    std::vector<LevelWait> levelWaits;    // level (flag / notify) waits in maps/*.gsc
    int untouchedSpawners = 0;            // spawners with a count that never saw a spawn attempt
    int deadSpawnWaitCount() const { int n = 0; for (const auto& d : deadSpawnWaits) n += d.waiters; return n; }
    int softlockGroups() const;           // waited groups that are soft-locks
    int unwaitedSoftlockGroups() const;
};

SoftlockReport analyzeSoftlocks(World& w);
// Report lines (one per line, no trailing newline); `full` adds the per-spawner list and the level waits.
std::vector<std::string> formatSoftlockReport(const SoftlockReport& r, bool full);

} // namespace iw2::game
