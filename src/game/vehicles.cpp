// Script vehicles: info_vehicle_node paths and the path-following think (docs/spec/vehicles.md). The rules mirror
// the executable's path code (node linking 0x52ada0, path step 0x52b290, vehicle think 0x5110a0).
#include "game/world.h"

#include <cmath>
#include <stdexcept>

namespace iw2::game {

using namespace script;

namespace {

constexpr float kFrameTime = 0.05f;    // 0x591e54
constexpr float kMphToIps = 17.6f;     // 0x5923e0: inches per second per mile per hour
constexpr float kUnsetAngle = 3.14159274f; // 0x5a95ec: node angles not given by the map

float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

// Normalizes in place and returns the original length (0x4210d0).
float normalize(Vec3& v) {
    const float len = std::sqrt(dot(v, v));
    if (len != 0.0f) {
        const float inv = 1.0f / len;
        for (float& c : v) c *= inv;
    }
    return len;
}

// Direction to pitch/yaw/roll in degrees (0x421b30).
Vec3 vectorToAngles(const Vec3& v) {
    if (v[1] == 0.0f && v[0] == 0.0f) return {v[2] > 0.0f ? 270.0f : 90.0f, 0.0f, 0.0f};
    float yaw = std::atan2(v[1], v[0]) * 180.0f / float(M_PI);
    if (yaw < 0) yaw += 360.0f;
    const float forward = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    float pitch = std::atan2(v[2], forward) * -180.0f / float(M_PI);
    if (pitch < 0) pitch += 360.0f;
    return {pitch, yaw, 0.0f};
}

// 16-bit angle quantization into (-180, 180] (0x424040).
float angleNormalize180(float a) {
    const int32_t i = int32_t(a * 182.044449f) & 0xffff; // truncating conversion (0x565994)
    float r = float(i) * 0.0054931640625f;
    if (r > 180.0f) r -= 360.0f;
    return r;
}

// Interpolates from a to b the short way round (0x423ee0).
float lerpAngle(float a, float b, float t) {
    if (b - a > 180.0f) b -= 360.0f;
    if (b - a < -180.0f) b += 360.0f;
    return (b - a) * t + a;
}

// Moves `from` towards `to` by rate * dt of the difference (0x420a90 / 0x420a20).
float approachAngle(float to, float from, float rate, float dt) {
    while (to - from > 180.0f) to -= 360.0f;
    while (to - from < -180.0f) to += 360.0f;
    const float diff = to - from;
    const float step = diff * rate * dt;
    float r = to;
    if (std::fabs(diff) > 0.001f && std::fabs(diff) >= std::fabs(step)) r = step + from;
    return angleNormalize180(r);
}

float numberField(const ObjectPtr& o, const char* key, float def) {
    const Value v = o->get(key);
    return v.isNumber() ? v.asFloat() : def;
}

// Speed (or lookahead) of a node without its own value, interpolated by distance between the nearest earlier and
// later nodes that have one (0x529f70; lookahead 0x52a100 uses "greater than zero").
float resolveNodeValue(const std::vector<VehicleNode>& nodes, int self, float VehicleNode::*field, bool positive) {
    auto has = [&](int n) { return positive ? nodes[size_t(n)].*field > 0.0f : nodes[size_t(n)].*field >= 0.0f; };
    if (nodes[size_t(self)].*field >= 0.0f) return nodes[size_t(self)].*field;
    const int count = int(nodes.size());
    float prevValue = -1.0f, nextValue = -1.0f, prevDist = 0, nextDist = 0;
    int n = nodes[size_t(self)].prev;
    for (int steps = 0; n >= 0 && steps < count;) {
        prevDist += nodes[size_t(n)].length;
        ++steps;
        if (has(n)) { prevValue = nodes[size_t(n)].*field; break; }
        n = nodes[size_t(n)].prev;
        if (n == self) break;
    }
    n = self;
    for (int steps = 0; steps < count;) {
        ++steps;
        if (has(n)) { nextValue = nodes[size_t(n)].*field; break; }
        const int nx = nodes[size_t(n)].next;
        if (nx < 0 || nx == self) break;
        nextDist += nodes[size_t(n)].length;
        n = nx;
    }
    if (prevValue >= 0.0f) {
        if (nextValue < 0.0f) return prevValue;
        const float total = prevDist + nextDist;
        if (total > 0.0f) return (nextValue - prevValue) * (prevDist / total) + prevValue;
        return 0.0f;
    }
    if (nextValue >= 0.0f) return nextValue;
    return 0.0f;
}

// Angles of a rotate node without its own, interpolated between neighbouring nodes that have them (0x52a290).
Vec3 resolveRotateAngles(const std::vector<VehicleNode>& nodes, int self) {
    auto unset = [](const Vec3& a) { return a[0] == kUnsetAngle && a[1] == kUnsetAngle && a[2] == kUnsetAngle; };
    if (!unset(nodes[size_t(self)].angles)) return nodes[size_t(self)].angles;
    const int count = int(nodes.size());
    Vec3 before{kUnsetAngle, kUnsetAngle, kUnsetAngle}, after = before;
    float prevDist = 0, nextDist = 0;
    int n = nodes[size_t(self)].prev;
    for (int steps = 0; n >= 0 && steps < count;) {
        prevDist += nodes[size_t(n)].length;
        ++steps;
        if (!unset(nodes[size_t(n)].angles)) { before = nodes[size_t(n)].angles; break; }
        n = nodes[size_t(n)].prev;
        if (n == self) break;
    }
    n = self;
    for (int steps = 0; steps < count;) {
        ++steps;
        if (!unset(nodes[size_t(n)].angles)) { after = nodes[size_t(n)].angles; break; }
        const int nx = nodes[size_t(n)].next;
        if (nx < 0 || nx == self) break;
        nextDist += nodes[size_t(n)].length;
        n = nx;
    }
    if (unset(before) && unset(after)) return {0, 0, 0};
    if (unset(before)) return after;
    if (unset(after)) return before;
    const float total = prevDist + nextDist;
    if (total > 0.0f) {
        const float t = prevDist / total;
        return {lerpAngle(before[0], after[0], t), lerpAngle(before[1], after[1], t), lerpAngle(before[2], after[2], t)};
    }
    return {0, 0, 0};
}

// Point `lookahead * speed` ahead of the path position, following the node chain (0x52a840).
Vec3 lookaheadPoint(const std::vector<VehicleNode>& nodes, const VehiclePathPos& p) {
    const VehicleNode* n = &nodes[size_t(p.node)];
    float dist = p.lookahead * p.speed + n->length * p.frac;
    float along = dist;
    for (size_t steps = 0; steps < nodes.size(); ++steps) {
        along = 0.0f;
        if (n->next < 0 || n->length == 0.0f) break;
        along = dist;
        if (dist < n->length) break;
        dist -= n->length;
        n = &nodes[size_t(n->next)];
        along = dist;
    }
    return {along * n->dir[0] + n->origin[0], along * n->dir[1] + n->origin[1], along * n->dir[2] + n->origin[2]};
}

// Finds the segment the position lies on, measured along the vehicle's heading `fwd` (0x52a8e0). Returns whether
// node `waitNode` was among the nodes examined.
bool projectOnPath(const std::vector<VehicleNode>& nodes, VehiclePathPos& p, const Vec3& fwd, int waitNode) {
    int n = p.node;
    bool passedWait = false;
    float frac = p.frac;
    for (size_t steps = 0; steps < nodes.size(); ++steps) {
        if (n == waitNode) passedWait = true;
        const VehicleNode& node = nodes[size_t(n)];
        frac = 0.0f;
        if (node.next < 0 || node.length == 0.0f) break;
        const VehicleNode& next = nodes[size_t(node.next)];
        const float behind = dot(sub(p.origin, node.origin), fwd);
        const float ahead = dot(sub(next.origin, p.origin), fwd);
        if (behind == 0.0f && ahead == 0.0f) break;
        if (behind >= 0.0f && ahead >= 0.0f) {
            frac = behind / (ahead + behind);
            break;
        }
        n = node.next;
        frac = p.frac;
    }
    p.node = n;
    p.frac = frac;
    const VehicleNode& node = nodes[size_t(n)];
    p.end = node.next < 0;
    if (node.next < 0) {
        p.speed = node.speed;
        p.lookahead = node.lookahead;
    } else {
        const VehicleNode& next = nodes[size_t(node.next)];
        p.speed = (next.speed - node.speed) * frac + node.speed;
        p.lookahead = (next.lookahead - node.lookahead) * frac + node.lookahead;
    }
    // Weight of the rotate-node angles (0x52a640).
    if (node.next < 0) {
        p.rotateFrac = node.rotate ? 1.0f : 0.0f;
    } else {
        const bool nextRotate = nodes[size_t(node.next)].rotate;
        if (node.rotate) p.rotateFrac = nextRotate ? 1.0f : 1.0f - frac;
        else p.rotateFrac = nextRotate ? frac : 0.0f;
    }
    return passedWait;
}

// Blends the heading with rotate-node angles (0x52a6c0).
void applyRotateNodes(const std::vector<VehicleNode>& nodes, VehiclePathPos& p) {
    const VehicleNode& node = nodes[size_t(p.node)];
    Vec3 from, to;
    if (node.next < 0) {
        if (node.rotate) p.angles = node.angles;
        return;
    }
    const VehicleNode& next = nodes[size_t(node.next)];
    if (!node.rotate && !next.rotate) return;
    if (node.rotate && next.rotate) { from = node.angles; to = next.angles; }
    else if (node.rotate) { from = node.angles; to = p.angles; }
    else { from = p.angles; to = next.angles; }
    for (int i = 0; i < 3; ++i) p.angles[size_t(i)] = angleNormalize180(lerpAngle(from[size_t(i)], to[size_t(i)], p.rotateFrac));
}

int nodeByName(const std::vector<VehicleNode>& nodes, const std::string& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == name) return int(i);
    return -1;
}

// One path step: move speed * frame time towards the lookahead point (0x52b290). Returns whether the wait node was
// passed.
bool stepPath(std::vector<VehicleNode>& nodes, VehiclePathPos& p, int waitNode) {
    if (p.end) return false;
    int switched = -1;
    if (p.hasSwitch) {
        switched = nodeByName(nodes, p.switched.name);
        if (switched >= 0) nodes[size_t(switched)] = p.switched;
    }
    bool passed = false;
    p.lookPoint = lookaheadPoint(nodes, p);
    Vec3 dir = sub(p.lookPoint, p.origin);
    if (normalize(dir) <= 0.0f) {
        p.end = true;
    } else {
        p.angles = vectorToAngles(dir);
        for (float& a : p.angles) a = angleNormalize180(a);
        const float step = p.speed * kFrameTime;
        for (int i = 0; i < 3; ++i) p.origin[size_t(i)] += dir[size_t(i)] * step;
        passed = projectOnPath(nodes, p, dir, waitNode);
        applyRotateNodes(nodes, p);
    }
    if (switched >= 0) nodes[size_t(switched)] = p.original;
    return passed;
}

} // namespace

// Links vehicle nodes by targetname/target and resolves directions, lengths, speeds, lookaheads and angles
// (spawn 0x52b490, linking 0x52ada0).
void World::linkVehicleNodes() {
    vnodes_.clear();
    vnodeIndex_.clear();
    for (const auto& o : vehicleNodes_) {
        VehicleNode n;
        n.obj = o;
        n.rotate = o->get("classname").toString() == "info_vehicle_node_rotate";
        if (o->get("targetname").isString()) n.name = o->get("targetname").asString();
        if (o->get("target").isString()) n.target = o->get("target").asString();
        if (o->get("origin").isVector()) n.origin = o->get("origin").asVector();
        if (o->get("angles").isVector()) n.angles = o->get("angles").asVector();
        n.speed = numberField(o, "speed", -1.0f);
        if (n.speed >= 0.0f) n.speed *= kMphToIps;
        n.lookahead = numberField(o, "lookahead", -1.0f);
        vnodeIndex_[o.get()] = int(vnodes_.size());
        vnodes_.push_back(std::move(n));
    }
    const int count = int(vnodes_.size());
    for (int i = 0; i < count; ++i) {
        VehicleNode& n = vnodes_[size_t(i)];
        n.next = nodeByName(vnodes_, n.target);
        for (int j = 0; j < count; ++j)
            if (j != i && !n.name.empty() && vnodes_[size_t(j)].target == n.name) { n.prev = j; break; }
        if (n.next == i) n.next = -1;
        if (n.prev == i) n.prev = -1;
    }
    for (VehicleNode& n : vnodes_) {
        if (n.next < 0) continue;
        n.dir = sub(vnodes_[size_t(n.next)].origin, n.origin);
        n.length = normalize(n.dir);
        if (!n.rotate) n.angles = vectorToAngles(n.dir);
    }
    for (int i = 0; i < count; ++i) {
        VehicleNode& n = vnodes_[size_t(i)];
        n.speed = resolveNodeValue(vnodes_, i, &VehicleNode::speed, false);
        n.lookahead = resolveNodeValue(vnodes_, i, &VehicleNode::lookahead, true);
        if (n.speed < 0.0f) print("Vehicle path node at( " + std::to_string(n.origin[0]) + ", " + std::to_string(n.origin[1]) + ", " +
                                  std::to_string(n.origin[2]) + " ) has negative speed");
        if (n.rotate) n.angles = resolveRotateAngles(vnodes_, i);
        for (float& a : n.angles) a = angleNormalize180(a);
        if (n.speed <= 0.0f || n.lookahead <= 0.0f) n.next = -1;
        if (n.next < 0) {
            if (n.speed <= 0.0f) n.speed = 1.0f;
            if (n.lookahead <= 0.0f) n.lookahead = 1.0f;
        }
    }
    // Vehicle node fields are read-only engine fields (0x5a95f8): scripts see the resolved values.
    for (const VehicleNode& n : vnodes_) {
        n.obj->set("speed", Value(n.speed));
        n.obj->set("lookahead", Value(n.lookahead));
        n.obj->set("angles", Value(n.angles));
    }
}

int World::vehicleNodeIndex(const Value& v) const {
    if (!v.isObject()) throw std::runtime_error("Not a vehicle node");
    auto it = vnodeIndex_.find(v.asObject().get());
    if (it == vnodeIndex_.end()) throw std::runtime_error("Not a vehicle node");
    return it->second;
}

VehicleState& World::vehicleState(const Value& self) {
    Entity* e = entityOf(self);
    if (!e) throw std::runtime_error("not an entity");
    if (e->classname != "script_vehicle") throw std::runtime_error("entity " + std::to_string(e->number) + " is not a script_vehicle");
    if (!e->vehicle) e->vehicle = std::make_unique<VehicleState>(); // vehicle init 0x50da80: no path, waits unset
    return *e->vehicle;
}

// Path following for vehicles after startpath (0x5110a0); other vehicles stand still here.
// TODO: ground alignment of vehicle types 0/1 (0x50ef70), free physics without a path (0x510d30) and player
// driving are not implemented.
void World::thinkVehicles() {
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (!e.vehicle || e.obj->removed || !e.vehicle->started) continue;
        VehicleState& v = *e.vehicle;
        VehiclePathPos& p = v.path;
        if (p.node < 0) continue;
        const Vec3 prevOrigin = e.obj->get("origin").asVector(), prevAngles = e.obj->get("angles").asVector();
        const float prevSpeed = v.speed;
        if (v.speedMode == 0) {
            v.speed = p.speed;
        } else {
            const float target = v.speedMode == 2 ? p.speed : v.targetSpeed;
            if (target <= v.speed) {
                v.speed -= v.accel * kFrameTime;
                if (v.speed < target) v.speed = target;
            } else {
                v.speed += v.accel * kFrameTime;
                if (target < v.speed) v.speed = target;
            }
            if (v.speedMode == 2 && v.speed == target) v.speedMode = 0;
        }
        if (p.speed > 0.0f) v.stepFrac += v.speed / p.speed;
        VehiclePathPos ahead = p;
        stepPath(vnodes_, ahead, v.waitNode);
        bool passedWait = false;
        while (v.stepFrac > 1.0f) {
            p = ahead;
            v.stepFrac -= 1.0f;
            if (stepPath(vnodes_, ahead, v.waitNode)) passedWait = true;
        }
        if (p.end) v.speed = 0.0f;
        Vec3 origin, angles;
        for (int i = 0; i < 3; ++i) {
            origin[size_t(i)] = (ahead.origin[size_t(i)] - p.origin[size_t(i)]) * v.stepFrac + p.origin[size_t(i)];
            angles[size_t(i)] = lerpAngle(p.angles[size_t(i)], ahead.angles[size_t(i)], v.stepFrac);
        }
        static const float rates[3] = {6.0f, 4.0f, 6.0f};
        for (int i = 0; i < 3; ++i) angles[size_t(i)] = approachAngle(angles[size_t(i)], prevAngles[size_t(i)], rates[i], kFrameTime);
        for (int i = 0; i < 3; ++i) v.velocity[size_t(i)] = (origin[size_t(i)] - prevOrigin[size_t(i)]) * 20.0f;
        e.obj->set("origin", Value(origin));
        e.obj->set("angles", Value(angles));
        if (passedWait && v.waitNode >= 0) vm_.notify(e.obj, "reached_wait_node");
        if (p.end) vm_.notify(e.obj, "reached_end_node");
        if (v.waitSpeed >= 0.0f) {
            const bool up = prevSpeed <= v.waitSpeed && v.speed >= v.waitSpeed;
            const bool down = prevSpeed >= v.waitSpeed && v.speed <= v.waitSpeed;
            if (up || down) vm_.notify(e.obj, "reached_wait_speed");
        }
    }
}

// Vehicle methods (registration table, docs/spec/builtins.md 0x513070…0x513ee0).
void World::registerVehicleBuiltins() {
    vm_.registerBuiltin("attachpath", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        const int n = vehicleNodeIndex(c.arg(0));
        const VehicleNode& node = vnodes_[size_t(n)];
        VehiclePathPos p;
        p.node = n;
        p.speed = node.speed;
        p.lookahead = node.lookahead;
        p.rotateFrac = node.rotate ? 1.0f : 0.0f;
        p.origin = node.origin;
        p.angles = node.angles;
        p.lookPoint = node.origin;
        v.path = p;
        c.self.asObject()->set("origin", Value(node.origin));
        c.self.asObject()->set("angles", Value(node.angles));
        // TODO: types 0/1 also snap to the ground here (0x50ef70 without settling).
        return Value();
    });
    vm_.registerBuiltin("startpath", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        if (v.path.node < 0) throw std::runtime_error("Can't start path on a vehicle that hasn't been attached");
        v.started = true;
        return Value();
    });
    vm_.registerBuiltin("setswitchnode", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        const int from = vehicleNodeIndex(c.arg(0)), to = vehicleNodeIndex(c.arg(1));
        VehiclePathPos& p = v.path;
        p.original = vnodes_[size_t(from)];
        p.switched = vnodes_[size_t(from)];
        p.switched.next = to;
        p.switched.dir = sub(vnodes_[size_t(to)].origin, vnodes_[size_t(from)].origin);
        p.switched.length = normalize(p.switched.dir);
        p.hasSwitch = true;
        return Value();
    });
    vm_.registerBuiltin("setwaitnode", [this](CallContext& c) {
        vehicleState(c.self).waitNode = vehicleNodeIndex(c.arg(0));
        return Value();
    });
    vm_.registerBuiltin("setwaitspeed", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        v.waitSpeed = c.arg(0).asFloat() * kMphToIps;
        if (v.waitSpeed < 0.0f) throw std::runtime_error("Cannot have a negative wait speed on a vehicle");
        return Value();
    });
    vm_.registerBuiltin("setspeed", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        v.speedMode = 1;
        v.targetSpeed = c.arg(0).asFloat() * kMphToIps;
        v.accel = c.arg(1).asFloat() * kMphToIps;
        if (v.targetSpeed < 0.0f) throw std::runtime_error("Cannot set negative speed on vehicle");
        if (v.accel < 0.0f) throw std::runtime_error("Cannot set negative acceleration on vehicle");
        return Value();
    });
    vm_.registerBuiltin("resumespeed", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        v.speedMode = 2;
        v.accel = c.arg(0).asFloat() * kMphToIps;
        if (v.accel < 0.0f) throw std::runtime_error("Cannot set negative acceleration on vehicle");
        return Value();
    });
    vm_.registerBuiltin("getspeed", [this](CallContext& c) { return Value(vehicleState(c.self).speed); });
    vm_.registerBuiltin("getspeedmph", [this](CallContext& c) { return Value(vehicleState(c.self).speed * 0.0568181835f); });
    vm_.registerBuiltin("setvehicleteam", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        const std::string t = c.arg(0).toString();
        if (t == "axis") v.team = 1;
        else if (t == "allies") v.team = 2;
        else if (t == "none") v.team = 0;
        else throw std::runtime_error("setVehicleTeam: invalid team used must be 'axis', 'allies', or 'none'");
        return Value();
    });
    // freevehicle turns the vehicle into a script_vehicle_corpse and notifies "death" (0x513850).
    vm_.registerBuiltin("freevehicle", [this](CallContext& c) {
        vehicleState(c.self);
        Entity* e = entityOf(c.self);
        e->vehicle.reset();
        e->classname = "script_vehicle_corpse";
        e->obj->set("classname", Value("script_vehicle_corpse"));
        vm_.notify(e->obj, "death");
        return Value();
    });
    // No player drives vehicles yet, so a vehicle has no owner (0x513ab0 returns nothing then).
    vm_.registerBuiltin("getvehicleowner", [this](CallContext& c) { vehicleState(c.self); return Value(); });
    vm_.registerBuiltin("makevehicleusable", [this](CallContext& c) { vehicleState(c.self).usable = true; return Value(); });
    vm_.registerBuiltin("makevehicleunusable", [this](CallContext& c) { vehicleState(c.self).usable = false; return Value(); });
}

} // namespace iw2::game
