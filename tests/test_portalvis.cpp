// Cells, portals and the portal flood (game/portalvis.h, docs/spec/render-hints.md "World and static model culling"). The second half
// runs on duhoc_assault when IW2_MAIN is set.
#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/portalvis.h"
#include "game/viewprojection.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace iw2::game;

namespace {

int bad = 0;
void chk(bool c, int line) {
    if (!c) {
        std::printf("line %d failed\n", line);
        ++bad;
    }
}

bool inside(const std::vector<ViewPlane>& planes, float x, float y, float z) {
    for (const ViewPlane& p : planes)
        if (p.n[0] * x + p.n[1] * y + p.n[2] * z + p.d <= 0.0f) return false;
    return true;
}

// Two box cells A (x 0..100) and B (x 100..200) joined by a 20 x 20 portal at x = 100.
VisWorld twoCells() {
    VisWorld w;
    w.planes = {{1, 0, 0, 100}};
    VisCell a, b;
    a.mins[0] = 0, a.maxs[0] = 100;
    b.mins[0] = 100, b.maxs[0] = 200;
    for (VisCell* c : {&a, &b}) c->mins[1] = c->mins[2] = -50, c->maxs[1] = c->maxs[2] = 50;
    a.firstPortal = 0, a.portalCount = 1, a.surfaces = {0};
    b.firstPortal = 1, b.portalCount = 1, b.surfaces = {1};
    w.cells = {a, b};
    VisPortal ab, ba;
    ab.n[0] = 1, ab.dist = 100, ab.cell = 1;
    ba.n[0] = -1, ba.dist = -100, ba.cell = 0;
    for (VisPortal* p : {&ab, &ba}) {
        p->verts = {{100, -10, -10}, {100, 10, -10}, {100, 10, 10}, {100, -10, 10}};
        p->axisA[1] = 1;
        p->axisB[2] = p->n[0]; // n x A for A = +y
    }
    w.portals = {ab, ba};
    w.nodes = {{0, {-2, -1}}}; // in front of x = 100: leaf 1 (cell B), else leaf 0 (cell A)
    w.leafCell = {0, 1};
    return w;
}

// A 90 degree view from `eye` along +x or -x: four side planes and the near plane.
std::vector<ViewPlane> view90(const float eye[3], float dir) {
    const float s = std::sqrt(0.5f);
    std::vector<ViewPlane> v;
    const float ns[4][3] = {{dir * s, s, 0}, {dir * s, -s, 0}, {dir * s, 0, s}, {dir * s, 0, -s}};
    for (const auto& n : ns) v.push_back({{n[0], n[1], n[2]}, -(n[0] * eye[0] + n[1] * eye[1] + n[2] * eye[2])});
    const float fwd[3] = {dir, 0, 0};
    v.push_back(nearViewPlane(eye, fwd));
    return v;
}

} // namespace

int main() {
    // Camera cell.
    {
        const VisWorld w = twoCells();
        const float pa[3] = {50, 0, 0}, pb[3] = {150, 0, 0}, on[3] = {100, 0, 0};
        chk(cellForPoint(w, pa) == 0 && cellForPoint(w, pb) == 1, __LINE__);
        chk(cellForPoint(w, on) == 0, __LINE__); // exactly on the plane: the back child
    }
    // Polygon clip: a square halved by x <= 5 (inside = n . p + d > 0 with the 0.001 tolerances).
    {
        std::vector<VisPoint> sq = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
        const ViewPlane pl{{-1, 0, 0}, 5.0f};
        chk(clipPortalPolygon(sq, pl) == 4, __LINE__);
        float maxX = 0;
        for (const auto& p : sq) maxX = std::max(maxX, p[0]);
        chk(std::abs(maxX - 4.998f) < 1e-3f, __LINE__); // cut where n . p + d - 0.001 = 0
        std::vector<VisPoint> out = {{6, 0, 0}, {9, 0, 0}, {9, 3, 0}};
        chk(clipPortalPolygon(out, pl) == 0 && out.empty(), __LINE__);
        std::vector<VisPoint> onPlane = {{4.999f, 0, 0}, {4.999f, 5, 0}, {4.999f, 5, 5}};
        chk(clipPortalPolygon(onPlane, pl) == 0, __LINE__); // only "on the plane" vertices: nothing inside
    }
    // Convex hull drops interior and collinear points.
    {
        const auto h = convexHull2D({{0, 0}, {4, 0}, {2, 0}, {4, 4}, {0, 4}, {2, 2}, {1, 1}});
        chk(h.size() == 4, __LINE__);
    }
    // Small portals: projected with w = z, x / w, y / w.
    {
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        const std::vector<VisPoint> tiny = {{0, 0, 1}, {0.2f, 0, 1}, {0.2f, 0.2f, 1}, {0, 0.2f, 1}};
        const std::vector<VisPoint> big = {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        const std::vector<VisPoint> close = {{0, 0, 0.1f}, {0.01f, 0, 1}, {0.01f, 0.01f, 1}};
        chk(portalIsSmall(tiny, m, 0.02f), __LINE__);  // 0.04 * 0.25 = 0.01 < 0.02
        chk(!portalIsSmall(big, m, 0.02f), __LINE__);  // 0.25
        chk(!portalIsSmall(close, m, 0.02f), __LINE__); // a point with w < 0.125 makes it large
        chk(!portalIsSmall(tiny, m, 0.0f), __LINE__);  // r_portalMinClipArea 0: never small
    }
    // Flood: from cell A looking at the portal, cell B is walked with the planes through the portal.
    {
        const VisWorld w = twoCells();
        const float eye[3] = {50, 0, 0};
        FloodParams fp;
        fp.eye[0] = 50, fp.forward[0] = 1;
        const auto v = view90(eye, 1);
        const PortalFlood f = portalFlood(w, 0, v, 1, fp);
        chk(!f.fellBack && f.visits.size() == 2 && f.portalsTaken == 1, __LINE__);
        chk(f.visits[0].cell == 0 && f.visits[0].planeList == 0 && f.visits[1].cell == 1, __LINE__);
        const auto& bp = f.planeLists[size_t(f.visits[1].planeList)];
        chk(bp.size() == 5, __LINE__);                     // four edge planes and the near plane
        chk(inside(bp, 150, 0, 0) && inside(bp, 150, 19, 19), __LINE__); // through the portal (half width 20 at x 150)
        chk(!inside(bp, 150, 25, 0) && !inside(bp, 150, 0, -25), __LINE__);
        // Looking away: the portal is behind the eye, cell B is not reached.
        const PortalFlood g = portalFlood(w, 0, view90(eye, -1), 1, fp);
        chk(g.visits.size() == 1 && g.portalsTaken == 0, __LINE__);
        // Standing in the portal: cell B is walked at once with the view planes.
        const float eye2[3] = {100, 0, 0};
        FloodParams fp2 = fp;
        fp2.eye[0] = 100;
        const PortalFlood h = portalFlood(w, 0, view90(eye2, 1), 1, fp2);
        chk(h.visits.size() >= 2 && h.visits[1].cell == 1 && h.visits[1].planeList == 0, __LINE__); // (and back into A through B's portal)
        // Outside every cell: no flood.
        chk(portalFlood(w, -1, v, 1, fp).fellBack, __LINE__);
        // r_portalWalkLimit 1 stops at the first taken portal (before its cell).
        FloodParams fp3 = fp;
        fp3.walkLimit = 1;
        chk(portalFlood(w, 0, v, 1, fp3).visits.size() == 1, __LINE__);
    }

    // duhoc_assault: from the beach the flood reaches the camera cell, and every surface it keeps also passes the view planes.
    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        const auto data = vfs.read("maps/duhoc_assault.d3dbsp");
        chk(data.has_value(), __LINE__);
        if (data) {
            const iw2::assets::D3dBsp bsp(*data);
            const VisWorld w = parseVisWorld(bsp.lump(4), bsp.lump(10), bsp.lump(11), bsp.lump(17), bsp.lump(22), bsp.lump(23), bsp.lump(24),
                                             bsp.lump(25), bsp.lump(26));
            chk(w.valid() && w.cells.size() == 45 && w.portals.size() == 276, __LINE__);
            std::vector<int> seen(6342, 0);
            size_t surfaces = 0;
            for (const VisCell& c : w.cells)
                for (int s : c.surfaces)
                    if (s >= 0 && s < 6342) surfaces += seen[size_t(s)]++ == 0;
            chk(surfaces == 6342, __LINE__); // every soup of model 0 belongs to a cell (60 through cull groups of two cells)
            const float eye[3] = {2119.2f, -4996.0f, 58.2f};
            const int cell = cellForPoint(w, eye);
            chk(cell >= 0, __LINE__);
            for (const float yaw : {0.0f, 90.0f, 180.0f, 270.0f}) {
                const float r = yaw * 0.0174532925f, fwd[3] = {std::cos(r), std::sin(r), 0};
                // View-projection as iw2view builds it: right = fwd x up, up = right x fwd; 80 degree 4:3 fov at 16:9.
                const float rt[3] = {fwd[1], -fwd[0], 0}, up[3] = {0, 0, 1};
                const float t = 1.0f / std::tan(2.0f * std::atan(std::tan(40.0f * 0.0174532925f) * 0.75f) / 2.0f);
                const auto proj = sceneProjection(t / (16.0f / 9.0f), t, 4.0f);
                const float view[16] = {rt[0], up[0], -fwd[0], 0, rt[1], up[1], -fwd[1], 0, rt[2], up[2], -fwd[2], 0,
                                        -(rt[0] * eye[0] + rt[1] * eye[1]), -eye[2], fwd[0] * eye[0] + fwd[1] * eye[1], 1};
                float vp[16];
                for (int c = 0; c < 4; ++c)
                    for (int rr = 0; rr < 4; ++rr) {
                        float sum = 0;
                        for (int k = 0; k < 4; ++k) sum += proj[size_t(k * 4 + rr)] * view[c * 4 + k];
                        vp[c * 4 + rr] = sum;
                    }
                ViewPlane side[4];
                frustumSidePlanes(vp, side);
                std::vector<ViewPlane> planes(side, side + 4);
                planes.push_back(nearViewPlane(eye, fwd));
                FloodParams fp;
                for (int a = 0; a < 3; ++a) fp.eye[a] = eye[a], fp.forward[a] = fwd[a];
                fp.viewProj = vp;
                const PortalFlood f = portalFlood(w, cell, planes, 1, fp);
                chk(!f.fellBack && !f.visits.empty() && f.visits[0].cell == cell, __LINE__);
                // Every portal plane list lies inside the view: a sample of points inside a visit's planes is inside the view planes.
                size_t checked = 0;
                for (const CellVisit& vis : f.visits) {
                    const auto& pl = f.planeLists[size_t(vis.planeList)];
                    const VisCell& c = w.cells[size_t(vis.cell)];
                    for (int i = 0; i < 8; ++i) {
                        const float x = c.mins[0] + (c.maxs[0] - c.mins[0]) * float((i * 37) % 11) / 10.0f;
                        const float y = c.mins[1] + (c.maxs[1] - c.mins[1]) * float((i * 53) % 13) / 12.0f;
                        const float z = c.mins[2] + (c.maxs[2] - c.mins[2]) * float((i * 71) % 7) / 6.0f;
                        if (inside(pl, x, y, z)) {
                            ++checked;
                            chk(inside(planes, x, y, z) || std::abs(x - eye[0]) + std::abs(y - eye[1]) < 1.0f, __LINE__);
                        }
                    }
                }
                std::printf("portalvis: yaw %.0f cell %d visits %zu portals taken %d (%zu sample points)\n", double(yaw), cell, f.visits.size(),
                            f.portalsTaken, checked);
            }
        }
    }
    std::printf(bad ? "FAIL\n" : "portalvis: ok\n");
    return bad ? 1 : 0;
}
