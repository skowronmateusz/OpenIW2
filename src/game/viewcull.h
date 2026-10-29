// Box-versus-view-plane cull of the gfx renderer (dll 1.3), reduced to the two depth planes OpenIW2 can model without the full plane list.
// Scene setup 0x1000a910 builds the plane list (four side planes from the fov via 0x10006c20, then 0x100062e0 appends the near and the
// far plane) and every scene object is culled with it: AABB-tree nodes (0x10008110 -> 0x10006340), cell boxes (0x100092f0, 0x100093e0),
// static models (0x100083e0), xmodel scene entities (0x100087e0) and brush-model entities (0x10009210), the last four through
// 0x10006420 on a world-space box. See docs/spec/render-hints.md.
#pragma once

namespace iw2::game {

// One plane as the dll stores it: n . p + d > 0 is inside. Test of a box (mins, maxs): the "positive vertex" is chosen per axis (max when
// the normal component is > 0, else min; the dll precomputes the choice as byte offsets 0 / 0xc in the plane record), and the box is
// outside when n . pv + d <= 0 (0x10006420: fcomps against 0 and test 0x41, so equality culls). Terms are summed x, y, z, then d.
struct ViewPlane {
    float n[3];
    float d;
};

inline bool boxOutsidePlane(const ViewPlane& pl, const float mins[3], const float maxs[3]) {
    float s = 0.0f;
    for (int a = 0; a < 3; ++a) s += (pl.n[a] > 0.0f ? maxs[a] : mins[a]) * pl.n[a];
    return s + pl.d <= 0.0f;
}

// Near plane (0x1000a400): normal = view forward, d = 0.1 - forward . eye (constant 0x1019bf48), so a box is culled when its furthest
// corner along the view direction is at depth <= -0.1 (slightly behind the eye).
inline ViewPlane nearViewPlane(const float eye[3], const float fwd[3]) {
    const float dotv = fwd[2] * eye[2] + fwd[1] * eye[1] + fwd[0] * eye[0];
    return {{fwd[0], fwd[1], fwd[2]}, 0.1f - dotv};
}
// Far plane (0x1000a4e3, only when FarDist != 0, see farPlaneDistance in fog.h): normal = -forward, d = far + forward . eye, so a box
// is culled when its nearest corner along the view direction is at depth >= far.
inline ViewPlane farViewPlane(const float eye[3], const float fwd[3], float farDist) {
    const float nx = -fwd[0], ny = -fwd[1], nz = -fwd[2];
    const float dotv = nz * eye[2] + ny * eye[1] + nx * eye[0];
    return {{nx, ny, nz}, farDist - dotv};
}

// True when the box is removed by the near or the far plane. farDist 0 = no far plane (the dll then leaves the far pointer null).
inline bool boxCulledByDepthPlanes(const float mins[3], const float maxs[3], const float eye[3], const float fwd[3], float farDist) {
    if (boxOutsidePlane(nearViewPlane(eye, fwd), mins, maxs)) return true;
    return farDist != 0.0f && boxOutsidePlane(farViewPlane(eye, fwd, farDist), mins, maxs);
}

// World-space axis-aligned box of a local box under a column-major 4x4 transform (columns 0..2 = the basis vectors, column 3 = origin):
// the min / max over the eight transformed corners. (The dll's own box builders, 0x10008500 / 0x10008900, are not decoded; this is
// the plain enclosing box, which never culls more than any tighter box would.)
inline void transformedBoxBounds(const float mins[3], const float maxs[3], const float m[16], float outMin[3], float outMax[3]) {
    for (int i = 0; i < 3; ++i) outMin[i] = outMax[i] = m[12 + i];
    for (int i = 0; i < 3; ++i)
        for (int a = 0; a < 3; ++a) {
            const float lo = m[a * 4 + i] * mins[a], hi = m[a * 4 + i] * maxs[a];
            outMin[i] += lo < hi ? lo : hi;
            outMax[i] += lo < hi ? hi : lo;
        }
}

} // namespace iw2::game
