// Third-person multiplayer players: the animation driver on the "multiplayer" tree and the weapon in the hands.
// Implemented from the spec in docs/spec/mp-players.md. The original keeps this state in the client game; this
// offline port keeps it on the server's client entity (its AnimState steps with every other entity in World::frame).

#include "game/mpplayers.h"
#include "game/world.h"
#include "util/log.h"

#include <cmath>

namespace iw2::game {

using script::Value;

namespace {

constexpr float kDefaultBlend = 0.2f; // TODO(spec): the blend time of a command without "blendtime"

int animLengthMs(AnimLibrary& lib, const std::string& name) {
    const assets::XAnim* a = lib.anim(name);
    return a ? int(a->length() * 1000.0f + 0.5f) : 0;
}

} // namespace

const PlayerAnimScript* World::mpAnimScript() {
    if (!mp_) return nullptr;
    MpAnimData& d = mp_->animData;
    if (!d.loaded) {
        d.loaded = true;
        if (const auto bytes = vfs_.read("mp/playeranim.script")) {
            auto s = std::make_unique<PlayerAnimScript>();
            std::string err;
            if (s->parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err))
                d.script = std::move(s);
            else
                mp_->prints.push_back("mp/playeranim.script: " + err);
        }
    }
    return d.script.get();
}

PaContext World::mpAnimContext(int client) {
    MpClient& cl = mp_->clients[size_t(client)];
    PaContext ctx;
    const std::string weapon = cl.ent ? cl.ent->obj->get("$weapon").toString() : std::string();
    if (!weapon.empty() && weapon != "none") {
        const std::string type = weaponField(weapon, "playerAnimType");
        ctx.playerAnimType = type.empty() ? "none" : type;
        ctx.weaponClass = weaponField(weapon, "weaponClass");
        for (char& c : ctx.playerAnimType) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : ctx.weaponClass) c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    PaMovement mv = cl.animInput.move;
    if (!cl.animInput.valid && cl.ent) { // no mover filled the input: the entity's velocity, standing, forward
        mv = PaMovement{};
        mv.xySpeed = std::sqrt(cl.ent->velocity[0] * cl.ent->velocity[0] + cl.ent->velocity[1] * cl.ent->velocity[1]);
        mv.forwardMove = mv.xySpeed > 0 ? 1.0f : 0.0f;
    }
    mv.damageTimerMs = cl.damageTimerMs;
    mv.damageDurationMs = cl.damageDurationMs;
    mv.stumbleTimeMs = int(cvarFloat("player_dmgtimer_stumbleTime", 500.0f));
    mv.flinchYaw = cl.flinchYaw;
    // Ladder and mantle (docs/spec/mp-players.md "Ladder and mantle move types"). The server's clients: from the recorded
    // view state their mover fills every move. A network client's puppet: the animation the server sends in legsAnim.
    if (!mpRemote_) {
        mv.climb = cl.view.climb;
        if (cl.view.mantling)
            if (const MantleAnims* m = mpMantleAnims()) {
                PlayerState ps;
                ps.mantling = true;
                ps.mantleTrans = cl.view.mantleTrans;
                ps.mantleTimeMs = cl.view.mantleTimeMs;
                mv.mantleAnim = mantleAnimIndex(ps, *m);
            }
        ctx.holdMoveType = cl.view.climb != 0 && cl.view.climbHold;
    }
    ctx.moveType = paMoveType(mv);
    if (mpRemote_ && cl.animInput.serverLegsNode > 0)
        if (const PlayerAnimScript* script = mpAnimScript())
            if (const AnimTree* tree = animLib_.tree("multiplayer"); tree && size_t(cl.animInput.serverLegsNode) < tree->nodes().size()) {
                // The climb or mantle move type whose animation the server plays (the one the entity state names).
                const std::string& anim = tree->nodes()[size_t(cl.animInput.serverLegsNode)].name;
                static const char* const kTypes[] = {"climbup", "climbdown", "mantle_up_57", "mantle_up_51", "mantle_up_45", "mantle_up_39",
                                                     "mantle_up_33", "mantle_up_27", "mantle_up_21", "mantle_over_high", "mantle_over_mid",
                                                     "mantle_over_low"};
                for (const char* t : kTypes) {
                    PaContext probe = ctx;
                    probe.moveType = t;
                    const PaItem* it = script->animation(t, probe);
                    bool plays = false;
                    for (const PaCommand& c : it ? it->commands : std::vector<PaCommand>{})
                        for (const PaPart& p : c.parts) plays |= p.body != PaBody::Torso && p.anim == anim;
                    if (plays) {
                        ctx.moveType = t;
                        break;
                    }
                }
            }
    ctx.strafing = paStrafing(mv);
    ctx.weaponPosition = cl.animInput.ads ? "ads" : "hip";
    return ctx;
}

int World::mpPlayCommand(Entity& e, const PaItem& item, bool restart) {
    if (item.commands.empty() || !e.anim) return -1;
    MpAnimData& d = mp_->animData;
    d.seed = d.seed * 1103515245u + 12345u;
    const PaCommand& cmd = item.commands[size_t((d.seed >> 16) % item.commands.size())];
    const AnimTree& tree = e.anim->tree();
    const int torso = tree.find("torso");
    const float blend = cmd.blendMs >= 0 ? float(cmd.blendMs) / 1000.0f : kDefaultBlend;
    int duration = cmd.durationMs;
    const int64_t now = timeMs();
    MpPlayerAnim* st = nullptr;
    if (const int c = mpClientOf(e); c >= 0) st = &mp_->clients[size_t(c)].anim;
    for (const PaPart& p : cmd.parts) {
        const int n = tree.find(p.anim);
        if (n < 0) continue; // a name the tree lacks plays nothing
        AnimState::SetArgs a;
        a.blendTime = blend;
        a.knob = true;
        a.restart = restart;
        e.anim->set(n, a);
        const int len = duration >= 0 ? duration : animLengthMs(animLib_, p.anim);
        if (p.body != PaBody::Torso && st) st->legsNode = n;
        if (p.body == PaBody::Torso) {
            if (st) st->torsoHeldUntilMs = now + len, st->torsoActive = true;
        } else if (p.body == PaBody::Both && (!st || now >= st->torsoHeldUntilMs)) {
            e.anim->clear(torso, blend);
            if (st) st->torsoActive = false;
        }
        if (duration < 0) duration = len;
    }
    return duration;
}

bool World::mpPlayerDrawn(const Entity& e) const {
    if (!mp_ || !e.isPlayer || (&e == player_ && !killcamSwapped_)) return false; // the killcam draws the local body too
    const int c = mpClientOf(e);
    if (c < 0 || mp_->clients[size_t(c)].state != MpClientState::Active) return false;
    // The client the local spectator follows is the view itself (its state replaces the spectator's, docs/spec/mp-spectate.md).
    if (c == mpFollowing(mp_->localClient)) return false;
    return e.obj->get("sessionstate").toString() == "playing";
}

std::string World::mpPlayerWeaponModel(const Entity& e) const {
    if (!mp_) return {};
    const int c = mpClientOf(e);
    return c < 0 ? std::string() : mp_->clients[size_t(c)].anim.weaponModel;
}

void World::mpPlayersFrame() {
    const PlayerAnimScript* script = mpAnimScript();
    const AnimTree* tree = animLib_.tree("multiplayer");
    if (!script || !tree) return;
    const int torso = tree->find("torso");
    const int64_t now = timeMs();
    for (size_t ci = 0; ci < mp_->clients.size(); ++ci) {
        MpClient& cl = mp_->clients[ci];
        Entity* e = cl.ent;
        if (cl.state != MpClientState::Active || !e) continue; // the local client too: its body is drawn in the killcam
        MpPlayerAnim& st = cl.anim;
        if (e->obj->get("sessionstate").toString() != "playing") {
            st.legsItem = nullptr; // a new life starts from its move-type animation
            st.legsNode = 0;
            st.legsHeldUntilMs = st.torsoHeldUntilMs = 0;
            cl.animInput = MpPlayerInput{};
            cl.damageTimerMs = 0;
            continue;
        }
        cl.damageTimerMs = std::max(cl.damageTimerMs - 50, 0); // decays one server frame (ps+0x90 in the move, 0x80e2700)
        if (!e->anim || &e->anim->tree() != tree) {
            e->anim = std::make_unique<AnimState>(*tree, animLib_);
            st.legsItem = nullptr;
        }
        const std::string weapon = e->obj->get("$weapon").toString();
        st.weaponModel = weapon.empty() || weapon == "none" ? std::string() : weaponField(weapon, "worldModel");
        if (e == player_) cl.animInput.ads = playerCmd_.adsFrac >= 0.5f; // the local client's aim (its body is drawn in the killcam)
        PaContext ctx = mpAnimContext(int(ci));
        // Just after a jump on a ladder the footsteps step chooses nothing: the playing move type goes on.
        if (ctx.holdMoveType && !st.moveType.empty()) ctx.moveType = st.moveType;
        st.moveType = ctx.moveType;
        // Events of this frame (docs/spec/mp-players.md, "Events").
        auto event = [&](const char* name) {
            if (const PaItem* it = script->event(name, ctx)) {
                if (static const bool rt = std::getenv("IW2_REMOTE_TRACE") != nullptr; rt) // debug: the event's animations (docs/debug-flags.md)
                    for (const auto& c : it->commands)
                        for (const auto& p : c.parts)
                            log::tracef("remote", "remotetrace t=%lld client=%zu animevent=%s body=%d anim=%s\n", (long long)now, ci, name, int(p.body), p.anim.c_str());
                bool legs = false;
                for (const auto& c : it->commands)
                    for (const auto& p : c.parts) legs |= p.body != PaBody::Torso;
                const int d = mpPlayCommand(*e, *it, true);
                if (legs) st.legsHeldUntilMs = now + std::max(d, 0), st.legsItem = nullptr;
            }
        };
        if (cl.animInput.jumped) event("jump");
        if (cl.animInput.reloaded) event("reload");
        if (cl.animInput.fired) {
            ctx.firing = true;
            event("fireweapon");
        }
        if (cl.animInput.shocked) event("shellshock");
        cl.animInput.fired = cl.animInput.reloaded = cl.animInput.jumped = cl.animInput.shocked = false;
        // The torso falls back to the legs' animation when its hold ends.
        if (st.torsoActive && now >= st.torsoHeldUntilMs) {
            e->anim->clear(torso, kDefaultBlend);
            st.torsoActive = false;
        }
        // The move type's animation, re-chosen when its item changes.
        // A move-type animation waits for the legs' hold to run out to its last 49 ms (0x80d913f).
        if (now + 49 >= st.legsHeldUntilMs) {
            const PaItem* it = script->animation(ctx.moveType, ctx);
            if (it && it != st.legsItem) {
                mpPlayCommand(*e, *it, false);
                st.legsItem = it;
            }
        }
    }
}

} // namespace iw2::game

namespace iw2::game {

int World::mpDeathAnim(Entity& e) {
    const int c = mpClientOf(e);
    const PlayerAnimScript* script = mpAnimScript();
    if (c < 0 || !script) return -1;
    MpPlayerAnim& st = mp_->clients[size_t(c)].anim;
    st.deathAnim.clear();
    const PaItem* it = script->event("death", mpAnimContext(c));
    if (!it || it->commands.empty()) return -1;
    MpAnimData& d = mp_->animData;
    d.seed = d.seed * 1103515245u + 12345u;
    const PaCommand& cmd = it->commands[size_t((d.seed >> 16) % it->commands.size())];
    if (cmd.parts.empty()) return -1;
    st.deathAnim = cmd.parts.front().anim;
    return cmd.durationMs >= 0 ? cmd.durationMs : animLengthMs(animLib_, st.deathAnim);
}

} // namespace iw2::game

namespace iw2::game {

MpPlayerInput* World::mpPlayerInput(int client) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return nullptr;
    return &mp_->clients[size_t(client)].animInput;
}

} // namespace iw2::game

namespace iw2::game {

int World::mpServerOnlyLegsNode(int client, const MpViewState& v) {
    const PlayerAnimScript* script = mpAnimScript();
    const AnimTree* tree = animLib_.tree("multiplayer");
    if (!script || !tree || !mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return 0;
    PaMovement mv;
    mv.climb = v.climb;
    if (v.mantling)
        if (const MantleAnims* m = mpMantleAnims()) {
            PlayerState ps;
            ps.mantling = true;
            ps.mantleTrans = v.mantleTrans;
            ps.mantleTimeMs = v.mantleTimeMs;
            mv.mantleAnim = mantleAnimIndex(ps, *m);
        }
    const std::string type = paMoveType(mv);
    if (!paServerOnlyMoveType(type)) return 0;
    PaContext ctx = mpAnimContext(client);
    ctx.moveType = type;
    const PaItem* it = script->animation(type, ctx);
    for (const PaCommand& c : it ? it->commands : std::vector<PaCommand>{})
        for (const PaPart& p : c.parts)
            if (p.body != PaBody::Torso)
                if (const int n = tree->find(p.anim); n > 0) return n;
    return 0;
}

int World::mpLegsAnimWire(int client, const MpViewState* archived) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return 0;
    if (archived) return mpServerOnlyLegsNode(client, *archived);
    const MpPlayerAnim& st = mp_->clients[size_t(client)].anim;
    return paServerOnlyMoveType(st.moveType) ? st.legsNode : 0;
}

} // namespace iw2::game
