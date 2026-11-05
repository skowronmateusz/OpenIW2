// Soft-lock diagnostics (softlock.h). The AI-group bookkeeping it reads is the level script's: level._ai_group[name] has
// spawnercount / aicount (maps/_spawner.gsc aigroup_*), and the waits are maps/_utility.gsc waittill_aigroupcleared and
// getAIGroupCount polls. A flood spawner is driven by a flood_think-style thread on the spawner (a thread that calls
// dospawn / stalingradspawn); the trigger that starts it is waited on until it fires.
#include "game/softlock.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace iw2::game {

using namespace script;

SpawnerLife classifySpawner(const SpawnerLiveness& l) {
    if (l.driverThreads > 0) return SpawnerLife::Driven;
    if (l.triggerWaiters > 0 || l.referencingThreads > 0) return SpawnerLife::Pending;
    return SpawnerLife::Dead;
}

bool groupIsSoftlock(int spawnercount, const std::vector<SpawnerLife>& spawnersWithCount) {
    if (spawnercount <= 0) return false;
    for (SpawnerLife l : spawnersWithCount)
        if (l != SpawnerLife::Dead) return false;
    return true;
}

int SoftlockReport::softlockGroups() const {
    int n = 0;
    for (const auto& g : groups) n += g.softlock && g.waited;
    return n;
}
int SoftlockReport::unwaitedSoftlockGroups() const {
    int n = 0;
    for (const auto& g : groups) n += g.softlock && !g.waited;
    return n;
}

namespace {

std::string strField(const ObjectPtr& o, const char* f) {
    const Value v = o->get(f);
    return v.isString() ? v.asString() : std::string();
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool isAiGroupFn(const std::string& n) { return n == "waittill_aigroupcleared" || n == "getAIGroupCount" || n == "getaigroupcount" || n == "getAIGroupAI" || n == "getaigroupai"; }

} // namespace

SoftlockReport analyzeSoftlocks(World& w) {
    SoftlockReport rep;
    VM& vm = w.vm();
    const auto threads = vm.threadInfos();
    const auto& stats = w.spawnerStats();

    // level._ai_group: name -> (spawnercount, aicount)
    std::map<std::string, std::pair<int, int>> groups;
    {
        const Value g = vm.level()->get("_ai_group");
        if (g.isArray())
            for (const auto& [k, v] : std::get<ArrayPtr>(g.v)->items)
                if (k.isString && v.isObject()) {
                    const Value sc = v.asObject()->get("spawnercount"), ac = v.asObject()->get("aicount");
                    groups[k.s] = {sc.isInt() ? sc.asInt() : 0, ac.isInt() ? ac.asInt() : 0};
                }
    }

    // trigger entities waited on for "trigger": target -> waiter count
    std::map<std::string, int> triggerWaiters;
    for (const auto& t : threads)
        if (t.state == ThreadState::WaitingNotify && t.waitObject && !t.waitObject->removed && t.waitEvent == "trigger") {
            const std::string tg = strField(t.waitObject, "target");
            if (!tg.empty()) ++triggerWaiters[tg];
        }
    // trigger entities that start flood spawners (targetname flood_spawner), by target
    std::set<std::string> floodTargets, antispawnTargets;
    for (const auto& e : w.entities()) {
        if (e->obj->removed) continue;
        const std::string tn = strField(e->obj, "targetname");
        if (tn == "flood_spawner") floodTargets.insert(strField(e->obj, "target"));
        if (tn == "antispawn") antispawnTargets.insert(strField(e->obj, "script_noteworthy"));
    }

    // Waits on groups: the group names a wait frame names, per thread.
    std::map<std::string, std::vector<std::string>> waitSites;
    std::set<uint64_t> aiGroupWaiterThreads;
    for (const auto& t : threads)
        for (size_t fi = 0; fi < t.frames.size(); ++fi) {
            const auto& fr = t.frames[fi];
            const bool wf = fr.function == "waittill_aigroupcleared";
            bool calls = false;
            for (const auto& c : fr.callees) calls = calls || isAiGroupFn(c);
            if (!wf && !(calls && fi + 1 == t.frames.size())) { // callers of the wait function are covered by the callee's frame; pollers are innermost
                if (!(calls && fi + 1 < t.frames.size() && t.frames[fi + 1].function == "waittill_aigroupcleared")) continue;
            }
            const std::string site = fr.file + ":" + std::to_string(fr.line) + " " + fr.function;
            for (const auto* names : {&fr.localStrings, &fr.literals})
                for (const auto& n : *names)
                    if (groups.count(n) && !contains(waitSites[n], site)) waitSites[n].push_back(site);
            aiGroupWaiterThreads.insert(t.id);
        }

    // Does any live thread, apart from the group waiters and the tracker functions, name this string?
    std::string firstRefSite;
    auto referencingThreads = [&](const std::string& name, uint64_t exceptSelfNumber) {
        int n = 0;
        if (name.empty()) return n;
        for (const auto& t : threads) {
            if (aiGroupWaiterThreads.count(t.id)) continue;
            bool hit = false;
            for (const auto& fr : t.frames) {
                if (fr.function.rfind("aigroup_", 0) == 0) continue;
                if (fr.self.isObject() && uint64_t(fr.self.asObject()->id) == exceptSelfNumber) continue;
                if (contains(fr.localStrings, name) || contains(fr.literals, name)) {
                    if (!hit && firstRefSite.empty()) firstRefSite = fr.file + ":" + std::to_string(fr.line) + " " + fr.function;
                    hit = true;
                }
            }
            n += hit;
        }
        return n;
    };

    auto liveOf = [&](Entity& e) {
        SpawnerLiveness l;
        const std::string tn = strField(e.obj, "targetname"), nw = strField(e.obj, "script_noteworthy");
        for (const auto& t : threads)
            for (const auto& fr : t.frames)
                if (fr.self.isObject() && fr.self.asObject() == e.obj && (contains(fr.callees, "dospawn") || contains(fr.callees, "stalingradspawn"))) {
                    ++l.driverThreads;
                    break;
                }
        if (!tn.empty()) {
            if (const auto it = triggerWaiters.find(tn); it != triggerWaiters.end()) l.triggerWaiters = it->second;
            l.referencingThreads = referencingThreads(tn, e.obj->id);
        }
        if (l.referencingThreads == 0 && !nw.empty() && nw != "flood") l.referencingThreads = referencingThreads(nw, e.obj->id);
        return l;
    };

    std::map<std::string, std::vector<size_t>> groupSpawners;
    for (const auto& ep : w.entities()) {
        Entity& e = *ep;
        if (!e.isSpawner || e.obj->removed) continue;
        const auto st = stats.find(e.number);
        const Value cv = e.obj->get("count");
        const int count = cv.isInt() ? cv.asInt() : 0;
        const std::string grp = strField(e.obj, "script_aigroup");
        const bool inLiveGroup = !grp.empty() && groups.count(grp) && count > 0;
        if (st == stats.end() && !inLiveGroup) {
            if (count > 0) ++rep.untouchedSpawners;
            continue;
        }
        SpawnerReport s;
        s.number = e.number;
        s.targetname = strField(e.obj, "targetname");
        s.noteworthy = strField(e.obj, "script_noteworthy");
        s.aigroup = grp;
        if (const Value o = e.obj->get("origin"); o.isVector()) s.origin = o.asVector();
        s.count = count;
        if (st != stats.end()) {
            s.spawned = st->second.spawned;
            s.initialCount = st->second.initialCount >= 0 ? st->second.initialCount : count + s.spawned;
            s.refusals = st->second.refusals;
            s.lastRefusal = st->second.lastRefusal;
            s.lastRefusalMs = st->second.lastRefusalMs;
            s.lastSpawnMs = st->second.lastSpawnMs;
            s.attempted = true;
        } else {
            s.initialCount = count;
        }
        const Value rc = e.obj->get("realcount");
        s.flood = rc.isDefined() || s.noteworthy.find("flood") != std::string::npos || floodTargets.count(s.targetname) > 0;
        firstRefSite.clear();
        s.antispawn = !s.targetname.empty() && antispawnTargets.count(s.targetname) > 0;
        s.live = liveOf(e);
        s.namedBy = firstRefSite;
        s.life = classifySpawner(s.live);
        rep.spawners.push_back(s);
        if (inLiveGroup) groupSpawners[grp].push_back(rep.spawners.size() - 1);
    }

    for (const auto& [name, sc] : groups) {
        AiGroupReport g;
        g.name = name;
        g.spawnercount = sc.first;
        g.aicount = sc.second;
        if (const auto it = waitSites.find(name); it != waitSites.end()) { g.waitSites = it->second; g.waited = true; }
        std::vector<SpawnerLife> lives;
        if (const auto it = groupSpawners.find(name); it != groupSpawners.end())
            for (size_t i : it->second) {
                g.spawners.push_back(i);
                lives.push_back(rep.spawners[i].life);
                g.deadSpawners += rep.spawners[i].life == SpawnerLife::Dead;
            }
        g.softlock = groupIsSoftlock(g.spawnercount, lives);
        rep.groups.push_back(std::move(g));
    }

    for (size_t i = 0; i < rep.spawners.size(); ++i) {
        const SpawnerReport& s = rep.spawners[i];
        if (!s.flood || !s.attempted || s.count <= 0 || s.spawned >= s.initialCount) continue;
        if (s.life == SpawnerLife::Driven) ++rep.shortLive;
        else if (s.antispawn) ++rep.shortAntispawn;
        else rep.shortSpawners.push_back(i);
    }

    // Map-script threads waiting for a spawner's "spawned" while no live thread can start the spawner (waittill_aiAndSpawners_dead and the like).
    {
        std::map<const Object*, Entity*> byObject;
        for (const auto& ep : w.entities())
            if (ep->isSpawner && !ep->obj->removed) byObject[ep->obj.get()] = ep.get();
        std::map<std::pair<const Object*, std::string>, int> waits;
        for (const auto& t : threads) {
            if (t.state != ThreadState::WaitingNotify || t.waitEvent != "spawned" || !t.waitObject || t.frames.empty()) continue;
            const auto& fr = t.frames.back();
            // the library's own prethink / override threads sit on every spawner
            if (fr.file == "maps/_spawner.gsc" || fr.file == "maps/_load.gsc" || fr.file.rfind("animscripts/", 0) == 0) continue;
            if (byObject.count(t.waitObject.get())) ++waits[{t.waitObject.get(), fr.file + ":" + std::to_string(fr.line) + " " + fr.function}];
        }
        for (const auto& [key, n] : waits) {
            Entity& e = *byObject[key.first];
            const Value cv = e.obj->get("count");
            if (!cv.isInt() || cv.asInt() <= 0) continue;
            // only a spawner whose spawn was refused and never retried: one nothing has tried yet may still be started by a script
            // that holds it in an array (not visible here)
            const auto st = stats.find(e.number);
            if (st == stats.end() || st->second.refusals.empty()) continue;
            firstRefSite.clear();
            if (classifySpawner(liveOf(e)) != SpawnerLife::Dead) continue;
            SpawnWait d;
            d.targetname = strField(e.obj, "targetname");
            d.site = key.second;
            if (const Value o = e.obj->get("origin"); o.isVector()) d.origin = o.asVector();
            d.count = cv.asInt();
            d.waiters = n;
            d.refusals = st->second.refusals;
            rep.deadSpawnWaits.push_back(std::move(d));
        }
    }

    // Level (flag / notify) waits in the map and library scripts: the event, the wait site, and how many other threads name the event.
    std::map<std::pair<std::string, std::string>, LevelWait> lw;
    for (const auto& t : threads) {
        if (t.state != ThreadState::WaitingNotify || !t.waitObject || t.frames.empty()) continue;
        const auto& fr = t.frames.back();
        if (fr.file.rfind("maps/", 0) != 0) continue;
        const bool isLevel = t.waitObject == vm.level();
        if (!isLevel && !t.waitObject->removed) continue;
        LevelWait& x = lw[{t.waitEvent, fr.file + ":" + std::to_string(fr.line) + " " + fr.function}];
        x.event = t.waitEvent;
        x.site = fr.file + ":" + std::to_string(fr.line) + " " + fr.function;
        ++x.count;
        x.freedObject = x.freedObject || t.waitObject->removed;
    }
    for (auto& [k, x] : lw) {
        for (const auto& t : threads) {
            if (t.state == ThreadState::WaitingNotify && t.waitEvent == x.event && !t.frames.empty() && t.waitObject && t.waitObject->kind == "level") continue;
            bool hit = false;
            for (const auto& fr : t.frames) hit = hit || contains(fr.localStrings, x.event) || contains(fr.literals, x.event);
            x.otherRefs += hit;
        }
        rep.levelWaits.push_back(x);
    }
    return rep;
}

namespace {

std::string fmtMs(int64_t ms) {
    if (ms < 0) return "-";
    char b[32];
    std::snprintf(b, sizeof b, "%.1fs", double(ms) / 1000.0);
    return b;
}

const char* lifeName(SpawnerLife l) { return l == SpawnerLife::Driven ? "driven" : l == SpawnerLife::Pending ? "pending" : "DEAD"; }

std::string spawnerLine(const SpawnerReport& s) {
    char b[256];
    std::snprintf(b, sizeof b, "targetname=%s origin=(%g %g %g) count=%d initial=%d spawned=%d", s.targetname.empty() ? "<none>" : s.targetname.c_str(),
                  double(s.origin[0]), double(s.origin[1]), double(s.origin[2]), s.count, s.initialCount, s.spawned);
    std::string out = b;
    if (s.flood) out += " flood";
    if (s.antispawn) out += " antispawn-trigger";
    out += std::string(" ") + lifeName(s.life) + " (drivers " + std::to_string(s.live.driverThreads) + ", trigger waiters " + std::to_string(s.live.triggerWaiters) +
           ", named by " + std::to_string(s.live.referencingThreads) + " threads" +
           (s.namedBy.empty() ? std::string() : ", first " + s.namedBy) + ")";
    if (!s.refusals.empty()) {
        out += " refused:";
        for (const auto& [r, n] : s.refusals) out += " " + std::to_string(n) + "x[" + r + "]";
        out += " last [" + s.lastRefusal + "] at " + fmtMs(s.lastRefusalMs);
    }
    if (s.lastSpawnMs >= 0) out += " last spawn " + fmtMs(s.lastSpawnMs);
    return out;
}

} // namespace

std::vector<std::string> formatSoftlockReport(const SoftlockReport& r, bool full) {
    std::vector<std::string> out;
    for (const auto& g : r.groups) {
        if (!g.waited && !g.softlock && !full) continue;
        out.push_back(std::string(g.softlock ? "SOFTLOCK " : "group ") + g.name + " spawnercount=" + std::to_string(g.spawnercount) + " aicount=" + std::to_string(g.aicount) +
                      (g.waited ? " waited" : " unwaited") + (g.softlock && !g.waited ? " (no live wait yet)" : ""));
        for (const auto& s : g.waitSites) out.push_back("    wait " + s);
        if (g.spawnercount > 0)
            for (size_t i : g.spawners) out.push_back("    spawner " + spawnerLine(r.spawners[i]));
        if (g.softlock && g.spawners.empty()) out.push_back("    no spawner with a count is left in the group, but spawnercount is positive");
    }
    for (size_t i : r.shortSpawners) out.push_back("SHORT-SPAWNER " + spawnerLine(r.spawners[i]));
    for (const auto& d : r.deadSpawnWaits) {
        char b[200];
        std::snprintf(b, sizeof b, "DEAD-SPAWN-WAIT x%d targetname=%s origin=(%g %g %g) count=%d ", d.waiters, d.targetname.empty() ? "<none>" : d.targetname.c_str(),
                      double(d.origin[0]), double(d.origin[1]), double(d.origin[2]), d.count);
        std::string line = std::string(b) + "waited at " + d.site;
        if (!d.refusals.empty()) {
            line += " refused:";
            for (const auto& [rs, n] : d.refusals) line += " " + std::to_string(n) + "x[" + rs + "]";
        }
        out.push_back(line);
    }
    if (full) {
        for (size_t i = 0; i < r.spawners.size(); ++i)
            out.push_back("spawner " + spawnerLine(r.spawners[i]) + (r.spawners[i].aigroup.empty() ? "" : " group=" + r.spawners[i].aigroup));
        for (const auto& x : r.levelWaits)
            out.push_back("levelwait x" + std::to_string(x.count) + " " + x.site + " event=\"" + x.event + "\" named by " + std::to_string(x.otherRefs) + " other threads" +
                          (x.freedObject ? " FREED-OBJECT" : x.otherRefs == 0 ? " no-other-reference" : ""));
    }
    return out;
}

} // namespace iw2::game
