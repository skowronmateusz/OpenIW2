// Cells, portals and the portal flood of the 1.3 renderer, implemented from docs/spec/render-hints.md "World and static model culling"
// (loaders 0x10003fd0 / 0x100041b0 / 0x10004770, camera cell 0x10007da0, flood 0x1000a060 / 0x10009e30, portal clip 0x10007f30, hull
// 0x10058310, plane list 0x10007470, small portals 0x10006de0 / 0x10009c00).
#pragma once

#include "game/viewcull.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace iw2::game {

using VisPoint = std::array<float, 3>;

struct VisPortal {
    float n[3] = {0, 0, 0};
    float dist = 0;       // plane: n . p = dist
    int cell = -1;        // the cell behind the portal
    std::vector<VisPoint> verts;
    float axisA[3] = {0, 0, 0}, axisB[3] = {0, 0, 0}; // orthonormal axes of the portal plane
};

struct VisCell {
    float mins[3] = {0, 0, 0}, maxs[3] = {0, 0, 0};
    int firstPortal = 0, portalCount = 0;
    std::vector<int> surfaces; // world surfaces (soups) of the cell: its AABB-tree root range and its cull groups
};

struct VisWorld {
    std::vector<VisCell> cells;
    std::vector<VisPortal> portals;
    struct Node { int plane = 0; int child[2] = {0, 0}; }; // child < 0: leaf -1 - child
    std::vector<Node> nodes;
    std::vector<int> leafCell;                    // per leaf: its cell, -1 outside every cell
    std::vector<std::array<float, 4>> planes;     // lump 4: normal, dist
    bool valid() const { return !cells.empty() && !nodes.empty(); }
};

// Builds the cell data from the raw lumps (4 planes, 10 cull groups, 11 cull group indices, 17 portal vertices, 22 AABB trees, 23 cells,
// 24 portals, 25 nodes, 26 leaves). Rows that point outside their lumps are ignored; a map without cells gives an invalid world.
VisWorld parseVisWorld(std::span<const uint8_t> planes, std::span<const uint8_t> cullGroups, std::span<const uint8_t> cullGroupIndices,
                       std::span<const uint8_t> portalVerts, std::span<const uint8_t> trees, std::span<const uint8_t> cells,
                       std::span<const uint8_t> portals, std::span<const uint8_t> nodes, std::span<const uint8_t> leafs);

// Cell of a point (0x10007da0): down the node tree, in front of a plane (n . p - dist > 0) to child 0, else child 1; -1 outside.
int cellForPoint(const VisWorld& w, const float p[3]);

// Clips a polygon by one plane with the portal clip's tolerances (0x10007f30): e = n . p + d - 0.001; below -0.001 outside, above
// 0.001 inside, else on the plane (kept as it is); new points on edges between an inside and an outside vertex only. Returns the count
// left (0 when no vertex is inside).
size_t clipPortalPolygon(std::vector<VisPoint>& poly, const ViewPlane& pl);

// 2D convex hull (counter-clockwise, collinear points within 0.001 dropped).
std::vector<std::array<float, 2>> convexHull2D(std::vector<std::array<float, 2>> pts);

// The plane list through a portal hull (0x10007470): per hull edge the plane through the eye and the edge, pushed 0.001 outward and
// facing the hull, then `tail` (near and far planes).
std::vector<ViewPlane> portalEdgePlanes(const std::vector<VisPoint>& hull, const float eye[3], const std::vector<ViewPlane>& tail);

// Small portal test (0x10006de0): true when, projected with the column-major view-projection, a quarter of the bounding rectangle's
// area or a quarter of the polygon's area is below minClipArea; a point with w < 0.125 makes the portal large.
bool portalIsSmall(const std::vector<VisPoint>& hull, const float viewProj[16], float minClipArea);

// One walk of a cell with a plane list.
struct CellVisit {
    int cell = -1;
    int planeList = -1; // index into PortalFlood::planeLists
};

struct PortalFlood {
    std::vector<std::vector<ViewPlane>> planeLists;
    std::vector<CellVisit> visits;
    int portalsTaken = 0;
    bool fellBack = false; // the safety limit was hit: use the view planes for every surface
};

struct FloodParams {
    float eye[3] = {0, 0, 0};
    float forward[3] = {1, 0, 0};
    const float* viewProj = nullptr; // column-major, clip = m * p
    float minClipArea = 0.02f;       // r_portalMinClipArea
    int walkLimit = 0;               // r_portalWalkLimit (0: none)
    int safetyLimit = 4096;          // OpenIW2: taken portals before the flood gives up
};

// The flood from the camera cell. viewPlanes = side planes, then near and (when there is one) far; `tailCount` is how many planes at the
// end of viewPlanes (near, far) every portal plane list carries too.
PortalFlood portalFlood(const VisWorld& w, int cameraCell, const std::vector<ViewPlane>& viewPlanes, size_t tailCount, const FloodParams& fp);

} // namespace iw2::game
