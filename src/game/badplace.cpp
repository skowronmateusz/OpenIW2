// Bad places (docs/spec/ai.md "Bad places"): badplace_cylinder / badplace_arc / badplace_delete, the links they block
// for the planner, the awareness event that makes actors flee and the badplace state (4).
#include "game/world.h"

#include <cmath>

namespace iw2::game {

using namespace script;

namespace {

constexpr float kDegToRad = 0.017453292f; // 0x591f50

Vec3 originOf(const ObjectPtr& o) {
    const Value v = o->get("origin");
    return v.isVector() ? v.asVector() : Vec3{0, 0, 0};
}

// 0x4240a0: add or subtract 360 until the angle is in [0, 360).
float normalize360(float a) {
    while (a < 0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

int teamOf(const Entity& e) {
    const Value t = e.obj->get("team");
    const std::string s = t.isString() ? t.asString() : std::string();
    return s == "axis" ? 1 : s == "allies" ? 2 : s == "neutral" ? 3 : 0;
}

// 0x4ec8d0: team names from argument `from` on: axis 2, allies 4, neutral 8.
uint32_t parseTeams(CallContext& c, size_t from, const char* fn) {
    uint32_t mask = 0;
    for (size_t i = from; i < c.argc(); ++i) {
        const std::string t = c.arg(i).toString();
        if (t == "axis") mask |= 2;
        else if (t == "allies") mask |= 4;
        else if (t == "neutral") mask |= 8;
        else throw std::runtime_error("unknown team '" + t + "' in " + fn + " (should be axis, allies, or neutral)");
    }
    return mask;
}

} // namespace

// 0x425c90: a point inside a bad place volume: 2D distance minus 15 within the radius, height within ±half height of
// the centre, and the yaw from the centre (through 16 bits) strictly between the left and right angles (wrapping
// when left ≥ right).
bool World::insideBadPlace(const BadPlace& b, const Vec3& p) const {
    const float dx = p[0] - b.origin[0], dy = p[1] - b.origin[1];
    const float t = std::sqrt(dx * dx + dy * dy) - 15.0f;
    if (b.radius * b.radius < t * t) return false;
    if (b.origin[2] - b.halfHeight > p[2] || b.origin[2] + b.halfHeight < p[2]) return false;
    float yaw = 0.0f;
    if (dx != 0.0f || dy != 0.0f) {
        yaw = std::atan2(dy, dx) * 57.29578f;
        if (yaw < 0.0f) yaw += 360.0f;
    }
    yaw = float(int32_t(yaw * 182.04445f) & 0xffff) * 0.0054931641f;
    if (b.left < b.right) return yaw < b.right && yaw > b.left;
    return yaw < b.right || yaw > b.left;
}

// 0x523b70 / 0x523ca0: inside any active bad place.
bool World::inBadPlace(const Vec3& p) const {
    for (const BadPlace& b : badPlaces_)
        if (b.type == 1 && insideBadPlace(b, p)) return true;
    return false;
}

// 0x5199f0: links that lead into a bad place get `delta` added to their per-team block count (0x519980). Nodes within
// radius + 256 (2D) and half height + 128 of the centre are scanned; a link counts when its target is not farther from
// the sector's centroid than its start, when it is not entirely above or below the volume, and when one end is inside
// the circle or the segment crosses it inside the height; for an arc it must also not lie on the outer side of the
// sector's edge planes (both ends behind one plane for arcs under 180°, behind both for wider ones).
void World::markBadPlaceLinks(const BadPlace& b, int delta) {
    if (linkBlock_.size() != paths_.size()) {
        linkBlock_.assign(paths_.size(), {});
        for (size_t i = 0; i < paths_.size(); ++i) linkBlock_[i].assign(paths_.links(i).size(), {});
    }
    const float l = b.left * kDegToRad, r = b.right * kDegToRad;
    const float n1x = -std::sin(l), n1y = std::cos(l), n2x = std::sin(r), n2y = -std::cos(r);
    const float d1 = n1y * b.origin[1] + n1x * b.origin[0], d2 = n2y * b.origin[1] + n2x * b.origin[0];
    float span = b.right - b.left;
    if (span < 0.0f) span += 360.0f;
    const bool narrow = span < 180.0f;
    const float mid = (span * 0.5f + b.left) * kDegToRad;
    const float c = std::sin(span * 0.0087266462f) / span * 76.394372f * b.radius; // 4·r·sin(θ/2) / 3θ
    const float cx = std::cos(mid) * c + b.origin[0], cy = std::sin(mid) * c + b.origin[1];
    const float r2 = b.radius * b.radius, h2 = b.halfHeight * b.halfHeight;
    const float scanR = (b.radius + 256.0f) * (b.radius + 256.0f), scanH = (b.halfHeight + 128.0f) * (b.halfHeight + 128.0f);
    const bool full = b.left == 0.0f && b.right == 360.0f;
    for (size_t i = 0; i < paths_.size(); ++i) {
        const Vec3& p = paths_.position(i);
        const float px = p[0] - b.origin[0], py = p[1] - b.origin[1], pz = p[2] - b.origin[2];
        const float pd2 = px * px + py * py;
        if (!(pd2 < scanR && pz * pz < scanH)) continue;
        const float pc = (cx - p[0]) * (cx - p[0]) + (cy - p[1]) * (cy - p[1]);
        const auto& links = paths_.links(i);
        for (size_t li = 0; li < links.size(); ++li) {
            const Vec3& q = paths_.position(links[li].target);
            const float qx = q[0] - b.origin[0], qy = q[1] - b.origin[1], qz = q[2] - b.origin[2];
            if ((cx - q[0]) * (cx - q[0]) + (cy - q[1]) * (cy - q[1]) > pc) continue;
            if (pz < b.halfHeight) {
                if (!(pz > -b.halfHeight || qz > -b.halfHeight)) continue;
            } else if (!(qz < b.halfHeight)) {
                continue;
            }
            bool hit = pd2 <= r2 || qx * qx + qy * qy <= r2;
            if (!hit) {
                const float ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2];
                const float bb = -(ex * px + ey * py);
                const float ee = ex * ex + ey * ey;
                const float disc = bb * bb - (pd2 - r2) * ee;
                if (bb > 0.0f && disc > 0.0f) {
                    const float sq = std::sqrt(disc);
                    const float t1 = (bb - sq) / ee;
                    if (t1 < 1.0f) {
                        const float z1 = ez * t1 + pz;
                        if (z1 * z1 <= h2) {
                            hit = true;
                        } else {
                            const float t2 = (sq + bb) / ee;
                            const float z2 = t2 * ez + pz;
                            hit = t2 < 1.0f && (z2 * z2 <= h2 || z2 * z1 < 0.0f);
                        }
                    }
                }
            }
            if (!hit) continue;
            if (!full) {
                const bool p1 = n1x * p[0] + n1y * p[1] - d1 < 0.0f, q1 = n1x * q[0] + n1y * q[1] - d1 < 0.0f;
                const bool p2 = n2x * p[0] + n2y * p[1] - d2 < 0.0f, q2 = n2x * q[0] + n2y * q[1] - d2 < 0.0f;
                if (narrow ? ((p1 && q1) || (p2 && q2)) : (p1 && q1 && p2 && q2)) continue;
            }
            for (int t = 0; t < 4; ++t) {
                if (!(b.teamMask >> t & 1)) continue;
                const int v = linkBlock_[i][li][size_t(t)] + delta;
                if (v < 0) throw std::runtime_error("bad place link count below zero");
                if (v > 255) throw std::runtime_error("too many bad places on a path link");
                linkBlock_[i][li][size_t(t)] = uint8_t(v);
            }
        }
    }
}

// 0x523430: free a slot; a cylinder/arc unblocks its links.
void World::removeBadPlace(size_t i) {
    BadPlace& b = badPlaces_[i];
    if (b.type == 1) markBadPlaceLinks(b, -1);
    b = BadPlace{};
}

// 0x5234a0 / 0x523630: a named bad place replaces the one of that name; the expiry is now + duration, forever for a
// named one without a duration (an anonymous one needs a duration). The links are blocked, then every actor whose
// remaining path uses a blocked link drops it (0x503d60 → 0x545610).
void World::addBadPlace(const std::string& name, int durationMs, uint32_t teamMask, BadPlace data) {
    if (!name.empty())
        for (size_t i = 0; i < badPlaces_.size(); ++i)
            if (badPlaces_[i].type != 0 && badPlaces_[i].name == name) removeBadPlace(i);
    int64_t expire = timeMs_ + durationMs;
    if (durationMs <= 0) {
        if (name.empty()) throw std::runtime_error("anonymous bad places must have a duration");
        expire = 0x7fffffff;
    } else if (durationMs < 250) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "WARNING: A badplace was created with duration [%.2f second], which is less than the ping time [%.2f second]",
                      durationMs * 0.001, 0.25);
        print(buf);
    }
    size_t slot = badPlaces_.size();
    for (size_t i = 0; i < badPlaces_.size() && slot == badPlaces_.size(); ++i)
        if (badPlaces_[i].type == 0) slot = i;
    if (slot == badPlaces_.size()) throw std::runtime_error("too many bad places (more than 32)");
    data.name = name;
    data.expireMs = expire;
    data.teamMask = teamMask;
    data.type = 1;
    data.eventMs = timeMs_;
    badPlaces_[slot] = data;
    markBadPlaceLinks(badPlaces_[slot], 1);
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (!e.isActor || e.obj->removed || !actorHasPath(e)) continue;
        const int team = teamOf(e);
        bool blocked = false;
        for (int i = e.path.count - 2; i >= 0 && !blocked; --i) {
            const int a = e.path.pts[size_t(i + 1)].node, b = e.path.pts[size_t(i)].node;
            if (a < 0 || b < 0) continue;
            const auto& links = paths_.links(size_t(a));
            for (size_t li = 0; li < links.size(); ++li)
                if (links[li].target == b && linkBlocked(size_t(a), li, team)) blocked = true;
        }
        if (blocked) clearActorPath(e);
    }
}

// 0x523be0: after bad places went away, actors in the badplace state that are no longer inside one stop fleeing.
void World::recheckBadPlaceActors() {
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (!e.isActor || e.obj->removed || e.ai.stack[size_t(e.ai.depth)] != AiBadplace) continue;
        if (!inBadPlace(originOf(e.obj))) e.ai.badPlace = false;
    }
}

// Every frame (0x523a60): expired bad places are removed; every 250 ms each one sends the bad place event (0x535a30,
// type 0x14): actors of its teams inside it notice it with probability badplaceawareness.
void World::runBadPlaces() {
    bool removed = false;
    for (size_t i = 0; i < badPlaces_.size(); ++i) {
        BadPlace& b = badPlaces_[i];
        if (b.type == 0) continue;
        if (timeMs_ >= b.expireMs) {
            removeBadPlace(i);
            removed = true;
            continue;
        }
        if (timeMs_ - b.eventMs < 250) continue;
        if (b.type == 1 && b.left != b.right) {
            for (const auto& ep : entities_) {
                Entity& e = *ep;
                if (!e.isActor || e.obj->removed || !(b.teamMask >> teamOf(e) & 1)) continue;
                if (!insideBadPlace(b, originOf(e.obj))) continue;
                const Value aw = e.obj->get("badplaceawareness");
                if (float(rng_() % 32768) <= (aw.isNumber() ? aw.asFloat() : 0.0f) * 32767.0f) e.ai.badPlace = true;
            }
        }
        b.eventMs = timeMs_;
    }
    if (removed) recheckBadPlaceActors();
}

// 0x523f10: a flee target: none without bad places; else the first node of the current path (from the point behind
// the actor towards its end) that is outside every bad place and usable, when a path to it exists; else the nodes
// within the largest bad place radius + 100 (0x523d10: 80 units of height, outside bad places, usable, facing the
// enemy and covering, by distance² · 0.9) in order until one can be reached. The node reached is claimed.
bool World::badPlaceFlee(Entity& e) {
    float maxR = -1.0f;
    for (const BadPlace& b : badPlaces_)
        if (b.type == 1 && maxR < b.radius) maxR = b.radius;
    if (maxR < 0.0f) return false;
    auto take = [&](int node) {
        e.obj->set("keepclaimednode", Value(int32_t(0)));
        e.obj->set("keepclaimednodeingoal", Value(int32_t(0)));
        claimNode(e, node);
        return true;
    };
    if (actorHasPath(e)) {
        int node = -1;
        for (int i = e.path.count - 1; i >= 0 && node < 0; --i) {
            const int n = e.path.pts[size_t(i)].node;
            if (n > 0 && !inBadPlace(originOf(nodes_[size_t(n)].obj)) && nodeUsable(e, n)) node = n;
        }
        if (node >= 0 && fleePathToNode(e, node)) return take(node);
    }
    const Vec3 o = originOf(e.obj);
    std::vector<std::pair<int, float>> list;
    std::string script;
    for (const auto& [node, d2] : nodesInRange(o, maxR + 100.0f, 80.0f, 256, 0xfffffffe)) {
        const Vec3 p = originOf(nodes_[size_t(node)].obj);
        if (inBadPlace(p) || !nodeUsable(e, node) || !nodeFacesEnemy(e, node)) continue;
        if (coverScriptAt(e, node, true, script) && script.empty()) continue;
        const float dx = p[0] - o[0], dy = p[1] - o[1], dz = p[2] - o[2];
        list.push_back({node, (dx * dx + dy * dy + dz * dz) * 0.9f});
    }
    std::stable_sort(list.begin(), list.end(), [this](const auto& a, const auto& b) {
        const bool la = !paths_.links(size_t(a.first)).empty(), lb = !paths_.links(size_t(b.first)).empty();
        if (la != lb) return la;
        return a.second < b.second;
    });
    for (const auto& [node, key] : list)
        if (fleePathToNode(e, node)) return take(node);
    return false;
}

// 0x503890 (plain planning): a path that already ends at the node is kept (a truncated one while more than a quarter
// of it is left); standing at the node needs none; else a new path to it. True when the actor is there or has a path.
bool World::fleePathToNode(Entity& e, int node) {
    const Vec3 no = originOf(nodes_[size_t(node)].obj), o = originOf(e.obj);
    auto same = [](const Vec3& a, const Vec3& b) {
        const float dz = b[2] - a[2];
        return dz * dz <= 6400.0f && (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) <= 4.0f;
    };
    if (!actorHasPath(e)) {
        if (same(no, o)) return true;
    } else {
        const bool keep = !(e.path.flags & 4) || e.path.planned < e.path.count * 4;
        if (keep && same(no, pathEnd(e))) return true;
        clearActorPath(e);
    }
    return pathToNodeDirect(e, node) && actorHasPath(e);
}

// Badplace state think (0x524060): while the flag (+0x2128) holds: perception, then along a flee path (facing 4,
// running; plan time cleared), or, without one, standing (the stop choice) as long as the actor is inside a bad place
// and a flee target exists; otherwise the flag clears and the state is replaced by exposed.
// TODO(spec): the followed / blocking actor (+0x2148 / +0x728) that clears the path while fleeing.
int World::badPlaceThink(Entity& e) {
    if (e.ai.badPlace) {
        actorPerceive(e);
        if (actorHasPath(e)) {
            e.ai.facing = 4;
            e.ai.planTimeMs = 0;
            followActorPath(e, true);
            updateClaimedNode(e);
            return 0;
        }
        if (inBadPlace(originOf(e.obj)) && badPlaceFlee(e)) {
            setActorAnimScript(e, stopScript(e), 0, 1); // 0x53ae40
            updateClaimedNode(e);
            return 0;
        }
        e.ai.badPlace = false;
    }
    aiSetState(e, AiExposed);
    return 1;
}

void World::registerBadPlaceBuiltins() {
    // badplace_delete(name) (0x4f5800 → 0x5235c0).
    vm_.registerBuiltin("badplace_delete", [this](CallContext& c) {
        if (c.argc() != 1) throw std::runtime_error("Incorrect number of parameters");
        const std::string name = c.arg(0).toString();
        if (name.empty()) throw std::runtime_error("badplace_delete called with name \"\"");
        for (size_t i = 0; i < badPlaces_.size(); ++i)
            if (badPlaces_[i].type != 0 && badPlaces_[i].name == name) removeBadPlace(i);
        recheckBadPlaceActors();
        return Value();
    });
    // badplace_cylinder(name, duration, origin, radius, height, team…) (0x4f5880): duration seconds rounded to ms,
    // half the height, the full circle; no team means all three.
    vm_.registerBuiltin("badplace_cylinder", [this](CallContext& c) {
        if (c.argc() < 5) throw std::runtime_error("Incorrect badplace_cylinder() call.");
        const std::string name = c.arg(0).toString();
        const int ms = int(std::lrint(c.arg(1).asFloat() * 1000.0f + 0.5));
        BadPlace b;
        b.origin = c.arg(2).asVector();
        b.radius = c.arg(3).asFloat();
        b.halfHeight = c.arg(4).asFloat() * 0.5f;
        b.left = 0.0f;
        b.right = 360.0f;
        uint32_t teams = parseTeams(c, 5, "badplace_cylinder");
        addBadPlace(name, ms, teams ? teams : 0xe, b);
        return Value();
    });
    // badplace_arc(name, duration, origin, radius, height, direction, left, right, team…) (0x4f5990): the arc from
    // yaw(direction) − left to yaw(direction) + right, the full circle when that spans 360° or more.
    vm_.registerBuiltin("badplace_arc", [this](CallContext& c) {
        if (c.argc() < 8) throw std::runtime_error("Incorrect usage for badplace_arc()");
        const std::string name = c.arg(0).toString();
        const int ms = int(std::lrint(c.arg(1).asFloat() * 1000.0f + 0.5));
        BadPlace b;
        b.origin = c.arg(2).asVector();
        b.radius = c.arg(3).asFloat();
        b.halfHeight = c.arg(4).asFloat() * 0.5f;
        const Vec3 dir = c.arg(5).asVector();
        float yaw = 0.0f;
        if (dir[0] != 0.0f || dir[1] != 0.0f) {
            yaw = std::atan2(dir[1], dir[0]) * 57.29578f;
            if (yaw < 0.0f) yaw += 360.0f;
        }
        b.left = yaw - c.arg(6).asFloat();
        if (b.left > yaw) throw std::runtime_error("left angle < 0 in badplace_arc");
        b.right = c.arg(7).asFloat() + yaw;
        if (b.right < yaw) throw std::runtime_error("right angle < 0 in badplace_arc");
        if (b.right - b.left >= 360.0f) {
            b.left = 0.0f;
            b.right = 360.0f;
        } else {
            b.left = normalize360(b.left);
            b.right = normalize360(b.right);
        }
        uint32_t teams = parseTeams(c, 8, "badplace_arc");
        addBadPlace(name, ms, teams ? teams : 0xe, b);
        return Value();
    });
}

} // namespace iw2::game
