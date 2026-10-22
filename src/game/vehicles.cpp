// Script vehicles: info_vehicle_node paths and the path-following think (docs/spec/vehicles.md). The rules mirror
// the executable's path code (node linking 0x52ada0, path step 0x52b290, vehicle think 0x5110a0).
#include "game/world.h"

#include <cmath>
#include <cctype>
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

void anglesToAxis(const Vec3& a, Vec3& fwd, Vec3& right, Vec3& up) {
    const float rad = 0.0174532924f; // 0x591f50
    const float sy = std::sin(a[1] * rad), cy = std::cos(a[1] * rad);
    const float sp = std::sin(a[0] * rad), cp = std::cos(a[0] * rad);
    const float sr = std::sin(a[2] * rad), cr = std::cos(a[2] * rad);
    fwd = {cp * cy, cp * sy, -sp};
    right = {cr * sy - sr * sp * cy, cr * cy * -1.0f - sr * sp * sy, sr * cp * -1.0f};
    up = {sr * sy + cr * sp * cy, cr * sp * sy - sr * cy, cr * cp};
}

// Pitch of a vector (0x421a70).
float pitchOf(const Vec3& v) {
    if (v[1] == 0.0f && v[0] == 0.0f) return v[2] > 0.0f ? -90.0f : 90.0f;
    return std::atan2(v[2], std::sqrt(v[0] * v[0] + v[1] * v[1])) * -180.0f / float(M_PI);
}

// Angles from forward and left axes (0x424b70).
Vec3 axisToAngles(const Vec3& fwd, const Vec3& left) {
    Vec3 a = vectorToAngles(fwd);
    const float rad = 0.0174532924f;
    const float cy = std::cos(-a[1] * rad), sy = std::sin(-a[1] * rad);
    const float x1 = left[0] * cy - left[1] * sy, y1 = left[1] * cy + left[0] * sy;
    const float cp = std::cos(-a[0] * rad), sp = std::sin(-a[0] * rad);
    const float x2 = x1 * cp + left[2] * sp, z2 = left[2] * cp - x1 * sp;
    const float p = pitchOf({x2, y1, z2});
    if (y1 < 0.0f) a[2] = p + (p >= 0.0f ? -180.0f : 180.0f);
    else a[2] = -p;
    return a;
}

std::string lowerCopy(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Surface type names (table 0x5a0344, index 1..22; 0 is "none", others "default").
const char* const kSurfaceNames[23] = {"none", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh", "foliage",
                                       "glass", "grass", "gravel", "ice", "metal", "mud", "paper", "plaster", "rock",
                                       "sand", "snow", "water", "wood", "asphalt"};
// Wheel tags in wheel order (string constants 0x1c7f15a…0x1c7f164, table 0x5a91bc).
const char* const kWheelTags[6] = {"tag_wheel_front_left", "tag_wheel_front_right", "tag_wheel_back_left",
                                   "tag_wheel_back_right", "tag_wheel_middle_left", "tag_wheel_middle_right"};
const char* const kWheelNames[6] = {"front_left", "front_right", "back_left", "back_right", "middle_left", "middle_right"};

// Moves a towards b... at a constant rate (0x420970 / 0x4208f0): returns the angle `from` turned towards `to` by
// rate * dt degrees, or `to` once within one step.
float turnAngle(float to, float from, float rate, float dt) {
    while (to - from > 180.0f) to -= 360.0f;
    while (to - from < -180.0f) to += 360.0f;
    const float diff = to - from;
    float step = rate * dt;
    if (diff <= 0.0f) step = -step;
    float r = to;
    if (std::fabs(diff) > 0.001f && std::fabs(diff) >= std::fabs(step)) r = step + from;
    return angleNormalize180(r);
}

// Difference a - b wrapped into [-180, 180] (0x423f50).
float angleDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
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

// Loads vehicles/<type> (0x50cf60); an unknown type falls back to "defaultvehicle" (0x50d1b0).
const VehicleInfo* World::vehicleInfo(const std::string& type) {
    const std::string key = lowerCopy(type);
    auto it = vehicleInfos_.find(key);
    if (it != vehicleInfos_.end()) return it->second.get();
    std::unique_ptr<VehicleInfo> info;
    if (auto d = vfs_.read("vehicles/" + key)) {
        const std::string text(d->begin(), d->end());
        if (text.rfind("VEHICLEFILE", 0) == 0) {
            std::vector<std::string> parts;
            size_t start = 11;
            if (start < text.size() && text[start] == '\\') ++start;
            for (size_t i = start; i <= text.size(); ++i)
                if (i == text.size() || text[i] == '\\') { parts.push_back(text.substr(start, i - start)); start = i + 1; }
            info = std::make_unique<VehicleInfo>();
            info->name = key;
            static const char* const types[] = {"4 wheel", "tank", "plane", "boat", "artillery"};
            for (size_t i = 0; i + 1 < parts.size(); i += 2) {
                const std::string k = lowerCopy(parts[i]);
                const std::string& v = parts[i + 1];
                const float f = float(std::atof(v.c_str()));
                if (k == "type") {
                    for (int t = 0; t < 5; ++t)
                        if (lowerCopy(v) == types[t]) info->type = t;
                } else if (k == "maxspeed") info->maxSpeed = f * kMphToIps;
                else if (k == "accel") info->accel = f * kMphToIps;
                else if (k == "collisionspeed") info->collisionSpeed = f * kMphToIps;
                else if (k == "enginesndspeed") info->engineSndSpeed = f * kMphToIps;
                else if (k == "suspensiontravel") info->suspensionTravel = f;
                else if (k == "maxbodypitch") info->maxBodyPitch = f;
                else if (k == "maxbodyroll") info->maxBodyRoll = f;
                else if (k == "turretweapon") info->turretWeapon = v;
                else if (k == "turrethorizspanleft") info->turretHorizSpanLeft = f;
                else if (k == "turrethorizspanright") info->turretHorizSpanRight = f;
                else if (k == "turretvertspanup") info->turretVertSpanUp = f;
                else if (k == "turretvertspandown") info->turretVertSpanDown = f;
                else if (k == "turretrotrate") info->turretRotRate = f;
                else if (k == "quadbarrel") info->quadBarrel = std::atoi(v.c_str()) != 0;
            }
        }
    }
    if (!info) {
        if (key == "defaultvehicle") throw std::runtime_error("Cannot find vehicle info for 'defaultvehicle'");
        print("^3WARNING: couldn't find vehicle info for '" + type + "', attempting to use 'defaultvehicle'");
        const VehicleInfo* def = vehicleInfo("defaultvehicle");
        vehicleInfos_[key] = std::make_unique<VehicleInfo>(*def);
        return vehicleInfos_[key].get();
    }
    return (vehicleInfos_[key] = std::move(info)).get();
}

// Settles a wheeled or tracked vehicle on the ground under its wheel tags (0x50ef70): pitch and roll from the
// plane through the wheels, height from that plane. `baseZ` is the height of the previous frame.
void World::alignVehicleToGround(Entity& e, Vec3& origin, Vec3& angles, float baseZ, const Vec3& prevAngles, bool settle) {
    VehicleState& v = *e.vehicle;
    if (!v.info || (v.info->type != 0 && v.info->type != 1)) return;
    const int wheels = v.info->type != 0 ? 6 : 4;
    const Value model = e.obj->get("model");
    const assets::XModelParts* parts = model.isString() ? skeleton(model.asString()) : nullptr;
    if (!parts) return;
    const assets::XPose pose = e.anim ? e.anim->pose(*parts) : assets::bindPose(*parts);
    Vec3 tagPos[6];
    for (int w = 0; w < wheels; ++w) {
        bool found = false;
        for (size_t b = 0; b < parts->bones.size(); ++b)
            if (lowerCopy(parts->bones[b].name) == kWheelTags[w]) { tagPos[w] = pose.position[b]; found = true; break; }
        if (!found) {
            print("Script vehicle [" + model.asString() + "] needs [" + kWheelTags[w] + "]");
            return;
        }
    }
    Vec3 fwd, right, up;
    anglesToAxis(angles, fwd, right, up);
    const Vec3 left{-right[0], -right[1], -right[2]};
    Vec3 p[6];
    for (int w = 0; w < wheels; ++w) {
        const Vec3& o = tagPos[w];
        Vec3 wp;
        const Vec3 base{origin[0], origin[1], baseZ};
        for (int i = 0; i < 3; ++i) wp[size_t(i)] = left[size_t(i)] * o[1] + up[size_t(i)] * o[2] + fwd[size_t(i)] * o[0] + base[size_t(i)];
        const Vec3 start{wp[0], wp[1], wp[2] + 64.0f}, end{wp[0], wp[1], wp[2] - 256.0f};
        const TraceResult tr = collision_->trace(start, end, {0, 0, 0}, {0, 0, 0}, 0x211); // mask 0x10211 when driven
        float hitZ = end[2];
        v.wheelSurface[w] = 0;
        if (tr.fraction < 1.0f) {
            hitZ = (end[2] - start[2]) * tr.fraction + start[2];
            v.wheelSurface[w] = int((tr.surfaceFlags >> 20) & 0x1f);
        }
        if (settle) {
            const float h = (v.wheelVel[w] - 40.0f) * kFrameTime + v.wheelHeight[w];
            v.wheelVel[w] -= 40.0f;
            v.wheelHeight[w] = h;
            if (h < hitZ) { v.wheelHeight[w] = hitZ; v.wheelVel[w] = 0; }
        } else {
            v.wheelHeight[w] = hitZ;
            v.wheelVel[w] = 0;
        }
        p[w] = {wp[0], wp[1], v.wheelHeight[w]};
    }
    Vec3 d1, d2;
    for (int i = 0; i < 3; ++i) {
        d1[size_t(i)] = (p[1][size_t(i)] + p[3][size_t(i)]) * 0.5f - (p[0][size_t(i)] + p[2][size_t(i)]) * 0.5f;
        d2[size_t(i)] = (p[1][size_t(i)] + p[0][size_t(i)]) * 0.5f - (p[2][size_t(i)] + p[3][size_t(i)]) * 0.5f;
    }
    normalize(d1);
    normalize(d2);
    const Vec3 n{d2[2] * d1[1] - d2[1] * d1[2], d1[2] * d2[0] - d2[2] * d1[0], d2[1] * d1[0] - d1[1] * d2[0]};
    float dist = dot(p[0], n);
    for (int w = 1; w < wheels; ++w) {
        const float h = dot(n, p[w]);
        if (v.info->suspensionTravel < h - dist) dist = h - v.info->suspensionTravel;
    }
    Vec3 l{fwd[2] * n[1] - fwd[1] * n[2], fwd[0] * n[2] - fwd[2] * n[0], fwd[1] * n[0] - fwd[0] * n[1]};
    normalize(l);
    Vec3 f{l[1] * n[2] - l[2] * n[1], l[2] * n[0] - l[0] * n[2], l[0] * n[1] - l[1] * n[0]};
    normalize(f);
    const Vec3 a = axisToAngles(f, l);
    angles[0] = std::clamp(approachAngle(a[0], prevAngles[0], 6.0f, kFrameTime), -60.0f, 60.0f);
    angles[2] = std::clamp(approachAngle(a[2], prevAngles[2], 6.0f, kFrameTime), -60.0f, 60.0f);
    if (n[2] != 0.0f) origin[2] = -((n[1] * origin[1] + n[0] * origin[0]) - dist) / n[2];
}

// World position of a model tag on the vehicle (0x4d75b0): the tag's model-space position through the entity's
// axes. Turret bone rotation is not applied (TODO: the client-side turret controllers).
Vec3 World::vehicleTagPosition(Entity& e, const char* tag, bool& found) {
    found = false;
    const Value model = e.obj->get("model");
    const assets::XModelParts* parts = model.isString() ? skeleton(model.asString()) : nullptr;
    const Vec3 origin = e.obj->get("origin").asVector(), angles = e.obj->get("angles").asVector();
    if (!parts) return origin;
    const assets::XPose pose = e.anim ? e.anim->pose(*parts) : assets::bindPose(*parts);
    Vec3 o{};
    for (size_t b = 0; b < parts->bones.size(); ++b)
        if (lowerCopy(parts->bones[b].name) == tag) { o = pose.position[b]; found = true; break; }
    if (!found) return origin;
    Vec3 fwd, right, up;
    anglesToAxis(angles, fwd, right, up);
    Vec3 w;
    for (int i = 0; i < 3; ++i) w[size_t(i)] = up[size_t(i)] * o[2] - right[size_t(i)] * o[1] + fwd[size_t(i)] * o[0] + origin[size_t(i)];
    return w;
}

// Turns the turret towards its target (0x5102e0): relative pitch/yaw of the target seen from tag_barrel in the
// vehicle's frame, approached at turretRotRate degrees per second, clamped to the turret spans. Within 1° on both
// axes it notifies "turret_on_target", and for an entity target "turret_on_vistarget" when the line from the barrel
// to the target is clear.
void World::updateVehicleTurret(Entity& e) {
    VehicleState& v = *e.vehicle;
    const Value h = e.obj->get("health");
    if (!v.hasTarget || !h.isNumber() || h.asInt() < 1 || !v.info) return;
    Entity* target = v.targetEnt ? entityOf(Value(v.targetEnt)) : nullptr;
    if (v.targetEnt && (!target || target->obj->removed)) return;
    bool found = false;
    const Vec3 barrel = vehicleTagPosition(e, "tag_barrel", found);
    if (!found) return;
    Vec3 point = v.targetVec;
    if (target) {
        const Vec3 o = target->obj->get("origin").asVector();
        for (int i = 0; i < 3; ++i) point[size_t(i)] = o[size_t(i)] + v.targetOffset[size_t(i)];
    }
    Vec3 dir = sub(point, barrel);
    normalize(dir);
    Vec3 fwd, right, up;
    anglesToAxis(e.obj->get("angles").asVector(), fwd, right, up);
    const Vec3 local{dot(dir, fwd), -dot(dir, right), dot(dir, up)};
    Vec3 want = vectorToAngles(local);
    const float dPitch = angleDelta(want[0], v.turretPitch), dYaw = angleDelta(want[1], v.turretYaw);
    v.turretPitch = turnAngle(want[0], v.turretPitch, v.info->turretRotRate, kFrameTime);
    v.turretYaw = turnAngle(want[1], v.turretYaw, v.info->turretRotRate, kFrameTime);
    v.turretPitch = std::clamp(v.turretPitch, -v.info->turretVertSpanUp, v.info->turretVertSpanDown);
    v.turretYaw = std::clamp(v.turretYaw, -v.info->turretHorizSpanRight, v.info->turretHorizSpanLeft);
    e.obj->set("$turretangles", Value(Vec3{v.turretPitch, v.turretYaw, 0}));
    if (std::fabs(dPitch) < 1.0f && std::fabs(dYaw) < 1.0f) {
        vm_.notify(e.obj, "turret_on_target");
        if (target) {
            const TraceResult tr = collision_->trace(barrel, point, {0, 0, 0}, {0, 0, 0}, 0x801);
            if (tr.fraction >= 1.0f) vm_.notify(e.obj, "turret_on_vistarget");
        }
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
    if (!e->vehicle) {
        e->vehicle = std::make_unique<VehicleState>(); // vehicle init 0x50da80: no path, waits unset
        e->vehicle->info = vehicleInfo(e->obj->get("vehicletype").toString());
    }
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
        alignVehicleToGround(e, origin, angles, prevOrigin[2], prevAngles, true);
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
    for (const auto& ep : entities_)
        if (ep->vehicle && !ep->obj->removed) updateVehicleTurret(*ep);
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
        Vec3 origin = node.origin, angles = node.angles;
        alignVehicleToGround(*entityOf(c.self), origin, angles, origin[2], node.angles, false);
        c.self.asObject()->set("origin", Value(origin));
        c.self.asObject()->set("angles", Value(angles));
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
    auto requireTurret = [this](CallContext& c) -> VehicleState& {
        VehicleState& v = vehicleState(c.self);
        const Value h = c.self.asObject()->get("health");
        if (!h.isNumber() || h.asFloat() <= 0.0f) throw std::runtime_error("Vehicle must have health to control the turret");
        return v;
    };
    vm_.registerBuiltin("setturrettargetvec", [requireTurret](CallContext& c) {
        VehicleState& v = requireTurret(c);
        v.hasTarget = true;
        v.targetEnt.reset();
        v.targetVec = c.arg(0).asVector();
        v.targetOffset = {0, 0, 0};
        return Value();
    });
    vm_.registerBuiltin("setturrettargetent", [this, requireTurret](CallContext& c) {
        VehicleState& v = requireTurret(c);
        if (!entityOf(c.arg(0))) throw std::runtime_error("not an entity");
        v.hasTarget = true;
        v.targetEnt = c.arg(0).asObject();
        v.targetVec = {0, 0, 0};
        v.targetOffset = c.arg(1).isVector() ? c.arg(1).asVector() : Vec3{0, 0, 0};
        return Value();
    });
    vm_.registerBuiltin("clearturrettarget", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        v.hasTarget = false;
        v.targetEnt.reset();
        v.targetVec = v.targetOffset = {0, 0, 0};
        return Value();
    });
    // fireturret (0x514260): bullet weapons fire one 8192-unit bullet from the barrel (two for quadBarrel,
    // alternating the flash tags); projectile weapons need projectiles, which are not implemented (TODO).
    vm_.registerBuiltin("fireturret", [this, requireTurret](CallContext& c) {
        VehicleState& v = requireTurret(c);
        Entity* e = entityOf(c.self);
        if (!v.info || v.info->turretWeapon.empty()) throw std::runtime_error("No weapon specified for [" + e->obj->get("model").toString() + "]");
        const auto* w = weapon(v.info->turretWeapon);
        const std::string type = w && w->count("weapontype") ? w->at("weapontype") : std::string();
        if (type != "bullet" && type != "projectile") throw std::runtime_error("Vehicles only support bullet and projectile weapons");
        bool found = false;
        vehicleTagPosition(*e, "tag_barrel", found);
        if (!found) throw std::runtime_error("No tag_barrel for [" + e->obj->get("model").toString() + "]");
        if (type == "bullet") {
            static const char* const flash[4] = {"tag_flash", "tag_flash_11", "tag_flash_2", "tag_flash_22"};
            const int count = v.info->quadBarrel ? 2 : 1;
            const int first = v.info->quadBarrel && v.altBarrel ? 2 : 0;
            // Direction: the body's axes turned by the turret angles (inferred: the original reads the flash tag's
            // axis, which the turret bone controllers rotate).
            Vec3 a = e->obj->get("angles").asVector();
            a[0] += v.turretPitch;
            a[1] += v.turretYaw;
            Vec3 fwd, right, up;
            anglesToAxis(a, fwd, right, up);
            for (int k = 0; k < count; ++k) {
                bool ok = false;
                const Vec3 start = vehicleTagPosition(*e, flash[first + k], ok);
                if (!ok) throw std::runtime_error(std::string("No ") + flash[first + k] + " for [" + e->obj->get("model").toString() + "]");
                const Vec3 end{start[0] + fwd[0] * 8192.0f, start[1] + fwd[1] * 8192.0f, start[2] + fwd[2] * 8192.0f};
                fireBullet(*e, start, end, weaponDamage(v.info->turretWeapon));
            }
        }
        if (v.info->quadBarrel) v.altBarrel = !v.altBarrel;
        return Value();
    });
    vm_.registerBuiltin("getwheelsurface", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        if (!v.info || (v.info->type != 0 && v.info->type != 1))
            throw std::runtime_error("Vehicle type [" + (v.info ? v.info->name : std::string()) + "] has no wheels");
        const std::string w = c.arg(0).toString();
        int i = -1;
        for (int k = 0; k < 6; ++k)
            if (w == kWheelNames[k]) i = k;
        if (i < 0) throw std::runtime_error("unknown wheel " + w);
        if (v.info->type == 0 && i > 3) throw std::runtime_error("Vehicle has no middle wheels");
        const int s = v.wheelSurface[i];
        return Value(s == 0 ? "none" : s < 23 ? kSurfaceNames[s] : "default");
    });
    vm_.registerBuiltin("makevehicleusable", [this](CallContext& c) { vehicleState(c.self).usable = true; return Value(); });
    vm_.registerBuiltin("makevehicleunusable", [this](CallContext& c) { vehicleState(c.self).usable = false; return Value(); });
}

} // namespace iw2::game
