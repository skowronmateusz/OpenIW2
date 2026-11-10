// Third-person multiplayer players (docs/spec/mp-players.md): the player animation script grammar and lookup, the move
// type from the movement, and in a DM world with a bot: the composed model, the animation on the multiplayer tree, the
// weapon in the hands, the events, and the body left at a death. The world part needs the game files (IW2_MAIN).

#include "game/playeranimscript.h"
#include "game/world.h"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2::game;

std::string firstAnim(const PaItem* it, PaBody* body = nullptr) {
    if (!it || it->commands.empty() || it->commands[0].parts.empty()) return "<none>";
    if (body) *body = it->commands[0].parts[0].body;
    return it->commands[0].parts[0].anim;
}

// A small script in the shipped grammar (own text).
void grammarTests() {
    const char* text = R"(
// comment
DEFINES
set weaponclass auto = mg AND smg
set movetype moving = walk AND run
ANIMATIONS
STATE RELAXED
{
}
STATE COMBAT
{
	idle
	{
		weaponclass pistol, weapon_position ads
		{
			both a_pistol_ads
		}
		weaponclass auto
		{
			both a_auto
		}
		default
		{
			both a_default
		}
	}
	walk
	{
		strafing left
		{
			legs a_left
		}
	}
}
EVENTS
fireweapon
{
	mounted mg42
	{
	}
	weaponclass auto, movetype moving
	{
		torso t_auto_move duration 150
	}
	default
	{
		torso t_one blendtime 10
		torso t_two
	}
}
)";
    PlayerAnimScript s;
    std::string err;
    CHECK(s.parse(text, &err));
    if (!err.empty()) std::cerr << err << "\n";
    CHECK(s.animationCount() == 2 && s.eventCount() == 1);
    PaContext c;
    c.weaponClass = "pistol";
    CHECK(firstAnim(s.animation("idle", c)) == "a_default"); // hip: the ads item does not hold
    c.weaponPosition = "ads";
    CHECK(firstAnim(s.animation("idle", c)) == "a_pistol_ads");
    c = PaContext{};
    c.weaponClass = "smg"; // through the define
    CHECK(firstAnim(s.animation("idle", c)) == "a_auto");
    c.moveType = "walk";
    CHECK(s.animation("walk", c) == nullptr); // no item holds
    c.strafing = "left";
    PaBody b{};
    CHECK(firstAnim(s.animation("walk", c), &b) == "a_left" && b == PaBody::Legs);
    // Events: the empty mounted block holds first when mounted, the define of move types, modifiers, alternatives.
    c.mounted = true;
    const PaItem* m = s.event("fireweapon", c);
    CHECK(m && m->commands.empty());
    c.mounted = false;
    const PaItem* e = s.event("fireweapon", c);
    CHECK(firstAnim(e, &b) == "t_auto_move" && b == PaBody::Torso && e->commands[0].durationMs == 150);
    c.moveType = "idle";
    e = s.event("fireweapon", c);
    CHECK(e && e->commands.size() == 2 && e->commands[0].blendMs == 10 && e->commands[1].blendMs == -1);
    // Syntax errors are reported with a line.
    PlayerAnimScript bad;
    CHECK(!bad.parse("EVENTS\nfireweapon\n{\n default\n {\n  wobble x\n }\n}\n", &err) && err.find("line 6") != std::string::npos);
}

void moveTypeTests() {
    PaMovement m;
    {   // The damage timer: the first stumbleTime ms of a hit replace the move type (default 500 of a 750 ms hit).
        PaMovement h;
        h.damageTimerMs = 700, h.damageDurationMs = 750;
        CHECK(paMoveType(h) == "flinch_forward");
        h.flinchYaw = 90;
        CHECK(paMoveType(h) == "flinch_left");
        h.flinchYaw = -180;
        CHECK(paMoveType(h) == "flinch_backward");
        h.flinchYaw = 270;
        CHECK(paMoveType(h) == "flinch_right");
        h.xySpeed = 100, h.forwardMove = 1;
        CHECK(paMoveType(h) == "stumble_forward");
        h.forwardMove = -1;
        CHECK(paMoveType(h) == "stumble_backward");
        h.slow = true;
        CHECK(paMoveType(h) == "stumble_walk_backward");
        h.viewHeightTarget = 40;
        CHECK(paMoveType(h) == "stumble_crouch_backward");
        h.viewHeightTarget = 11;
        CHECK(paMoveType(h) == "walkpronebk");
        h.viewHeightTarget = 60, h.damageTimerMs = 250; // at the threshold of 750 - 500
        CHECK(paMoveType(h) == "walkbk");
        h.damageDurationMs = 300, h.damageTimerMs = 10; // a short hit: the threshold stops at 0
        CHECK(paMoveType(h) == "stumble_walk_backward");
    }
    CHECK(paMoveType(m) == "idle");
    m.viewHeightTarget = 40;
    CHECK(paMoveType(m) == "idlecr");
    m.viewHeightTarget = 11;
    CHECK(paMoveType(m) == "idleprone");
    m.xySpeed = 9.9f; // below player_moveThreshhold
    CHECK(paMoveType(m) == "idleprone");
    m.xySpeed = 50;
    m.forwardMove = 1;
    CHECK(paMoveType(m) == "walkprone");
    m.forwardMove = -1;
    CHECK(paMoveType(m) == "walkpronebk");
    m.viewHeightTarget = 40;
    CHECK(paMoveType(m) == "runcrbk");
    m.slow = true;
    CHECK(paMoveType(m) == "walkcrbk");
    m.forwardMove = 0; // sideways counts as forward
    CHECK(paMoveType(m) == "walkcr");
    m.viewHeightTarget = 60;
    CHECK(paMoveType(m) == "walk");
    m.slow = false;
    CHECK(paMoveType(m) == "run");
    m.forwardMove = -1;
    CHECK(paMoveType(m) == "runbk");
    CHECK(paStrafing(m) == "not");
    m.forwardMove = 0;
    m.rightMove = -1;
    CHECK(paStrafing(m) == "left");
    m.rightMove = 1;
    CHECK(paStrafing(m) == "right");
}

void shippedScriptTests(const iw2::fs::Vfs& vfs) {
    const auto bytes = vfs.read("mp/playeranim.script");
    CHECK(bytes.has_value());
    if (!bytes) return;
    PlayerAnimScript s;
    std::string err;
    CHECK(s.parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), &err));
    if (!err.empty()) std::cerr << err << "\n";
    CHECK(s.animationCount() >= 30 && s.eventCount() >= 13);
    PaContext c;
    c.playerAnimType = "thompson";
    c.weaponClass = "smg";
    CHECK(firstAnim(s.animation("idle", c)) == "pb_stand_alert"); // two-handed default
    c.moveType = "run";
    CHECK(s.animation("run", c) != nullptr);
    c.moveType = "idleprone";
    PaBody b{};
    CHECK(firstAnim(s.event("fireweapon", c), &b) == "pt_prone_shoot_auto" && b == PaBody::Torso);
    c.playerAnimType = "other";
    c.weaponClass = "pistol";
    c.moveType = "idle";
    c.weaponPosition = "ads";
    CHECK(firstAnim(s.animation("idle", c)) == "pb_stand_ads_pistol");
    // Death: by move type, alternatives in one item.
    c = PaContext{};
    c.moveType = "idleprone";
    CHECK(firstAnim(s.event("death", c)) == "pb_prone_death_quickdeath");
    c.moveType = "idle";
    const PaItem* d = s.event("death", c);
    CHECK(d && d->commands.size() > 1);
}

void worldTests(const iw2::fs::Vfs& vfs) {
    World w(vfs, "mp_toujane", true);
    w.cvars["scr_dm_timelimit"] = "0";
    w.mpStart("dm");
    const int c0 = w.mpConnect("player");
    w.mpBegin(c0);
    w.frame();
    w.mpAutoJoin(c0);
    w.frame();
    const int bot = w.mpAddBot("b1");
    w.mpBotSetCombat(bot, false);
    for (int i = 0; i < 80; ++i) w.frame();
    const MpClient& cl = w.mpState().clients[size_t(bot)];
    Entity* e = cl.ent;
    CHECK(e && e->obj->get("sessionstate").toString() == "playing");
    if (!e) return;
    // Model composed by the gametype scripts: a body and at least a head attached.
    CHECK(e->obj->get("model").toString().rfind("xmodel/playerbody", 0) == 0);
    CHECK(!e->attachments.empty());
    // Drawn in third person, with the multiplayer tree and the weapon in the hands; the local player is not drawn.
    CHECK(w.mpPlayerDrawn(*e));
    CHECK(!w.mpPlayerDrawn(*w.player()));
    CHECK(e->anim && e->anim->tree().name() == "multiplayer");
    CHECK(w.mpPlayerWeaponModel(*e).rfind("xmodel/weapon_", 0) == 0);
    if (!e->anim) return;
    const AnimTree& tree = e->anim->tree();
    auto weightOf = [&](const char* name) {
        const int n = tree.find(name);
        return n < 0 ? -1.0f : e->anim->nodeStates()[size_t(n)].weight;
    };
    // Bots spawn standing still with an SMG or rifle: the standing alert animation on the legs.
    // (or already on its way: "runbk" when the first link of its path is a ladder down from a roof spawn)
    CHECK(cl.anim.moveType == "idle" || cl.anim.moveType == "run" || cl.anim.moveType == "runbk");
    CHECK(weightOf("legs") > 0.9f);
    // Walking the bot somewhere: run animation on the legs.
    w.mpBotSetGoal(bot, w.mpState().clients[size_t(c0)].ent->obj->get("origin").asVector());
    bool ran = false;
    for (int i = 0; i < 60 && !ran; ++i) {
        w.frame();
        ran = cl.anim.moveType == "run";
    }
    CHECK(ran);
    // Firing while moving plays nothing (the shipped script's empty block for automatic weapons on the move) ...
    w.mpPlayerInput(bot)->fired = true;
    w.frame();
    CHECK(!cl.anim.torsoActive);
    // ... standing still it plays the torso animation: a second, script-only client that nothing moves (the driver takes
    // its movement from the entity's velocity) is idle.
    const int c2 = w.mpConnect("p2");
    w.frame();
    w.mpBegin(c2);
    w.frame();
    w.mpAutoJoin(c2);
    for (int i = 0; i < 20; ++i) w.frame();
    const MpClient& cl2 = w.mpState().clients[size_t(c2)];
    CHECK(cl2.ent && w.mpPlayerDrawn(*cl2.ent) && cl2.anim.moveType == "idle");
    if (cl2.ent && cl2.ent->anim) {
        const AnimTree& t2 = cl2.ent->anim->tree();
        w.mpPlayerInput(c2)->fired = true;
        w.frame();
        CHECK(cl2.anim.torsoActive);
        CHECK(cl2.ent->anim->nodeStates()[size_t(t2.find("torso"))].goalWeight > 0.9f);
        // The hold (the command's duration, 150 ms) ends and the torso is cleared.
        for (int i = 0; i < 5; ++i) w.frame();
        CHECK(!cl2.anim.torsoActive);
        CHECK(cl2.ent->anim->nodeStates()[size_t(t2.find("torso"))].goalWeight == 0);
    }
    // Hit locations: bullets at a standing client meet its bone hit boxes (head, torso, legs), not its box.
    if (cl2.ent) {
        const auto o = cl2.ent->obj->get("origin").asVector();
        const float yaw = cl2.ent->obj->get("angles").asVector()[1] * 3.14159265f / 180.0f;
        const float fx = std::cos(yaw), fy = std::sin(yaw);
        auto shoot = [&](float z) {
            const iw2::script::Vec3 a{o[0] + fx * 100, o[1] + fy * 100, o[2] + z}, b{o[0] - fx * 100, o[1] - fy * 100, o[2] + z};
            const auto r = w.debugBulletLocation(a, b, nullptr);
            std::cout << "hit at +" << z << ": entity " << r.first << " " << r.second << "\n";
            return r.first == cl2.ent->number ? r.second : std::string("<miss>");
        };
        const std::string head = shoot(64), chest = shoot(48), legs = shoot(20);
        CHECK(head == "head" || head == "helmet" || head == "neck");
        CHECK(chest.rfind("torso", 0) == 0);
        CHECK(legs.find("leg") != std::string::npos);
        CHECK(shoot(90) == "<miss>"); // above the head: the box would have been hit up to 70, the bones are not there
    }
    // Death: the body wears the model and attachments and plays a death animation.
    const size_t before = w.entities().size();
    w.mpDebugDamage(bot, 0, 1000, "MOD_RIFLE_BULLET", "torso_upper");
    for (int i = 0; i < 3; ++i) w.frame();
    CHECK(e->obj->get("sessionstate").toString() == "dead");
    CHECK(!w.mpPlayerDrawn(*e));
    CHECK(!cl.anim.deathAnim.empty());
    Entity* body = nullptr;
    for (size_t i = before; i < w.entities().size(); ++i)
        if (w.entities()[i]->classname == "playercorpse") body = w.entities()[i].get();
    CHECK(body != nullptr);
    if (body) {
        CHECK(body->obj->get("model").toString() == e->obj->get("model").toString());
        CHECK(body->attachments.size() == e->attachments.size());
        CHECK(body->anim && body->anim->tree().find(cl.anim.deathAnim) >= 0);
    }
    CHECK(w.vm().errors().empty());
    for (const auto& m : w.vm().errors()) std::cerr << "script error: " << m << "\n";
}

} // namespace

int main() {
    grammarTests();
    moveTypeTests();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, world part skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    shippedScriptTests(vfs);
    worldTests(vfs);
    std::cout << (failures ? "FAILED" : "ok") << "\n";
    return failures ? 1 : 0;
}
