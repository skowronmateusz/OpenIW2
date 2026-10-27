// Script vehicles: info_vehicle_node paths and the path-following think (docs/spec/vehicles.md). The rules mirror
// the executable's path code (node linking 0x52ada0, path step 0x52b290, vehicle think 0x5110a0).
#include "game/world.h"
#include "game/vehicledrive.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
                else if (k == "rotrate") info->rotRate = f;
                else if (k == "rotaccel") info->rotAccel = f;
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
                else if (k == "turretspinsnd") info->turretSpinSnd = v;
                else if (k == "turretstopsnd") info->turretStopSnd = v;
                else if (k == "lowidlesnd") info->lowIdleSnd = v;
                else if (k == "highidlesnd") info->highIdleSnd = v;
                else if (k == "lowenginesnd") info->lowEngineSnd = v;
                else if (k == "highenginesnd") info->highEngineSnd = v;
                else if (k == "texurescroll") info->texScroll = std::atoi(v.c_str());
                else if (k == "texurescrollscale") info->texScrollScale = f;
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
        const bool driven = v.owner >= 0;
        const TraceResult tr = collision_->trace(start, end, {0, 0, 0}, {0, 0, 0}, driven ? 0x10211u : 0x211u);
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
    // The height follows the plane unless a player drives the vehicle off a path (then the physics keep it).
    if ((v.started || v.owner < 0) && n[2] != 0.0f) origin[2] = -((n[1] * origin[1] + n[0] * origin[0]) - dist) / n[2];
    for (int i = 0; i < 3; ++i) v.angularVelocity[size_t(i)] = angleDelta(angles[size_t(i)], prevAngles[size_t(i)]) * 20.0f;
}

// World position of a model tag on the vehicle (0x4d75b0): the tag's model-space position through the entity's
// axes, with the turret bone controls of the server controller 0x5127a0 (DObj controls through 0x4746e0): bone info+0x210
// (tag_turret) gets angles (0, es+0x74 = turret yaw, 0), bone info+0x214 (tag_barrel) gets (es+0x70 = turret pitch, 0, 0).
// TODO(spec): the body-jolt control on bone info+0x20c (angles es+0x64, 0, es+0x6c); that the three info slots are
// tag_body / tag_turret / tag_barrel follows the init order 0x5071b4..0x5071e3 and the cgame use (0x4ac354).
// turnedForward (optional): the world direction the body's forward axis takes through the controls of this tag
// (rotation of the controlled tag times the inverse of its bind rotation, rotated into the world).
Vec3 World::vehicleTagPosition(Entity& e, const char* tag, bool& found, Vec3* turnedForward) {
    found = false;
    const Value model = e.obj->get("model");
    const assets::XModelParts* parts = model.isString() ? skeleton(model.asString()) : nullptr;
    const Vec3 origin = e.obj->get("origin").asVector(), angles = e.obj->get("angles").asVector();
    if (!parts) return origin;
    assets::XPose pose = e.anim ? e.anim->pose(*parts) : assets::bindPose(*parts);
    const assets::XPose uncontrolled = pose;
    if (e.vehicle) {
        std::vector<std::pair<int, assets::Quat>> ctl;
        for (size_t b = 0; b < parts->bones.size(); ++b) {
            const std::string n = lowerCopy(parts->bones[b].name);
            if (n == "tag_body") ctl.push_back({int(b), assets::anglesQuat(e.vehicle->joltPitch, 0, e.vehicle->joltRoll)});
            else if (n == "tag_turret") ctl.push_back({int(b), assets::anglesQuat(0, e.vehicle->turretYaw, 0)});
            else if (n == "tag_barrel") ctl.push_back({int(b), assets::anglesQuat(e.vehicle->turretPitch, 0, 0)});
        }
        pose = assets::applyControls(*parts, pose, ctl);
    }
    Vec3 o{};
    size_t tagBone = 0;
    for (size_t b = 0; b < parts->bones.size(); ++b)
        if (lowerCopy(parts->bones[b].name) == tag) { o = pose.position[b]; tagBone = b; found = true; break; }
    if (!found) return origin;
    Vec3 fwd, right, up;
    anglesToAxis(angles, fwd, right, up);
    if (turnedForward) {
        const assets::Quat& r0 = uncontrolled.rotation[tagBone];
        const assets::Quat d = assets::multiply(pose.rotation[tagBone], assets::Quat{-r0.x, -r0.y, -r0.z, r0.w});
        const auto m = assets::rotate(d, {1, 0, 0});
        for (int i = 0; i < 3; ++i)
            (*turnedForward)[size_t(i)] = up[size_t(i)] * m[2] - right[size_t(i)] * m[1] + fwd[size_t(i)] * m[0];
    }
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
    Vec3 want;
    Vec3 frame = e.obj->get("angles").asVector();
    if (v.owner >= 0 && player_ && v.owner == player_->number) {
        // Driven: the wanted direction keeps the view yaw (roll 0); the frame is the body, with the view yaw while
        // the vehicle turns towards the view (ps 0x40000 clear). Relative angles = axis(want) · axis(frame)ᵀ.
        const float viewYaw = playerCmd_.viewAngles[1];
        if (!(pmFlags_ & 0x40000)) frame[1] = viewYaw;
        Vec3 wa = vectorToAngles(dir);
        wa[1] = viewYaw;
        wa[2] = 0.0f;
        Vec3 wf, wr, wu;
        anglesToAxis(wa, wf, wr, wu);
        anglesToAxis(frame, fwd, right, up);
        const Vec3 left{-right[0], -right[1], -right[2]}, wl{-wr[0], -wr[1], -wr[2]};
        want = axisToAngles({dot(wf, fwd), dot(wf, left), dot(wf, up)}, {dot(wl, fwd), dot(wl, left), dot(wl, up)});
    } else {
        anglesToAxis(frame, fwd, right, up);
        const Vec3 local{dot(dir, fwd), -dot(dir, right), dot(dir, up)};
        want = vectorToAngles(local);
    }
    const float dPitch = angleDelta(want[0], v.turretPitch), dYaw = angleDelta(want[1], v.turretYaw);
    v.turretPitch = turnAngle(want[0], v.turretPitch, v.info->turretRotRate, kFrameTime);
    v.turretYaw = turnAngle(want[1], v.turretYaw, v.info->turretRotRate, kFrameTime);
    const float turnedPitch = v.turretPitch, turnedYaw = v.turretYaw; // before the span clamp (stack 0x20/0x24)
    v.turretPitch = std::clamp(v.turretPitch, -v.info->turretVertSpanUp, v.info->turretVertSpanDown);
    v.turretYaw = std::clamp(v.turretYaw, -v.info->turretHorizSpanRight, v.info->turretHorizSpanLeft);
    e.obj->set("$turretangles", Value(Vec3{v.turretPitch, v.turretYaw, 0}));
    // +0x18c (0x510620..0x51068c): 2 when |dPitch| >= 2 and the turned (unclamped) pitch is exactly 0, or |dYaw| >= 2 and the
    // turned yaw is exactly 0 (slots: |delta| 0x14/0x18, turned angles 0x20/0x24, constants 2.0 @0x591ed0 and 0.0
    // @0x591e00); otherwise 2 -> 1 -> 0. The "== 0" test is as the asm reads, not a guess.
    if ((std::fabs(dPitch) >= 2.0f && turnedPitch == 0.0f) || (std::fabs(dYaw) >= 2.0f && turnedYaw == 0.0f)) v.turretMoved = true;
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
        const Value sf = e->obj->get("spawnflags");
        e->vehicle->usable = sf.isInt() && (sf.asInt() & 1) != 0; // 0x50dcd5
    }
    return *e->vehicle;
}

// Path following after startpath (0x5110a0).
void World::vehiclePathMove(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles) {
    VehicleState& v = *e.vehicle;
    VehiclePathPos& p = v.path;
    if (p.node < 0) return;
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

// Free movement without a path (0x510d30): the driver's command (zero without one) through the drive physics, then
// ground alignment for wheeled and tracked vehicles.
void World::vehicleFreeMove(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles) {
    VehicleState& v = *e.vehicle;
    DriveCommand cmd;
    if (v.owner >= 0 && player_ && v.owner == player_->number) {
        pmFlags_ |= 0x40000;
        // TODO(spec): the client state bit ps+0xc 0x8000 that also blocks the command is not tracked.
        if (!(pmFlags_ & 0x80000)) {
            cmd.forward = playerCmd_.forward;
            cmd.right = playerCmd_.right;
            cmd.jump = (playerCmd_.buttons & 0x400) ? 127 : 0;
            if (cmd.jump) pmFlags_ &= ~0x40000u;
        }
        cmd.viewYaw = playerCmd_.viewAngles[1];
    }
    DriveParams params;
    if (v.info) params = {v.info->maxSpeed, v.info->accel, v.info->rotRate, v.info->rotAccel};
    if (v.joltAmp > 0.0f) params.maxSpeed = v.joltSpeed, params.accel = v.joltDecel; // 0x50de51..0x50de87
    DriveBody b;
    b.origin = e.obj->get("origin").asVector();
    b.angles = e.obj->get("angles").asVector();
    b.prevAngles = prevAngles;
    b.velocity = v.velocity;
    b.localVelocity = v.localVelocity;
    b.angularVelocity = v.angularVelocity;
    DriveTrace t;
    t.passEntity = e.number;
    driveMove(*collision_, b, params, cmd, t);
    v.velocity = b.velocity;
    v.localVelocity = b.localVelocity;
    v.angularVelocity = b.angularVelocity;
    Vec3 origin = b.origin, angles = b.angles;
    alignVehicleToGround(e, origin, angles, prevOrigin[2], prevAngles, true); // also sets the angular velocity
    v.forwardSpeed = std::fabs(b.localVelocity[0]);
    e.obj->set("origin", Value(origin));
    e.obj->set("angles", Value(angles));
    // Engine sound fractions (0x510fe6): DiffTrack at rate 4 per second towards idle 1 / engine 0 while the driver's forward
    // command is 0, else idle 0 / engine 1 (the step size is the 0x40800000 = 4 argument times the frame time).
    {
        const float step = 4.0f * kFrameTime;
        const float ti = cmd.forward == 0 ? 1.0f : 0.0f, te = 1.0f - ti;
        v.idleFrac += std::clamp(ti - v.idleFrac, -step, step);
        v.engFrac += std::clamp(te - v.engFrac, -step, step);
    }
}

// Aim of a player-driven vehicle (0x5108e0): the turret target becomes the point the player looks at (eye + view
// direction × 10240, cut short by a trace), "turret_fire" is notified while attack is held and the fire timer has
// run out, and a blocked tag_barrel → tag_flash line marks the shot as unbent (+0x19c).
void World::vehiclePlayerAim(Entity& e) {
    VehicleState& v = *e.vehicle;
    v.hasTarget = false;
    v.targetEnt.reset();
    v.targetOffset = {0, 0, 0};
    Entity* owner = v.owner >= 0 ? entityByNumber(v.owner) : nullptr;
    if (!owner || owner != player_ || (pmFlags_ & 0x80000)) return;
    if (v.fireTimerMs < 1) {
        // TODO(spec): ps+0xc bit 0x8000 also blocks firing; it is not tracked.
        if (playerCmd_.buttons & 1) vm_.notify(e.obj, "turret_fire");
    } else {
        v.fireTimerMs -= 50;
    }
    v.hasTarget = true;
    bool flashFound = false, barrelFound = false;
    const Vec3 flash = vehicleTagPosition(e, "tag_flash", flashFound);
    const Vec3 barrel = vehicleTagPosition(e, "tag_barrel", barrelFound);
    if (!flashFound) throw std::runtime_error("Player vehicle has no tag_flash");
    if (!barrelFound) throw std::runtime_error("Player vehicle has no tag_barrel");
    const Vec3 o = owner->obj->get("origin").asVector();
    const Vec3 eye{o[0], o[1], o[2] + 60.0f}; // in a vehicle the player stands with view height 60 (0x509d10)
    const float rad = 0.0174532924f;
    const float yaw = playerCmd_.viewAngles[1] * rad, pitch = playerCmd_.viewAngles[0] * rad;
    const Vec3 dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
    const Vec3 end{dir[0] * 10240.0f + eye[0], dir[1] * 10240.0f + eye[1], dir[2] * 10240.0f + eye[2]};
    v.aimPoint = end;
    const TraceResult tr = collision_->trace(eye, end, {0, 0, 0}, {0, 0, 0}, 0x280e891, owner->number);
    if (tr.fraction < 1.0f)
        for (int i = 0; i < 3; ++i) v.aimPoint[size_t(i)] = (end[size_t(i)] - eye[size_t(i)]) * tr.fraction + eye[size_t(i)];
    v.targetVec = v.aimPoint;
    const TraceResult sight = collision_->trace(barrel, flash, {0, 0, 0}, {0, 0, 0}, 0x810211, e.number);
    if (sight.fraction < 1.0f || sight.startSolid) v.sightBlocked = true;
}

// Vehicle think (0x512b80), for every script_vehicle: the owner dies → it is let out; types other than artillery
// move along their path or freely; a driven vehicle aims where the player looks; then the turret.
// TODO(spec): the remaining calls 0x510770 / 0x5107e0 / 0x510870 / 0x510be0.

// G_TryPushingEntity (0x52be60): the entity is moved by the pusher's move and turned about the pusher's origin by its
// turn; if that box position (entity clip mask, 0x811 without one; the entity passes) is blocked (0x52bd20) offsets
// are searched: z 0, ±4, ±8 … and x, y ±4, ±8 … below maxs.x / 2 (only when that exceeds 4), the first free one is
// taken; a pushed client's view yaw turns with the pusher. Returns false when no position is free.
// TODO(spec): the ground entity reset, the actor look yaws (0x504910), the player's mins/maxs per stance.
bool World::tryPushEntity(Entity& ent, Entity& pusher, const Vec3& move, const Vec3& turn) {
    const Vec3 o = ent.obj->get("origin").asVector(), po = pusher.obj->get("origin").asVector();
    const Vec3 moved{o[0] + move[0], o[1] + move[1], o[2] + move[2]};
    const Axis r = anglesAxis(turn);
    const Vec3 rel{moved[0] - po[0], moved[1] - po[1], moved[2] - po[2]};
    const Vec3 base = axisApply(po, r, rel);
    const Vec3 mins{-15, -15, 0}, maxs{15, 15, ent.isActor ? 72.0f : 70.0f};
    const uint32_t mask = ent.isActor ? 0x2820011u : 0x281c011u; // r.clipmask: actor spawn 0x4fc745, player 0x4e90ec
    auto blocked = [&](const Vec3& p) { return collision_->trace(p, p, mins, maxs, mask, ent.number).startSolid; };
    auto commit = [&](const Vec3& p) {
        ent.obj->set("origin", Value(p));
        if (ent.isPlayer) {
            playerTeleport_ = p;
            Vec3 va = playerCmd_.viewAngles;
            va[1] += turn[1];
            playerViewAngles_ = va;
        }
        return true;
    };
    if (!blocked(base)) return commit(base);
    const float half = maxs[0] / 2.0f;
    if (half > 4.0f) {
        for (float z = 0; z < half; z += 4.0f)
            for (float zs = -z; zs <= z; zs += (z == 0 ? 1.0f : 2.0f * z))
                for (float x = 4.0f; x < half; x += 4.0f)
                    for (float xs = -x; xs <= x; xs += 2.0f * x)
                        for (float y = 4.0f; y < half; y += 4.0f)
                            for (float ys = -y; ys <= y; ys += 2.0f * y) {
                                const Vec3 p{base[0] + xs, base[1] + ys, base[2] + zs};
                                if (!blocked(p)) return commit(p);
                            }
    }
    return !blocked(base);
}

// Touch (0x50ff00) of a solid vehicle after its move: players and living actors in contact with its collmap (a
// zero-length box trace against contents 0x800000 that starts in the vehicle) are notified "touch" (and the vehicle
// with them); then, unless linked or the vehicle neither moved nor turned (≥ 0.001 squared), they are pushed and
// one that cannot be pushed is crushed (999999, MOD_CRUSH = 9) — not an actor in the animscripted or negotiation
// state (0x4ff310). TODO(spec): script movers (eType 5) and the touch callbacks; the entity list box.
void World::vehicleTouch(Entity& e, const Vec3& prevOrigin, const Vec3& prevAngles) {
    if (vehicleCollmap(e).empty()) return;
    const Vec3 o = e.obj->get("origin").asVector(), a = e.obj->get("angles").asVector();
    const Vec3 move{o[0] - prevOrigin[0], o[1] - prevOrigin[1], o[2] - prevOrigin[2]};
    const Vec3 turn{angleSubtract(a[0], prevAngles[0]), angleSubtract(a[1], prevAngles[1]), angleSubtract(a[2], prevAngles[2])};
    const bool moving = move[0] * move[0] + move[1] * move[1] + move[2] * move[2] >= 0.001f ||
                        turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2] >= 0.001f;
    syncEntityClips();
    const int owner = e.vehicle ? e.vehicle->owner : -1;
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& t = *entities_[i];
        if (&t == &e || t.number == owner || t.obj->removed || !(t.isPlayer || t.isActor) || !isAlive(t)) continue;
        const Vec3 to = t.obj->get("origin").asVector();
        const Vec3 mins{-15, -15, 0}, maxs{15, 15, t.isActor ? 72.0f : 70.0f};
        const TraceResult tr = collision_->trace(to, to, mins, maxs, 0x800000u, t.number);
        if (!tr.startSolid || tr.entity != e.number) continue;
        vm_.notify(e.obj, "touch", {Value(t.obj)});
        vm_.notify(t.obj, "touch", {Value(e.obj)});
        if (links_.count(t.number) || (t.isPlayer && playerLinked()) || !moving) continue;
        if (t.isActor && (std::find(t.ai.predicted.begin(), t.ai.predicted.begin() + t.ai.predictedDepth + 1, int(AiScripted)) !=
                              t.ai.predicted.begin() + t.ai.predictedDepth + 1 ||
                          std::find(t.ai.predicted.begin(), t.ai.predicted.begin() + t.ai.predictedDepth + 1, int(AiNegotiation)) !=
                              t.ai.predicted.begin() + t.ai.predictedDepth + 1))
            continue;
        if (tryPushEntity(t, e, move, turn)) continue;
        if (std::getenv("IW2_ROCKET_DEBUG")) print("vehicle " + std::to_string(e.number) + " crushes " + std::to_string(t.number));
        damage(t, &e, 999999, {0, 0, 0}, to, "MOD_CRUSH", "none");
    }
}

// Path vehicles (0x511513): f = clamp(|speed| / engineSndSpeed); engine = f, idle = 1 - f.
// Every think (0x510be0): a dead vehicle or one with the engine off silences both pairs; otherwise the loops follow
// the fractions. The original culls by the listener distance (0x44247b) and keeps the pitch chosen at the loop start.
// TODO(spec): the turret spin / stop sounds (turretSpinSnd es+6 loop, turretStopSnd event 0xb3).
// 0x50dcf0: direction (world) against the body's forward / right axes, 2D-normalized (0x421160), scaled by the
// intensity (clamped 0..1) and info maxBodyPitch (+0x78) / maxBodyRoll (+0x7c); amplitude 1, phase 0.
// +0x1e4 / +0x1e8 (speed fraction x |speed|, deceleration) replace maxSpeed / accel in the free physics while it runs (0x50de40).
void World::vehicleJolt(Entity& e, const Vec3& dir, float intensity, float speedFraction, float decel) {
    VehicleState& v = *e.vehicle;
    if (!v.info) return;
    Vec3 f, r, u;
    anglesToAxis(e.obj->get("angles").asVector(), f, r, u);
    float a = dot(dir, f), b = dot(dir, r);
    const float len = std::sqrt(a * a + b * b);
    if (len > 0.0f) a /= len, b /= len;
    const float s = intensity > 0.0f ? std::min(intensity, 1.0f) : 0.0f;
    v.joltDirF = a * s * v.info->maxBodyPitch;
    v.joltDirR = b * s * v.info->maxBodyRoll;
    v.joltAmp = 1.0f;
    v.joltPhase = 0.0f;
    v.joltSpeed = speedFraction * (v.started ? std::fabs(v.speed) : v.forwardSpeed); // +0x1e4 = fraction * +0x1b0
    v.joltDecel = decel;                                                                // +0x1e8
}

void World::updateVehicleEngineSound(Entity& e) {
    VehicleState& v = *e.vehicle;
    if (!v.info) return;
    if (v.started && v.info->engineSndSpeed > 0.0f) {
        v.engFrac = std::clamp(std::fabs(v.speed) / v.info->engineSndSpeed, 0.0f, 1.0f);
        v.idleFrac = 1.0f - v.engFrac;
    }
    const Value h = e.obj->get("health");
    const bool alive = !h.isNumber() || h.asInt() >= 1;
    const Vec3 origin = e.obj->get("origin").asVector();
    auto pair = [&](int& handle, const std::string& lo, const std::string& hi, float f) {
        const bool want = alive && v.engineOn && !lo.empty() && !hi.empty();
        if (!want) {
            if (handle) stopSound(handle);
            handle = 0;
        } else if (!handle) handle = playBlendSound(lo, hi, f, origin);
        else setBlendSound(handle, f, origin);
    };
    // Turret sounds (0x510be0): state 2 loops turretSpinSnd (es+6), state 1 plays turretStopSnd once (event 0xb3).
    if (v.turretSnd == 2 && alive && !v.info->turretSpinSnd.empty()) {
        if (!v.spinHandle) v.spinHandle = playSound(v.info->turretSpinSnd, &origin, nullptr);
    } else if (v.spinHandle) {
        stopSound(v.spinHandle);
        v.spinHandle = 0;
    }
    if (v.turretSnd == 1 && alive && !v.info->turretStopSnd.empty()) playSound(v.info->turretStopSnd, &origin, nullptr);
    pair(v.idleHandle, v.info->lowIdleSnd, v.info->highIdleSnd, v.idleFrac);
    pair(v.engHandle, v.info->lowEngineSnd, v.info->highEngineSnd, v.engFrac);
}

std::string World::playerVehicleTurretWeapon() {
    const Entity* v = player_ && player_->usingVehicle >= 0 ? entityByNumber(player_->usingVehicle) : nullptr;
    return v && v->vehicle && v->vehicle->info ? v->vehicle->info->turretWeapon : std::string();
}

void World::thinkVehicles() {
    for (size_t i = 0; i < entities_.size(); ++i) {
        Entity& e = *entities_[i];
        if (e.obj->removed || e.classname != "script_vehicle") continue;
        VehicleState& v = vehicleState(Value(e.obj));
        const Vec3 prevOrigin = e.obj->get("origin").asVector(), prevAngles = e.obj->get("angles").asVector();
        if (v.owner >= 0) {
            Entity* owner = entityByNumber(v.owner);
            const Value h = owner ? owner->obj->get("health") : Value();
            if (owner && (!h.isNumber() || h.asInt() < 1)) unlinkPlayerFromVehicle(e, *owner);
        }
        if (!e.vehicle || e.obj->removed) continue;
        if (v.info && v.info->type != 4) {
            if (v.started) vehiclePathMove(e, prevOrigin, prevAngles);
            else vehicleFreeMove(e, prevOrigin, prevAngles);
        }
        // Path notifies run scripts at once; freevehicle there drops the vehicle state.
        if (!e.vehicle || e.obj->removed) continue;
        vehicleTouch(e, prevOrigin, prevAngles);
        v.sightBlocked = false;
        if (v.owner >= 0) vehiclePlayerAim(e); // +0x174 bit 0, set by VEH_LinkPlayer
        v.turretMoved = false;
        updateVehicleTurret(e);
        if (v.turretMoved) v.turretSnd = 2;
        else if (v.turretSnd == 2) v.turretSnd = 1;
        else if (v.turretSnd == 1) v.turretSnd = 0;
        if (e.vehicle) vehicleJoltStep(v.joltAmp, v.joltPhase, v.joltDirF, v.joltDirR, v.joltPitch, v.joltRoll); // 0x510770
        // +0x1b0 is the path speed in path mode (0x5110a0: stepFrac +0x1ac += +0x1b0 / pointSpeed) and |forward speed| in free physics.
        // 0x510870 (after the turret/aim calls, before 0x510be0): tread texture scroll accumulator.
        if (v.info) {
            const auto dv = cvars.find("g_vehicleTexScrollScale");
            const float dvar = dv == cvars.end() ? 0.0f : float(std::atof(dv->second.c_str()));
            v.texScroll = vehicleTexScrollStep(v.info->texScroll, v.texScroll, v.started ? v.speed : v.forwardSpeed, dvar, v.info->texScrollScale);
        }
        updateVehicleEngineSound(e);
        // The driver rides at tag_player (linked in VEH_LinkPlayer).
        if (Entity* owner = v.owner >= 0 ? entityByNumber(v.owner) : nullptr) {
            bool found = false;
            const Vec3 pos = vehicleTagPosition(e, "tag_player", found);
            if (found) owner->obj->set("origin", Value(pos));
        }
    }
}

// A player starts using a vehicle (VEH_LinkPlayer 0x511610): the player is placed at tag_player with the vehicle's
// pitch and yaw (roll 0) and linked to that tag; both entities record each other.
// pm_flags become 0x60000 (in a vehicle, view turns with it; 0x80000 cleared).
// TODO(spec): the second tag the original prefers for the angles (vehicle +0x214).
void World::linkPlayerToVehicle(Entity& vehicle, Entity& player) {
    VehicleState& v = *vehicle.vehicle;
    if (player.usingVehicle >= 0) throw std::runtime_error("VEH_LinkPlayer: Player is already using a vehicle");
    if (v.owner >= 0) throw std::runtime_error("VEH_LinkPlayer: Vehicle already has an owner");
    bool found = false;
    const Vec3 pos = vehicleTagPosition(vehicle, "tag_player", found);
    if (!found) throw std::runtime_error("VEH_LinkPlayer: Trying to use vehicle without a bone [tag_player]");
    const Vec3 a = vehicle.obj->get("angles").asVector();
    player.obj->set("origin", Value(pos));
    player.obj->set("angles", Value(Vec3{a[0], a[1], 0.0f}));
    Link l;
    l.parent = vehicle.number;
    l.tag = "tag_player";
    links_[player.number] = l;
    if (v.targetEnt && v.targetEnt == player.obj) { // a turret aimed at the new driver loses its target
        v.hasTarget = false;
        v.targetEnt.reset();
    }
    v.owner = player.number;
    player.usingVehicle = vehicle.number;
    if (&player == player_) pmFlags_ = (pmFlags_ & ~0x80000u) | 0x60000u;
}

// The player leaves the vehicle (VEH_UnlinkPlayer 0x511780): unlinked and placed at tag_detach, keeping its view
// pitch and yaw; pm_flags 0xe0000 are cleared.
void World::unlinkPlayerFromVehicle(Entity& vehicle, Entity& player) {
    if (player.usingVehicle < 0) throw std::runtime_error("VEH_UnlinkPlayer: Player is not using a vehicle");
    links_.erase(player.number);
    bool found = false;
    const Vec3 pos = vehicleTagPosition(vehicle, "tag_detach", found);
    if (!found) print("VEH_UnlinkPlayer: Warning - no [tag_detach] on vehicle"); // then the vehicle origin + maxs z (0)
    player.obj->set("origin", Value(pos));
    // The player keeps its view pitch and yaw (client +0xc4 / +0xc8).
    Vec3 a{0, 0, 0};
    if (&player == player_) a = playerCmd_.viewAngles;
    else if (const Value pa = player.obj->get("angles"); pa.isVector()) a = pa.asVector();
    player.obj->set("angles", Value(Vec3{a[0], a[1], 0.0f}));
    vehicle.vehicle->owner = -1;
    player.usingVehicle = -1;
    if (&player == player_) pmFlags_ &= ~0xe0000u;
}

// A use of a vehicle (0x5341a0 / useby 0x4ef020): "trigger" with the user, then the vehicle's use function
// (0x512f00): a player gets in, or out when already using a vehicle.
void World::useVehicle(Entity& vehicle, Entity& user) {
    vm_.notify(vehicle.obj, "trigger", {Value(user.obj)});
    if (!user.isPlayer || vehicle.obj->removed) return;
    if (user.usingVehicle >= 0) {
        if (Entity* ridden = entityByNumber(user.usingVehicle); ridden && ridden->vehicle) unlinkPlayerFromVehicle(*ridden, user);
    } else {
        linkPlayerToVehicle(vehicle, user);
    }
}

// World bounds of a vehicle for the use scan: the absbox of the vehicle's collmap brush (0x50dc40) at its origin and
// angles. TODO(spec): a vehicle without a collmap; the xmodel bounds stand in for it (inferred).
bool World::vehicleUseBounds(Entity& e, Vec3& mins, Vec3& maxs) {
    std::pair<Vec3, Vec3> local;
    const std::string brush = vehicleCollmap(e);
    if (brush.empty() || !submodelBounds(brush, local.first, local.second)) {
        const Value model = e.obj->get("model");
        if (!model.isString() || !skeleton(model.asString())) return false;
        auto mb = modelBounds_.find(model.asString());
        if (mb == modelBounds_.end()) return false;
        local = mb->second;
    }
    const Vec3 origin = e.obj->get("origin").asVector();
    Vec3 fwd, right, up;
    anglesToAxis(e.obj->get("angles").asVector(), fwd, right, up);
    mins = {1e30f, 1e30f, 1e30f};
    maxs = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const Vec3 l{(c & 1) ? local.second[0] : local.first[0], (c & 2) ? local.second[1] : local.first[1],
                     (c & 4) ? local.second[2] : local.first[2]};
        for (int i = 0; i < 3; ++i) {
            const float w = origin[size_t(i)] + fwd[size_t(i)] * l[0] - right[size_t(i)] * l[1] + up[size_t(i)] * l[2];
            mins[size_t(i)] = std::min(mins[size_t(i)], w);
            maxs[size_t(i)] = std::max(maxs[size_t(i)], w);
        }
    }
    return true;
}

// The ridden vehicle's tag_body axes (rows forward, left, up) for the view (cgame 0x4aeb60).
bool World::playerVehicleBodyAxis(std::array<Vec3, 3>& axis) {
    if (!player_ || player_->usingVehicle < 0 || !(pmFlags_ & 0x20000)) return false;
    Entity* e = entityByNumber(player_->usingVehicle);
    if (!e || e->obj->removed) return false;
    Vec3 fwd, right, up;
    anglesToAxis(e->obj->get("angles").asVector(), fwd, right, up);
    const Vec3 left{-right[0], -right[1], -right[2]};
    assets::Quat q;
    const Value model = e->obj->get("model");
    if (const assets::XModelParts* parts = model.isString() ? skeleton(model.asString()) : nullptr) {
        const assets::XPose pose = e->anim ? e->anim->pose(*parts) : assets::bindPose(*parts);
        for (size_t b = 0; b < parts->bones.size(); ++b)
            if (lowerCopy(parts->bones[b].name) == "tag_body") { q = pose.rotation[b]; break; }
    }
    static const Vec3 units[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int r = 0; r < 3; ++r) {
        const Vec3 l = assets::rotate(q, units[r]);
        for (int i = 0; i < 3; ++i) axis[size_t(r)][size_t(i)] = fwd[size_t(i)] * l[0] + left[size_t(i)] * l[1] + up[size_t(i)] * l[2];
    }
    return true;
}

// Delta of an animation at normalized time 0 (0x478740 → 0x475aa0 with weight 1): the yaw quaternion (z, w) and the
// translation of its delta tracks; (0, 1) and zero for an animation without delta.
static void animStartDelta(const assets::XAnim* a, float& qz, float& qw, Vec3& t) {
    qz = 0.0f; qw = 1.0f; t = {0, 0, 0};
    if (!a || !a->delta) return;
    if (!a->deltaRot.keys.empty()) {
        const assets::Quat q = assets::sampleRotRaw(a->deltaRot, 0.0f, a->keyCount());
        if (q.z != 0.0f || q.w != 0.0f) { qz = q.z; qw = q.w; }
    }
    if (!a->deltaTrans.keys.empty()) {
        const auto p = assets::sampleTrans(a->deltaTrans, 0.0f, a->keyCount());
        t = {p[0], p[1], p[2]};
    }
}

// Translation of an animation's delta at normalized time t (0x478740 → 0x475810; frame t × (keyCount − 1),
// docs/spec/xanim.md); zero without a delta.
static Vec3 animDeltaTrans(const assets::XAnim* a, float t) {
    if (!a || !a->delta || a->deltaTrans.keys.empty()) return {0, 0, 0};
    const auto p = assets::sampleTrans(a->deltaTrans, t * float(a->keyCount() - 1), a->keyCount());
    return {p[0], p[1], p[2]};
}

// Vehicle methods (registration table, docs/spec/builtins.md 0x513070…0x513ee0).
void World::registerVehicleBuiltins() {
    // getstartorigin(origin, angles, anim) (0x53be50): origin + the start delta in the frame of angles
    // (forward, left, up).
    vm_.registerBuiltin("getstartorigin", [this](CallContext& c) {
        if (!std::holds_alternative<script::AnimRef>(c.arg(2).v)) throw std::runtime_error("type is not an animation");
        const Vec3 o = c.arg(0).asVector(), ang = c.arg(1).asVector();
        float qz, qw;
        Vec3 t;
        animStartDelta(animLib_.anim(std::get<script::AnimRef>(c.arg(2).v).name), qz, qw, t);
        Vec3 f, r, u;
        anglesToAxis(ang, f, r, u);
        Vec3 out;
        for (int i = 0; i < 3; ++i) out[size_t(i)] = u[size_t(i)] * t[2] - r[size_t(i)] * t[1] + f[size_t(i)] * t[0] + o[size_t(i)];
        return Value(out);
    });
    // getcycleoriginoffset(angles, anim) (0x53c080): the delta translation from time 0 to time 1 in the frame of
    // angles (forward, left, up).
    vm_.registerBuiltin("getcycleoriginoffset", [this](CallContext& c) {
        if (!std::holds_alternative<script::AnimRef>(c.arg(1).v)) throw std::runtime_error("type is not an animation");
        const Vec3 ang = c.arg(0).asVector();
        const assets::XAnim* a = animLib_.anim(std::get<script::AnimRef>(c.arg(1).v).name);
        const Vec3 t0 = animDeltaTrans(a, 0.0f), t1 = animDeltaTrans(a, 1.0f);
        const Vec3 d{t1[0] - t0[0], t1[1] - t0[1], t1[2] - t0[2]};
        Vec3 f, r, u;
        anglesToAxis(ang, f, r, u);
        Vec3 out;
        for (int i = 0; i < 3; ++i) out[size_t(i)] = f[size_t(i)] * d[0] - r[size_t(i)] * d[1] + u[size_t(i)] * d[2];
        return Value(out);
    });
    // getstartangles(origin, angles, anim) (0x53bf80): the start delta's yaw (0x423900: atan2(2zw, 1 − 2z²) for a
    // normalized quaternion) as an axis (0x424b20) applied in the frame of angles (0x422450), back to angles
    // (0x424b70).
    vm_.registerBuiltin("getstartangles", [this](CallContext& c) {
        if (!std::holds_alternative<script::AnimRef>(c.arg(2).v)) throw std::runtime_error("type is not an animation");
        const Vec3 ang = c.arg(1).asVector();
        float qz, qw;
        Vec3 t;
        animStartDelta(animLib_.anim(std::get<script::AnimRef>(c.arg(2).v).name), qz, qw, t);
        const float s = 2.0f / (qz * qz + qw * qw);
        const float yaw = std::atan2(qw * qz * s, 1.0f - s * qz * qz) * 57.2957763671875f;
        Vec3 yf, yr, yu;
        anglesToAxis({0, yaw, 0}, yf, yr, yu);
        Vec3 f, r, u;
        anglesToAxis(ang, f, r, u);
        const Vec3 left{-r[0], -r[1], -r[2]}, yl{-yr[0], -yr[1], -yr[2]};
        Vec3 nf, nl;
        for (int i = 0; i < 3; ++i) {
            nf[size_t(i)] = yf[0] * f[size_t(i)] + yf[1] * left[size_t(i)] + yf[2] * u[size_t(i)];
            nl[size_t(i)] = yl[0] * f[size_t(i)] + yl[1] * left[size_t(i)] + yl[2] * u[size_t(i)];
        }
        return Value(axisToAngles(nf, nl));
    });
    // useby(user) (0x4ef020): notifies "trigger" with the user, then runs the entity type's use function
    // (table 0x5a5740). For vehicles (0x512f00) a player gets in, or out when already using a vehicle.
    // TODO(spec): the use functions of turrets (0x529280) and entity type 8 (0x529450).
    vm_.registerBuiltin("useby", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        Entity* user = entityOf(c.arg(0));
        if (!e || !user) throw std::runtime_error("not an entity");
        if (e->classname == "script_vehicle" && !e->obj->removed) {
            vehicleState(c.self);
            useVehicle(*e, *user);
        } else {
            vm_.notify(e->obj, "trigger", {Value(user->obj)});
        }
        return Value();
    });
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
    // joltbody(point, intensity [, speedFraction, deceleration]) (0x5136e0): dir = origin - point, normalized.
    vm_.registerBuiltin("joltbody", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        (void)v;
        Entity* e = entityOf(c.self);
        const Vec3 o = e->obj->get("origin").asVector(), p = c.arg(0).asVector();
        Vec3 d = sub(o, p);
        normalize(d);
        if (c.argc() > 2 && (c.arg(2).asFloat() < 0.0f || c.arg(2).asFloat() > 1.0f))
            throw std::runtime_error("Speed fraction must be between 0 and 1");
        if (c.argc() > 3 && c.arg(3).asFloat() < 0.0f) throw std::runtime_error("Deceleration can't be negative");
        vehicleJolt(*e, d, c.arg(1).asFloat(), c.argc() > 2 ? c.arg(2).asFloat() : 0.0f, c.argc() > 3 ? c.arg(3).asFloat() * kMphToIps : 0.0f);
        return Value();
    });
    // Consumers: +0x190 compass ownerdraw 88 (0x4c1700, World::compassVehicles); +0x194/+0x196 the crosshair target lines (0x534b61).
    vm_.registerBuiltin("addvehicletocompass", [this](CallContext& c) {
        vehicleState(c.self).onCompass = true;
        return Value();
    });
    vm_.registerBuiltin("removevehiclefromcompass", [this](CallContext& c) {
        vehicleState(c.self).onCompass = false;
        return Value();
    });
    vm_.registerBuiltin("setvehiclelookattext", [this](CallContext& c) {
        auto& vs = vehicleState(c.self);
        // Strings are stored as script strings (0x513d16 -> +0x194); a localized reference keeps just its key.
        auto text = [](const Value& v) { return std::holds_alternative<LocalizedString>(v.v) ? std::get<LocalizedString>(v.v).key : v.toString(); };
        vs.lookAtText = c.argc() > 0 ? text(c.arg(0)) : std::string();
        if (c.argc() > 1) vs.lookAtText2 = text(c.arg(1)); // 0x513d4e: the second string only with more than one argument
        return Value();
    });
    vm_.registerBuiltin("startenginesound", [this](CallContext& c) {
        vehicleState(c.self).engineOn = true; // 0x513e80: +0x1ec = 1
        return Value();
    });
    vm_.registerBuiltin("stopenginesound", [this](CallContext& c) {
        vehicleState(c.self).engineOn = false; // 0x513ee0: +0x1ec = 0
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
        VehicleState& fv = vehicleState(c.self);
        if (fv.idleHandle) stopSound(fv.idleHandle);
        if (fv.engHandle) stopSound(fv.engHandle);
        if (fv.spinHandle) stopSound(fv.spinHandle);
        Entity* e = entityOf(c.self);
        e->vehicle.reset();
        e->classname = "script_vehicle_corpse";
        e->obj->set("classname", Value("script_vehicle_corpse"));
        e->obj->set("$takedamage", Value(false)); // 0x511d1e: ent+0x111 = 0 and health (+0x13c) = 0 (0x511c85)
        e->obj->set("health", Value(int32_t(0)));
        vm_.notify(e->obj, "death");
        return Value();
    });
    // getvehicleowner (0x513ab0): the player using the vehicle, nothing otherwise.
    vm_.registerBuiltin("getvehicleowner", [this](CallContext& c) {
        const VehicleState& v = vehicleState(c.self);
        Entity* o = v.owner >= 0 ? entityByNumber(v.owner) : nullptr;
        return o ? Value(o->obj) : Value();
    });
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
    // alternating the flash tags), projectile weapons a rocket from each flash tag (0x531a40 with no spread →
    // G_FireRocket 0x527810). Then the fire event: EV_FIRE_WEAPON (0x9e), quad barrels EV_FIRE_QUADBARREL_1 / _2
    // (0xb0 / 0xb1, by the barrel pair); the client (0x4b7800 per flash tag: tag_flash, tag_flash_11, tag_flash_2,
    // tag_flash_22 for the quad events) plays the weapon's fireSound and, for an entity that is not the local
    // client, its worldFlashEffect at the flash tag. TODO(spec): the effect bolted to the tag, the shell eject
    // (0x4b7730), the recoil jolt (0x50dcf0), the aim bend towards a driving player's aim point and the barrel
    // pull-back (+0x19c / +0x198).
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
        {
            static const char* const flash[4] = {"tag_flash", "tag_flash_11", "tag_flash_2", "tag_flash_22"};
            const int count = v.info->quadBarrel ? 2 : 1;
            const int first = v.info->quadBarrel && v.altBarrel ? 2 : 0;
            // Direction: the body's forward axis through the turret bone controls (rotation composition, no Euler sum;
            // inferred that the original reads the flash tag's axis, which the controllers 0x5127a0 rotate).
            Vec3 fwd{};
            bool fOk = false;
            vehicleTagPosition(*e, "tag_flash", fOk, &fwd);
            if (!fOk) {
                Vec3 a = e->obj->get("angles").asVector(), r, u;
                anglesToAxis(a, fwd, r, u);
            }
            const std::string flashFx = weaponField(v.info->turretWeapon, "worldFlashEffect");
            auto shotDir = [&](const Vec3& start) {
                // Aim bend (0x5144e7): a player-driven single barrel shoots towards the player's aim point
                // (+0x1bc), at most 10° (0x591e8c / 0x59213c) off the flash direction in pitch and in yaw.
                // TODO(spec): the barrel pull-back +0x19c / +0x198 (always 0 here).
                if (v.owner < 0 || v.info->quadBarrel) return fwd;
                const Vec3 d{v.aimPoint[0] - start[0], v.aimPoint[1] - start[1], v.aimPoint[2] - start[2]};
                if (d[0] == 0 && d[1] == 0 && d[2] == 0) return fwd;
                const Vec3 fa = vectorToAngles(fwd), aa = vectorToAngles(d);
                auto clamp10 = [](float x) { return x >= 10.0f ? 10.0f : x <= -10.0f ? -10.0f : x; };
                const float p = angleSubtract(fa[0], clamp10(angleSubtract(fa[0], aa[0])));
                const float y = angleSubtract(fa[1], clamp10(angleSubtract(fa[1], aa[1])));
                const float rad = float(M_PI) / 180.0f;
                return Vec3{std::cos(p * rad) * std::cos(y * rad), std::cos(p * rad) * std::sin(y * rad), -std::sin(p * rad)};
            };
            vehicleJolt(*e, {-fwd[0], -fwd[1], -fwd[2]}, 1.0f, 0.0f, 0.0f); // 0x5148a3: 0x50dcf0(-flash forward, 1, 0, 0)
            const std::string fireSnd = weaponField(v.info->turretWeapon, "fireSound");
            for (int k = 0; k < count; ++k) {
                bool ok = false;
                const Vec3 start = vehicleTagPosition(*e, flash[first + k], ok);
                if (!ok) throw std::runtime_error(std::string("No ") + flash[first + k] + " for [" + e->obj->get("model").toString() + "]");
                const Vec3 dir = shotDir(start);
                if (type == "bullet") {
                    const Vec3 end{start[0] + dir[0] * 8192.0f, start[1] + dir[1] * 8192.0f, start[2] + dir[2] * 8192.0f};
                    fireBullet(*e, start, end, weaponDamage(v.info->turretWeapon), e, v.info->turretWeapon);
                } else {
                    spawnRocket(*e, v.info->turretWeapon, start, dir);
                }
            }
            // The fire event's client side: the quad events show both tags of the pair, EV_FIRE_WEAPON tag_flash.
            for (int k = 0; k < (v.info->quadBarrel ? 2 : 1); ++k) {
                bool ok = false;
                const Vec3 at = vehicleTagPosition(*e, flash[first + k], ok);
                if (!ok) continue;
                if (!fireSnd.empty()) startSound(fireSnd, &at);
                if (!flashFx.empty()) fxEvents_.push_back({flashFx, at, fwd});
            }
        }
        if (v.info->quadBarrel) v.altBarrel = !v.altBarrel;
        // The player's fire timer (+0x184) restarts with the weapon's fireTime (ms, weapon +0x204).
        if (w && w->count("firetime")) v.fireTimerMs = int(float(std::atof(w->at("firetime").c_str())) * 1000.0f);
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
