// Corpse limit, docs/spec/ai.md "Corpse limit": the slot picker's order (0x4fd0d0), the frame-end trim (0x4d9710), the cached count
// and the table across a save and load (0x4e524b), and that the animation fast paths change nothing. The world part needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/animtree.h"
#include "game/collision.h"
#include "game/corpses.h"
#include "game/savestate.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>
#include <map>

using namespace iw2::game;
using iw2::script::Value;
using iw2::script::Vec3;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static int pickFrom(const std::vector<Vec3>& at, int count = -1) {
    CorpseSlots s;
    s.setCount(count < 0 ? int(at.size()) : count);
    for (size_t i = 0; i < at.size(); ++i) s.put(int(i), int(i) + 100);
    return s.pick({0, 0, 0}, {1, 0, 0}, [&](int n) { return at[size_t(n - 100)]; });
}

static void picker() {
    CorpseSlots s;
    s.setCount(4);
    s.put(0, 7);
    s.put(2, 8);
    auto none = [](int) { return Vec3{0, 0, 0}; };
    CHECK(s.pick({0, 0, 0}, {1, 0, 0}, none) == 1); // the first free slot, whatever is around it
    // Behind beats everything, even a much farther corpse in front or off to the side; the farthest behind wins.
    CHECK(pickFrom({{5000, 0, 0}, {-10, 0, 0}, {0, 4000, 0}, {-20, 1, 0}}) == 3);
    // Exactly side on is dot 0, not behind: the off-view group.
    CHECK(pickFrom({{5000, 0, 0}, {0, 100, 0}, {0, 300, 0}}) == 2);
    // 45 degrees counts as off the view, a little inside does not.
    CHECK(pickFrom({{5000, 0, 0}, {100, 100, 0}}) == 1);
    CHECK(pickFrom({{5000, 100, 0}, {100, 99, 0}}) == 0);
    // All inside the cone: the farthest.
    CHECK(pickFrom({{100, 0, 0}, {900, 0, 0}, {500, 0, 0}}) == 1);
    // A tie keeps the lower slot.
    CHECK(pickFrom({{-100, 0, 0}, {-100, 0, 0}}) == 0);
    // Slots above the count are not looked at, and count 0 always gives slot 0.
    CHECK(pickFrom({{-10, 0, 0}, {-20, 0, 0}, {-900, 0, 0}}, 2) == 1);
    CHECK(pickFrom({{-10, 0, 0}, {-20, 0, 0}}, 0) == 0);
}

static void trim() {
    CorpseSlots s;
    s.setCount(6);
    for (int i : {0, 1, 3, 5}) s.put(i, 10 + i);
    CHECK(s.trim(6).empty());
    CHECK((s.trim(2) == std::vector<int>{13, 15})); // by slot index from the new count up
    CHECK(s.count() == 2 && s.used() == 2 && s.at(3) == -1 && s.at(0) == 10);
    CHECK(s.trim(10).empty() && s.count() == 10); // raising only moves the count
    s.release(10);
    CHECK(s.at(0) == -1 && s.used() == 1);
}

// The xanim cache and the dry-run skip: stepping keeps giving the same times as a second state on its own library, a tree used
// through another library resolves again, and without a notify name no fraction is reported and the state is untouched.
static void animation(iw2::fs::Vfs& vfs) {
    AnimLibrary libA(vfs), libB(vfs);
    const AnimTree* tree = libA.tree("generic_human");
    CHECK(tree != nullptr);
    if (!tree) return;
    int leaf = -1;
    for (size_t i = 0; i < tree->nodes().size() && leaf < 0; ++i)
        if (tree->nodes()[i].children.empty() && libA.anim(tree->nodes()[i].name)) leaf = int(i);
    CHECK(leaf >= 0);
    if (leaf < 0) return;
    AnimState a(*tree, libA), b(*tree, libB);
    AnimState::SetArgs args;
    a.set(leaf, args);
    b.set(leaf, args);
    const auto none = [](const std::string&, const std::string&) {};
    for (int i = 0; i < 40; ++i) {
        a.updateWeights(0.05f);
        b.updateWeights(0.05f);
        const auto before = a.snapshot();
        CHECK(a.noteFraction(0.05f) == 1.0f);
        CHECK(a.snapshot()[size_t(leaf)].time == before[size_t(leaf)].time);
        a.updateTruncated(0.05f, true, none);
        b.update(0.05f, none, false);
        CHECK(a.time(leaf) == b.time(leaf));
    }
    CHECK(a.time(leaf) > 0.0f);
}

static int corpseCount(World& w) {
    int n = 0;
    for (const auto& e : w.entities())
        if (e->isActor && !e->obj->removed && e->ai.corpse) ++n;
    return n;
}

static void world(iw2::fs::Vfs& vfs) {
    World w(vfs, "cityhall");
    w.prepareStart();
    CHECK(w.cvars["ai_corpseCount"] == "20"); // the deliberate openiw2 default
    CHECK(w.corpseSlots().count() == 20);
    w.cvars["ai_corpseCount"] = "4";
    w.frame();
    CHECK(w.corpseSlots().count() == 4);
    Entity* spawner = nullptr;
    for (const auto& e : w.entities())
        if (e->isSpawner && !e->obj->removed) { spawner = e.get(); break; }
    CHECK(spawner != nullptr && w.player() != nullptr);
    if (!spawner || !w.player()) return;
    std::vector<iw2::script::ObjectPtr> made;
    for (int i = 0; i < 7; ++i) {
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* a = w.debugSpawnActor(*spawner, true);
        CHECK(a != nullptr);
        if (!a) return;
        a->obj->set("origin", Value(Vec3{float(100 * i), 0, 0}));
        made.push_back(a->obj);
        w.debugMakeCorpse(*a);
        CHECK(corpseCount(w) == std::min(i + 1, 4));
    }
    for (const auto& o : made) CHECK(o->removed); // the conversion releases the script object (0x4fdd31)
    int live = 0;
    for (int i = 0; i < 4; ++i) {
        Entity* c = w.debugEntity(w.corpseSlots().at(i));
        CHECK(c != nullptr && !c->obj->removed && c->anim != nullptr);
        live += c && c->obj->get("origin").isVector();
    }
    CHECK(live == 4); // the corpse keeps its placement on the new object

    // What actors know about a corpse goes with its script object (the address can return for a new actor), and so do their references.
    {
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* watcher = w.debugSpawnActor(*spawner, true);
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* dead = w.debugSpawnActor(*spawner, true);
        CHECK(watcher && dead);
        if (!watcher || !dead) return;
        const iw2::script::Object* key = dead->obj.get();
        watcher->sentientInfo[key].lastKnownMs = 1;
        w.debugSetEnemy(*watcher, dead);
        CHECK(dead->attackers == 1);
        watcher->ai.followed = dead;
        watcher->ai.blocker = dead;
        watcher->ai.bumpedEnt = dead->number;
        w.debugMakeCorpse(*dead);
        CHECK(watcher->sentientInfo.count(key) == 0);
        CHECK(!watcher->obj->get("enemy").isDefined() && dead->attackers == 0); // 0x514dda
        {
            // A corpse has contents 0x4000000: only that bit hits it, bullets and moves pass (0x4fdc75).
            w.frame();
            const Vec3 o = dead->obj->get("origin").asVector();
            const Vec3 a{o[0] - 50, o[1], o[2] + 30}, b{o[0] + 50, o[1], o[2] + 30};
            const auto& col = w.debugCollision();
            CHECK(col.trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x4000000u).entity == dead->number);
            CHECK(col.trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x280e831u).entity != dead->number);
            CHECK(col.trace(a, b, {0, 0, 0}, {0, 0, 0}, 0x2820011u).entity != dead->number);
        }
        CHECK(!watcher->ai.followed && !watcher->ai.blocker && watcher->ai.bumpedEnt == 0x7ff);
        w.debugFree(*watcher);

        // A corpse a resumed thread deleted during its "death" notify stays freed: no new object.
        const auto obj = dead->obj;
        w.debugFree(*dead);
        w.debugReleaseCorpseObject(*dead);
        CHECK(dead->obj == obj && dead->obj->removed);
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* refill = w.debugSpawnActor(*spawner, true); // the slot it left is free again
        CHECK(refill != nullptr);
        if (!refill) return;
        w.debugMakeCorpse(*refill);
        CHECK(corpseCount(w) == 4);
    }

    // Any freed entity drops its own link and the links of its children; the child stays where it is (0x4d6720).
    {
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* parent = w.debugSpawnActor(*spawner, true);
        Entity& child = w.debugSpawn("script_origin");
        Entity& other = w.debugSpawn("script_origin");
        CHECK(parent != nullptr);
        if (!parent) return;
        child.obj->set("origin", Value(Vec3{1, 2, 3}));
        w.debugLink(child, *parent);
        MoverChannel& move = w.debugMoverPos(child);
        move.active = true;
        move.tr.type = 3;
        move.tr.startMs = w.timeMs();
        move.tr.durationMs = 1000;
        w.debugLink(*parent, other);
        w.debugFree(*parent);
        CHECK(!w.debugLinked(child) && !w.debugLinked(*parent));
        CHECK(!move.active && move.tr.type == 0); // the move ends, silently (0x532970)
        CHECK(child.obj->get("origin").asVector() == (Vec3{1, 2, 3}) && !child.obj->removed);
    }

    // Raised, then saved with four corpses, loaded with a lower dvar: 64 until the first frame end, then trimmed.
    w.cvars["ai_corpseCount"] = "10";
    w.frame();
    CHECK(w.corpseSlots().count() == 10 && corpseCount(w) == 4);
    std::array<int, kCorpseSlots> slots{};
    for (int i = 0; i < kCorpseSlots; ++i) slots[size_t(i)] = w.corpseSlots().at(i);
    SaveRequest req;
    req.name = "corpses";
    req.description = "corpses";
    LoadedSave ls;
    CHECK(parseSaveFile(buildSaveFile(w, req), ls).empty());
    World l(vfs, "cityhall");
    l.prepareStart();
    l.cvars["ai_corpseCount"] = "2";
    restoreSave(l, ls);
    CHECK(l.cvars["ai_corpseCount"] == "2"); // not taken back from the save
    CHECK(l.corpseSlots().count() == kCorpseSlots);
    for (int i = 0; i < kCorpseSlots; ++i) CHECK(l.corpseSlots().at(i) == slots[size_t(i)]);
    CHECK(corpseCount(l) == 4);
    l.frame();
    CHECK(l.corpseSlots().count() == 2 && corpseCount(l) == 2 && l.corpseSlots().used() == 2);

    // The sentient release: the team reads "dead", an actor fighting it is notified "enemy", and favoriteenemy goes (0x4fe170, 0x514dab).
    {
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* victim = w.debugSpawnActor(*spawner, true);
        Entity* foe = w.debugSpawnActor(*spawner, true);
        CHECK(victim && foe);
        if (!victim || !foe) return;
        victim->obj->set("team", Value(std::string("axis")));
        foe->obj->set("team", Value(std::string("allies")));
        victim->obj->set("health", Value(int32_t(0)));
        CHECK(w.debugIsSentient(*victim)); // dead by health, still a sentient until the release
        w.debugSetEnemy(*foe, victim);
        foe->obj->set("favoriteenemy", Value(victim->obj));
        CHECK(victim->attackers == 1);
        int notified = 0;
        const auto foeObj = foe->obj;
        w.vm().notifyHook = [&notified, foeObj](const iw2::script::ObjectPtr& o, const std::string& ev) { notified += o == foeObj && ev == "enemy"; };
        w.debugReleaseSentient(*victim);
        w.vm().notifyHook = nullptr;
        CHECK(notified == 1);
        CHECK(victim->obj->get("team").toString() == "dead");
        CHECK(!foe->obj->get("enemy").isDefined() && !foe->obj->get("favoriteenemy").isDefined() && victim->attackers == 0);
        CHECK(w.debugIsSentient(*victim));

        // A script variable and the actor and sentient fields do not survive the conversion; the entity table does (0x56f4a8, 0x4f9d00).
        victim->obj->set("mymark", Value(int32_t(1)));
        victim->obj->set("targetname", Value(std::string("corpse_lookup")));
        victim->obj->set("accuracy", Value(1.0f));
        w.debugMakeCorpse(*victim);
        CHECK(!w.debugIsSentient(*victim));
        CHECK(!victim->obj->removed && !victim->obj->get("mymark").isDefined() && !victim->obj->get("team").isDefined() &&
              !victim->obj->get("accuracy").isDefined());
        CHECK(victim->obj->get("targetname").toString() == "corpse_lookup" && victim->obj->get("origin").isVector() &&
              victim->obj->get("health").isDefined());
        w.debugFree(*foe);
        w.debugFree(*victim);
    }

    // A touch record queued at the frame end goes through the save (0x4e3d11 / 0x4e528c) and is delivered once; a freed toucher's is not saved.
    {
        spawner->obj->set("count", Value(int32_t(5)));
        Entity* toucher = w.debugSpawnActor(*spawner, true);
        Entity* gone = w.debugSpawnActor(*spawner, true);
        CHECK(toucher && gone);
        if (!toucher || !gone) return;
        w.debugQueueTouch(*spawner, *toucher);
        w.debugQueueTouch(*spawner, *gone);
        w.debugFree(*gone);
        SaveRequest treq;
        treq.name = "touch";
        treq.description = "touch";
        LoadedSave tls;
        CHECK(parseSaveFile(buildSaveFile(w, treq), tls).empty());
        World tl(vfs, "cityhall");
        tl.prepareStart();
        restoreSave(tl, tls);
        CHECK(tl.debugTouchCount() == 1);
        tl.debugDeliverTouches();
        CHECK(tl.debugTouchCount() == 0);
        w.debugDeliverTouches();
        w.debugFree(*toucher);
    }
}

// An old save has no table: an actor still in its death animscript is left to convert normally, a finished one takes a slot.
static void oldSave(iw2::fs::Vfs& vfs) {
    World w(vfs, "toujane");
    w.start();
    Entity* a = nullptr;
    for (int i = 0; i < 400 && !a; ++i) {
        w.frame();
        for (const auto& e : w.entities())
            if (e->isActor && !e->isSpawner && !e->obj->removed && e->animScriptThread && w.vm().isThreadAlive(e->animScriptThread)) a = e.get();
    }
    CHECK(a != nullptr);
    if (!a) return;
    a->obj->set("health", Value(int32_t(0)));
    a->ai.alive = false;
    a->ai.stack[size_t(a->ai.depth)] = AiDeath;
    const auto obj = a->obj;
    w.debugRestoreCorpsesOldSave();
    CHECK(!a->ai.corpse && a->obj == obj && !obj->removed);
    a->animScriptThread = 0;
    w.debugRestoreCorpsesOldSave();
    CHECK(a->ai.corpse && obj->removed);
}

int main() {
    picker();
    trim();
    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        animation(vfs);
        world(vfs);
        oldSave(vfs);
    } else {
        std::cout << "IW2_MAIN not set, world part skipped\n";
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
