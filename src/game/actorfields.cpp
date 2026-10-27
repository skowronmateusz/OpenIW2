#include "game/actorfields.h"

namespace iw2::game {

using script::Value;

void applyActorFieldDefaults(script::Object& a) {
    // The actor slot is zeroed on allocation (FUN_004fc8d0), then FUN_004fdde0 stores these defaults.
    a.set("fovcosine", Value(0.01f));
    a.set("maxsightdistsqrd", Value(67108864.0f)); // 8192²
    a.set("pacifistwait", Value(20.0f));           // stored as 20000 ms, read back in seconds
    a.set("suppressionwait", Value(5.0f));         // stored as 5000 ms, read back in seconds
    a.set("followmin", Value(int32_t(-5)));
    a.set("followmax", Value(int32_t(-1)));
    a.set("walkdist", Value(128.0f));
    a.set("interval", Value(96.0f));
    a.set("grenadeawareness", Value(0.33f));
    a.set("badplaceawareness", Value(0.75f));
    a.set("goodshootposvalid", Value(int32_t(0)));

    // The spawn function FUN_004fc640 then stores these.
    a.set("health", Value(int32_t(100)));
    a.set("maxhealth", Value(int32_t(100)));
    a.set("accuracy", Value(0.2f));
    a.set("accuracystationarymod", Value(1.0f));
    a.set("dropweapon", Value(int32_t(1)));
    a.set("drawoncompass", Value(int32_t(1)));
    a.set("keepclaimednode", Value(int32_t(0)));
    a.set("keepclaimednodeingoal", Value(int32_t(0)));
    // The goal radius and height are set to 0 through their clamping setters, which raise them to the minimums
    // 4 and 80.
    a.set("goalradius", Value(4.0f));
    a.set("goalheight", Value(80.0f));

    // Left zero by the allocation and never stored by the engine: only scripts change them.
    a.set("pathenemyfightdist", Value(0.0f));
    a.set("pathenemylookahead", Value(0.0f));
    a.set("pacifist", Value(int32_t(0)));
    a.set("chainfallback", Value(int32_t(0)));
    a.set("dontavoidplayer", Value(int32_t(0)));
    a.set("grenadeammo", Value(int32_t(0)));
    a.set("allowdeath", Value(int32_t(0)));

    // desiredangle (+0x124) is also 0.0 from the zeroed allocation (no field-default write in FUN_004fdde0);
    // the movement frame refreshes it (FUN_00504ca0, world.cpp moveActor). animscripts read it
    // (animscripts/stop.gsc:555 ProneStill), so it must exist as a float before the first move.
    a.set("desiredangle", Value(0.0f));
}

} // namespace iw2::game
