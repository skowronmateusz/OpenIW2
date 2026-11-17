// Third-person multiplayer players: the animation driver on the "multiplayer" tree and the weapon in the hands.
// Implemented from the spec in docs/spec/mp-players.md. The original keeps this state in the client game; this
// offline port keeps it on the server's client entity (its AnimState steps with every other entity in World::frame).

#include "game/mpplayers.h"
#include "game/world.h"
#include "game/axis.h"
#include "util/log.h"

#include <cctype>
#include <cmath>

namespace iw2::game {

using script::Value;

namespace {

constexpr float kDefaultBlend = 0.2f; // TODO(spec): the blend time of a command without "blendtime"
constexpr int kAnimToggleBit = 0x200;  // legsAnim / torsoAnim: started again (MP 0x4f98af, docs/spec/mp-players.md)
constexpr int kAnimEventJumpBack = 4, kAnimEventLand = 5; // the EVENTS numbers of the movement's legs events

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
    if (const int serverNode = cl.animInput.serverLegsNode & ~kAnimToggleBit; mpRemote_ && serverNode > 0)
        if (const PlayerAnimScript* script = mpAnimScript())
            if (const AnimTree* tree = animLib_.tree("multiplayer"); tree && size_t(serverNode) < tree->nodes().size()) {
                // The climb or mantle move type whose animation the server plays (the one the entity state names).
                const std::string& anim = tree->nodes()[size_t(serverNode)].name;
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
    // On a map turret (lnxded 0x80d9afc..0x80d9b5d: condition mounted from eFlags 0x300) and the attack held (condition
    // firing from the command's attack button, 0x80d9b9f..0x80d9bfb); a puppet from its entity state.
    ctx.mounted = mpTurretOf(client) != nullptr;
    ctx.firing = mpRemote_ ? cl.animInput.remoteFiring : (mpClientButtons(client) & 1u) != 0;
    return ctx;
}

int World::mpPlayCommand(Entity& e, const PaItem& item, bool restart) {
    if (item.commands.empty() || !e.anim) return -1;
    MpAnimData& d = mp_->animData;
    d.seed = d.seed * 1103515245u + 12345u;
    return mpPlayPaCommand(e, item.commands[size_t((d.seed >> 16) % item.commands.size())], restart);
}

int World::mpPlayPaCommand(Entity& e, const PaCommand& cmd, bool restart) {
    if (!e.anim) return -1;
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
        if (p.body != PaBody::Torso && st) st->legsNode = n, st->legsTurretAnim = cmd.turretAnim;
        if (p.body == PaBody::Torso) {
            if (st) st->torsoHeldUntilMs = now + len, st->torsoActive = true, st->torsoNode = n;
        } else if (p.body == PaBody::Both && (!st || now >= st->torsoHeldUntilMs)) {
            e.anim->clear(torso, blend);
            if (st) st->torsoActive = false;
        }
        if (duration < 0) duration = len;
    }
    return duration;
}

Entity* World::mpTurretOf(int client) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return nullptr;
    if (!mpRemote_) {
        uint32_t bits = 0;
        int lock = 0;
        return const_cast<Entity*>(mpClientTurretUse(client, bits, lock));
    }
    const int index = mp_->clients[size_t(client)].animInput.remoteTurretMapIndex;
    if (index <= 0) return nullptr;
    for (const auto& e : entities_)
        if (e->mapIndex == index && e->turret && !e->obj->removed) return e.get();
    return nullptr;
}

// The pose of a client on a turret (MP 0x4dbe60, docs/spec/mp-players.md "On a turret"): the probe of the single-player
// gunner pose step over the height children of the turret animation node and their turret-yaw grandchildren, without
// the actor's pitch bound.
void World::mpTurretPose(Entity& e, Entity& t, int top) {
    if (!e.anim || !t.turret || top <= 0) return;
    AnimState& an = *e.anim;
    const auto& nodes = an.tree().nodes();
    if (size_t(top) >= nodes.size() || nodes[size_t(top)].children.empty()) return;
    // tag_weapon of the turret model with its bone controls (0x4ccd00): no tag, no pose (0x4dbf1c).
    const auto models = entityModels(t);
    if (models.empty()) return;
    const HitBoxModel& m = models.front();
    int tag = -1;
    for (size_t i = 0; i < m.parts->bones.size() && tag < 0; ++i) {
        std::string n = m.parts->bones[i].name;
        for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (n == "tag_weapon") tag = int(i);
    }
    if (tag < 0) return;
    const std::array<float, 3> fwd = assets::rotate(m.rotation[size_t(tag)], {1.0f, 0.0f, 0.0f});
    const float tagYaw = (fwd[0] == 0.0f && fwd[1] == 0.0f) ? 0.0f : std::atan2(fwd[1], fwd[0]) * 180.0f / 3.14159274f;
    const float rotInc = weaponFloat(t.turret->weapon, "animHorRotateInc"); // turret weapon +0x564
    const Value tov = t.obj->get("origin"), tav = t.obj->get("angles"), eov = e.obj->get("origin");
    if (!tov.isVector() || !eov.isVector()) return;
    const Axis tAxis = anglesAxis(tav.isVector() ? tav.asVector() : Vec3{0, 0, 0});
    const float h = axisLocal(tov.asVector(), tAxis, eov.asVector())[2] - m.position[size_t(tag)][2];
    auto setNode = [&](int n, float goal, float blendTime) {
        AnimState::SetArgs a;
        a.weight = goal;
        a.blendTime = blendTime;
        an.set(n, a);
    };
    auto rateSet = [&](int n, float goal) { // the blend time reaches the goal in one frame (0x4dc22e..0x4dc265)
        const float d = std::fabs(an.nodeStates()[size_t(n)].weight - goal) * 20.0f;
        setNode(n, goal < 0.001f ? 0.0f : goal, d > 0.0f ? 1.0f / d : 0.0f);
    };
    auto exeChild = [&](int n, int j) { // the exe's child order is the reverse of the file's (docs/spec/animtree.md)
        const auto& c = nodes[size_t(n)].children;
        return c[c.size() - 1 - size_t(j)];
    };
    an.clearChildren(top, 0.0f);
    const int count = int(nodes[size_t(top)].children.size());
    int i = 0, child = 0, k = 0, prevK = 0;
    float frac = 0.0f, prevFrac = 0.0f, z = 0.0f, prevZ = 0.0f, qz = 0.0f, qw = 1.0f;
    std::array<float, 3> trans{0, 0, 0};
    for (; i < count; ++i) {
        child = exeChild(top, i);
        setNode(child, 1.0f, 1.0f);
        const int gc = int(nodes[size_t(child)].children.size());
        if (gc == 0) return;
        float f = float(gc) * 0.5f - (rotInc != 0.0f ? tagYaw / rotInc : 0.0f);
        if (f < 0.0f) f = 0.0f;
        else if (!(f < float(gc - 1))) f = float(gc - 1);
        k = int(f);
        frac = f - float(k);
        setNode(exeChild(child, k), 1.0f - frac < 0.001f ? 0.0f : 1.0f - frac, 1.0f);
        if (frac != 0.0f) setNode(exeChild(child, k + 1), frac < 0.001f ? 0.0f : frac, 1.0f);
        an.nodeDelta(child, qz, qw, trans);
        z = trans[2];
        if (!(z < h)) break;
        prevZ = z;
        prevK = k;
        prevFrac = frac;
    }
    an.clearChildren(top, 0.0f);
    rateSet(exeChild(child, k), 1.0f - frac);
    if (frac != 0.0f) rateSet(exeChild(child, k + 1), frac);
    if (i != 0 && i != count) {
        const float tw = (h - prevZ) / (z - prevZ);
        rateSet(child, tw);
        const int prevChild = exeChild(top, i - 1);
        rateSet(prevChild, 1.0f - tw);
        rateSet(exeChild(prevChild, prevK), 1.0f - prevFrac);
        if (prevFrac != 0.0f) rateSet(exeChild(prevChild, prevK + 1), prevFrac);
    } else {
        rateSet(child, 1.0f); // 0x4dc502..0x4dc55f
    }
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
            st.legsNode = st.torsoNode = st.eventLegsNode = st.serverLegsPlayed = 0;
            st.moveType.clear();
            st.rateSampled = false;
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
        // On a turret the body is built without the hand weapon (MP 0x4fb266..0x4fb273: entity state weapon unused with
        // eFlags 0x300).
        if (ctx.mounted) st.weaponModel.clear();
        // The footsteps step chooses no move type just after a jump on a ladder, nor in the air while neither climbing nor
        // mantling (lnxded 0x80e58e9..0x80e58f8, 0x80e5a26..0x80e5a34): the animation that plays goes on. A network client's
        // puppet gets that from the server's legsAnim instead.
        const bool airborne = !mpRemote_ && cl.animInput.onGround == 0 && cl.view.climb == 0 && !cl.view.mantling;
        const bool hold = !st.moveType.empty() && (ctx.holdMoveType || airborne);
        if (hold) ctx.moveType = st.moveType;
        // Events of this frame (docs/spec/mp-players.md, "Events"). An unforced one is refused while the legs or the torso
        // it plays on are held for more than 49 ms (0x80d90d6).
        auto event = [&](const char* name, const PaContext& ec, bool forced) -> bool {
            const PaItem* it = script->event(name, ec);
            if (!it) return false;
            bool legs = false, torsoPart = false;
            for (const auto& c : it->commands)
                for (const auto& p : c.parts) (p.body == PaBody::Torso ? torsoPart : legs) = true;
            if (!forced && ((legs && now + 49 < st.legsHeldUntilMs) || (torsoPart && now + 49 < st.torsoHeldUntilMs))) return false;
            if (static const bool rt = std::getenv("IW2_REMOTE_TRACE") != nullptr; rt) // debug: the event's animations (docs/debug-flags.md)
                for (const auto& c : it->commands)
                    for (const auto& p : c.parts)
                        log::tracef("remote", "remotetrace t=%lld client=%zu animevent=%s body=%d anim=%s\n", (long long)now, ci, name, int(p.body), p.anim.c_str());
            const int d = mpPlayCommand(*e, *it, true);
            if (legs) st.legsHeldUntilMs = now + std::max(d, 0), st.legsItem = nullptr;
            return legs;
        };
        // The movement's jump or landing (docs/spec/mp-players.md "Jumps and landings"): called inside the move, so the
        // conditions see the move type of the previous step. Its legs animation is sent to the other clients until a move
        // type is chosen again.
        if (const int ev = cl.animInput.legsEvent; ev != 0) {
            PaContext ec = ctx;
            if (!st.moveType.empty()) ec.moveType = st.moveType;
            if (event(ev == kAnimEventJumpBack ? "jumpbk" : ev == kAnimEventLand ? "land" : "jump", ec, cl.animInput.legsEventForced)) {
                st.eventLegsNode = st.legsNode;
                st.legsToggle ^= kAnimToggleBit;
            }
            cl.animInput.legsEvent = 0;
            cl.animInput.legsEventForced = false;
        }
        st.moveType = ctx.moveType;
        if (cl.animInput.reloaded) event("reload", ctx, true);
        if (cl.animInput.fired) {
            ctx.firing = true;
            event("fireweapon", ctx, true);
        }
        if (cl.animInput.shocked) event("shellshock", ctx, true);
        cl.animInput.fired = cl.animInput.reloaded = cl.animInput.shocked = false;
        // The torso falls back to the legs' animation when its hold ends.
        if (st.torsoActive && now >= st.torsoHeldUntilMs) {
            e->anim->clear(torso, kDefaultBlend);
            st.torsoActive = false;
        }
        // A puppet whose server sends a legs animation that is no climb or mantle (a jump, a landing): played as it is,
        // started again when the number or its toggle bit changes; no move type meanwhile.
        const int serverLegs = mpRemote_ && (cl.animInput.serverLegsNode & ~kAnimToggleBit) > 0 && !paServerOnlyMoveType(ctx.moveType)
                                   ? cl.animInput.serverLegsNode : 0;
        if (serverLegs != 0) {
            if (serverLegs != st.serverLegsPlayed) {
                const int node = serverLegs & ~kAnimToggleBit;
                if (size_t(node) < tree->nodes().size()) {
                    PaCommand cmd;
                    PaPart part{PaBody::Legs, tree->nodes()[size_t(node)].name};
                    if (const PaCommand* c = script->legsCommand(part.anim, {"jump", "jumpbk", "land"})) {
                        cmd.blendMs = c->blendMs;
                        for (const PaPart& p : c->parts)
                            if (p.anim == part.anim) part.body = p.body;
                    }
                    cmd.parts.push_back(part);
                    mpPlayPaCommand(*e, cmd, true);
                }
                st.serverLegsPlayed = serverLegs;
                st.legsItem = nullptr;
            }
        } else {
            st.serverLegsPlayed = 0;
            // The move type's animation, re-chosen when its item changes; not while the step chooses nothing.
            // A move-type animation waits for the legs' hold to run out to its last 49 ms (0x80d913f).
            if (!hold && now + 49 >= st.legsHeldUntilMs) {
                const PaItem* it = script->animation(ctx.moveType, ctx);
                if (it && it != st.legsItem) {
                    mpPlayCommand(*e, *it, false);
                    st.legsItem = it;
                    st.eventLegsNode = 0;
                }
            }
        }
        // A turret animation (a command with `turretanim`) on the legs of a client on a turret: its pose from the turret
        // (MP 0x4dc789..0x4dc797 -> 0x4dbe60, which needs the legs animation's turret flag 4, 0x4dbec1; TODO(spec): taken
        // to be the `turretanim` modifier).
        if (ctx.mounted && st.legsTurretAnim)
            if (Entity* t = mpTurretOf(int(ci))) mpTurretPose(*e, *t, st.legsNode);
        // The animation rate from the movement (docs/spec/mp-players.md "Animation rate from the movement", MP 0x4f9d80):
        // the legs' and the torso's animation play at the body's speed over the animation's move speed, sampled once per
        // world frame.
        if (const Value ov = e->obj->get("origin"); ov.isVector() && (!st.rateSampled || now != st.rateSampleMs)) {
            const Vec3 o = ov.asVector();
            const float dtS = st.rateSampled ? float(now - st.rateSampleMs) * 0.001f : 0.0f;
            const float dx = o[0] - st.rateSampleOrigin[0], dy = o[1] - st.rateSampleOrigin[1], dz = o[2] - st.rateSampleOrigin[2];
            for (const int n : {st.legsNode, st.torsoActive ? st.torsoNode : 0}) {
                if (n <= 0 || size_t(n) >= e->anim->nodeStates().size()) continue;
                const float moveSpeed = mpAnimMoveSpeed(n);
                float rate = 1.0f;
                if (moveSpeed > 0 && dtS > 0) {
                    const bool ladder = script->ladderAnimation(tree->nodes()[size_t(n)].name);
                    const float dist = ladder ? std::fabs(dz) : std::sqrt(dx * dx + dy * dy + dz * dz);
                    rate = paMoveRate(dist / dtS / moveSpeed, moveSpeed, ladder);
                }
                e->anim->nodeStates()[size_t(n)].rate = rate;
            }
            st.rateSampled = true;
            st.rateSampleMs = now;
            st.rateSampleOrigin = {o[0], o[1], o[2]};
        }
    }
}

float World::mpAnimMoveSpeed(int node) {
    if (!mp_) return 0.0f;
    auto& cache = mp_->animData.moveSpeed;
    if (const auto it = cache.find(node); it != cache.end()) return it->second;
    float speed = 0.0f;
    const AnimTree* tree = animLib_.tree("multiplayer");
    if (tree && node > 0 && size_t(node) < tree->nodes().size() && tree->nodes()[size_t(node)].children.empty())
        if (const assets::XAnim* a = animLib_.anim(tree->nodes()[size_t(node)].name); a && a->length() > 0 && !a->deltaTrans.keys.empty()) {
            const int keys = a->keyCount();
            const auto p0 = assets::sampleTrans(a->deltaTrans, 0.0f, keys), p1 = assets::sampleTrans(a->deltaTrans, float(keys - 1), keys);
            const float d0 = p1[0] - p0[0], d1 = p1[1] - p0[1], d2 = p1[2] - p0[2];
            speed = std::sqrt(d0 * d0 + d1 * d1 + d2 * d2) / a->length();
        }
    cache[node] = speed;
    return speed;
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
    if (st.eventLegsNode > 0) return st.eventLegsNode | st.legsToggle;
    return paServerOnlyMoveType(st.moveType) ? st.legsNode : 0;
}

} // namespace iw2::game
