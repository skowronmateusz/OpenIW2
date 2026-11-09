// A vehicle's script tag queries follow its turret (docs/spec/vehicles.md "Tag queries"), and a stationary gun crew turns with the gun:
// `gettagorigin` / `gettagangles` / the linkto parent frame read the pose with the turret bone controls, so elalamein's flak88 crew stays at the turned
// tag_turret and the turn animations start. Needs IW2_MAIN (the map, its script and the flak88 model).
#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

static float angleDiff(float a, float b) {
    float d = std::fmod(a - b, 360.0f);
    if (d < -180.0f) d += 360.0f;
    if (d > 180.0f) d -= 360.0f;
    return std::fabs(d);
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "flak crew test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "elalamein");
    w.start();

    // Flak 1 (origin -5669 -6943 165): its crew spawns when the player stands in the map's trigger radius for it.
    Entity* flak = nullptr;
    for (const auto& e : w.entities())
        if (e->vehicle && e->obj->get("script_flak88").toString() == "1") flak = e.get();
    CHECK(flak != nullptr);
    if (!flak) return 1;
    w.setPlayerOrigin({-6223, -6984, 150}, {0, 0, 0});
    for (int f = 0; f < 100; ++f) w.frame();

    std::vector<Entity*> crew;
    for (const auto& e : w.entities())
        if (e->isActor && !e->obj->removed && e->obj->get("crewposition").isDefined() && e->obj->get("script_flak88").toString() == "1") crew.push_back(e.get());
    CHECK(crew.size() >= 2);
    if (crew.size() < 2) return 1;
    for (Entity* c : crew) CHECK(c->scripted != nullptr); // the crew plays its animations through animscripted

    Vec3 restTag, restAngles;
    const bool haveTag = w.scriptTagFrame(*flak, "tag_turret", restTag, restAngles);
    CHECK(haveTag);
    if (!haveTag) return 1;
    CHECK(angleDiff(restAngles[1], 0.0f) < 0.5f); // angles 0 0 0, the turret at rest
    std::vector<Vec3> rest;
    for (Entity* c : crew) rest.push_back(c->obj->get("origin").asVector());
    const std::string firstTurning = flak->obj->get("turning").toString();
    CHECK(firstTurning == "none");

    // The turret control turns tag_turret and everything under it for the script queries (0x5127a0 -> 0x4d7390 -> 0x4eec20).
    flak->vehicle->turretYaw = 45.0f;
    Vec3 turned, turnedAngles;
    CHECK(w.scriptTagFrame(*flak, "tag_turret", turned, turnedAngles));
    CHECK(angleDiff(turnedAngles[1], 45.0f) < 0.5f);
    CHECK(std::fabs(turned[0] - restTag[0]) + std::fabs(turned[1] - restTag[1]) < 1.0f); // the tag is on the rotation axis
    // A tag off the axis takes the rotation: the barrel tag agrees with the renderer's controlled position.
    Vec3 barrelScript, barrelAngles, barrelRender;
    CHECK(w.scriptTagFrame(*flak, "tag_barrel", barrelScript, barrelAngles));
    CHECK(w.entityTagPosition(flak->number, "tag_barrel", barrelRender));
    CHECK(std::fabs(barrelScript[0] - barrelRender[0]) + std::fabs(barrelScript[1] - barrelRender[1]) + std::fabs(barrelScript[2] - barrelRender[2]) < 0.1f);

    // gettagangles is AxisToAngles of the whole frame (0x4eee30): a rolled tag reports its roll.
    flak->vehicle->joltRoll = 10.0f;
    Vec3 bodyOrigin, bodyAngles;
    if (w.scriptTagFrame(*flak, "tag_body", bodyOrigin, bodyAngles)) CHECK(angleDiff(bodyAngles[2], 10.0f) < 0.5f);
    flak->vehicle->joltRoll = 0.0f;
    flak->vehicle->turretYaw = 0.0f;

    // The scripted crew: while the gun keeps turning left the yaw of tag_turret rises, the script's `turning` field goes to
    // "turnleft" (maps/_flak88.gsc:299-306) and the crew animation becomes a turn animation (:114-115); the crew follows the turn.
    bool sawTurning = false, sawTurnAnim = false;
    const int frames = 400;
    for (int f = 0; f < frames; ++f) {
        flak->vehicle->turretYaw = float(f + 1) * 0.15f; // 3 degrees a second, slower than the gun's own traverse
        w.frame();
        if (flak->obj->get("turning").toString() == "turnleft") sawTurning = true;
        for (Entity* c : crew)
            if (c->scripted && c->scripted->anim && c->anim->tree().nodes()[size_t(c->scripted->anim)].name.find("turnleft") != std::string::npos) sawTurnAnim = true;
    }
    CHECK(sawTurning);
    CHECK(sawTurnAnim);

    // Stop turning and let the crew settle: every man has moved around tag_turret with the gun (a link and the animations both read the turned tag).
    const float yaw = flak->vehicle->turretYaw;
    for (int f = 0; f < 200; ++f) w.frame();
    Vec3 tagNow, anglesNow;
    w.scriptTagFrame(*flak, "tag_turret", tagNow, anglesNow);
    CHECK(angleDiff(anglesNow[1], yaw) < 0.5f);
    int followed = 0;
    for (size_t i = 0; i < crew.size(); ++i) {
        const Vec3 o = crew[i]->obj->get("origin").asVector();
        const float a0 = std::atan2(rest[i][1] - restTag[1], rest[i][0] - restTag[0]) * 57.29578f;
        const float a1 = std::atan2(o[1] - tagNow[1], o[0] - tagNow[0]) * 57.29578f;
        if (angleDiff(a1 - a0, yaw) < 25.0f) ++followed; // the crew animations move each man a little, so a wide tolerance
    }
    CHECK(followed == int(crew.size()));
    return failures ? 1 : 0;
}
