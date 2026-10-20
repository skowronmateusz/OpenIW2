#include "game/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace iw2::game {
namespace {

constexpr float kSurfaceEpsilon = 0.125f;

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec normalize(Vec v) {
    const float l = std::sqrt(dot(v, v));
    if (l > 0) for (float& x : v) x /= l;
    return v;
}

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

} // namespace

CollisionWorld::CollisionWorld(const assets::D3dBsp& bsp) {
    const auto mats = bsp.lump(0);
    auto contentsOf = [&](int m) -> uint32_t {
        if (m < 0 || size_t(m) * 72 + 72 > mats.size()) return Contents::Solid;
        return rd<uint32_t>(mats.data() + size_t(m) * 72 + 68);
    };

    // Brushes: lump 6 {int16 sideCount; int16 material}, lump 5 sides {float/int32 value; int32 material}.
    // The first 6 sides of each brush are axial bounds (min x, max x, min y, max y, min z, max z);
    // the rest are {int32 plane; int32 material} referencing lump 4.
    const auto planes = bsp.lump(4), sides = bsp.lump(5), brushes = bsp.lump(6);
    // Only the world model's brushes (model 0 in lump 35: int32 firstBrush at +40, numBrushes at +44) are static
    // world collision; later brushes belong to brush entities such as triggers and script_brushmodels.
    const auto models = bsp.lump(35);
    size_t worldBrushes = brushes.size() / 4;
    if (models.size() >= 48) worldBrushes = size_t(std::max(0, rd<int32_t>(models.data() + 44)));
    size_t sidePos = 0;
    for (size_t b = 0; b + 4 <= brushes.size() && b / 4 < worldBrushes; b += 4) {
        const int16_t sideCount = rd<int16_t>(brushes.data() + b);
        const int16_t material = rd<int16_t>(brushes.data() + b + 2);
        if (sideCount < 6 || (sidePos + size_t(sideCount)) * 8 > sides.size()) throw std::runtime_error("collision: bad brush sides");
        Brush br;
        br.contents = contentsOf(material);
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = rd<float>(sides.data() + (sidePos + size_t(axis * 2)) * 8);
            const float hi = rd<float>(sides.data() + (sidePos + size_t(axis * 2 + 1)) * 8);
            br.mins[size_t(axis)] = lo;
            br.maxs[size_t(axis)] = hi;
            Vec n{0, 0, 0};
            n[size_t(axis)] = -1;
            br.planes.push_back({n, -lo});
            n[size_t(axis)] = 1;
            br.planes.push_back({n, hi});
        }
        for (int s = 6; s < sideCount; ++s) {
            const int32_t p = rd<int32_t>(sides.data() + (sidePos + size_t(s)) * 8);
            if (p < 0 || size_t(p) * 16 + 16 > planes.size()) throw std::runtime_error("collision: bad plane index");
            const uint8_t* pp = planes.data() + size_t(p) * 16;
            br.planes.push_back({{rd<float>(pp), rd<float>(pp + 4), rd<float>(pp + 8)}, rd<float>(pp + 12)});
        }
        sidePos += size_t(sideCount);
        brushes_.push_back(std::move(br));
    }

    // Collision triangles: lump 29 vertices {float unused; float xyz[3]}, lump 31 triangles
    // {float plane[4], svec[4], tvec[4]; int32 verts[3]; int32 edges[3]}. All treated as solid (simplification).
    const auto verts = bsp.lump(29), tris = bsp.lump(31);
    const size_t nv = verts.size() / 16;
    for (size_t t = 0; t + 72 <= tris.size(); t += 72) {
        std::array<Vec, 3> tri;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const int32_t vi = rd<int32_t>(tris.data() + t + 48 + size_t(k) * 4);
            if (vi < 0 || size_t(vi) >= nv) { ok = false; break; }
            const uint8_t* vp = verts.data() + size_t(vi) * 16 + 4;
            tri[size_t(k)] = {rd<float>(vp), rd<float>(vp + 4), rd<float>(vp + 8)};
        }
        if (!ok) continue;
        tris_.push_back(tri);
        addTriangleBrush(tri[0], tri[1], tri[2], Contents::Solid);
    }
    buildGrid();
}

void CollisionWorld::addTriangleBrush(const Vec& a, const Vec& b, const Vec& c, uint32_t contents) {
    const Vec n = normalize(cross(sub(b, a), sub(c, a)));
    if (dot(n, n) < 0.5f) return;
    Brush br;
    br.contents = contents;
    for (int i = 0; i < 3; ++i) {
        br.mins[size_t(i)] = std::min({a[size_t(i)], b[size_t(i)], c[size_t(i)]}) - 1.0f;
        br.maxs[size_t(i)] = std::max({a[size_t(i)], b[size_t(i)], c[size_t(i)]}) + 1.0f;
    }
    // Front face, a thin back face, and three edge planes facing outwards.
    br.planes.push_back({n, dot(n, a)});
    br.planes.push_back({{-n[0], -n[1], -n[2]}, -dot(n, a) + 2.0f});
    const Vec v[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        const Vec e = sub(v[(i + 1) % 3], v[i]);
        const Vec en = normalize(cross(e, n));
        br.planes.push_back({en, dot(en, v[i])});
    }
    // Axial bevels keep box sweeps from catching on the edges of thin triangles.
    for (int axis = 0; axis < 3; ++axis) {
        Vec p{0, 0, 0};
        p[size_t(axis)] = 1;
        br.planes.push_back({p, br.maxs[size_t(axis)] - 1.0f});
        p[size_t(axis)] = -1;
        br.planes.push_back({p, -(br.mins[size_t(axis)] + 1.0f)});
    }
    brushes_.push_back(std::move(br));
}

void CollisionWorld::buildGrid() {
    if (brushes_.empty()) return;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const auto& b : brushes_) {
        x0 = std::min(x0, b.mins[0]); y0 = std::min(y0, b.mins[1]);
        x1 = std::max(x1, b.maxs[0]); y1 = std::max(y1, b.maxs[1]);
    }
    // Ignore absurd extents (skybox brushes can be huge).
    x0 = std::max(x0, -65536.0f); y0 = std::max(y0, -65536.0f);
    x1 = std::min(x1, 65536.0f); y1 = std::min(y1, 65536.0f);
    gridX0_ = x0;
    gridY0_ = y0;
    gridW_ = int((x1 - x0) / kCell) + 1;
    gridH_ = int((y1 - y0) / kCell) + 1;
    grid_.assign(size_t(gridW_) * size_t(gridH_), {});
    for (uint32_t i = 0; i < brushes_.size(); ++i) {
        const auto& b = brushes_[i];
        const int cx0 = std::clamp(int((b.mins[0] - x0) / kCell), 0, gridW_ - 1), cx1 = std::clamp(int((b.maxs[0] - x0) / kCell), 0, gridW_ - 1);
        const int cy0 = std::clamp(int((b.mins[1] - y0) / kCell), 0, gridH_ - 1), cy1 = std::clamp(int((b.maxs[1] - y0) / kCell), 0, gridH_ - 1);
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) grid_[size_t(y) * size_t(gridW_) + size_t(x)].push_back(i);
    }
    stamp_.assign(brushes_.size(), 0);
}

// Box-vs-brush sweep (the classic separating-plane clip of the id Tech 3 lineage).
void CollisionWorld::traceBrush(const Brush& b, const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, TraceResult& tr) const {
    float enterFrac = -1.0f, leaveFrac = 1.0f;
    const Plane* clipPlane = nullptr;
    bool startOut = false, endOut = false;
    for (const auto& p : b.planes) {
        // Expand the plane by the box: use the box corner that is furthest against the normal.
        Vec offset;
        for (int i = 0; i < 3; ++i) offset[size_t(i)] = p.normal[size_t(i)] < 0 ? maxs[size_t(i)] : mins[size_t(i)];
        const float dist = p.dist - dot(offset, p.normal);
        const float d1 = dot(start, p.normal) - dist;
        const float d2 = dot(end, p.normal) - dist;
        if (d2 > 0) endOut = true;
        if (d1 > 0) startOut = true;
        if (d1 > 0 && (d2 >= kSurfaceEpsilon || d2 >= d1)) return; // completely in front of this plane
        if (d1 <= 0 && d2 <= 0) continue;
        if (d1 > d2) {
            const float f = std::max(0.0f, (d1 - kSurfaceEpsilon) / (d1 - d2));
            if (f > enterFrac) { enterFrac = f; clipPlane = &p; }
        } else {
            const float f = std::min(1.0f, (d1 + kSurfaceEpsilon) / (d1 - d2));
            if (f < leaveFrac) leaveFrac = f;
        }
    }
    if (!startOut) {
        tr.startSolid = true;
        if (!endOut) tr.allSolid = true;
        tr.fraction = 0;
        tr.contents = b.contents;
        return;
    }
    if (enterFrac < leaveFrac && enterFrac > -1 && enterFrac < tr.fraction && clipPlane) {
        tr.fraction = std::max(0.0f, enterFrac);
        tr.normal = clipPlane->normal;
        tr.contents = b.contents;
    }
}

TraceResult CollisionWorld::trace(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const {
    TraceResult tr;
    tr.endPos = end;
    if (grid_.empty()) return tr;
    Vec lo, hi;
    for (int i = 0; i < 3; ++i) {
        lo[size_t(i)] = std::min(start[size_t(i)], end[size_t(i)]) + mins[size_t(i)] - 1;
        hi[size_t(i)] = std::max(start[size_t(i)], end[size_t(i)]) + maxs[size_t(i)] + 1;
    }
    const int cx0 = std::clamp(int((lo[0] - gridX0_) / kCell), 0, gridW_ - 1), cx1 = std::clamp(int((hi[0] - gridX0_) / kCell), 0, gridW_ - 1);
    const int cy0 = std::clamp(int((lo[1] - gridY0_) / kCell), 0, gridH_ - 1), cy1 = std::clamp(int((hi[1] - gridY0_) / kCell), 0, gridH_ - 1);
    ++stampValue_;
    for (int y = cy0; y <= cy1; ++y)
        for (int x = cx0; x <= cx1; ++x)
            for (uint32_t bi : grid_[size_t(y) * size_t(gridW_) + size_t(x)]) {
                if (stamp_[bi] == stampValue_) continue;
                stamp_[bi] = stampValue_;
                const Brush& b = brushes_[bi];
                if (!(b.contents & mask)) continue;
                if (b.mins[0] > hi[0] || b.maxs[0] < lo[0] || b.mins[1] > hi[1] || b.maxs[1] < lo[1] || b.mins[2] > hi[2] || b.maxs[2] < lo[2])
                    continue;
                traceBrush(b, start, end, mins, maxs, tr);
                if (tr.allSolid) break;
            }
    for (int i = 0; i < 3; ++i) tr.endPos[size_t(i)] = start[size_t(i)] + (end[size_t(i)] - start[size_t(i)]) * tr.fraction;
    return tr;
}

} // namespace iw2::game
