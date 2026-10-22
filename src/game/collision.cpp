#include "game/collision.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

// Trace and position test against world model 0, following the original collision code (docs/spec/collision.md).
// Function names in comments refer to the procedures of CoD2SP_s.exe 1.3 listed there.

namespace iw2::game {
namespace {

constexpr float kSurfaceClipEpsilon = 0.125f; // DAT_00591f80
constexpr int kMaxPositionLeafs = 1024;      // FUN_00416350

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Plane {
    Vec normal;
    float dist;
    uint8_t type; // 0/1/2 when the normal is exactly +x/+y/+z, else 3 (FUN_00411b10)
};

struct Material {
    uint32_t surfaceFlags;
    uint32_t contents;
};

struct BrushSide {
    int plane;
    int material;
};

struct Brush {
    Vec mins, maxs;
    uint32_t contents;
    int firstSide, numSides;     // non-axial sides
    int16_t axialMaterial[2][3]; // [0] = min side, [1] = max side
};

struct Node {
    int plane;
    int16_t children[2]; // negative: leaf -1 - child
};

struct Leaf {
    uint16_t firstCollAabb = 0, collAabbCount = 0;
    uint32_t brushContents = 0, terrainContents = 0;
    Vec mins{}, maxs{};
    int leafBrushNode = 0;
};

struct LeafBrushNode {
    uint8_t axis = 0;
    int16_t leafBrushCount = 0; // > 0: leaf with brushes; < 0: node + 1 holds the brushes that straddle the split
    uint32_t contents = 0;
    float dist = -FLT_MAX;
    int firstLeafBrush = 0; // leaves: offset into leafBrushes
    float range = 0;
    uint16_t children[2]{}; // relative node offsets
};

struct Edge {
    Vec origin, axis0, axis1, dir;
};

struct Triangle {
    float plane[4], svec[4], tvec[4];
    int verts[3], edges[3];
};

struct Border {
    float distEq[3]; // normal x, normal y, dist
    float zBase, zSlope, start, length;
};

struct Partition {
    uint8_t triCount, borderCount;
    int firstTri, firstBorder;
};

struct AabbTree {
    Vec origin, halfSize;
    uint16_t material, childCount;
    int firstChild; // child trees, or the partition when childCount is 0
};

struct TraceWork {
    Vec start, end;
    Vec invDelta; // 1 / (start - end), 0 where equal
    Vec delta;
    float deltaLen, deltaLenSq;
    Vec midpoint, halfDelta, halfDeltaAbs;
    Vec size; // box half extents
    Vec boundsMins, boundsMaxs;
    uint32_t contents;
    bool isPoint, axialCullOnly;
    float radius, offsetZ;
    Vec extents; // radius, radius, offsetZ + radius
    uint32_t checkcount;
};

} // namespace

struct CollisionWorld::Data {
    std::vector<Material> materials;
    std::vector<Plane> planes;
    std::vector<BrushSide> sides;
    std::vector<Brush> brushes;
    std::vector<Node> nodes;
    std::vector<Leaf> leafs;
    std::vector<uint16_t> leafBrushes;
    std::vector<LeafBrushNode> leafBrushNodes;
    std::vector<Vec> verts;
    std::vector<Edge> edges;
    std::vector<Triangle> tris;
    std::vector<Border> borders;
    std::vector<Partition> partitions;
    std::vector<AabbTree> aabbTrees;
    // Brush submodels (lump 35; index 0 unused): bounds grown by 1, bounding radius, and a leaf over the model's
    // brushes and AABB trees (FUN_00410740, FUN_004114a0).
    struct Model {
        Vec mins{}, maxs{};
        float radius = 0;
        Leaf leaf;
    };
    std::vector<Model> models;
    std::vector<EntityClip> entityClips;

    // Per-trace visit stamps (one thread, like the original's thread-local copies).
    mutable uint32_t checkcount = 0;
    mutable std::vector<uint32_t> edgeStamps, vertStamps;
    mutable std::vector<uint16_t> partitionStamps;

    int buildLeafBrushNode(int first, int count, Vec mins, Vec maxs);
    double splitScore(int first, int count, const Vec& mins, const Vec& maxs, int axis, float& value) const;

    // Trace.
    void traceThroughTree(const TraceWork& tw, int num, const float* p1, const float* p2, TraceResult& tr) const;
    void traceThroughLeaf(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const;
    bool traceThroughLeafBrushes(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const;
    void traceThroughLeafBrushNode(const TraceWork& tw, int node, const float* p1, const float* p2, TraceResult& tr) const;
    void traceThroughBrush(const TraceWork& tw, const Brush& b, TraceResult& tr) const;
    void traceThroughAabbTree(const TraceWork& tw, const AabbTree& tree, TraceResult& tr) const;
    void traceThroughAabbTree_r(const TraceWork& tw, const AabbTree& tree, TraceResult& tr) const;
    void traceCapsuleThroughTriangle(const TraceWork& tw, const Triangle& tri, float offsetZ, TraceResult& tr) const;
    void traceCapsuleThroughBorder(const TraceWork& tw, const Border& b, TraceResult& tr) const;
    void tracePointThroughTriangle(const TraceWork& tw, const Triangle& tri, TraceResult& tr) const;

    // Sight trace (FUN_00418a70 and callees): a non-zero hit id when something blocks.
    int sightThroughTree(const TraceWork& tw, int num, const float* p1, const float* p2, TraceResult& tr) const;
    int sightThroughLeaf(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const;
    int sightThroughLeafBrushNode(const TraceWork& tw, int node, const float* p1, const float* p2) const;
    bool sightThroughBrush(const TraceWork& tw, const Brush& b) const;

    // Position test (start == end).
    void positionTest(const TraceWork& tw, TraceResult& tr) const;
    void boxLeafnums(const Vec& mins, const Vec& maxs, int nodeNum, std::vector<int>& list) const;
    bool testInLeafBrushes(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const;
    void testInLeafBrushNode(const TraceWork& tw, int node, TraceResult& tr) const;
    void testInBrush(const TraceWork& tw, const Brush& b, TraceResult& tr) const;
    void testInLeafTerrain(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const;
    void positionTestInAabbTree_r(const TraceWork& tw, const AabbTree& tree, TraceResult& tr) const;
    void testCapsuleInTriangle(const TraceWork& tw, const Triangle& tri, TraceResult& tr) const;

    // FUN_00414040/FUN_00414200 compare the zero-extended 16-bit stamp with the sign-extended low 16 bits of the
    // counter, so a partition is never skipped while those bits are 0x8000 or above.
    bool markPartition(const TraceWork& tw, int partition) const {
        uint16_t& stamp = partitionStamps[size_t(partition)];
        if (int32_t(stamp) == int32_t(int16_t(uint16_t(tw.checkcount)))) return false;
        stamp = uint16_t(tw.checkcount);
        return true;
    }

    void setHitMaterial(TraceResult& tr, int material) const {
        tr.material = material;
        tr.surfaceFlags = materials[size_t(material)].surfaceFlags;
    }
};

namespace {

// FUN_00412d10: true when the swept box cannot touch the tree's box (separating axis test).
bool cullTrace(const TraceWork& tw, const AabbTree& tree) {
    const float d[3] = {tw.midpoint[0] - tree.origin[0], tw.midpoint[1] - tree.origin[1], tw.midpoint[2] - tree.origin[2]};
    const float e[3] = {tw.size[0] + tree.halfSize[0], tw.size[1] + tree.halfSize[1], tw.size[2] + tree.halfSize[2]};
    for (int i = 0; i < 3; ++i)
        if (!(e[i] + tw.halfDeltaAbs[size_t(i)] >= std::fabs(d[i]))) return true;
    if (tw.axialCullOnly) return false;
    const Vec& hd = tw.halfDelta;
    const Vec& ha = tw.halfDeltaAbs;
    if (!(e[1] * ha[2] + e[2] * ha[1] >= std::fabs(d[2] * hd[1] - d[1] * hd[2]))) return true;
    if (!(e[0] * ha[2] + e[2] * ha[0] >= std::fabs(d[0] * hd[2] - d[2] * hd[0]))) return true;
    if (!(e[0] * ha[1] + e[1] * ha[0] >= std::fabs(d[1] * hd[0] - d[0] * hd[1]))) return true;
    return false;
}

// FUN_00419450: true when the segment misses the box (no epsilon).
bool cullBox(const TraceWork& tw, const Vec& mins, const Vec& maxs, float fraction) {
    float enterFrac = 0, leaveFrac = fraction;
    for (int pass = 0; pass < 2; ++pass) {
        const float s = pass == 0 ? -1.0f : 1.0f;
        const Vec& bound = pass == 0 ? mins : maxs;
        for (size_t i = 0; i < 3; ++i) {
            const float d1 = (tw.start[i] - bound[i]) * s;
            const float d2 = (tw.end[i] - bound[i]) * s;
            if (d1 <= 0) {
                if (d2 > 0) {
                    const float f = d1 * tw.invDelta[i] * s;
                    if (f <= enterFrac) return true;
                    if (f - leaveFrac < 0) leaveFrac = f;
                }
            } else {
                if (d2 > 0) return true;
                const float f = d1 * tw.invDelta[i] * s;
                if (leaveFrac <= f) return true;
                if (enterFrac - f < 0) enterFrac = f;
            }
        }
    }
    return false;
}

// FUN_00425a50 (generic form of the sign-bit jump table): 1 front, 2 back, 3 both.
int boxOnPlaneSide(const Vec& mins, const Vec& maxs, const Plane& p) {
    float dist1 = 0, dist2 = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (p.normal[i] < 0) {
            dist1 += p.normal[i] * mins[i];
            dist2 += p.normal[i] * maxs[i];
        } else {
            dist1 += p.normal[i] * maxs[i];
            dist2 += p.normal[i] * mins[i];
        }
    }
    return (dist1 >= p.dist ? 1 : 0) + (dist2 < p.dist ? 2 : 0);
}

void lerp4(float* out, const float* a, const float* b, float f) {
    for (int i = 0; i < 4; ++i) out[i] = (b[i] - a[i]) * f + a[i];
}

// The 0.7 rule shared by the triangle, edge and vertex hits in FUN_004133c0.
void keepFloorNormal(TraceResult& tr, const Triangle& tri, float endZ, float startZ) {
    if (tri.plane[2] >= 0.7f && tr.normal[2] >= 0 && tr.normal[2] < 0.7f && endZ < startZ)
        tr.normal = {tri.plane[0], tri.plane[1], tri.plane[2]};
}

} // namespace

// ---------------------------------------------------------------------------------------------- loading

CollisionWorld::CollisionWorld(const assets::D3dBsp& bsp) : d_(std::make_unique<Data>()) {
    Data& d = *d_;
    auto fail = [](const char* what) { throw std::runtime_error(std::string("collision: ") + what); };

    const auto mats = bsp.lump(0);
    for (size_t o = 0; o + 72 <= mats.size(); o += 72) d.materials.push_back({rd<uint32_t>(mats.data() + o + 64), rd<uint32_t>(mats.data() + o + 68)});
    auto checkMaterial = [&](int m) {
        if (m < 0 || size_t(m) >= d.materials.size()) fail("bad material index");
    };

    // FUN_00411b10
    const auto planes = bsp.lump(4);
    for (size_t o = 0; o + 16 <= planes.size(); o += 16) {
        Plane p;
        p.normal = {rd<float>(planes.data() + o), rd<float>(planes.data() + o + 4), rd<float>(planes.data() + o + 8)};
        p.dist = rd<float>(planes.data() + o + 12);
        p.type = p.normal[0] == 1.0f ? 0 : p.normal[1] == 1.0f ? 1 : p.normal[2] == 1.0f ? 2 : 3;
        d.planes.push_back(p);
    }

    // FUN_004116c0: brush lump {int16 sideCount; int16 material}; side lump 8 bytes. The first 6 sides of each
    // brush are axial {float value; int32 material} (min x, max x, min y, max y, min z, max z), the rest
    // {int32 plane; int32 material}.
    const auto sides = bsp.lump(5), brushes = bsp.lump(6);
    size_t sidePos = 0;
    for (size_t o = 0; o + 4 <= brushes.size(); o += 4) {
        const int sideCount = rd<int16_t>(brushes.data() + o);
        const int material = rd<int16_t>(brushes.data() + o + 2);
        if (sideCount < 6) fail("brush has fewer than 6 sides");
        if ((sidePos + size_t(sideCount)) * 8 > sides.size()) fail("bad brush sides");
        Brush b;
        for (int axis = 0; axis < 3; ++axis)
            for (int side = 0; side < 2; ++side) {
                const uint8_t* p = sides.data() + (sidePos + size_t(axis * 2 + side)) * 8;
                (side == 0 ? b.mins : b.maxs)[size_t(axis)] = rd<float>(p);
                const int m = rd<int32_t>(p + 4);
                checkMaterial(m);
                b.axialMaterial[side][axis] = int16_t(m);
            }
        b.firstSide = int(d.sides.size());
        b.numSides = sideCount - 6;
        for (int s = 6; s < sideCount; ++s) {
            const uint8_t* p = sides.data() + (sidePos + size_t(s)) * 8;
            const int plane = rd<int32_t>(p), m = rd<int32_t>(p + 4);
            if (plane < 0 || size_t(plane) >= d.planes.size()) fail("bad plane index");
            checkMaterial(m);
            d.sides.push_back({plane, m});
        }
        checkMaterial(material);
        b.contents = d.materials[size_t(material)].contents & 0xdffffffbu;
        sidePos += size_t(sideCount);
        d.brushes.push_back(b);
    }

    // FUN_00411c30
    const auto leafBrushes = bsp.lump(27);
    for (size_t o = 0; o + 4 <= leafBrushes.size(); o += 4) {
        const uint32_t v = rd<uint32_t>(leafBrushes.data() + o);
        if (v > 0xffff || v >= d.brushes.size()) fail("bad leaf brush");
        d.leafBrushes.push_back(uint16_t(v));
    }

    // FUN_004115d0
    const auto nodes = bsp.lump(25);
    for (size_t o = 0; o + 36 <= nodes.size(); o += 36) {
        Node n;
        n.plane = rd<int32_t>(nodes.data() + o);
        if (n.plane < 0 || size_t(n.plane) >= d.planes.size()) fail("bad node plane");
        for (int c = 0; c < 2; ++c) {
            const int32_t v = rd<int32_t>(nodes.data() + o + 4 + size_t(c) * 4);
            if (v != int16_t(v)) fail("node child out of range");
            n.children[c] = int16_t(v);
        }
        d.nodes.push_back(n);
    }

    // FUN_00411d10, FUN_00411df0, FUN_004120f0, FUN_004121d0, FUN_00412340, FUN_004123e0
    const auto verts = bsp.lump(29);
    for (size_t o = 0; o + 16 <= verts.size(); o += 16)
        d.verts.push_back({rd<float>(verts.data() + o + 4), rd<float>(verts.data() + o + 8), rd<float>(verts.data() + o + 12)});
    const auto edges = bsp.lump(30);
    for (size_t o = 0; o + 56 <= edges.size(); o += 56) {
        const uint8_t* p = edges.data() + o;
        auto v3 = [&](size_t at) { return Vec{rd<float>(p + at), rd<float>(p + at + 4), rd<float>(p + at + 8)}; };
        Edge e{v3(4), v3(16), v3(28), v3(40)};
        const float scale = 1.0f / rd<float>(p + 52);
        for (float& x : e.dir) x *= scale;
        d.edges.push_back(e);
    }
    const auto tris = bsp.lump(31);
    for (size_t o = 0; o + 72 <= tris.size(); o += 72) {
        Triangle t;
        std::memcpy(&t, tris.data() + o, 72);
        for (int k = 0; k < 3; ++k) {
            if (t.verts[k] >= int(d.verts.size())) fail("bad triangle vertex");
            if (t.edges[k] >= int(d.edges.size())) fail("bad triangle edge");
        }
        d.tris.push_back(t);
    }
    const auto borders = bsp.lump(32);
    for (size_t o = 0; o + 28 <= borders.size(); o += 28) {
        Border b;
        std::memcpy(&b, borders.data() + o, 28);
        d.borders.push_back(b);
    }
    const auto parts = bsp.lump(33);
    for (size_t o = 0; o + 12 <= parts.size(); o += 12) {
        Partition p{parts[o + 2], parts[o + 3], rd<int32_t>(parts.data() + o + 4), rd<int32_t>(parts.data() + o + 8)};
        if (p.firstTri < 0 || size_t(p.firstTri) + p.triCount > d.tris.size()) fail("bad partition triangles");
        if (p.firstBorder < 0 || size_t(p.firstBorder) + p.borderCount > d.borders.size()) fail("bad partition borders");
        d.partitions.push_back(p);
    }
    const auto trees = bsp.lump(34);
    for (size_t o = 0; o + 32 <= trees.size(); o += 32) {
        const uint8_t* p = trees.data() + o;
        AabbTree t{{rd<float>(p), rd<float>(p + 4), rd<float>(p + 8)},
                   {rd<float>(p + 12), rd<float>(p + 16), rd<float>(p + 20)},
                   rd<uint16_t>(p + 24), rd<uint16_t>(p + 26), rd<int32_t>(p + 28)};
        checkMaterial(t.material);
        d.aabbTrees.push_back(t);
    }
    for (const AabbTree& t : d.aabbTrees) {
        const size_t end = size_t(t.firstChild) + (t.childCount ? t.childCount : 1);
        if (t.firstChild < 0 || end > (t.childCount ? d.aabbTrees.size() : d.partitions.size())) fail("bad AABB tree link");
    }

    // FUN_00411910: leaf lump 36 bytes, +8 firstCollAabb, +12 collAabbCount, +16 firstLeafBrush, +20 leafBrushCount.
    // FUN_00411a10 then fills the contents, bounds and leaf brush tree of every leaf.
    const auto leafs = bsp.lump(26);
    d.leafBrushNodes.emplace_back(); // node 0 stays empty (FUN_00412650)
    for (size_t o = 0; o + 36 <= leafs.size(); o += 36) {
        const uint8_t* p = leafs.data() + o;
        const uint32_t firstAabb = rd<uint32_t>(p + 8), aabbCount = rd<uint32_t>(p + 12);
        if (firstAabb > 0xffff || aabbCount > 0xffff || firstAabb + aabbCount > d.aabbTrees.size()) fail("bad leaf AABB range");
        Leaf leaf;
        leaf.firstCollAabb = uint16_t(firstAabb);
        leaf.collAabbCount = uint16_t(aabbCount);
        const int firstBrush = rd<int32_t>(p + 16), brushCount = rd<int32_t>(p + 20);
        if (firstBrush < 0 || brushCount < 0 || size_t(firstBrush) + size_t(brushCount) > d.leafBrushes.size()) fail("bad leaf brush range");
        for (int i = 0; i < brushCount; ++i) leaf.brushContents |= d.brushes[d.leafBrushes[size_t(firstBrush + i)]].contents;
        for (uint32_t i = 0; i < aabbCount; ++i) leaf.terrainContents |= d.materials[d.aabbTrees[firstAabb + i].material].contents;
        // FUN_004112c0
        if (brushCount != 0) {
            Vec mins{FLT_MAX, FLT_MAX, FLT_MAX}, maxs{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (int i = 0; i < brushCount; ++i) {
                const Brush& b = d.brushes[d.leafBrushes[size_t(firstBrush + i)]];
                for (size_t k = 0; k < 3; ++k) {
                    mins[k] = std::min(mins[k], b.mins[k]);
                    maxs[k] = std::max(maxs[k], b.maxs[k]);
                }
            }
            for (size_t k = 0; k < 3; ++k) {
                leaf.mins[k] = mins[k] - kSurfaceClipEpsilon;
                leaf.maxs[k] = maxs[k] + kSurfaceClipEpsilon;
            }
            leaf.leafBrushNode = d.buildLeafBrushNode(firstBrush, brushCount, mins, maxs);
        }
        d.leafs.push_back(leaf);
    }

    // FUN_00410740 / FUN_004114a0: lump 35 rows {mins, maxs, …, +0x20 firstCollAabb, +0x24 collAabbCount,
    // +0x28 firstBrush, +0x2c brushCount}; model 0 is the world.
    const auto mlump = bsp.lump(35);
    for (size_t o = 0; o + 48 <= mlump.size(); o += 48) {
        const uint8_t* p = mlump.data() + o;
        Data::Model m;
        float r2 = 0;
        for (size_t k = 0; k < 3; ++k) {
            m.mins[k] = rd<float>(p + 4 * k) - 1.0f;
            m.maxs[k] = rd<float>(p + 12 + 4 * k) + 1.0f;
            const float e = std::max(std::fabs(m.mins[k]), std::fabs(m.maxs[k]));
            r2 += e * e;
        }
        m.radius = std::sqrt(r2);
        if (o != 0) {
            const uint32_t firstAabb = rd<uint32_t>(p + 0x20), aabbCount = rd<uint32_t>(p + 0x24);
            const int firstBrush = rd<int32_t>(p + 0x28), brushCount = rd<int32_t>(p + 0x2c);
            if (firstAabb > 0xffff || aabbCount > 0xffff || firstAabb + aabbCount > d.aabbTrees.size()) fail("bad model AABB range");
            if (firstBrush < 0 || brushCount < 0 || size_t(firstBrush) + size_t(brushCount) > d.brushes.size()) fail("bad model brush range");
            Leaf& leaf = m.leaf;
            leaf.firstCollAabb = uint16_t(firstAabb);
            leaf.collAabbCount = uint16_t(aabbCount);
            for (uint32_t i = 0; i < aabbCount; ++i) leaf.terrainContents |= d.materials[d.aabbTrees[firstAabb + i].material].contents;
            if (brushCount != 0) {
                const int first = int(d.leafBrushes.size());
                Vec bmins{FLT_MAX, FLT_MAX, FLT_MAX}, bmaxs{-FLT_MAX, -FLT_MAX, -FLT_MAX};
                for (int i = 0; i < brushCount; ++i) {
                    if (firstBrush + i > 0xffff) fail("model brush index too large");
                    d.leafBrushes.push_back(uint16_t(firstBrush + i));
                    const Brush& b = d.brushes[size_t(firstBrush + i)];
                    leaf.brushContents |= b.contents;
                    for (size_t k = 0; k < 3; ++k) {
                        bmins[k] = std::min(bmins[k], b.mins[k]);
                        bmaxs[k] = std::max(bmaxs[k], b.maxs[k]);
                    }
                }
                for (size_t k = 0; k < 3; ++k) {
                    leaf.mins[k] = bmins[k] - kSurfaceClipEpsilon;
                    leaf.maxs[k] = bmaxs[k] + kSurfaceClipEpsilon;
                }
                leaf.leafBrushNode = d.buildLeafBrushNode(first, brushCount, bmins, bmaxs); // FUN_004112c0
            }
        }
        d.models.push_back(m);
    }
    if (d.nodes.empty() || d.leafs.empty()) fail("map has no nodes or leafs");
    for (const Node& n : d.nodes)
        for (int16_t c : n.children)
            if (c >= int(d.nodes.size()) || (c < 0 && -1 - c >= int(d.leafs.size()))) fail("bad node child");

    d.edgeStamps.assign(d.edges.size(), 0);
    d.vertStamps.assign(d.verts.size(), 0);
    d.partitionStamps.assign(d.partitions.size(), 0);
}

CollisionWorld::CollisionWorld() = default;
CollisionWorld::~CollisionWorld() = default;
CollisionWorld::CollisionWorld(CollisionWorld&&) noexcept = default;
CollisionWorld& CollisionWorld::operator=(CollisionWorld&&) noexcept = default;

size_t CollisionWorld::brushCount() const { return d_ ? d_->brushes.size() : 0; }
size_t CollisionWorld::triangleCount() const { return d_ ? d_->tris.size() : 0; }

// FUN_00410930: scores a split of the leaf's brushes at `value` on `axis`, and moves `value` midway between the
// highest brush below and the lowest brush above it. The original computes in x87 extended precision.
double CollisionWorld::Data::splitScore(int first, int count, const Vec& mins, const Vec& maxs, int axis, float& value) const {
    double maxBelow = -FLT_MAX;
    float minAbove = FLT_MAX;
    int below = -1, above = -1;
    for (int i = 0; i < count; ++i) {
        const Brush& b = brushes[leafBrushes[size_t(first + i)]];
        if (b.mins[size_t(axis)] < value) {
            if (b.maxs[size_t(axis)] <= value) {
                ++below;
                if (maxBelow < b.maxs[size_t(axis)]) maxBelow = b.maxs[size_t(axis)];
            }
        } else {
            ++above;
            if (b.mins[size_t(axis)] < minAbove) minAbove = b.mins[size_t(axis)];
        }
    }
    const int n = above - below >= 0 ? below : above;
    value = float((double(minAbove) + maxBelow) * 0.5);
    if (n < 1) return 0;
    double gap = double(maxs[size_t(axis)]) - maxBelow;
    const double gapAbove = double(minAbove) - mins[size_t(axis)];
    if (gap - gapAbove >= 0) gap = gapAbove;
    return gap * n;
}

// FUN_00410bd0: builds the leaf brush tree. Reorders leafBrushes[first, first + count) in place, like the original.
int CollisionWorld::Data::buildLeafBrushNode(int first, int count, Vec mins, Vec maxs) {
    const int nodeIndex = int(leafBrushNodes.size());
    leafBrushNodes.emplace_back();

    float best = 0, split = 0;
    int axis = -1;
    for (int a = 0; a < 3; ++a)
        for (int i = 0; i < count; ++i) {
            const Brush& b = brushes[leafBrushes[size_t(first + i)]];
            for (float candidate : {b.mins[size_t(a)], b.maxs[size_t(a)]}) {
                const double score = splitScore(first, count, mins, maxs, a, candidate);
                if (score > best) {
                    best = float(score);
                    split = candidate;
                    axis = a;
                }
            }
        }

    if (axis < 0) {
        LeafBrushNode& node = leafBrushNodes[size_t(nodeIndex)];
        if (count != int16_t(count)) throw std::runtime_error("collision: too many brushes in a leaf");
        node.leafBrushCount = int16_t(count);
        for (int i = 0; i < count; ++i) node.contents |= brushes[leafBrushes[size_t(first + i)]].contents;
        node.firstLeafBrush = first;
        return nodeIndex;
    }

    const std::vector<uint16_t> list(leafBrushes.begin() + first, leafBrushes.begin() + first + count);
    int out = first, straddle = 0;
    for (uint16_t bi : list) {
        const Brush& b = brushes[bi];
        if (b.mins[size_t(axis)] < split && b.maxs[size_t(axis)] > split) leafBrushes[size_t(out + straddle++)] = bi;
    }
    if (straddle != 0) {
        const int child = buildLeafBrushNode(out, straddle, mins, maxs);
        LeafBrushNode& node = leafBrushNodes[size_t(nodeIndex)];
        node.leafBrushCount = -1;
        node.contents = leafBrushNodes[size_t(child)].contents;
        out += straddle;
    }
    float range = FLT_MAX;
    leafBrushNodes[size_t(nodeIndex)].axis = uint8_t(axis);
    leafBrushNodes[size_t(nodeIndex)].dist = split;
    for (int side = 0; side < 2; ++side) {
        int n = 0;
        for (uint16_t bi : list) {
            const Brush& b = brushes[bi];
            float gap;
            if (side == 0) {
                if (!(split <= b.mins[size_t(axis)])) continue;
                gap = b.mins[size_t(axis)] - split;
            } else {
                if (!(b.maxs[size_t(axis)] <= split)) continue;
                gap = split - b.maxs[size_t(axis)];
            }
            if (gap - range < 0) range = gap;
            if (size_t(out + n) >= leafBrushes.size()) throw std::runtime_error("collision: leaf brush tree overflow");
            leafBrushes[size_t(out + n++)] = bi;
        }
        Vec childMins = mins, childMaxs = maxs;
        if (side == 0) childMins[size_t(axis)] = range + split;
        else childMaxs[size_t(axis)] = split - range;
        const int child = buildLeafBrushNode(out, n, childMins, childMaxs);
        LeafBrushNode& node = leafBrushNodes[size_t(nodeIndex)];
        node.children[side] = uint16_t(child - nodeIndex);
        node.contents |= leafBrushNodes[size_t(child)].contents;
        out += n;
    }
    leafBrushNodes[size_t(nodeIndex)].range = range;
    return nodeIndex;
}

// ---------------------------------------------------------------------------------------------- trace

// FUN_00417320
void CollisionWorld::Data::traceThroughTree(const TraceWork& tw, int num, const float* p1In, const float* p2, TraceResult& tr) const {
    float p1[4] = {p1In[0], p1In[1], p1In[2], p1In[3]};
    for (;;) {
        if (num < 0) {
            traceThroughLeaf(tw, leafs[size_t(-1 - num)], tr);
            return;
        }
        const Node& node = nodes[size_t(num)];
        const Plane& plane = planes[size_t(node.plane)];
        float t1, t2, offset;
        if (plane.type < 3) {
            t1 = p1[plane.type] - plane.dist;
            t2 = p2[plane.type] - plane.dist;
            offset = tw.size[plane.type] + kSurfaceClipEpsilon;
        } else {
            t1 = p1[0] * plane.normal[0] + p1[1] * plane.normal[1] + p1[2] * plane.normal[2] - plane.dist;
            t2 = p2[0] * plane.normal[0] + p2[1] * plane.normal[1] + p2[2] * plane.normal[2] - plane.dist;
            offset = tw.isPoint ? 0.125f : 2048.0f;
        }
        const float diff = t2 - t1;
        const float tmin = diff >= 0 ? t1 : t2;
        if (!(tmin < offset)) {
            num = node.children[0];
            continue;
        }
        const float tmax = t1 - t2 >= 0 ? t1 : t2;
        if (!(tmax > -offset)) {
            num = node.children[1];
            continue;
        }
        if (tr.fraction <= p1[3]) return;
        float frac2, frac;
        int side;
        if (std::fabs(diff) <= 4.76837158203125e-07f) { // DAT_005921dc
            frac2 = 0;
            frac = 1;
            side = 0;
        } else {
            const float t = diff >= 0 ? -t1 : t1;
            const float idist = 1.0f / std::fabs(diff);
            frac2 = (t - offset) * idist;
            frac = (t + offset) * idist;
            side = diff < 0 ? 0 : 1;
        }
        if (1.0f - frac < 0) frac = 1.0f;
        float mid[4];
        lerp4(mid, p1, p2, frac);
        traceThroughTree(tw, node.children[side], p1, mid, tr);
        if (frac2 < 0) frac2 = 0;
        lerp4(p1, p1, p2, frac2);
        num = node.children[side ^ 1];
    }
}

// FUN_00416c20
void CollisionWorld::Data::traceThroughLeaf(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const {
    if (tr.fraction == 0) return;
    if ((tw.contents & leaf.brushContents) != 0 && traceThroughLeafBrushes(tw, leaf, tr)) return;
    if ((tw.contents & leaf.terrainContents) == 0) return;
    for (int i = 0; i < leaf.collAabbCount; ++i) {
        if (tr.fraction == 0) return;
        traceThroughAabbTree(tw, aabbTrees[size_t(leaf.firstCollAabb + i)], tr);
    }
}

// FUN_00416b30: returns true when the trace ends up with fraction 0.
bool CollisionWorld::Data::traceThroughLeafBrushes(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const {
    Vec mins, maxs;
    for (size_t i = 0; i < 3; ++i) {
        mins[i] = leaf.mins[i] - tw.size[i];
        maxs[i] = leaf.maxs[i] + tw.size[i];
    }
    if (cullBox(tw, mins, maxs, tr.fraction)) return false;
    const float p1[4] = {tw.start[0], tw.start[1], tw.start[2], 0};
    const float p2[4] = {tw.end[0], tw.end[1], tw.end[2], tr.fraction};
    traceThroughLeafBrushNode(tw, leaf.leafBrushNode, p1, p2, tr);
    return tr.fraction == 0;
}

// FUN_00416820
void CollisionWorld::Data::traceThroughLeafBrushNode(const TraceWork& tw, int nodeIndex, const float* p1In, const float* p2, TraceResult& tr) const {
    float p1[4] = {p1In[0], p1In[1], p1In[2], p1In[3]};
    if ((tw.contents & leafBrushNodes[size_t(nodeIndex)].contents) == 0) return;
    for (;;) {
        const LeafBrushNode& node = leafBrushNodes[size_t(nodeIndex)];
        if (node.leafBrushCount != 0) {
            if (node.leafBrushCount > 0) {
                for (int i = 0; i < node.leafBrushCount; ++i) {
                    const Brush& b = brushes[leafBrushes[size_t(node.firstLeafBrush + i)]];
                    if (tw.contents & b.contents) traceThroughBrush(tw, b, tr);
                }
                return;
            }
            traceThroughLeafBrushNode(tw, nodeIndex + 1, p1, p2, tr);
        }
        const float t1 = p1[node.axis] - node.dist;
        const float t2 = p2[node.axis] - node.dist;
        const float offset = tw.size[node.axis] + kSurfaceClipEpsilon - node.range;
        const float tmax = t1 - t2 >= 0 ? t1 : t2;
        const float diff = t2 - t1;
        const float tmin = diff >= 0 ? t1 : t2;
        uint16_t next;
        if (tmin < offset) {
            if (tmax > -offset) {
                if (tr.fraction <= p1[3]) return;
                float frac2, frac;
                int side;
                if (std::fabs(diff) <= 4.76837158203125e-07f) {
                    frac2 = 0;
                    frac = 1;
                    side = 0;
                } else {
                    const float t = diff >= 0 ? -t1 : t1;
                    const float idist = 1.0f / std::fabs(diff);
                    frac2 = (t - offset) * idist;
                    frac = (t + offset) * idist;
                    side = diff < 0 ? 0 : 1;
                }
                if (1.0f - frac < 0) frac = 1.0f;
                float mid[4];
                lerp4(mid, p1, p2, frac);
                traceThroughLeafBrushNode(tw, nodeIndex + node.children[side], p1, mid, tr);
                if (frac2 < 0) frac2 = 0;
                lerp4(p1, p1, p2, frac2);
                next = node.children[side ^ 1];
            } else {
                next = node.children[1];
            }
        } else {
            if (!(tmax > -offset)) return;
            next = node.children[0];
        }
        nodeIndex += next;
        if ((tw.contents & leafBrushNodes[size_t(nodeIndex)].contents) == 0) return;
    }
}

// FUN_00416480
void CollisionWorld::Data::traceThroughBrush(const TraceWork& tw, const Brush& b, TraceResult& tr) const {
    float enterFrac = 0, leaveFrac = tr.fraction;
    bool endInside = true;
    // Clip side: an axial side (normal ±1 on one axis) or a non-axial brush side.
    bool hasClip = false;
    Vec clipNormal{};
    int clipMaterial = 0;

    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? -1.0f : 1.0f;
        const Vec& bound = side == 0 ? b.mins : b.maxs;
        for (size_t i = 0; i < 3; ++i) {
            const float d1 = (tw.start[i] - bound[i]) * s - tw.extents[i];
            const float d2 = (tw.end[i] - bound[i]) * s - tw.extents[i];
            if (d1 <= 0) {
                if (d2 > 0) {
                    const float f = d1 * tw.invDelta[i] * s;
                    if (f <= enterFrac) return;
                    endInside = false;
                    if (f - leaveFrac < 0) leaveFrac = f;
                }
            } else {
                if (std::min(d1, kSurfaceClipEpsilon) <= d2) return;
                const float f = (d1 - kSurfaceClipEpsilon) * tw.invDelta[i] * s;
                if (leaveFrac <= f) return;
                if (d2 > 0) endInside = false;
                if (enterFrac < f || !hasClip) {
                    if (enterFrac < f) enterFrac = f;
                    hasClip = true;
                    clipNormal = {0, 0, 0};
                    clipNormal[i] = s;
                    clipMaterial = b.axialMaterial[side][i];
                }
            }
        }
    }

    for (int k = 0; k < b.numSides; ++k) {
        const BrushSide& bs = sides[size_t(b.firstSide + k)];
        const Plane& p = planes[size_t(bs.plane)];
        const float dist = std::fabs(tw.offsetZ * p.normal[2]) + p.dist + tw.radius;
        const float d1 = dot(tw.start, p.normal) - dist;
        const float d2 = dot(tw.end, p.normal) - dist;
        if (d1 <= 0) {
            if (d2 > 0) {
                if (!(d1 <= leaveFrac * (d1 - d2))) {
                    leaveFrac = d1 / (d1 - d2);
                    if (leaveFrac <= enterFrac) return;
                }
                endInside = false;
            }
        } else {
            if (std::min(d1, kSurfaceClipEpsilon) <= d2) return;
            if (d2 > 0) endInside = false;
            if (d1 - kSurfaceClipEpsilon <= (d1 - d2) * enterFrac) {
                if (!hasClip) {
                    hasClip = true;
                    clipNormal = p.normal;
                    clipMaterial = bs.material;
                }
            } else {
                enterFrac = (d1 - kSurfaceClipEpsilon) / (d1 - d2);
                hasClip = true;
                clipNormal = p.normal;
                clipMaterial = bs.material;
                if (leaveFrac <= enterFrac) return;
            }
        }
    }

    tr.contents = b.contents;
    if (hasClip) {
        tr.fraction = enterFrac;
        tr.normal = clipNormal;
        setHitMaterial(tr, clipMaterial);
        return;
    }
    tr.startSolid = true;
    if (!endInside) return;
    tr.fraction = 0;
    tr.allSolid = true;
}

// FUN_004142d0
void CollisionWorld::Data::traceThroughAabbTree(const TraceWork& tw, const AabbTree& tree, TraceResult& tr) const {
    const Material& m = materials[tree.material];
    if ((m.contents & tw.contents) == 0) return;
    const float old = tr.fraction;
    traceThroughAabbTree_r(tw, tree, tr);
    if (tr.fraction < old) {
        setHitMaterial(tr, tree.material);
        tr.contents = m.contents;
    }
}

// FUN_00414040
void CollisionWorld::Data::traceThroughAabbTree_r(const TraceWork& tw, const AabbTree& tree, TraceResult& tr) const {
    if (cullTrace(tw, tree)) return;
    if (tree.childCount != 0) {
        for (int i = 0; i < tree.childCount; ++i) traceThroughAabbTree_r(tw, aabbTrees[size_t(tree.firstChild + i)], tr);
        return;
    }
    if (!markPartition(tw, tree.firstChild)) return;
    const Partition& part = partitions[size_t(tree.firstChild)];
    if (tw.isPoint) {
        for (int i = 0; i < part.triCount; ++i) tracePointThroughTriangle(tw, tris[size_t(part.firstTri + i)], tr);
        return;
    }
    for (int i = 0; i < part.triCount; ++i) {
        const Triangle& tri = tris[size_t(part.firstTri + i)];
        traceCapsuleThroughTriangle(tw, tri, tw.offsetZ, tr);
        if (tri.plane[2] < 0) traceCapsuleThroughTriangle(tw, tri, -tw.offsetZ, tr);
    }
    if ((tw.delta[0] != 0 || tw.delta[1] != 0) && tw.offsetZ != 0)
        for (int i = 0; i < part.borderCount; ++i) traceCapsuleThroughBorder(tw, borders[size_t(part.firstBorder + i)], tr);
}

// FUN_00412e40
void CollisionWorld::Data::tracePointThroughTriangle(const TraceWork& tw, const Triangle& tri, TraceResult& tr) const {
    const float* n = tri.plane;
    const float dEnd = tw.end[0] * n[0] + tw.end[2] * n[2] + tw.end[1] * n[1] - n[3];
    if (!(dEnd < 0)) return;
    const float dStart = tw.start[0] * n[0] + tw.start[1] * n[1] + tw.start[2] * n[2] - n[3];
    if (!(dStart > 0)) return;
    const float denom = dStart - dEnd;
    float frac = (dStart - kSurfaceClipEpsilon) / denom;
    if (frac < 0) frac = 0;
    if (!(frac < tr.fraction)) return;
    const float f = dStart / denom;
    const float x = f * tw.delta[0] + tw.start[0], y = f * tw.delta[1] + tw.start[1], z = f * tw.delta[2] + tw.start[2];
    const float s = x * tri.svec[0] + y * tri.svec[1] + z * tri.svec[2] - tri.svec[3];
    if (!(s >= -0.001f && s <= 1.001f)) return; // DAT_005920c8, DAT_005920c4
    const float t = x * tri.tvec[0] + y * tri.tvec[1] + z * tri.tvec[2] - tri.tvec[3];
    if (!(t >= -0.001f) || !(s + t <= 1.001f)) return;
    tr.fraction = frac;
    tr.normal = {n[0], n[1], n[2]};
}

// FUN_004133c0: sweeps the capsule end sphere at height -offsetZ against one triangle, its edges and vertices.
void CollisionWorld::Data::traceCapsuleThroughTriangle(const TraceWork& tw, const Triangle& tri, float offsetZ, TraceResult& tr) const {
    const float* n = tri.plane;
    const float endZ = tw.end[2] - offsetZ;
    const float reps = tw.radius + kSurfaceClipEpsilon;
    const float dEnd = tw.end[1] * n[1] + tw.end[0] * n[0] + endZ * n[2] - n[3];
    if (reps <= dEnd) return;
    const float sx = tw.start[0], sy = tw.start[1], sz = tw.start[2] - offsetZ;
    const float dStart = sy * n[1] + sx * n[0] + sz * n[2] - n[3];
    const float dd = dStart - dEnd;
    if (!(dd > 9.999999747378752e-05f)) return; // DAT_00592170

    if (dStart <= -reps) {
        // The start sphere is behind the plane: test the capsule segment against the triangle along z.
        float twoZ = offsetZ + offsetZ;
        const float dOther = twoZ * n[2] + dStart;
        if (dOther <= -reps) return;
        const float zIn = (-reps - dStart) * (1.0f / n[2]);
        const float s0 = sx * tri.svec[0] + sz * tri.svec[2] + sy * tri.svec[1] - tri.svec[3];
        const float t0 = sx * tri.tvec[0] + sz * tri.tvec[2] + sy * tri.tvec[1] - tri.tvec[3];
        const float s = zIn * tri.svec[2] + s0;
        float t = 0;
        if (s < 0 || (t = zIn * tri.tvec[2] + t0) < 0 || s + t > 1.0f) {
            if (reps <= dOther) twoZ = (reps - dStart) * (1.0f / n[2]);
            const float s2 = twoZ * tri.svec[2] + s0;
            if (s2 < 0) return;
            const float t2 = twoZ * tri.tvec[2] + t0;
            if (t2 < 0) return;
            if (1.0f < s2 + t2) return;
        }
        tr.normal = {n[0], n[1], n[2]};
        tr.fraction = 0;
        tr.startSolid = true;
        return;
    }

    float frac = 0, px = sx, py = sy, pz = sz;
    const float over = dStart - reps;
    if (over > 0) {
        frac = over / dd;
        if (tr.fraction < frac) return;
        pz = frac * tw.delta[2] + sz;
        px = frac * tw.delta[0] + sx;
        py = frac * tw.delta[1] + sy;
    }
    const float s = px * tri.svec[0] + pz * tri.svec[2] + py * tri.svec[1] - tri.svec[3];
    const float t = px * tri.tvec[0] + pz * tri.tvec[2] + py * tri.tvec[1] - tri.tvec[3];
    const unsigned outside = unsigned(1.0f < s + t) | unsigned(s < 0) << 1 | unsigned(t < 0) << 2;
    if (outside == 0) {
        tr.normal = {n[0], n[1], n[2]};
        tr.fraction = frac;
        if (dStart < tw.radius) tr.startSolid = true;
        return;
    }

    for (int k = 0; k < 3; ++k) {
        if ((outside & (1u << k)) == 0) {
            const int v = tri.verts[k];
            if (v < 0 || vertStamps[size_t(v)] == tw.checkcount) continue;
            vertStamps[size_t(v)] = tw.checkcount;
            const Vec& vp = verts[size_t(v)];
            const Vec dv{sx - vp[0], sy - vp[1], sz - vp[2]};
            const float b = dv[0] * tw.delta[0] + dv[1] * tw.delta[1] + dv[2] * tw.delta[2];
            if (!(b < 0)) continue;
            const float lenSq = dv[1] * dv[1] + dv[2] * dv[2] + dv[0] * dv[0];
            const float c = lenSq - reps * reps;
            if (c <= 0) {
                const float inv = 1.0f / std::sqrt(lenSq);
                tr.normal = {dv[0] * inv, dv[1] * inv, dv[2] * inv};
                keepFloorNormal(tr, tri, endZ, sz);
                tr.fraction = 0;
                if (lenSq < tw.radius * tw.radius) tr.startSolid = true;
                return;
            }
            const float disc = b * b - tw.deltaLenSq * c;
            if (!(disc >= 0)) continue;
            const float f = (-std::sqrt(disc) - b) / tw.deltaLenSq;
            if (!(f < tr.fraction)) continue;
            const float inv = 1.0f / reps;
            tr.normal = {(f * tw.delta[0] + dv[0]) * inv, (f * tw.delta[1] + dv[1]) * inv, (f * tw.delta[2] + dv[2]) * inv};
            keepFloorNormal(tr, tri, endZ, sz);
            tr.fraction = f;
        } else {
            const int e = tri.edges[k];
            if (e < 0 || edgeStamps[size_t(e)] == tw.checkcount) continue;
            edgeStamps[size_t(e)] = tw.checkcount;
            const Edge& edge = edges[size_t(e)];
            const Vec dv{sx - edge.origin[0], sy - edge.origin[1], sz - edge.origin[2]};
            const float a0 = dot(dv, edge.axis0), a1 = dot(dv, edge.axis1);
            const float da0 = edge.axis0[0] * tw.delta[0] + tw.delta[1] * edge.axis0[1] + tw.delta[2] * edge.axis0[2];
            const float da1 = tw.delta[0] * edge.axis1[0] + tw.delta[1] * edge.axis1[1] + tw.delta[2] * edge.axis1[2];
            const float b = da0 * a0 + da1 * a1;
            if (!(b < 0)) continue;
            const float lenSq = a0 * a0 + a1 * a1;
            const float c = lenSq - reps * reps;
            if (c > 0) {
                const float aa = da0 * da0 + da1 * da1;
                const float disc = b * b - aa * c;
                if (!(disc > 0)) continue;
                const float f = (-std::sqrt(disc) - b) / aa;
                if (!(f < tr.fraction)) continue;
                const float along = (f * tw.delta[0] + dv[0]) * edge.dir[0] + (f * tw.delta[1] + dv[1]) * edge.dir[1] + (f * tw.delta[2] + dv[2]) * edge.dir[2];
                if (!(std::fabs(along - 0.5f) <= 0.5f)) continue;
                const float inv = 1.0f / reps;
                const float k0 = (da0 * f + a0) * inv, k1 = (da1 * f + a1) * inv;
                tr.normal = {k0 * edge.axis0[0] + k1 * edge.axis1[0], k0 * edge.axis0[1] + k1 * edge.axis1[1], k0 * edge.axis0[2] + k1 * edge.axis1[2]};
                keepFloorNormal(tr, tri, endZ, sz);
                tr.fraction = f;
            } else {
                const float along = dv[0] * edge.dir[0] + dv[1] * edge.dir[1] + dv[2] * edge.dir[2];
                if (!(std::fabs(along - 0.5f) <= 0.5f)) continue;
                Vec nrm{a0 * edge.axis0[0] + a1 * edge.axis1[0], a0 * edge.axis0[1] + a1 * edge.axis1[1], a0 * edge.axis0[2] + a1 * edge.axis1[2]};
                const float len = std::sqrt(nrm[2] * nrm[2] + nrm[1] * nrm[1] + nrm[0] * nrm[0]); // FUN_004210d0
                if (len != 0) for (float& x : nrm) x *= 1.0f / len;
                tr.normal = nrm;
                keepFloorNormal(tr, tri, endZ, sz);
                tr.fraction = 0;
                if (lenSq < tw.radius * tw.radius) tr.startSolid = true;
                return;
            }
        }
    }
}

// FUN_00413cb0: sweeps the capsule's vertical side against a partition border (a vertical wall edge).
void CollisionWorld::Data::traceCapsuleThroughBorder(const TraceWork& tw, const Border& b, TraceResult& tr) const {
    const float nx = b.distEq[0], ny = b.distEq[1];
    const float dn = nx * tw.delta[0] + tw.delta[1] * ny;
    if (!(dn < 0)) return;
    const float reps = tw.radius + kSurfaceClipEpsilon;
    float f = (reps - (tw.start[1] * ny + tw.start[0] * nx - b.distEq[2])) / dn;
    if (!(f < tr.fraction) || !(-reps <= f * tw.deltaLen)) return;
    const float dz = f * tw.delta[2];
    const float sz = tw.start[2];
    float along = ((f * tw.delta[0] + tw.start[0]) * ny - (f * tw.delta[1] + tw.start[1]) * nx) - b.start;

    // Sweep against the vertical line at the border end `at` (0 = start, length = end).
    auto corner = [&](float at, float& hitAlong) -> bool {
        const float cx = tw.start[0] - (b.distEq[2] * nx + (at + b.start) * ny);
        const float cy = tw.start[1] - (b.distEq[2] * ny - (at + b.start) * nx);
        const float bb = cx * tw.delta[0] + cy * tw.delta[1];
        if (bb >= 0) return false;
        const float lenSq = cx * cx + cy * cy;
        const float c = lenSq - reps * reps;
        if (c < 0) {
            if (tw.offsetZ < std::fabs(tw.start[2] - (b.zSlope * at + b.zBase))) return false;
            tr.normal = {nx, ny, 0};
            tr.fraction = 0;
            if (tw.radius * tw.radius > lenSq) tr.startSolid = true;
            return false;
        }
        const float disc = bb * bb - c * tw.deltaLenSq;
        if (disc < 0) return false;
        f = (-bb - std::sqrt(disc)) / tw.deltaLenSq;
        if (tr.fraction <= f || f <= 0) return false;
        hitAlong = at;
        return true;
    };

    float zAt;
    if (along >= 0) {
        if (along <= b.length) {
            if (f < 0) f = 0;
            zAt = along;
        } else if (!corner(b.length, zAt)) {
            return;
        }
    } else if (!corner(0, zAt)) {
        return;
    }
    // The hit point's height (start z + f * delta z, using the f of the plane hit) must be within the
    // capsule's vertical segment of the border height at `zAt`.
    if (std::fabs((dz + sz) - (zAt * b.zSlope + b.zBase)) <= tw.offsetZ) {
        tr.fraction = f;
        tr.normal = {nx, ny, 0};
    }
}

// ---------------------------------------------------------------------------------------------- position test

// FUN_00416350
void CollisionWorld::Data::positionTest(const TraceWork& tw, TraceResult& tr) const {
    if (tr.allSolid) return;
    Vec mins, maxs;
    for (size_t i = 0; i < 3; ++i) {
        mins[i] = tw.start[i] - tw.size[i] - 1.0f;
        maxs[i] = tw.size[i] + tw.start[i] + 1.0f;
    }
    std::vector<int> list;
    boxLeafnums(mins, maxs, 0, list);
    for (int leafNum : list) {
        if (tr.allSolid) return;
        const Leaf& leaf = leafs[size_t(leafNum)];
        if ((tw.contents & leaf.brushContents) != 0 && testInLeafBrushes(tw, leaf, tr)) continue;
        if ((tw.contents & leaf.terrainContents) != 0) testInLeafTerrain(tw, leaf, tr);
    }
}

// FUN_004158e0 (leaf list capped at 1024)
void CollisionWorld::Data::boxLeafnums(const Vec& mins, const Vec& maxs, int nodeNum, std::vector<int>& list) const {
    while (nodeNum >= 0) {
        const Node& node = nodes[size_t(nodeNum)];
        const int s = boxOnPlaneSide(mins, maxs, planes[size_t(node.plane)]);
        if (s == 1) {
            nodeNum = node.children[0];
        } else {
            if (s != 2) boxLeafnums(mins, maxs, node.children[0], list);
            nodeNum = node.children[1];
        }
    }
    if (int(list.size()) < kMaxPositionLeafs) list.push_back(-1 - nodeNum);
}

// FUN_00416080: returns true when the test found the capsule all solid.
bool CollisionWorld::Data::testInLeafBrushes(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const {
    for (size_t i = 0; i < 3; ++i) {
        if (tw.boundsMaxs[i] <= leaf.mins[i]) return false;
        if (leaf.maxs[i] <= tw.boundsMins[i]) return false;
    }
    testInLeafBrushNode(tw, leaf.leafBrushNode, tr);
    return tr.allSolid;
}

// FUN_00415f80
void CollisionWorld::Data::testInLeafBrushNode(const TraceWork& tw, int nodeIndex, TraceResult& tr) const {
    if ((tw.contents & leafBrushNodes[size_t(nodeIndex)].contents) == 0) return;
    for (;;) {
        const LeafBrushNode& node = leafBrushNodes[size_t(nodeIndex)];
        if (node.leafBrushCount != 0) {
            if (node.leafBrushCount > 0) {
                for (int i = 0; i < node.leafBrushCount; ++i) {
                    const Brush& b = brushes[leafBrushes[size_t(node.firstLeafBrush + i)]];
                    if ((tw.contents & b.contents) == 0) continue;
                    testInBrush(tw, b, tr);
                    if (tr.allSolid) return;
                }
                return;
            }
            testInLeafBrushNode(tw, nodeIndex + 1, tr);
            if (tr.allSolid) return;
        }
        uint16_t next;
        if (tw.boundsMins[node.axis] <= node.dist) {
            if (node.dist <= tw.boundsMaxs[node.axis]) {
                testInLeafBrushNode(tw, nodeIndex + node.children[0], tr);
                if (tr.allSolid) return;
            }
            next = node.children[1];
        } else {
            next = node.children[0];
        }
        nodeIndex += next;
        if ((tw.contents & leafBrushNodes[size_t(nodeIndex)].contents) == 0) return;
    }
}

// FUN_00415ec0
void CollisionWorld::Data::testInBrush(const TraceWork& tw, const Brush& b, TraceResult& tr) const {
    for (size_t i = 0; i < 3; ++i)
        if (!(tw.boundsMins[i] <= b.maxs[i]) || !(b.mins[i] <= tw.boundsMaxs[i])) return;
    for (int k = 0; k < b.numSides; ++k) {
        const Plane& p = planes[size_t(sides[size_t(b.firstSide + k)].plane)];
        if (0 < dot(tw.start, p.normal) - (std::fabs(tw.offsetZ * p.normal[2]) + p.dist + tw.radius)) return;
    }
    tr.allSolid = true;
    tr.startSolid = true;
    tr.fraction = 0;
    tr.contents = b.contents;
}

// FUN_00414360
void CollisionWorld::Data::testInLeafTerrain(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const {
    for (int i = 0; i < leaf.collAabbCount; ++i) {
        const AabbTree& tree = aabbTrees[size_t(leaf.firstCollAabb + i)];
        const Material& m = materials[tree.material];
        if ((tw.contents & m.contents) == 0) continue;
        positionTestInAabbTree_r(tw, tree, tr);
        if (tr.allSolid) {
            setHitMaterial(tr, tree.material);
            tr.contents = m.contents;
            return;
        }
    }
}

// FUN_00414200
void CollisionWorld::Data::positionTestInAabbTree_r(const TraceWork& tw, const AabbTree& tree, TraceResult& tr) const {
    if (cullTrace(tw, tree)) return;
    if (tree.childCount != 0) {
        for (int i = 0; i < tree.childCount; ++i) positionTestInAabbTree_r(tw, aabbTrees[size_t(tree.firstChild + i)], tr);
        return;
    }
    if (!markPartition(tw, tree.firstChild)) return;
    const Partition& part = partitions[size_t(tree.firstChild)];
    for (int i = 0; i < part.triCount; ++i) testCapsuleInTriangle(tw, tris[size_t(part.firstTri + i)], tr);
}

// FUN_00412fa0
void CollisionWorld::Data::testCapsuleInTriangle(const TraceWork& tw, const Triangle& tri, TraceResult& tr) const {
    const float* n = tri.plane;
    float offsetZ = tw.offsetZ;
    if (n[2] < 0) offsetZ = -offsetZ;
    const float px = tw.start[0], py = tw.start[1], pz = tw.start[2] - offsetZ;
    const float r = tw.radius;
    const float d = px * n[0] + py * n[1] + pz * n[2] - n[3];
    if (!(d < r)) return;
    auto solid = [&] {
        tr.startSolid = true;
        tr.allSolid = true;
        tr.fraction = 0;
    };
    if (d > -r) {
        const float qx = -d * n[0] + px, qy = -d * n[1] + py, qz = -d * n[2] + pz;
        const float s = qx * tri.svec[0] + qy * tri.svec[1] + qz * tri.svec[2] - tri.svec[3];
        const float t = qx * tri.tvec[0] + qy * tri.tvec[1] + qz * tri.tvec[2] - tri.tvec[3];
        const unsigned outside = unsigned(1.0f < t + s) | unsigned(s < 0) << 1 | unsigned(t < 0) << 2;
        if (outside == 0) return solid();
        for (int k = 0; k < 3; ++k) {
            if ((outside & (1u << k)) == 0) {
                const int v = tri.verts[k];
                if (v < 0) continue;
                const Vec& vp = verts[size_t(v)];
                const float dx = px - vp[0], dy = py - vp[1], dz = pz - vp[2];
                if (dx * dx + dy * dy + dz * dz < r * r) return solid();
            } else {
                const int e = tri.edges[k];
                if (e < 0) continue;
                const Edge& edge = edges[size_t(e)];
                float dx = px - edge.origin[0], dy = py - edge.origin[1], dz = pz - edge.origin[2];
                const float along = dx * edge.dir[0] + dy * edge.dir[1] + dz * edge.dir[2];
                if (!(std::fabs(along - 0.5f) <= 0.5f)) continue;
                const float k2 = -(along / (edge.dir[2] * edge.dir[2] + edge.dir[1] * edge.dir[1] + edge.dir[0] * edge.dir[0]));
                dx = k2 * edge.dir[0] + dx;
                dy = k2 * edge.dir[1] + dy;
                dz = k2 * edge.dir[2] + dz;
                if (dx * dx + dy * dy + dz * dz < r * r) return solid();
            }
        }
        return;
    }
    // The sphere is behind the plane: test the capsule segment against the triangle along z.
    float twoZ = offsetZ + offsetZ;
    const float dOther = twoZ * n[2] + d;
    if (!(dOther > -r)) return;
    const float zIn = (-r - d) * (1.0f / n[2]);
    const float s0 = px * tri.svec[0] + pz * tri.svec[2] + py * tri.svec[1] - tri.svec[3];
    const float t0 = px * tri.tvec[0] + pz * tri.tvec[2] + py * tri.tvec[1] - tri.tvec[3];
    const float s = zIn * tri.svec[2] + s0;
    if (s >= 0) {
        const float t = zIn * tri.tvec[2] + t0;
        if (t >= 0 && s + t <= 1.0f) return solid();
    }
    if (r <= dOther) twoZ = (r - d) * (1.0f / n[2]);
    const float s2 = twoZ * tri.svec[2] + s0;
    if (!(s2 >= 0)) return;
    const float t2 = twoZ * tri.tvec[2] + t0;
    if (!(t2 >= 0)) return;
    if (1.0f < s2 + t2) return;
    solid();
}

// ---------------------------------------------------------------------------------------------- sight trace

// FUN_00418a70: the trace's BSP walk without fraction checks; the first blocking leaf ends it.
int CollisionWorld::Data::sightThroughTree(const TraceWork& tw, int num, const float* p1In, const float* p2, TraceResult& tr) const {
    float p1[4] = {p1In[0], p1In[1], p1In[2], 0};
    for (;;) {
        if (num < 0) return sightThroughLeaf(tw, leafs[size_t(-1 - num)], tr);
        const Node& node = nodes[size_t(num)];
        const Plane& plane = planes[size_t(node.plane)];
        float t1, t2, offset;
        if (plane.type < 3) {
            t1 = p1[plane.type] - plane.dist;
            t2 = p2[plane.type] - plane.dist;
            offset = tw.size[plane.type] + kSurfaceClipEpsilon;
        } else {
            t1 = p1[0] * plane.normal[0] + p1[1] * plane.normal[1] + p1[2] * plane.normal[2] - plane.dist;
            t2 = p2[0] * plane.normal[0] + p2[1] * plane.normal[1] + p2[2] * plane.normal[2] - plane.dist;
            offset = tw.isPoint ? 0.125f : 2048.0f;
        }
        const float diff = t2 - t1;
        const float tmin = diff >= 0 ? t1 : t2;
        if (!(tmin < offset)) {
            num = node.children[0];
            continue;
        }
        const float tmax = t1 - t2 >= 0 ? t1 : t2;
        if (!(tmax > -offset)) {
            num = node.children[1];
            continue;
        }
        float frac2, frac;
        int side;
        if (std::fabs(diff) <= 4.76837158203125e-07f) {
            frac2 = 0;
            frac = 1;
            side = 0;
        } else {
            const float t = diff >= 0 ? -t1 : t1;
            const float idist = 1.0f / std::fabs(diff);
            frac2 = (t - offset) * idist;
            frac = (t + offset) * idist;
            side = diff < 0 ? 0 : 1;
        }
        if (1.0f - frac < 0) frac = 1.0f;
        float mid[4];
        lerp4(mid, p1, p2, frac);
        if (const int hit = sightThroughTree(tw, node.children[side], p1, mid, tr)) return hit;
        if (frac2 < 0) frac2 = 0;
        lerp4(p1, p1, p2, frac2);
        num = node.children[side ^ 1];
    }
}

// FUN_00418500: brushes (FUN_00418470) give brush + 1, AABB trees numBrushes + 1 + tree.
int CollisionWorld::Data::sightThroughLeaf(const TraceWork& tw, const Leaf& leaf, TraceResult& tr) const {
    if ((tw.contents & leaf.brushContents) != 0) {
        Vec mins, maxs;
        for (size_t i = 0; i < 3; ++i) {
            mins[i] = leaf.mins[i] - tw.size[i];
            maxs[i] = leaf.maxs[i] + tw.size[i];
        }
        if (!cullBox(tw, mins, maxs, 1.0f)) {
            const float p1[4] = {tw.start[0], tw.start[1], tw.start[2], 0};
            const float p2[4] = {tw.end[0], tw.end[1], tw.end[2], 1};
            if (const int hit = sightThroughLeafBrushNode(tw, leaf.leafBrushNode, p1, p2)) return hit;
        }
    }
    if ((tw.contents & leaf.terrainContents) == 0) return 0;
    for (int i = 0; i < leaf.collAabbCount; ++i) {
        const int tree = leaf.firstCollAabb + i;
        if (tw.contents & materials[aabbTrees[size_t(tree)].material].contents) traceThroughAabbTree(tw, aabbTrees[size_t(tree)], tr);
        if (tr.fraction != 1.0f) return int(brushes.size()) + 1 + tree;
    }
    return 0;
}

// FUN_00418190: the leaf brush tree walk of the trace without fraction checks.
int CollisionWorld::Data::sightThroughLeafBrushNode(const TraceWork& tw, int nodeIndex, const float* p1In, const float* p2) const {
    float p1[4] = {p1In[0], p1In[1], p1In[2], 0};
    if ((tw.contents & leafBrushNodes[size_t(nodeIndex)].contents) == 0) return 0;
    for (;;) {
        const LeafBrushNode& node = leafBrushNodes[size_t(nodeIndex)];
        if (node.leafBrushCount != 0) {
            if (node.leafBrushCount > 0) {
                for (int i = 0; i < node.leafBrushCount; ++i) {
                    const uint16_t bi = leafBrushes[size_t(node.firstLeafBrush + i)];
                    if ((tw.contents & brushes[bi].contents) && sightThroughBrush(tw, brushes[bi])) return bi + 1;
                }
                return 0;
            }
            if (const int hit = sightThroughLeafBrushNode(tw, nodeIndex + 1, p1, p2)) return hit;
        }
        const float t1 = p1[node.axis] - node.dist;
        const float t2 = p2[node.axis] - node.dist;
        const float offset = tw.size[node.axis] + kSurfaceClipEpsilon - node.range;
        const float tmax = t1 - t2 >= 0 ? t1 : t2;
        const float diff = t2 - t1;
        const float tmin = diff >= 0 ? t1 : t2;
        uint16_t next;
        if (tmin < offset) {
            if (tmax > -offset) {
                float frac2, frac;
                int side;
                if (std::fabs(diff) <= 4.76837158203125e-07f) {
                    frac2 = 0;
                    frac = 1;
                    side = 0;
                } else {
                    const float t = diff >= 0 ? -t1 : t1;
                    const float idist = 1.0f / std::fabs(diff);
                    frac2 = (t - offset) * idist;
                    frac = (t + offset) * idist;
                    side = diff < 0 ? 0 : 1;
                }
                if (1.0f - frac < 0) frac = 1.0f;
                float mid[4];
                lerp4(mid, p1, p2, frac);
                if (const int hit = sightThroughLeafBrushNode(tw, nodeIndex + node.children[side], p1, mid)) return hit;
                if (frac2 < 0) frac2 = 0;
                lerp4(p1, p1, p2, frac2);
                next = node.children[side ^ 1];
            } else {
                next = node.children[1];
            }
        } else {
            if (!(tmax > -offset)) return 0;
            next = node.children[0];
        }
        nodeIndex += next;
        if ((tw.contents & leafBrushNodes[size_t(nodeIndex)].contents) == 0) return 0;
    }
}

// FUN_00417f60: the brush clip without epsilon; true when the segment enters the brush.
bool CollisionWorld::Data::sightThroughBrush(const TraceWork& tw, const Brush& b) const {
    float enterFrac = 0, leaveFrac = 1;
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? -1.0f : 1.0f;
        const Vec& bound = side == 0 ? b.mins : b.maxs;
        for (size_t i = 0; i < 3; ++i) {
            const float d1 = (tw.start[i] - bound[i]) * s - tw.extents[i];
            const float d2 = (tw.end[i] - bound[i]) * s - tw.extents[i];
            if (d1 <= 0) {
                if (d2 > 0) {
                    const float f = d1 * tw.invDelta[i] * s;
                    if (f <= enterFrac) return false;
                    if (f - leaveFrac < 0) leaveFrac = f;
                }
            } else {
                if (d2 > 0) return false;
                const float f = d1 * tw.invDelta[i] * s;
                if (leaveFrac <= f) return false;
                if (enterFrac - f < 0) enterFrac = f;
            }
        }
    }
    for (int k = 0; k < b.numSides; ++k) {
        const Plane& p = planes[size_t(sides[size_t(b.firstSide + k)].plane)];
        const float dist = std::fabs(p.normal[2] * tw.offsetZ) + p.dist + tw.radius;
        const float d1 = dot(tw.start, p.normal) - dist;
        const float d2 = dot(tw.end, p.normal) - dist;
        if (d1 <= 0) {
            if (d2 > 0 && leaveFrac * (d1 - d2) < d1) {
                leaveFrac = d1 / (d1 - d2);
                if (leaveFrac <= enterFrac) return false;
            }
        } else {
            if (d2 > 0) return false;
            if ((d1 - d2) * enterFrac < d1) {
                enterFrac = d1 / (d1 - d2);
                if (leaveFrac <= enterFrac) return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- box model 0x3ff

namespace {

// FUN_00421270: normalizes `v` into `out` and returns the length.
float normalizeTo(const Vec& v, Vec& out) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    out = v;
    if (l != 0) {
        const float il = 1.0f / l;
        out = {v[0] * il, v[1] * il, v[2] * il};
    }
    return l;
}

// The box model as a vertical capsule (FUN_00416120 / FUN_00417060 / FUN_004187e0).
struct BoxCapsule {
    Vec centre, top, bottom;
    float radius, halfHeight;
    uint32_t contents;
};

BoxCapsule boxCapsule(const Vec& mins, const Vec& maxs, uint32_t contents) {
    BoxCapsule b;
    for (size_t i = 0; i < 3; ++i) b.centre[i] = (maxs[i] + mins[i]) * 0.5f;
    const float hz = maxs[2] - b.centre[2];
    const float hx = maxs[0] - b.centre[0];
    b.radius = hx > hz ? hz : hx;
    b.halfHeight = hz - b.radius;
    b.top = {b.centre[0], b.centre[1], b.centre[2] + b.halfHeight};
    b.bottom = {b.centre[0], b.centre[1], b.centre[2] - b.halfHeight};
    b.contents = contents;
    return b;
}

bool boundsTouchBox(const TraceWork& tw, const Vec& mins, const Vec& maxs) {
    for (size_t i = 0; i < 3; ++i)
        if (!(tw.boundsMins[i] <= maxs[i] + 1.0f) || !(mins[i] - 1.0f <= tw.boundsMaxs[i])) return false;
    return true;
}

// FUN_00416cb0: sphere `a` (on the moving capsule) against sphere `p` of the box; true on a hit.
bool sphereHit(const TraceWork& tw, const Vec& a, const Vec& p, const Vec& endPoint, float boxRadius, uint32_t contents,
               TraceResult& tr) {
    const Vec d = {a[0] - p[0], a[1] - p[1], a[2] - p[2]};
    float r = boxRadius + tw.radius;
    r = r * r;
    const float c = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - r;
    if (c <= 0) {
        tr.fraction = 0;
        tr.startSolid = true;
        normalizeTo(d, tr.normal);
        tr.contents = contents;
        const float ex = endPoint[0] - p[0], ey = endPoint[1] - p[1], ez = endPoint[2] - p[2];
        if (ex * ex + ey * ey + ez * ez <= r) tr.allSolid = true;
        return true;
    }
    const float b = d[0] * tw.delta[0] + d[1] * tw.delta[1] + d[2] * tw.delta[2];
    if (!(b < 0)) return false;
    const float disc = b * b - tw.deltaLenSq * c;
    if (!(disc >= 0)) return false;
    Vec n;
    const float len = normalizeTo(d, n);
    const float t = (-b - std::sqrt(disc)) / tw.deltaLenSq + len * kSurfaceClipEpsilon / b;
    if (!(t < tr.fraction)) return false;
    tr.fraction = std::max(t, 0.0f);
    tr.normal = n;
    tr.contents = contents;
    return true;
}

// FUN_00416e70: the capsule's side against the box's vertical cylinder; true on a hit.
bool cylinderHit(const TraceWork& tw, const BoxCapsule& box, TraceResult& tr) {
    const float dx = tw.start[0] - box.centre[0], dy = tw.start[1] - box.centre[1], dz = tw.start[2] - box.centre[2];
    const float r = box.radius + tw.radius;
    const float c = dx * dx + dy * dy - r * r;
    const float limit = tw.offsetZ + box.halfHeight;
    if (c <= 0) {
        if (!(std::fabs(dz) <= limit)) return false;
        tr.fraction = 0;
        tr.startSolid = true;
        normalizeTo({dx, dy, 0}, tr.normal);
        tr.contents = box.contents;
        if (std::fabs(tw.end[2] - box.centre[2]) <= limit) tr.allSolid = true;
        return true;
    }
    const float b = dx * tw.delta[0] + dy * tw.delta[1];
    if (!(b < 0)) return false;
    const float a = tw.delta[1] * tw.delta[1] + tw.delta[0] * tw.delta[0];
    const float disc = b * b - a * c;
    if (!(disc >= 0)) return false;
    Vec n;
    const float eps = normalizeTo({dx, dy, 0}, n) * kSurfaceClipEpsilon / b;
    const float t = (-b - std::sqrt(disc)) / a + eps;
    if (!(t < tr.fraction)) return false;
    if (!(std::fabs((t - eps) * tw.delta[2] + tw.start[2] - box.centre[2]) <= limit)) return false;
    tr.fraction = std::max(t, 0.0f);
    tr.normal = n;
    tr.contents = box.contents;
    return true;
}

// FUN_00417060
void traceThroughBoxCapsule(const TraceWork& tw, const Vec& mins, const Vec& maxs, uint32_t contents, TraceResult& tr) {
    if (!boundsTouchBox(tw, mins, maxs)) return;
    const BoxCapsule box = boxCapsule(mins, maxs, contents);
    const float o = tw.offsetZ;
    const Vec sTop = {tw.start[0], tw.start[1], tw.start[2] + o}, sBot = {tw.start[0], tw.start[1], tw.start[2] - o};
    const Vec eTop = {tw.end[0], tw.end[1], tw.end[2] + o}, eBot = {tw.end[0], tw.end[1], tw.end[2] - o};
    if (sBot[2] > box.top[2]) {
        if (sphereHit(tw, sBot, box.top, eBot, box.radius, contents, tr)) return;
        if (!(tw.delta[2] < 0)) return;
    } else if (sTop[2] < box.bottom[2]) {
        if (sphereHit(tw, sTop, box.bottom, eTop, box.radius, contents, tr)) return;
        if (!(tw.delta[2] > 0)) return;
    }
    if (cylinderHit(tw, box, tr)) return;
    if (eBot[2] > box.top[2]) {
        if (sBot[2] <= box.top[2]) sphereHit(tw, sBot, box.top, eBot, box.radius, contents, tr);
    } else if (eTop[2] < box.bottom[2] && sTop[2] >= box.bottom[2]) {
        sphereHit(tw, sTop, box.bottom, eTop, box.radius, contents, tr);
    }
}

// FUN_00416120
void testInBoxCapsule(const TraceWork& tw, const Vec& mins, const Vec& maxs, TraceResult& tr) {
    const BoxCapsule box = boxCapsule(mins, maxs, 0);
    const float r = box.radius + tw.radius;
    const float r2 = r * r;
    auto solid = [&] {
        tr.allSolid = true;
        tr.startSolid = true;
        tr.fraction = 0;
    };
    const float dx = box.centre[0] - tw.start[0], dy = box.centre[1] - tw.start[1];
    const float zs[2] = {tw.start[2] + tw.offsetZ, tw.start[2] - tw.offsetZ};
    const float zb[2] = {box.top[2], box.bottom[2]};
    for (float b : zb)
        for (float s : zs) {
            const float dz = b - s;
            if (dx * dx + dy * dy + dz * dz < r2) solid();
        }
    if (std::fabs(tw.start[2] - box.centre[2]) <= box.halfHeight + tw.offsetZ && dx * dx + dy * dy < r2) solid();
}

// FUN_00418590: boolean sphere test of the sight trace (hit below fraction 1).
bool sightSphereHit(const TraceWork& tw, const Vec& a, const Vec& p, float boxRadius) {
    const Vec d = {a[0] - p[0], a[1] - p[1], a[2] - p[2]};
    const float r = boxRadius + tw.radius;
    const float c = d[1] * d[1] + d[0] * d[0] + d[2] * d[2] - r * r;
    if (c <= 0) return true;
    const float b = d[0] * tw.delta[0] + d[1] * tw.delta[1] + d[2] * tw.delta[2];
    if (!(b < 0)) return false;
    const float disc = b * b - tw.deltaLenSq * c;
    if (!(disc >= 0)) return false;
    Vec n;
    const float eps = b * kSurfaceClipEpsilon / normalizeTo(d, n);
    return (-b - std::sqrt(disc)) / tw.deltaLenSq + eps < 1.0f;
}

// FUN_004186a0: boolean cylinder test of the sight trace (uses the full |delta|² as the quadratic's a).
bool sightCylinderHit(const TraceWork& tw, const BoxCapsule& box) {
    const float dx = tw.start[0] - box.centre[0], dy = tw.start[1] - box.centre[1];
    float dz = tw.start[2] - box.centre[2];
    const float r = box.radius + tw.radius;
    const float c = dx * dx + dy * dy - r * r;
    if (!(c <= 0)) {
        const float b = dx * tw.delta[0] + dy * tw.delta[1];
        if (!(b < 0)) return false;
        const float disc = b * b - tw.deltaLenSq * c;
        if (!(disc >= 0)) return false;
        Vec n;
        const float eps = b * kSurfaceClipEpsilon / normalizeTo({dx, dy, 0}, n);
        const float t = (-b - std::sqrt(disc)) / tw.deltaLenSq + eps;
        if (!(t < 1.0f)) return false;
        dz = (t - eps) * tw.delta[2] + tw.start[2] - box.centre[2];
    }
    return std::fabs(dz) <= tw.offsetZ + box.halfHeight;
}

// FUN_004187e0
bool sightThroughBoxCapsule(const TraceWork& tw, const Vec& mins, const Vec& maxs) {
    if (!boundsTouchBox(tw, mins, maxs)) return false;
    const BoxCapsule box = boxCapsule(mins, maxs, 0);
    const float o = tw.offsetZ;
    const Vec sTop = {tw.start[0], tw.start[1], tw.start[2] + o}, sBot = {tw.start[0], tw.start[1], tw.start[2] - o};
    const float eTopZ = tw.end[2] + o, eBotZ = tw.end[2] - o;
    if (sBot[2] > box.top[2]) {
        if (sightSphereHit(tw, sBot, box.top, box.radius)) return true;
        if (!(tw.delta[2] < 0)) return false;
    } else if (sTop[2] < box.bottom[2]) {
        if (sightSphereHit(tw, sTop, box.bottom, box.radius)) return true;
        if (!(tw.delta[2] > 0)) return false;
    }
    if (sightCylinderHit(tw, box)) return true;
    if (eBotZ > box.top[2]) return sBot[2] <= box.top[2] && sightSphereHit(tw, sBot, box.top, box.radius);
    if (eTopZ < box.bottom[2] && sTop[2] >= box.bottom[2]) return sightSphereHit(tw, sTop, box.bottom, box.radius);
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------- entry

namespace {

// Trace setup shared by the world and model traces (FUN_00417640).
void setupTraceWork(TraceWork& tw, const Vec& start, const Vec& end, const Vec& size, uint32_t mask) {
    for (size_t i = 0; i < 3; ++i) {
        tw.size[i] = size[i];
        tw.start[i] = start[i];
        tw.end[i] = end[i];
        tw.midpoint[i] = (tw.end[i] + tw.start[i]) * 0.5f;
        tw.delta[i] = tw.end[i] - tw.start[i];
        tw.halfDelta[i] = tw.delta[i] * 0.5f;
        tw.halfDeltaAbs[i] = std::fabs(tw.halfDelta[i]);
        const float sd = tw.start[i] - tw.end[i]; // FUN_004193d0
        tw.invDelta[i] = sd != 0 ? 1.0f / sd : 0.0f;
    }
    tw.deltaLenSq = tw.delta[0] * tw.delta[0] + tw.delta[1] * tw.delta[1] + tw.delta[2] * tw.delta[2];
    tw.deltaLen = std::sqrt(tw.deltaLenSq);
    tw.radius = tw.size[0] <= tw.size[2] ? tw.size[0] : tw.size[2];
    tw.offsetZ = tw.size[2] - tw.radius;
    for (size_t i = 0; i < 3; ++i) {
        float lo = tw.start[i], hi = tw.end[i];
        if (hi < lo) std::swap(lo, hi);
        tw.boundsMins[i] = i == 2 ? (lo - tw.offsetZ) - tw.radius : lo - tw.radius;
        tw.boundsMaxs[i] = i == 2 ? hi + tw.offsetZ + tw.radius : hi + tw.radius;
    }
    tw.axialCullOnly = (tw.boundsMaxs[2] - tw.boundsMins[2]) * (tw.boundsMaxs[1] - tw.boundsMins[1]) * (tw.boundsMaxs[0] - tw.boundsMins[0]) <
                       tw.size[1] * tw.size[2] * tw.size[0] * tw.deltaLen * 16.0f;
    tw.contents = mask;
    tw.isPoint = tw.size[1] + tw.size[2] + tw.size[0] == 0;
    tw.extents = {tw.radius, tw.radius, tw.offsetZ + tw.radius};
}

void anglesToAxis(const Vec& a, Vec& fwd, Vec& right, Vec& up) {
    const float rad = 0.0174532924f;
    const float sy = std::sin(a[1] * rad), cy = std::cos(a[1] * rad);
    const float sp = std::sin(a[0] * rad), cp = std::cos(a[0] * rad);
    const float sr = std::sin(a[2] * rad), cr = std::cos(a[2] * rad);
    fwd = {cp * cy, cp * sy, -sp};
    right = {cr * sy - sr * sp * cy, -cr * cy - sr * sp * sy, -sr * cp};
    up = {sr * sy + cr * sp * cy, cr * sp * sy - sr * cy, cr * cp};
}

} // namespace

// FUN_00417640 for model 0.
TraceResult CollisionWorld::traceWorld(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const {
    TraceResult tr;
    tr.endPos = end;
    if (!d_) return tr;
    const Data& d = *d_;
    TraceWork tw{};
    Vec mid, size, s, e;
    for (size_t i = 0; i < 3; ++i) {
        mid[i] = (mins[i] + maxs[i]) * 0.5f;
        size[i] = maxs[i] - mid[i];
        s[i] = mid[i] + start[i];
        e[i] = mid[i] + end[i];
    }
    setupTraceWork(tw, s, e, size, mask);
    tw.checkcount = ++d.checkcount;
    if (start == end) {
        tw.isPoint = false;
        d.positionTest(tw, tr);
    } else {
        const float p1[4] = {tw.start[0], tw.start[1], tw.start[2], 0};
        const float p2[4] = {tw.end[0], tw.end[1], tw.end[2], tr.fraction};
        d.traceThroughTree(tw, 0, p1, p2, tr);
    }
    for (size_t i = 0; i < 3; ++i) tr.endPos[i] = start[i] + (end[i] - start[i]) * tr.fraction;
    return tr;
}

// FUN_00417c20: the box centre's path relative to the model origin, rotated into the model's axes (forward, left,
// up); FUN_00417640 with a submodel then tests or traces the model leaf; a hit normal is rotated back.
TraceResult CollisionWorld::traceModel(int model, const Vec& origin, const Vec& angles, const Vec& start, const Vec& end,
                                       const Vec& mins, const Vec& maxs, uint32_t mask) const {
    TraceResult tr;
    tr.endPos = end;
    if (!d_ || model <= 0 || size_t(model) >= d_->models.size()) return tr;
    const Data& d = *d_;
    const Leaf& leaf = d.models[size_t(model)].leaf;
    Vec mid, size, s, e;
    for (size_t i = 0; i < 3; ++i) {
        mid[i] = (mins[i] + maxs[i]) * 0.5f;
        size[i] = maxs[i] - mid[i];
        s[i] = mid[i] + start[i] - origin[i];
        e[i] = mid[i] + end[i] - origin[i];
    }
    const bool rotated = angles[0] != 0 || angles[1] != 0 || angles[2] != 0;
    Vec fwd{1, 0, 0}, left{0, 1, 0}, up{0, 0, 1};
    if (rotated) {
        Vec right;
        anglesToAxis(angles, fwd, right, up);
        left = {-right[0], -right[1], -right[2]};
        s = {dot(fwd, s), dot(left, s), dot(up, s)};
        e = {dot(fwd, e), dot(left, e), dot(up, e)};
    }
    TraceWork tw{};
    setupTraceWork(tw, s, e, size, mask);
    tw.checkcount = ++d.checkcount;
    if (start == end) {
        tw.isPoint = false;
        if ((mask & leaf.brushContents) == 0 || !d.testInLeafBrushes(tw, leaf, tr))
            if (!tr.allSolid && (mask & leaf.terrainContents) != 0) d.testInLeafTerrain(tw, leaf, tr);
    } else {
        d.traceThroughLeaf(tw, leaf, tr);
    }
    if (rotated && tr.fraction < 1.0f) {
        const Vec n = tr.normal;
        for (size_t i = 0; i < 3; ++i) tr.normal[i] = fwd[i] * n[0] + left[i] * n[1] + up[i] * n[2];
    }
    for (size_t i = 0; i < 3; ++i) tr.endPos[i] = start[i] + (end[i] - start[i]) * tr.fraction;
    return tr;
}

uint32_t CollisionWorld::modelContents(int model) const {
    if (!d_ || model <= 0 || size_t(model) >= d_->models.size()) return 0;
    const Leaf& l = d_->models[size_t(model)].leaf;
    return l.brushContents | l.terrainContents;
}

void CollisionWorld::setEntityClips(std::vector<EntityClip> clips) {
    if (d_) d_->entityClips = std::move(clips);
}

// FUN_00417c20 with zero angles → FUN_00417640 for model 0x3ff.
TraceResult CollisionWorld::traceBox(const Vec& origin, const Vec& boxMins, const Vec& boxMaxs, uint32_t boxContents, const Vec& start,
                                     const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask) const {
    TraceResult tr;
    tr.endPos = end;
    if ((boxContents & mask) == 0) return tr;
    Vec mid, size, s, e;
    for (size_t i = 0; i < 3; ++i) {
        mid[i] = (mins[i] + maxs[i]) * 0.5f;
        size[i] = maxs[i] - mid[i];
        s[i] = mid[i] + start[i] - origin[i];
        e[i] = mid[i] + end[i] - origin[i];
    }
    TraceWork tw{};
    setupTraceWork(tw, s, e, size, mask);
    if (start == end) testInBoxCapsule(tw, boxMins, boxMaxs, tr);
    else traceThroughBoxCapsule(tw, boxMins, boxMaxs, boxContents, tr);
    for (size_t i = 0; i < 3; ++i) tr.endPos[i] = start[i] + (end[i] - start[i]) * tr.fraction;
    return tr;
}

const std::vector<EntityClip>& CollisionWorld::entityClips() const {
    static const std::vector<EntityClip> none;
    return d_ ? d_->entityClips : none;
}

int CollisionWorld::ownerOf(int entity) const {
    if (entity < 0 || !d_) return -1;
    for (const EntityClip& c : d_->entityClips)
        if (c.id == entity) return c.owner;
    return -1;
}

// 0x44b840 / 0x44b6f0: contents, the pass entity, entities it owns and its owner.
bool CollisionWorld::skipClip(const EntityClip& c, uint32_t mask, int passEntity, int passOwner) {
    if ((c.contents & mask) == 0) return true;
    if (passEntity < 0) return false;
    return c.id == passEntity || c.owner == passEntity || (passOwner >= 0 && c.id == passOwner);
}

namespace {

// Absolute bounds of a clip (r.absmin / r.absmax): brush models use the model bounds (radius when rotated).
void clipAbsBounds(const EntityClip& c, const Vec& modelMins, const Vec& modelMaxs, float modelRadius, Vec& amin, Vec& amax) {
    const bool rotated = c.angles[0] != 0 || c.angles[1] != 0 || c.angles[2] != 0;
    for (size_t i = 0; i < 3; ++i) {
        if (c.model == 0) {
            amin[i] = c.origin[i] + c.mins[i];
            amax[i] = c.origin[i] + c.maxs[i];
        } else {
            amin[i] = c.origin[i] + (rotated ? -modelRadius : modelMins[i]);
            amax[i] = c.origin[i] + (rotated ? modelRadius : modelMaxs[i]);
        }
    }
}

} // namespace

// G_Trace (FUN_0044bf00): the world, then each entity clip whose contents match the mask and whose absolute bounds
// the swept box touches (0x41a590 / 0x44b840); the closest hit wins and names the entity.
TraceResult CollisionWorld::trace(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask, int passEntity,
                                  bool skipHitBoxes) const {
    TraceResult tr = traceWorld(start, end, mins, maxs, mask);
    if (!d_ || tr.fraction == 0.0f) return tr;
    const int passOwner = ownerOf(passEntity);
    for (const EntityClip& c : d_->entityClips) {
        if (skipClip(c, mask, passEntity, passOwner) || (skipHitBoxes && c.hitBoxes)) continue;
        if (c.model != 0 && (c.model < 0 || size_t(c.model) >= d_->models.size())) continue;
        Vec amin, amax;
        if (c.model) {
            const Data::Model& m = d_->models[size_t(c.model)];
            clipAbsBounds(c, m.mins, m.maxs, m.radius, amin, amax);
        } else {
            clipAbsBounds(c, {}, {}, 0, amin, amax);
        }
        // Swept box against the absolute bounds (FUN_00419450 against the entity's absmin/absmax).
        bool miss = false;
        for (size_t i = 0; i < 3 && !miss; ++i) {
            const float lo = std::min(start[i], start[i] + (end[i] - start[i]) * tr.fraction) + mins[i];
            const float hi = std::max(start[i], start[i] + (end[i] - start[i]) * tr.fraction) + maxs[i];
            miss = hi < amin[i] || lo > amax[i];
        }
        if (miss) continue;
        const float before = tr.fraction;
        TraceResult et = c.model ? traceModel(c.model, c.origin, c.angles, start, end, mins, maxs, mask)
                                 : traceBox(c.origin, c.mins, c.maxs, c.contents, start, end, mins, maxs, mask);
        if (et.fraction < before) { // the closer hit replaces the result (0x44b840)
            tr = et;
            tr.entity = c.id;
        }
    }
    return tr;
}

// FUN_0044c3c0: the world sight trace (FUN_00418d10, model 0), then every entity: brush models through their leaf,
// other entities as box model 0x3ff (FUN_0044bba0 / FUN_0044bc80 → FUN_004191a0).
bool CollisionWorld::sightBlocked(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs, uint32_t mask, int pass1,
                                  int pass2) const {
    if (!d_) return false;
    const Data& d = *d_;
    auto setup = [&](TraceWork& tw, const Vec& origin, const Vec& angles, Vec& fwd, Vec& left, Vec& up) {
        Vec mid, size, s, e;
        for (size_t i = 0; i < 3; ++i) {
            mid[i] = (mins[i] + maxs[i]) * 0.5f;
            size[i] = maxs[i] - mid[i];
            s[i] = mid[i] + start[i] - origin[i];
            e[i] = mid[i] + end[i] - origin[i];
        }
        if (angles[0] != 0 || angles[1] != 0 || angles[2] != 0) {
            Vec right;
            anglesToAxis(angles, fwd, right, up);
            left = {-right[0], -right[1], -right[2]};
            s = {dot(fwd, s), dot(left, s), dot(up, s)};
            e = {dot(fwd, e), dot(left, e), dot(up, e)};
        }
        setupTraceWork(tw, s, e, size, mask);
        tw.checkcount = ++d.checkcount;
    };
    Vec fwd, left, up;
    {
        TraceWork tw{};
        setup(tw, {0, 0, 0}, {0, 0, 0}, fwd, left, up);
        TraceResult tr;
        const float p1[4] = {tw.start[0], tw.start[1], tw.start[2], 0};
        const float p2[4] = {tw.end[0], tw.end[1], tw.end[2], 1};
        if (d.sightThroughTree(tw, 0, p1, p2, tr)) return true;
    }
    auto passed = [](const EntityClip& c, int pass) { return pass >= 0 && (c.id == pass || c.owner == pass); };
    for (const EntityClip& c : d.entityClips) {
        if ((c.contents & mask) == 0 || passed(c, pass1) || passed(c, pass2)) continue;
        TraceWork tw{};
        if (c.model == 0) {
            setup(tw, c.origin, {0, 0, 0}, fwd, left, up);
            if (sightThroughBoxCapsule(tw, c.mins, c.maxs)) return true;
            continue;
        }
        if (c.model < 0 || size_t(c.model) >= d.models.size()) continue;
        setup(tw, c.origin, c.angles, fwd, left, up);
        TraceResult tr;
        if (d.sightThroughLeaf(tw, d.models[size_t(c.model)].leaf, tr)) return true;
    }
    return false;
}

} // namespace iw2::game
