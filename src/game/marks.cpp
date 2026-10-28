// Impact marks (CoD2SP_s.exe 1.3 CG_ImpactMark 0x4ad430, pool 0x4ad3a0 / 0x4ad2d0, draw 0x4ad710; mark fragments
// from the 1.3 renderer gfx_d3d_x86_s.dll FUN_10015df0, refexport slot 0xe8). docs/spec/effects.md, "Marks".
#include "game/marks.h"

#include "fs/vfs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace iw2::game {
namespace {

MVec add(const MVec& a, const MVec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
MVec sub(const MVec& a, const MVec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
MVec mul(const MVec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const MVec& a, const MVec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
MVec cross(const MVec& a, const MVec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
MVec normalized(const MVec& v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0 ? mul(v, 1.0f / l) : MVec{0, 0, 0};
}

// 0x421f70: the basis vector of the smallest |component| projected off `src`, normalized.
MVec perpendicular(const MVec& src) {
    const float sq[3] = {src[0] * src[0], src[1] * src[1], src[2] * src[2]};
    int i = sq[1] < sq[0] ? 1 : 0;
    if (sq[2] < sq[i]) i = 2;
    MVec p = mul(src, -src[size_t(i)]);
    p[size_t(i)] += 1.0f;
    return normalized(p);
}

// 0x421550: `point` turned by `degrees` around the unit vector `dir`. TODO(spec): the turning direction is not
// checked against the matrix product in 0x421550 (the decal rotation is a random value from a symmetric range).
MVec rotateAround(const MVec& dir, const MVec& point, float degrees) {
    const float a = degrees * float(M_PI) / 180.0f, c = std::cos(a), s = std::sin(a);
    return add(add(mul(point, c), mul(cross(dir, point), s)), mul(dir, dot(dir, point) * (1.0f - c)));
}

// Clip vertex of FUN_100154b0: position, lightmap uv, normal (8 floats).
struct ClipVert {
    MVec p;
    float lm[2];
    MVec n;
};

// FUN_100154b0: distance d = p·n − dist; d > 0.5 front, d < −0.5 back, else on the plane. No front vertex: nothing
// left; no back vertex: unchanged; else front and on vertices stay and an edge from a front to a back vertex (or
// back to front) gets the point at d_i / (d_i − d_i+1) (FUN_10015430 lerps all 8 floats).
size_t clipPolygon(const ClipVert* in, size_t n, ClipVert* out, const MVec& normal, float dist) {
    float d[10];
    int side[10];
    int counts[3] = {0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        d[i] = dot(in[i].p, normal) - dist;
        side[i] = d[i] > 0.5f ? 0 : d[i] < -0.5f ? 1 : 2;
        ++counts[side[i]];
    }
    side[n] = side[0];
    d[n] = d[0];
    if (counts[0] == 0) return 0;
    if (counts[1] == 0) {
        std::copy(in, in + n, out);
        return n;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
        if (side[i] == 2) {
            out[m++] = in[i];
            continue;
        }
        if (side[i] == 0) out[m++] = in[i];
        if (side[i + 1] != 2 && side[i + 1] != side[i]) {
            const float t = d[i] / (d[i] - d[i + 1]);
            const ClipVert& a = in[i];
            const ClipVert& b = in[(i + 1) % n];
            ClipVert v;
            v.p = add(a.p, mul(sub(b.p, a.p), t));
            for (int k = 0; k < 2; ++k) v.lm[k] = a.lm[k] + (b.lm[k] - a.lm[k]) * t;
            v.n = add(a.n, mul(sub(b.n, a.n), t));
            out[m++] = v;
        }
    }
    return m;
}

} // namespace

// World surfaces for marks (FUN_100157f0 / FUN_10015640): triangle soups whose material does not have bit 4 at +0xc
// (set on skies, water and clip materials) and, in FUN_10015df0, whose material byte +0x30 has no 0x30 bits
// (mtlPolygonOffset, the statemap variable at 0x1018fe20: decal layers). The runtime material is the relocated materials/<name> file.
void MarkSystem::setWorld(const assets::D3dBsp& bsp, const fs::Vfs& vfs) {
    soups_ = bsp.soups();
    vertices_ = bsp.vertices();
    indices_ = bsp.indices();
    surfaces_.clear();
    marks_.clear();
    std::vector<int> usable(bsp.materials().size(), -1);
    for (size_t i = 0; i < bsp.materials().size(); ++i) {
        const auto md = vfs.read("materials/" + bsp.materials()[i].name);
        usable[i] = md && md->size() > 0x30 && !((*md)[0xc] & 4) && !((*md)[0x30] & 0x30) ? 1 : 0;
    }
    const auto& verts = bsp.vertices();
    const auto& idx = bsp.indices();
    // World surfaces only: model 0's soups (lump 35). TODO(spec): marks on brush models (model-space, can move or hide).
    const auto [worldFirst, worldCount] = bsp.modelSoups(0);
    for (size_t s = worldFirst; s < worldFirst + worldCount; ++s) {
        const auto& soup = bsp.soups()[s];
        if (soup.material >= usable.size() || usable[soup.material] != 1 || soup.indexCount < 3) continue;
        Surface surf;
        surf.soup = s;
        surf.mins = {1e30f, 1e30f, 1e30f};
        surf.maxs = {-1e30f, -1e30f, -1e30f};
        for (uint32_t k = 0; k < soup.indexCount; ++k) {
            const size_t vi = soup.firstVertex + idx[soup.firstIndex + k];
            if (vi >= verts.size()) continue;
            for (size_t c = 0; c < 3; ++c) {
                surf.mins[c] = std::min(surf.mins[c], verts[vi].position[c]);
                surf.maxs[c] = std::max(surf.maxs[c], verts[vi].position[c]);
            }
        }
        surfaces_.push_back(surf);
    }
}

void MarkSystem::markDrawn(int64_t nowMs) {
    for (Mark& m : marks_) m.lastVisibleMs = nowMs;
}

// CG_ImpactMark (0x4ad430), with cg_marks on: axis a0 = dir normalized (0x421270), a1 = the perpendicular of a0
// (0x421f70) turned by the rotation around a0 (0x421550), a2 = a0 × a1; corners o − r·a2 − r·a1, o + r·a2 − r·a1,
// o + r·(a1 + a2), o − r·a2 + r·a1. The renderer (FUN_10015df0, at most 0x400 points and 0x180 fragments) gathers
// the surfaces whose bounds touch the box of the corners ± r·a0 (FUN_10015c70), and for each of their triangles
// (i0, i1, i2) with n = (v0 − v1) × (v2 − v1), n·a0 ≥ 0 and |n|²/4 ≤ (n·a0)², clips it to the four edge planes
// ((c_i+1 − c_i) × a0, normalized, through c_i), a0·p ≥ a0·c0 − r and −a0·p ≥ −a0·c0 − r. A fragment that does not
// fit the remaining points is dropped; the gathering stops when the fragments are full or fewer than 3 points
// remain. Texture coordinates: s = (p − o)·a2·(0.5/r) + 0.5, t = (p − o)·a1·(0.5/r) + 0.5. Each fragment becomes one
// pool mark with the colour bytes (0x4ad1b0: round(c·255) clamped to 0..255) on every vertex.
// TODO(spec): DAT_00f79778 (marks are skipped while it is set), the surface order of the BSP walk (FUN_10015990;
// matters only when the limits are reached), the tangent / binormal the renderer also writes.
void MarkSystem::impactMark(const std::string& material, const MVec& origin, const MVec& dir, float rotationDeg,
                            const std::array<float, 4>& rgba, float radius, int64_t nowMs) {
    if (!enabled || surfaces_.empty() || limit <= 0 || !(radius > 0)) return;
    const MVec a0 = normalized(dir);
    const MVec a1 = rotateAround(a0, perpendicular(a0), rotationDeg);
    const MVec a2 = cross(a0, a1);
    const float r = radius;
    const MVec c[4] = {sub(sub(origin, mul(a2, r)), mul(a1, r)), sub(add(origin, mul(a2, r)), mul(a1, r)),
                       add(origin, mul(add(a1, a2), r)), add(sub(origin, mul(a2, r)), mul(a1, r))};
    MVec bmin{131072.0f, 131072.0f, 131072.0f}, bmax{-131072.0f, -131072.0f, -131072.0f};
    for (const MVec& p : c)
        for (const MVec& q : {add(p, mul(a0, r)), sub(p, mul(a0, r))})
            for (size_t k = 0; k < 3; ++k) {
                bmin[k] = std::min(bmin[k], q[k]);
                bmax[k] = std::max(bmax[k], q[k]);
            }
    MVec planeN[6];
    float planeD[6];
    for (int i = 0; i < 4; ++i) {
        planeN[i] = normalized(cross(sub(c[(i + 1) & 3], c[i]), a0));
        planeD[i] = dot(c[i], planeN[i]);
    }
    planeN[4] = a0;
    planeD[4] = dot(a0, c[0]) - r;
    planeN[5] = mul(a0, -1.0f);
    planeD[5] = dot(planeN[5], c[0]) - r;
    const float texScale = 0.5f / r;

    struct Fragment {
        int lightmap;
        std::vector<ClipVert> verts;
    };
    std::vector<Fragment> fragments;
    constexpr int kMaxPoints = 0x400, kMaxFragments = 0x180;
    int points = 0;
    const auto& verts = vertices_;
    const auto& idx = indices_;
    bool full = false;
    int dbgSurf = 0, dbgTri = 0, dbgFacing = 0;
    for (const Surface& s : surfaces_) {
        if (full) break;
        if (s.mins[0] > bmax[0] || s.maxs[0] < bmin[0] || s.mins[1] > bmax[1] || s.maxs[1] < bmin[1] || s.mins[2] > bmax[2] ||
            s.maxs[2] < bmin[2])
            continue;
        const auto& soup = soups_[s.soup];
        ++dbgSurf;
        for (uint32_t k = 0; k + 2 < soup.indexCount && !full; k += 3) {
            const assets::BspVertex* v[3];
            bool ok = true;
            for (int j = 0; j < 3; ++j) {
                const size_t vi = soup.firstVertex + idx[soup.firstIndex + k + uint32_t(j)];
                ok = ok && vi < verts.size();
                if (ok) v[j] = &verts[vi];
            }
            if (!ok) continue;
            const MVec p0 = v[0]->position, p1 = v[1]->position, p2 = v[2]->position;
            const MVec n = cross(sub(p0, p1), sub(p2, p1));
            const float nd = dot(n, a0);
            ++dbgTri;
            if (!(nd >= 0) || !(dot(n, n) * 0.25f <= nd * nd)) continue;
            ++dbgFacing;
            ClipVert buf[2][10];
            for (int j = 0; j < 3; ++j) buf[0][j] = {v[j]->position, {v[j]->lightmapUv[0], v[j]->lightmapUv[1]}, v[j]->normal};
            size_t cnt = 3;
            int cur = 0;
            for (int pl = 0; pl < 6 && cnt; ++pl) {
                cnt = clipPolygon(buf[cur], cnt, buf[cur ^ 1], planeN[pl], planeD[pl]);
                cur ^= 1;
            }
            if (cnt == 0 || int(cnt) > kMaxPoints - points) continue;
            fragments.push_back({int(soup.lightmap), std::vector<ClipVert>(buf[cur], buf[cur] + cnt)});
            points += int(cnt);
            if (int(fragments.size()) == kMaxFragments || points > kMaxPoints - 3) full = true;
        }
    }
    if (std::getenv("IW2_MARK_DEBUG"))
        std::fprintf(stderr, "mark %s at %.0f %.0f %.0f r %.1f dir %.2f %.2f %.2f: %d surfaces, %d tris, %d facing, %zu fragments\n",
                     material.c_str(), origin[0], origin[1], origin[2], radius, a0[0], a0[1], a0[2], dbgSurf, dbgTri, dbgFacing,
                     fragments.size());
    std::array<uint8_t, 4> bytes{};
    for (int k = 0; k < 4; ++k) {
        const long b = std::lround(rgba[size_t(k)] * 255.0f);
        bytes[size_t(k)] = uint8_t(std::clamp(b, 0L, 255L));
    }
    for (const Fragment& f : fragments) {
        // Pool (0x4ad3a0): a free entry, else the active mark with the smallest last-visible time (first one found
        // from the oldest) is reused; new marks go to the end of the active list.
        if (int(marks_.size()) >= limit) {
            auto oldest = marks_.begin();
            for (auto it = marks_.begin(); it != marks_.end(); ++it)
                if (it->lastVisibleMs < oldest->lastVisibleMs) oldest = it;
            marks_.erase(oldest);
        }
        Mark m;
        m.origin = origin;
        m.radius = radius;
        m.material = material;
        m.lightmap = f.lightmap;
        m.lastVisibleMs = nowMs;
        for (const ClipVert& cv : f.verts) {
            MarkVertex mv;
            mv.position = cv.p;
            mv.normal = cv.n;
            mv.rgba = bytes;
            const MVec d = sub(cv.p, origin);
            mv.uv[0] = dot(d, a2) * texScale + 0.5f;
            mv.uv[1] = dot(d, a1) * texScale + 0.5f;
            mv.lightmapUv[0] = cv.lm[0];
            mv.lightmapUv[1] = cv.lm[1];
            m.vertices.push_back(mv);
        }
        marks_.push_back(std::move(m));
    }
}

} // namespace iw2::game
