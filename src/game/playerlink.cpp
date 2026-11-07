// The player linked to an entity: playerlinktodelta, playerlinktoabsolute and the client's per-frame placement
// (docs/spec/animscripted.md "Player links"; 0x4eda80, 0x4edc60, 0x4ede30, 0x4d6ec0, ClientEndFrame 0x4e7f90).
#include "game/world.h"

#include <cmath>
#include <stdexcept>

namespace iw2::game {

using namespace script;

namespace {

// Rotation of an orthonormal frame as a quaternion (0x4262e0) and back (0x423680).
assets::Quat axisQuat(const Axis& m) {
    // m rows are the rotated unit vectors; the rotation matrix R has them as columns.
    const float r00 = m[0][0], r11 = m[1][1], r22 = m[2][2];
    const float r01 = m[1][0], r10 = m[0][1], r02 = m[2][0], r20 = m[0][2], r12 = m[2][1], r21 = m[1][2];
    assets::Quat q;
    const float tr = r00 + r11 + r22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r21 - r12) / s;
        q.y = (r02 - r20) / s;
        q.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

Axis quatAxis(const assets::Quat& q) {
    static const Vec3 units[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Axis m;
    for (int i = 0; i < 3; ++i) m[size_t(i)] = assets::rotate(q, units[i]);
    return m;
}

} // namespace

// playerlinktodelta(parent [, tag [, viewFraction]]) (0x4eda80): fraction default 0, the delta mode (client +0x8698
// = 0, pm_flags 0x10000000 off). playerlinktoabsolute(parent [, tag]) (0x4edc60): fraction 1, absolute, pm_flags
// 0x10000000 on. Both link the entity (0x4d6540 / 0x4d6ca0); the linked client ignores the offset (0x4d6ec0).
void World::registerPlayerLinkBuiltins() {
    auto linkPlayer = [this](CallContext& c, bool absolute) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("not an entity");
        if (!e->isPlayer) throw std::runtime_error("not a player entity");
        Entity* p = entityOf(c.arg(0));
        if (!p) throw std::runtime_error("not an entity");
        PlayerLink l;
        l.parent = p->number;
        if (c.args.size() > 1 && c.arg(1).isString()) l.tag = c.arg(1).asString();
        l.absolute = absolute;
        l.fraction = absolute ? 1.0f : (c.args.size() > 2 && c.arg(2).isNumber() ? c.arg(2).asFloat() : 0.0f);
        // Client +0x8680 (reference rotation taken) is cleared only by a client frame without a link (0x4e8023), so a link
        // replacing a live one keeps the reference.
        if (playerLink_ && playerLink_->haveRef) {
            l.haveRef = true;
            l.ref = playerLink_->ref;
        }
        playerLink_ = l;
        links_.erase(e->number);
    };
    vm_.registerBuiltin("playerlinktodelta", [linkPlayer](CallContext& c) {
        linkPlayer(c, false);
        return Value();
    });
    vm_.registerBuiltin("playerlinktoabsolute", [linkPlayer](CallContext& c) {
        linkPlayer(c, true);
        return Value();
    });
}

// 0x4d6ec0 from ClientEndFrame while linked: the frame of the parent's tag (0x4d6ad0). In delta mode the view
// turns by the fraction of the frame's rotation since the last frame (the first frame only takes the reference);
// absolute mode puts the view on the tag's angles. The eye is at the tag origin + up · view height, the origin that
// minus the view height in z, with no velocity. The original writes ps viewangles at the 20 Hz server rate and the
// client interpolates the player state between snapshots (CG_InterpolatePlayerState 0x4b9940: origin and view
// angles by the frame fraction 0x4ab740); here the new view angles go to the front end in their own channel
// (takePlayerLinkView), which spreads the change over the next server interval.
void World::updatePlayerLink() {
    if (!player_) return;
    // ClientEndFrame (0x4e7fd6) runs 0x4d6ec0 for any client whose entity has a link (ent+0x1c8), whichever call made it: playerlinkto*
    // or a plain linkto on the player (decoytown: the ride ends with linkto on a script_origin that then moves). The plain link
    // leaves the view fraction at 0 (client +0x8694), so only the position follows. A vehicle driver's link is placed by the
    // vehicle code (usingVehicle).
    PlayerLink plain;
    PlayerLink* plp = nullptr;
    Link l;
    if (playerLink_) {
        plp = &*playerLink_;
        l.parent = plp->parent;
        l.tag = plp->tag;
    } else if (const auto it = links_.find(player_->number); it != links_.end() && player_->usingVehicle < 0) {
        l = it->second;
        plp = &plain;
    } else {
        return;
    }
    PlayerLink& pl = *plp;
    Vec3 origin;
    Axis axis;
    if (!linkParentFrame(l, origin, axis)) {
        playerLink_.reset();
        return;
    }
    const assets::Quat q = axisQuat(axis);
    if (plp == &plain) {
        // a plain link: fraction 0, the view stays
    } else if (pl.absolute) {
        playerLinkView_ = LinkView{axisAngles(axis), true};
    } else if (!pl.haveRef) {
        pl.haveRef = true;
    } else {
        // Rotation since the last frame (q · conj(previous)), scaled towards identity by the fraction (0x4d6f97).
        const assets::Quat inv{-pl.ref.x, -pl.ref.y, -pl.ref.z, pl.ref.w};
        assets::Quat d = assets::multiply(q, inv);
        d.x *= pl.fraction;
        d.y *= pl.fraction;
        d.z *= pl.fraction;
        d.w = (d.w - 1.0f) * pl.fraction + 1.0f;
        const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z + d.w * d.w);
        if (n > 0.0f) {
            d.x /= n; d.y /= n; d.z /= n; d.w /= n;
            const assets::Quat v = multiply(d, axisQuat(anglesAxis(playerCmd_.viewAngles)));
            const Vec3 va = axisAngles(quatAxis(v));
            playerLinkView_ = LinkView{va, false};
            playerCmd_.viewAngles = va; // a second server frame before the next command builds on this one
        }
    }
    pl.ref = q;
    const float vh = playerCmd_.viewHeight; // ps +0xd4
    Vec3 o;
    for (int i = 0; i < 3; ++i) o[size_t(i)] = axis[2][size_t(i)] * vh + origin[size_t(i)];
    o[2] -= vh;
    player_->obj->set("origin", Value(o));
    player_->velocity = {0, 0, 0};
}

} // namespace iw2::game
