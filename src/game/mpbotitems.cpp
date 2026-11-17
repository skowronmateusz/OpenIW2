// Multiplayer bots: weapons from the ground (docs/spec/mp-bots.md "Weapons from the ground", "Detours"). An OpenIW2
// extension, implemented from the spec. A bot takes a weapon as a player does: it walks to the item, looks at it and
// presses use when the use search of its client offers the item (docs/spec/mp.md "Use search of every client");
// ammunition for a weapon it holds is taken by walking over the item (docs/spec/mp.md "Dropped weapons").
#include "game/mpbots.h"
#include "game/viewweapon.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

using script::Value;

namespace {
std::string lowerOf(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
float dist3(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

// The weapon items on the ground, for every bot (twice a second).
void World::mpBotRefreshItems() {
    if (timeMs_ - bots_->itemsMs < 500) return;
    bots_->itemsMs = timeMs_;
    bots_->items.clear();
    for (const auto& ep : entities_) {
        const Entity& e = *ep;
        if (e.obj->removed || e.isSpawner || e.classname.compare(0, 7, "weapon_") != 0) continue;
        const std::string w = lowerOf(e.classname.substr(7));
        if (!weapon(w) || lowerOf(weaponField(w, "weaponType")) == "grenade") continue;
        const Value o = e.obj->get("origin");
        if (!o.isVector()) continue;
        bots_->items.push_back({e.number, o.asVector(), w});
    }
}

// The items at a place are left alone by this bot for `ms`.
void World::mpBotIgnoreItems(MpBot& b, const Vec3& at, int ms) {
    auto& v = b.ignoreItems;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) { return timeMs_ >= e.second; }), v.end());
    if (v.size() >= 8) v.erase(v.begin());
    v.emplace_back(at, timeMs_ + ms);
}

// A detour the bot makes no way to (a second without 24 units of progress while walking to it): given up, and the
// item's place or the turret left alone (45 s / 60 s), so that the bot does not press against what is in between.
void World::mpBotDetourStuck(MpBot& b) {
    BotDetour& d = b.detour;
    if (d.kind == BotDetour::None) return;
    if (d.kind == BotDetour::Item || d.kind == BotDetour::KillSpot) mpBotIgnoreItems(b, d.pos, 45000);
    if (d.kind == BotDetour::Turret && size_t(d.turret) < b.turretIgnoreUntilMs.size()) b.turretIgnoreUntilMs[size_t(d.turret)] = timeMs_ + 60000;
    if (d.kind == BotDetour::Cover) b.coverCooldownMs = timeMs_ + 8000, b.inCover = false;
    d = {};
    b.path.clear();
    b.pathIdx = 0;
}

// What the bot wants from an item with the weapon `w` (botItemWant with the bot's inventory).
BotItemWant World::mpBotItemWant(Entity& self, const std::string& w) {
    Inventory& inv = inventory(self);
    BotItemIn in;
    in.itemClass = mpWeaponClass(w);
    in.owned = inv.owned.count(w) != 0;
    auto rounds = [&](const std::string& n) { return n.empty() ? 0 : inv.clip[weaponField(n, "clipName")] + inv.ammo[weaponField(n, "ammoName")]; };
    if (in.owned) {
        in.ownedReserve = inv.ammo[weaponField(w, "ammoName")];
        in.ownedMaxAmmo = weaponInt(w, "maxAmmo");
        in.ownedRounds = rounds(w);
    }
    for (int i = 0; i < 2; ++i) {
        const std::string& sw = inv.slot[size_t(i + 1)];
        if (sw.empty()) {
            in.freeSlot = true;
            continue;
        }
        in.slotClass[i] = mpWeaponClass(sw);
        in.slotRounds[i] = rounds(sw);
        in.slotClipSize[i] = std::max(1, weaponInt(sw, "clipSize"));
    }
    return botItemWant(in);
}

// Chooses and follows a detour to a weapon on the ground (twice a second while the bot has none). Returns true while a
// detour is on; `obj` is then the place to walk to.
bool World::mpBotItemDetour(MpBot& b, Entity& self, const Vec3& eye, bool busy, bool unarmed, BotObjective& obj) {
    const int64_t now = timeMs_;
    Inventory& inv = inventory(self);
    BotDetour& d = b.detour;
    const bool itemDetour = d.kind == BotDetour::Item || d.kind == BotDetour::KillSpot;
    if (itemDetour) {
        bool done = false;
        if (d.kind == BotDetour::Item) {
            Entity* it = entityByNumber(d.entity);
            const bool gone = !it || it->obj->removed || it->classname.compare(0, 7, "weapon_") != 0;
            if (gone) done = true; // taken (by the bot or someone else)
            else if (mpBotItemWant(self, lowerOf(it->classname.substr(7))).desire <= 0) done = true; // no longer wanted
            else d.pos = it->obj->get("origin").isVector() ? it->obj->get("origin").asVector() : d.pos;
        } else if (dist3(d.pos, b.ps.origin) < 70.0f) {
            done = true; // at the place of the kill: the item scan below takes over
            b.detourCheckMs = 0;
        }
        if (!done && now >= d.untilMs) { // not reached in time: leave it alone for a while
            mpBotIgnoreItems(b, d.pos, 45000);
            done = true;
        }
        if (done || (busy && !unarmed)) d = {};
    }
    if (b.detour.kind == BotDetour::None && !b.noNewDetour && !(busy && !unarmed) && now >= b.detourCheckMs) {
        b.detourCheckMs = now + 500 + (b.client * 37) % 100;
        mpBotRefreshItems();
        // The best item in the bot's view (the screen at the hip, docs/spec/mp-bots.md "Perception") with a clear line.
        const BotView view = botViewAngles(80.0f);
        float best = 0;
        for (const BotItem& it : bots_->items) {
            const float dist = dist3(it.pos, b.ps.origin);
            if (dist > 700.0f) continue;
            bool ignored = false;
            for (const auto& ig : b.ignoreItems)
                if (now < ig.second && dist3(it.pos, ig.first) < 48.0f) ignored = true;
            if (ignored) continue;
            const BotItemWant w = mpBotItemWant(self, it.weapon);
            if (w.desire <= 0 || dist > w.desire * 700.0f) continue;
            const float score = w.desire * (1.0f - dist / 800.0f);
            if (score <= best) continue;
            const Vec3 at{it.pos[0], it.pos[1], it.pos[2] + 8.0f};
            if (dist > 64.0f && !botInView(view, {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}, b.yaw, b.pitch)) continue;
            if (collision_->trace(eye, at, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, self.number).fraction < 0.98f) continue;
            // On the graph: a node within 48 units and 40 of height (an item on a roof or in a pit is out of reach).
            const int node = bots_->nav ? bots_->nav->nearest(it.pos, 48.0f, true) : -1;
            if (node < 0 || std::fabs(bots_->nav->nodes[size_t(node)][2] - it.pos[2]) > 40.0f) continue;
            // A team mate already on its way to this item (in a team game: team mates tell each other): left to that one.
            bool taken = false;
            if (mp_->gametype != "dm")
                for (const MpBot& o : bots_->bots)
                    if (o.client != b.client && o.detour.kind == BotDetour::Item && o.detour.entity == it.entity &&
                        size_t(o.client) < bots_->clients.size() && bots_->clients[size_t(o.client)].team == bots_->clients[size_t(b.client)].team)
                        taken = true;
            if (taken) continue;
            best = score;
            b.detour = {};
            b.detour.kind = BotDetour::Item;
            b.detour.entity = it.entity;
            b.detour.pos = it.pos;
            b.detour.use = w.use;
            b.detour.radius = w.use ? 26.0f : 8.0f;
            b.detour.replace = w.use && w.replace >= 0 ? inv.slot[size_t(w.replace + 1)] : std::string();
            b.detour.sinceMs = now;
            b.detour.untilMs = now + 4000 + int64_t(dist * 10.0f); // 4 s and the walk at 100 units/s
        }
        // Nothing in sight and a slot weapon without a round: to the place of the last kill of the past 30 s.
        if (b.detour.kind == BotDetour::None && now - b.killMs < 30000 && dist3(b.killPos, b.ps.origin) > 80.0f &&
            dist3(b.killPos, b.ps.origin) < 1500.0f) {
            bool need = false;
            for (int sl = 1; sl <= 2; ++sl)
                if (!inv.slot[size_t(sl)].empty() &&
                    inv.clip[weaponField(inv.slot[size_t(sl)], "clipName")] + inv.ammo[weaponField(inv.slot[size_t(sl)], "ammoName")] == 0)
                    need = true;
            // Only a place on the graph (a node within 64 units): the bot walks to that node.
            const int node = need && bots_->nav ? bots_->nav->nearest(b.killPos, 64.0f, true) : -1;
            if (node >= 0) {
                const auto& q = bots_->nav->nodes[size_t(node)];
                b.detour = {};
                b.detour.kind = BotDetour::KillSpot;
                b.detour.pos = {q[0], q[1], q[2]};
                b.detour.radius = 60.0f;
                b.detour.sinceMs = now;
                b.detour.untilMs = now + 6000 + int64_t(dist3(b.killPos, b.ps.origin) * 20.0f); // 6 s and 20 ms a unit (the way is not straight)
                b.killMs = -100000; // once
            }
        }
        if (b.detour.kind != BotDetour::None) b.path.clear(), b.pathIdx = 0;
    }
    if (b.detour.kind != BotDetour::Item && b.detour.kind != BotDetour::KillSpot) return false;
    obj = BotObjective{};
    obj.valid = true;
    obj.pos = b.detour.pos;
    obj.radius = b.detour.radius;
    obj.what = b.detour.kind == BotDetour::Item ? "pickup" : "kill spot";
    return true;
}

// A place near `from` that a threat at `threatEye` does not see: the nearest graph node 48..320 units away (within 60 of
// height) to which the lines from the threat's eye to the top of a crouched head, the knees and both shoulders there are blocked;
// one the bot reaches in a straight line before the others. -1 when there is none among the 24 nearest.
int World::mpBotFindCover(const Vec3& from, const Vec3& threatEye, int selfEntity) {
    const NavGraph& nav = mpNav();
    const size_t mainCount = nav.mainCount ? nav.mainCount : nav.size();
    int fallback = -1, tried = 0;
    for (int n : nav.within(from, 320.0f)) {
        if (size_t(n) >= mainCount) continue;
        const auto& q = nav.nodes[size_t(n)];
        if (dist3(q, from) < 48.0f || std::fabs(q[2] - from[2]) > 60.0f) continue;
        if (++tried > 24) break;
        if (collision_->trace(threatEye, Vec3{q[0], q[1], q[2] + 50.0f}, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, selfEntity).fraction >= 0.999f) continue;
        if (collision_->trace(threatEye, Vec3{q[0], q[1], q[2] + 18.0f}, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, selfEntity).fraction >= 0.999f) continue;
        // The whole crouched body is hidden, not only its middle: the shoulders 15 units to either side as well.
        const float tx = q[0] - threatEye[0], ty = q[1] - threatEye[1], tl = std::max(1.0f, std::hypot(tx, ty));
        const float sx = -ty / tl * 15.0f, sy = tx / tl * 15.0f;
        if (collision_->trace(threatEye, Vec3{q[0] + sx, q[1] + sy, q[2] + 46.0f}, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, selfEntity).fraction >= 0.999f) continue;
        if (collision_->trace(threatEye, Vec3{q[0] - sx, q[1] - sy, q[2] + 46.0f}, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, selfEntity).fraction >= 0.999f) continue;
        const TraceResult tr = collision_->trace(from, Vec3{q[0], q[1], std::max(q[2], from[2])}, {-15, -15, 18}, {15, 15, 60}, Contents::PlayerSolid, selfEntity);
        if (tr.fraction >= 1.0f && !tr.startSolid) return n;
        if (fallback < 0) fallback = n;
    }
    return fallback;
}

// Cover (docs/spec/mp-bots.md "Cover"): a hurt bot, or one with an empty clip, breaks off the fight for a place the
// enemy does not see, crouches there until it has reloaded and its health is back (at most 8 s), and comes out again.
// Returns true while such a detour is on; `obj` is then the place.
bool World::mpBotCoverDetour(MpBot& b, Entity& self, bool enemyVisible, float enemyDist, bool emptyClip, bool reloading, BotObjective& obj) {
    const int64_t now = timeMs_;
    BotDetour& d = b.detour;
    const int health = self.obj->get("health").isNumber() ? self.obj->get("health").asInt() : 100;
    if (d.kind == BotDetour::Cover) {
        const bool there = dist3(d.pos, b.ps.origin) < d.radius + 10.0f;
        if (there && !b.inCover) b.inCover = true, b.coverLeaveMs = now + 8000;
        bool leave = now >= d.untilMs && !there;                       // not reached in time
        if (b.inCover && now >= b.coverLeaveMs) leave = true;          // long enough
        if (b.inCover && health >= 75 && !emptyClip && !reloading) leave = true; // fit to fight again
        if (b.inCover && enemyVisible && now - d.sinceMs > 1500) leave = true;   // the enemy came round: no cover any more
        if (leave) {
            d = {};
            b.inCover = false;
            b.coverCooldownMs = now + 8000;
        }
    }
    if (d.kind != BotDetour::Cover && !b.noNewDetour && now >= b.coverCooldownMs && now >= b.coverSearchMs && b.enemy >= 0) {
        BotRetreatIn in;
        in.skill = b.skill;
        in.aggression = b.trait.aggression;
        in.health = health;
        in.hurtMs = int(std::min<int64_t>(now - b.hurtMs, 100000));
        in.enemyKnown = now - b.enemySeenMs < 4000;
        in.emptyClip = emptyClip && enemyVisible;
        in.enemyDist = enemyDist;
        if (botWantRetreat(in)) {
            b.coverSearchMs = now + 1000;
            const int node = mpBotFindCover(b.ps.origin, Vec3{b.enemyLastPos[0], b.enemyLastPos[1], b.enemyLastPos[2] + 64.0f}, self.number);
            if (node >= 0) {
                const auto& q = bots_->nav->nodes[size_t(node)];
                d = {};
                d.kind = BotDetour::Cover;
                d.pos = {q[0], q[1], q[2]};
                d.radius = 6.0f; // onto the node: cover ends a step beside it
                d.sinceMs = now;
                d.untilMs = now + 5000;
                b.inCover = false;
                b.path.clear();
                b.pathIdx = 0;
                ++b.stats.retreats;
            }
        }
    }
    if (d.kind != BotDetour::Cover) return false;
    obj = BotObjective{};
    obj.valid = true;
    obj.pos = d.pos;
    obj.radius = d.radius;
    obj.what = "cover";
    return true;
}

// At a weapon the bot wants to take with use: the weapon to hold (the one the new weapon replaces), and the use press
// once the use search of the bot's client offers the item. Returns true when use is pressed this frame.
bool World::mpBotItemUse(MpBot& b, Entity& self, std::string& weaponName) {
    const BotDetour& d = b.detour;
    if (d.kind != BotDetour::Item || !d.use) return false;
    const MpClient& cl = mp_->clients[size_t(b.client)];
    Inventory& inv = inventory(self);
    if (dist3(d.pos, b.ps.origin) > 120.0f) return false;
    // The new weapon takes the place of the held one (docs/spec/inventory.md): hold the one to give up.
    if (!d.replace.empty() && weaponName != d.replace && inv.owned.count(d.replace) && !cl.weaponDisabled) {
        self.obj->set("$weapon", Value(d.replace));
        weaponName = d.replace;
        b.clipWeapon = d.replace;
        b.weaponSwitchMs = timeMs_;
        return false;
    }
    if (cl.useEntity != d.entity || b.usePrev) return false;
    mpBotIgnoreItems(b, d.pos, 30000); // the weapon the bot leaves here is not taken back at once
    return true;
}

} // namespace iw2::game
