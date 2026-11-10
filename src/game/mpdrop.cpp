// Multiplayer dropped weapons (docs/spec/mp.md "Dropped weapons"): the dropItem builtin, the dropped-weapon slots of
// g_maxDroppedWeapons, and the weapon item touch / use of the non-local clients (bots and network players).
// Implemented from the spec; VAs are in the 1.3 Linux dedicated server.
#include "game/itempickup.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

float World::mpDropCvar(const std::string& name, float def) const {
    if (cvars.count(name)) return cvarFloat(name, def);
    return cvarFloat(lower(name), def);
}

// The slot of a new dropped weapon (0x8105ffa): the first empty one of the g_maxDroppedWeapons slots; with all in use, the
// item whose nearest playing client is farthest away is freed and its slot reused.
size_t World::mpDroppedSlot() {
    const int limit = std::clamp(int(mpDropCvar("g_maxDroppedWeapons", float(kMpMaxDroppedWeaponsDefault))), 1, 32);
    if (mpDropped_.size() != size_t(limit)) mpDropped_.resize(size_t(limit));
    for (size_t i = 0; i < mpDropped_.size(); ++i)
        if (!mpDropped_[i] || mpDropped_[i]->removed) return mpDropped_[i] = nullptr, i;
    size_t best = 0;
    float bestDist = -1.0f;
    for (size_t i = 0; i < mpDropped_.size(); ++i) {
        const Value io = mpDropped_[i]->get("origin");
        const Vec3 o = io.isVector() ? io.asVector() : Vec3{0, 0, 0};
        float nearest = 1e12f; // 0x5368d487
        for (const MpClient& cl : mp_->clients) {
            if (cl.state != MpClientState::Active || !cl.ent || cl.ent->obj->get("sessionstate").toString() != "playing") continue;
            const Vec3 p = cl.ent->obj->get("origin").asVector();
            const float d = (p[0] - o[0]) * (p[0] - o[0]) + (p[1] - o[1]) * (p[1] - o[1]) + (p[2] - o[2]) * (p[2] - o[2]);
            nearest = std::min(nearest, d);
        }
        if (nearest > bestDist) bestDist = nearest, best = i;
    }
    mpDropped_[best]->removed = true; // G_FreeEntity (0x811ee50)
    mpDropped_[best] = nullptr;
    return best;
}

// The player weapon drop (0x81064de) with Drop_Item (0x81063d4) and LaunchItem (0x81061cc).
Entity* World::mpDropWeapon(Entity& client, const std::string& weaponName, const std::string& tag) {
    const std::string w = lower(weaponName);
    if (!weapon(w)) return nullptr;
    Inventory& inv = inventory(client);
    if (!inv.owned.count(w)) return nullptr;
    int& stock = inv.ammo[weaponField(w, "ammoName")];
    int& clip = inv.clip[weaponField(w, "clipName")];
    // A clip-only weapon (def +0x340) with an empty clip is only taken away (0x8106581..0x81065c2).
    if (weaponInt(w, "clipOnly") != 0 && clip == 0) {
        takeWeapon(inv, w);
        return nullptr;
    }
    // Drop_Item: the item starts at the dropper's origin raised by half its box height (0x8106493..0x81064b2) and flies
    // along the dropper's yaw at g_dropForwardSpeed, upwards at g_dropUpSpeedBase + random() * g_dropUpSpeedRand.
    const Vec3 origin = client.obj->get("origin").asVector();
    float height = playerBoxHeight(); // the dropper's box height (maxs.z - mins.z)
    if (&client != player_) {
        // A non-local client's box follows its own stance (the view height target of its movement: 11 prone, 40 crouched).
        const MpPlayerInput* pi = mpPlayerInput(mpClientOf(client));
        const int vh = pi && pi->valid ? pi->move.viewHeightTarget : 60;
        height = vh == 11 ? 30.0f : vh == 40 ? 50.0f : 70.0f;
    }
    const Value av = client.obj->get("angles");
    const float yaw = (av.isVector() ? av.asVector()[1] : 0.0f) * float(M_PI) / 180.0f;
    const float fwd = mpDropCvar("g_dropForwardSpeed", 10.0f);
    Vec3 vel{std::cos(yaw) * fwd, std::sin(yaw) * fwd, 0.0f};
    vel[2] += mpDropCvar("g_dropUpSpeedBase", 10.0f) + randomRange(0.0f, 1.0f) * mpDropCvar("g_dropUpSpeedRand", 5.0f);
    const Vec3 start{origin[0], origin[1], origin[2] + height * 0.5f};

    const size_t slot = mpDroppedSlot();
    Entity& item = debugDropItem(w, start, vel); // LaunchItem: weapon_<name>, world model, TR_GRAVITY from now
    mpDropped_[slot] = item.obj;
    // The tag argument (default tag_weapon_right, 0x80fb0f8; 0x81068a1..0x81069d9): when the dropper's model has the tag the item
    // moves to the end of a trace from the box centre to the tag and takes the tag's angles; without it, the dropper's angles
    // (0x81069bf). The roll gains 90 degrees either way (0x81069d9).
    {
        Vec3 tagPos, tagAng;
        const bool haveTag = scriptTagFrame(client, lower(tag), tagPos, tagAng);
        Vec3 ang = haveTag ? tagAng : (av.isVector() ? av.asVector() : Vec3{0, 0, 0});
        ang[2] += 90.0f;
        item.obj->set("angles", Value(ang));
        if (haveTag) {
            const TraceResult tr = collision_->trace(start, tagPos, {-1, -1, -1}, {1, 1, 1}, Contents::PlayerClipMask, client.number);
            const Vec3 at{tr.endPos[0], tr.endPos[1], tr.endPos[2]};
            item.obj->set("origin", Value(at));
            item.obj->set("$trbase", Value(at));
        }
    }
    // The item spins while it falls: a linear angular trajectory from the launch (0x81069fe..0x8106a79).
    item.obj->set("$arate", Value(Vec3{randomRange(0.0f, 1.0f) * 50.0f, randomRange(0.0f, 1.0f) * 40.0f, randomRange(0.0f, 1.0f) * 60.0f}));
    item.obj->set("$abase", item.obj->get("angles"));
    item.obj->set("$atime", Value(float(timeMs_)));
    // The dropper is the item's owner (+0x90) until the think one second later (0x8106130) clears it.
    item.obj->set("$owner", Value(int32_t(client.number)));
    item.obj->set("$ownerUntil", Value(int32_t(timeMs_ + 1000)));
    // The player's stock and clip move onto the item (+0x1a0 / +0x1a8; none = -1) and are cleared (0x81065f9..0x8106862).
    item.obj->set("$ammoStock", Value(int32_t(stock != 0 ? stock : -1)));
    item.obj->set("$ammoClip", Value(int32_t(clip != 0 ? clip : -1)));
    // Without either the item cannot be touched (contents bit 0x200000 cleared, 0x810686c..0x810688a).
    if (stock == 0 && clip == 0) item.obj->set("$notouch", Value(true));
    stock = clip = 0;
    takeWeapon(inv, w);
    if (lower(client.obj->get("$weapon").toString()) == w) client.obj->set("$weapon", Value(std::string("none")));
    return &item;
}

// A non-local client's weapon items (G_TouchTriggers per client, Touch_Item 0x8105cac): touching takes what
// BG_CanItemBeGrabbed allows (ammunition of an owned weapon, a weapon for a free slot). `pressedOn` is the entity of this
// frame's press of use (the client's last use search, -1 without a press): on a weapon item it swaps the weapon like the
// local player's use, whether or not the client touches the item. The local player's HUD effects (pickup events, gm
// the pickup sound at the listener) are not raised for other clients; the game message goes to that client as its print
// command (letter f, 0x8105dbd: docs/spec/mp.md "Dropped weapons").
void World::mpClientItems(Entity& client, int pressedOn) {
    Entity* const saved = player_;
    const size_t events = pickupEvents_.size(), commands = clientCommands_.size();
    struct Restore { // the local player and its HUD side effects come back even when a script notify throws
        World& w; Entity* saved; size_t events, commands; int slot;
        ~Restore() {
            w.player_ = saved;
            w.remotePickup_ = false;
            w.pickupEvents_.resize(events);
            for (size_t k = commands; k < w.clientCommands_.size(); ++k)
                if (w.clientCommands_[k].name == "gm" && slot >= 0)
                    w.mp_->clientCommands.push_back({slot, "f \"" + w.clientCommands_[k].arg + "\""});
            w.clientCommands_.resize(commands);
        }
    } restore{*this, saved, events, commands, mpClientOf(client)};
    player_ = &client;
    remotePickup_ = true;
    const Vec3 po = client.obj->get("origin").asVector();
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& e = *entities_[i];
        if (e.obj->removed || e.classname.compare(0, 7, "weapon_") != 0 || e.obj->get("$notouch").truthy()) continue;
        const std::string w = lower(e.classname.substr(7));
        if (!weapon(w)) continue;
        const Value io = e.obj->get("origin");
        if (!io.isVector() || !playerTouchesItem(po, io.asVector())) continue;
        if (touchWeaponItem(e)) {
            e.obj->removed = true;
            continue;
        }
    }
    if (pressedOn < 0) return;
    if (Entity* e = entityByNumber(pressedOn); e && !e->obj->removed && e->classname.compare(0, 7, "weapon_") == 0)
        pickUpWeaponItem(*e);
}

void World::registerMpDropBuiltins() {
    // dropItem(name) (0x80fb05a): on a player only; a weapon name drops that weapon; returns the item or undefined.
    // The optional second argument names the tag the weapon leaves from (docs/spec/mp.md "Dropped weapons").
    vm_.registerBuiltin("dropitem", [this](script::CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || mpClientOf(*e) < 0) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        Entity* item = mpDropWeapon(*e, c.arg(0).toString(), c.argc() > 1 ? c.arg(1).toString() : std::string("tag_weapon_right"));
        return item ? Value(item->obj) : Value();
    });
}

} // namespace iw2::game
