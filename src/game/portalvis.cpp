// Cells, portals and the portal flood, implemented from docs/spec/render-hints.md "World and static model culling".
#include "game/portalvis.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iw2::game {
namespace {

template <typename T> T readAt(std::span<const uint8_t> b, size_t off) {
    T v;
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross3(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float planeValue(const ViewPlane& pl, const float* p) { return pl.n[0] * p[0] + pl.n[1] * p[1] + pl.n[2] * p[2] + pl.d; }

// All vertices of the portal on or outside one plane of the list (0x10007e50).
bool portalOutsidePlanes(const VisPortal& p, const std::vector<ViewPlane>& planes) {
    for (const ViewPlane& pl : planes) {
        bool anyIn = false;
        for (const VisPoint& v : p.verts)
            if (planeValue(pl, v.data()) > 0.0f) {
                anyIn = true;
                break;
            }
        if (!anyIn) return true;
    }
    return false;
}

// The eye inside the portal polygon (0x1005dc00), on the polygon projected along the normal's largest axis; either winding.
bool pointInPortal(const VisPortal& p, const float eye[3]) {
    int drop = 0;
    for (int a = 1; a < 3; ++a)
        if (std::abs(p.n[a]) > std::abs(p.n[drop])) drop = a;
    const int u = drop == 0 ? 1 : 0, v = drop == 2 ? 1 : 2;
    bool pos = false, neg = false;
    const size_t n = p.verts.size();
    for (size_t i = 0; i < n; ++i) {
        const VisPoint& a = p.verts[i];
        const VisPoint& b = p.verts[(i + 1) % n];
        const float c = (b[size_t(u)] - a[size_t(u)]) * (eye[v] - a[size_t(v)]) - (b[size_t(v)] - a[size_t(v)]) * (eye[u] - a[size_t(u)]);
        if (c > 0.0f) pos = true;
        if (c < 0.0f) neg = true;
    }
    return n >= 3 && !(pos && neg);
}

} // namespace

VisWorld parseVisWorld(std::span<const uint8_t> planes, std::span<const uint8_t> cullGroups, std::span<const uint8_t> cullGroupIndices,
                       std::span<const uint8_t> portalVerts, std::span<const uint8_t> trees, std::span<const uint8_t> cells,
                       std::span<const uint8_t> portals, std::span<const uint8_t> nodes, std::span<const uint8_t> leafs) {
    VisWorld w;
    for (size_t i = 0; i + 16 <= planes.size(); i += 16)
        w.planes.push_back({readAt<float>(planes, i), readAt<float>(planes, i + 4), readAt<float>(planes, i + 8), readAt<float>(planes, i + 12)});
    const size_t treeCount = trees.size() / 12, groupCount = cullGroups.size() / 32, groupIndexCount = cullGroupIndices.size() / 4;
    const size_t vertCount = portalVerts.size() / 12, portalCount = portals.size() / 16;
    for (size_t i = 0; i + 52 <= cells.size(); i += 52) {
        VisCell c;
        for (int a = 0; a < 3; ++a) {
            c.mins[a] = readAt<float>(cells, i + size_t(a) * 4);
            c.maxs[a] = readAt<float>(cells, i + 12 + size_t(a) * 4);
        }
        const int tree = readAt<int32_t>(cells, i + 24);
        c.firstPortal = readAt<int32_t>(cells, i + 28);
        c.portalCount = readAt<int32_t>(cells, i + 32);
        if (c.firstPortal < 0 || c.portalCount < 0 || size_t(c.firstPortal) + size_t(c.portalCount) > portalCount) c.firstPortal = c.portalCount = 0;
        const int firstGroup = readAt<int32_t>(cells, i + 36), groups = readAt<int32_t>(cells, i + 40);
        if (tree >= 0 && size_t(tree) < treeCount) {
            const int first = readAt<int32_t>(trees, size_t(tree) * 12), count = readAt<int32_t>(trees, size_t(tree) * 12 + 4);
            for (int s = 0; s < count; ++s) c.surfaces.push_back(first + s);
        }
        for (int k = 0; k < groups; ++k) {
            const size_t gi = size_t(firstGroup + k);
            if (firstGroup + k < 0 || gi >= groupIndexCount) continue;
            const int g = readAt<int32_t>(cullGroupIndices, gi * 4);
            if (g < 0 || size_t(g) >= groupCount) continue;
            const int first = readAt<int32_t>(cullGroups, size_t(g) * 32 + 24), count = readAt<int32_t>(cullGroups, size_t(g) * 32 + 28);
            for (int s = 0; s < count; ++s) c.surfaces.push_back(first + s);
        }
        w.cells.push_back(std::move(c));
    }
    for (size_t i = 0; i < portalCount; ++i) {
        VisPortal p;
        const int plane = readAt<int32_t>(portals, i * 16), first = readAt<int32_t>(portals, i * 16 + 8), count = readAt<int32_t>(portals, i * 16 + 12);
        p.cell = readAt<int32_t>(portals, i * 16 + 4);
        if (plane >= 0 && size_t(plane) < w.planes.size()) {
            for (int a = 0; a < 3; ++a) p.n[a] = w.planes[size_t(plane)][size_t(a)];
            p.dist = w.planes[size_t(plane)][3];
        }
        for (int k = 0; k < count; ++k)
            if (first + k >= 0 && size_t(first + k) < vertCount) {
                const size_t o = size_t(first + k) * 12;
                p.verts.push_back({readAt<float>(portalVerts, o), readAt<float>(portalVerts, o + 4), readAt<float>(portalVerts, o + 8)});
            }
        if (p.cell < 0 || size_t(p.cell) >= w.cells.size()) p.cell = -1;
        // Axes of the plane: a unit vector perpendicular to the normal (along the normal's smallest axis, made perpendicular), B = n x A.
        int small = 0;
        for (int a = 1; a < 3; ++a)
            if (std::abs(p.n[a]) < std::abs(p.n[small])) small = a;
        float e[3] = {0, 0, 0};
        e[small] = 1.0f;
        const float k = dot3(e, p.n);
        for (int a = 0; a < 3; ++a) p.axisA[a] = e[a] - k * p.n[a];
        const float len = std::sqrt(dot3(p.axisA, p.axisA));
        if (len > 0.0f)
            for (float& c : p.axisA) c /= len;
        cross3(p.n, p.axisA, p.axisB);
        w.portals.push_back(std::move(p));
    }
    for (size_t i = 0; i + 36 <= nodes.size(); i += 36)
        w.nodes.push_back({readAt<int32_t>(nodes, i), {readAt<int32_t>(nodes, i + 4), readAt<int32_t>(nodes, i + 8)}});
    for (size_t i = 0; i + 36 <= leafs.size(); i += 36) {
        const int cell = readAt<int32_t>(leafs, i + 24);
        w.leafCell.push_back(cell >= 0 && size_t(cell) < w.cells.size() ? cell : -1);
    }
    return w;
}

int cellForPoint(const VisWorld& w, const float p[3]) {
    if (w.nodes.empty()) return -1;
    int node = 0;
    for (size_t guard = 0; guard <= w.nodes.size(); ++guard) {
        const VisWorld::Node& nd = w.nodes[size_t(node)];
        if (nd.plane < 0 || size_t(nd.plane) >= w.planes.size()) return -1;
        const auto& pl = w.planes[size_t(nd.plane)];
        const float v = pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] - pl[3];
        const int child = nd.child[v > 0.0f ? 0 : 1];
        if (child < 0) {
            const size_t leaf = size_t(-1 - child);
            return leaf < w.leafCell.size() ? w.leafCell[leaf] : -1;
        }
        if (size_t(child) >= w.nodes.size()) return -1;
        node = child;
    }
    return -1;
}

size_t clipPortalPolygon(std::vector<VisPoint>& poly, const ViewPlane& pl) {
    const size_t n = poly.size();
    std::vector<float> e(n);
    std::vector<int> side(n); // 0 inside, 1 outside, 2 on the plane
    size_t in = 0, out = 0;
    for (size_t i = 0; i < n; ++i) {
        e[i] = planeValue(pl, poly[i].data()) - 0.001f;
        side[i] = e[i] < -0.001f ? 1 : e[i] > 0.001f ? 0 : 2;
        in += side[i] == 0;
        out += side[i] == 1;
    }
    if (in == 0) {
        poly.clear();
        return 0;
    }
    if (out == 0) return n;
    std::vector<VisPoint> res;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        if (side[i] != 1) res.push_back(poly[i]);
        if (side[i] != 2 && side[j] != 2 && side[i] != side[j]) {
            const float f = e[i] / (e[i] - e[j]);
            VisPoint q;
            for (size_t a = 0; a < 3; ++a) q[a] = poly[i][a] + (poly[j][a] - poly[i][a]) * f;
            res.push_back(q);
        }
    }
    poly = std::move(res);
    return poly.size();
}

std::vector<std::array<float, 2>> convexHull2D(std::vector<std::array<float, 2>> pts) {
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    auto cross = [](const std::array<float, 2>& o, const std::array<float, 2>& a, const std::array<float, 2>& b) {
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    };
    std::vector<std::array<float, 2>> h(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0.001f) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0.001f) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k > 1 ? k - 1 : k);
    return h;
}

std::vector<ViewPlane> portalEdgePlanes(const std::vector<VisPoint>& hull, const float eye[3], const std::vector<ViewPlane>& tail) {
    std::vector<ViewPlane> out;
    const size_t n = hull.size();
    float c[3] = {0, 0, 0};
    for (const VisPoint& p : hull)
        for (size_t a = 0; a < 3; ++a) c[a] += p[a] / float(n);
    for (size_t i = 0; i < n; ++i) {
        const VisPoint& p0 = hull[i];
        const VisPoint& p1 = hull[(i + 1) % n];
        const float a[3] = {p0[0] - eye[0], p0[1] - eye[1], p0[2] - eye[2]}, b[3] = {p1[0] - eye[0], p1[1] - eye[1], p1[2] - eye[2]};
        float nrm[3];
        cross3(b, a, nrm);
        const float len = std::sqrt(dot3(nrm, nrm));
        if (!(len > 0.0f)) continue;
        for (float& v : nrm) v /= len;
        ViewPlane pl{{nrm[0], nrm[1], nrm[2]}, 0.001f - dot3(nrm, p0.data())};
        if (planeValue(pl, c) < 0.0f) pl = {{-nrm[0], -nrm[1], -nrm[2]}, 0.001f + dot3(nrm, p0.data())};
        out.push_back(pl);
    }
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

bool portalIsSmall(const std::vector<VisPoint>& hull, const float m[16], float minClipArea) {
    if (!(minClipArea > 0.0f) || hull.size() < 3) return false;
    std::vector<std::array<float, 2>> ndc;
    float lo[2] = {1.0f, 1.0f}, hi[2] = {-1.0f, -1.0f};
    for (const VisPoint& p : hull) {
        const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (w < 0.125f) return false;
        const float x = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w, y = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
        ndc.push_back({x, y});
        lo[0] = std::min(lo[0], x), hi[0] = std::max(hi[0], x);
        lo[1] = std::min(lo[1], y), hi[1] = std::max(hi[1], y);
    }
    if ((hi[0] - lo[0]) * (hi[1] - lo[1]) * 0.25f < minClipArea) return true;
    float twice = 0.0f;
    for (size_t i = 0; i < ndc.size(); ++i) {
        const auto& a = ndc[i];
        const auto& b = ndc[(i + 1) % ndc.size()];
        twice += a[0] * b[1] - b[0] * a[1];
    }
    return std::abs(twice) * 0.125f < minClipArea;
}

PortalFlood portalFlood(const VisWorld& w, int cameraCell, const std::vector<ViewPlane>& viewPlanes, size_t tailCount, const FloodParams& fp) {
    PortalFlood out;
    if (cameraCell < 0 || size_t(cameraCell) >= w.cells.size()) {
        out.fellBack = true;
        return out;
    }
    const std::vector<ViewPlane> tail(viewPlanes.end() - std::ptrdiff_t(std::min(tailCount, viewPlanes.size())), viewPlanes.end());
    const size_t np = w.portals.size();
    std::vector<uint8_t> queued(np, 0), onChain(np, 0);
    std::vector<int> prev(np, -1);
    std::vector<std::vector<std::array<float, 2>>> points(np);
    struct Entry { int portal; float key; };
    std::vector<Entry> heap;
    auto heapLess = [](const Entry& a, const Entry& b) { return a.key > b.key; }; // min-heap by key
    out.planeLists.push_back(viewPlanes);
    const float fd = dot3(fp.forward, fp.eye);
    // Marks (or clears) the chain of portals that led to a cell; a broken or cyclic chain ends the walk.
    auto setChain = [&](int portal, uint8_t value) {
        for (size_t guard = 0; portal >= 0 && guard <= np; ++guard) {
            onChain[size_t(portal)] = value;
            portal = prev[size_t(portal)];
        }
    };
    // Cells reachable through portals that pass a plane list, each once (0x10009c00), for a small portal.
    auto smallFlood = [&](int start, int list) {
        std::vector<int> found;
        auto collect = [&](auto&& self, int cell, size_t depth) -> void {
            if (depth > w.cells.size()) return;
            const VisCell& c = w.cells[size_t(cell)];
            const std::vector<ViewPlane>& planes = out.planeLists[size_t(list)];
            for (int k = 0; k < c.portalCount; ++k) {
                const size_t pi = size_t(c.firstPortal + k);
                const VisPortal& p = w.portals[pi];
                if (p.cell < 0 || std::find(found.begin(), found.end(), p.cell) != found.end() || onChain[pi]) continue;
                if (dot3(p.n, fp.eye) - p.dist > 0.0f || portalOutsidePlanes(p, planes)) continue;
                std::vector<VisPoint> poly = p.verts;
                bool empty = false;
                for (const ViewPlane& pl : planes)
                    if (clipPortalPolygon(poly, pl) == 0) {
                        empty = true;
                        break;
                    }
                if (empty) continue;
                found.push_back(p.cell);
                self(self, p.cell, depth + 1);
            }
        };
        collect(collect, start, 0);
        for (int cell : found) out.visits.push_back({cell, list});
    };
    auto visit = [&](auto&& self, int cell, int chain, int list, bool large, size_t depth) -> void {
        out.visits.push_back({cell, list});
        if (depth > np) return;
        setChain(chain, 1);
        if (!large) {
            smallFlood(cell, list);
            setChain(chain, 0);
            return;
        }
        const VisCell& c = w.cells[size_t(cell)];
        for (int k = 0; k < c.portalCount; ++k) {
            const size_t pi = size_t(c.firstPortal + k);
            const VisPortal& p = w.portals[pi];
            if (p.cell < 0 || onChain[pi]) continue;
            const float v = dot3(p.n, fp.eye) - p.dist;
            const std::vector<ViewPlane>& planes = out.planeLists[size_t(list)];
            if (v > 0.0f || portalOutsidePlanes(p, planes)) continue;
            if (v <= -0.125f) {
                std::vector<VisPoint> poly = p.verts;
                bool empty = false;
                for (const ViewPlane& pl : planes)
                    if (clipPortalPolygon(poly, pl) == 0) {
                        empty = true;
                        break;
                    }
                if (empty) continue;
                for (const VisPoint& q : poly) points[pi].push_back({dot3(p.axisA, q.data()), dot3(p.axisB, q.data())});
                if (!queued[pi]) {
                    queued[pi] = 1;
                    prev[pi] = chain;
                    float key = 3.4e38f;
                    for (const VisPoint& q : p.verts) key = std::min(key, dot3(fp.forward, q.data()) - fd);
                    heap.push_back({int(pi), key});
                    std::push_heap(heap.begin(), heap.end(), heapLess);
                } else if (prev[pi] != chain) {
                    prev[pi] = -1;
                }
            } else if (pointInPortal(p, fp.eye)) {
                prev[pi] = -1;
                self(self, p.cell, int(pi), list, large, depth + 1);
                setChain(chain, 1); // the nested walk cleared its own chain only
            }
        }
        setChain(chain, 0);
    };
    visit(visit, cameraCell, -1, 0, true, 0);
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), heapLess);
        const int pi = heap.back().portal;
        heap.pop_back();
        queued[size_t(pi)] = 0;
        const std::vector<std::array<float, 2>> hull2 = convexHull2D(std::move(points[size_t(pi)]));
        points[size_t(pi)].clear();
        if (hull2.size() < 3) continue;
        ++out.portalsTaken;
        if (out.portalsTaken == fp.walkLimit) break;
        if (out.portalsTaken > fp.safetyLimit) {
            out.fellBack = true;
            break;
        }
        const VisPortal& p = w.portals[size_t(pi)];
        std::vector<VisPoint> hull;
        for (const auto& h : hull2) {
            VisPoint q;
            for (size_t a = 0; a < 3; ++a) q[a] = p.dist * p.n[a] + h[0] * p.axisA[a] + h[1] * p.axisB[a];
            hull.push_back(q);
        }
        out.planeLists.push_back(portalEdgePlanes(hull, fp.eye, tail));
        const bool small = fp.viewProj && portalIsSmall(hull, fp.viewProj, fp.minClipArea);
        visit(visit, p.cell, pi, int(out.planeLists.size()) - 1, !small, 0);
    }
    return out;
}

} // namespace iw2::game
