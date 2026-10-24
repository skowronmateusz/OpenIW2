// Impact marks: decals projected onto world surfaces (docs/spec/effects.md, "Marks").
#pragma once

#include "assets/d3dbsp.h"

#include <array>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

namespace iw2::fs {
class Vfs;
}

namespace iw2::game {

using MVec = std::array<float, 3>;

// Mark vertex: the world vertex layout (0x44 bytes) with the mark's colour and texture coordinates.
struct MarkVertex {
    MVec position{};
    MVec normal{};
    std::array<uint8_t, 4> rgba{};
    float uv[2]{};
    float lightmapUv[2]{};
};

// One fragment of a mark (mark pool entry, 0x288 bytes: origin +0xc, radius +0x18, material +0x1c, lightmap +0x20,
// vertex count +0x22, at most 9 vertices).
struct Mark {
    MVec origin{};
    float radius = 0;
    std::string material;
    int lightmap = 0;
    std::vector<MarkVertex> vertices;
    int64_t lastVisibleMs = 0; // +8: set when spawned and each time the mark is drawn (0x4ad710)
};

class MarkSystem {
public:
    // The world surfaces marks can land on; material flag bytes come from materials/<name>.
    void setWorld(const assets::D3dBsp& bsp, const fs::Vfs& vfs);
    // CG_ImpactMark (0x4ad430): a square of half size `radius` around `origin` facing `dir`, turned by
    // `rotationDeg`, coloured `rgba` (0..1).
    void impactMark(const std::string& material, const MVec& origin, const MVec& dir, float rotationDeg,
                    const std::array<float, 4>& rgba, float radius, int64_t nowMs);
    const std::list<Mark>& marks() const { return marks_; }
    // The draw (0x4ad710) stamps every drawn mark with the time.
    void markDrawn(int64_t nowMs);
    int limit = 1024; // cg_marksLimit (0x1040c48), default 1024
    bool enabled = true; // cg_marks (0xf48b40), default 1

private:
    struct Surface {
        size_t soup = 0;
        MVec mins{}, maxs{};
    };
    std::vector<assets::BspTriangleSoup> soups_;
    std::vector<assets::BspVertex> vertices_;
    std::vector<uint16_t> indices_;
    std::vector<Surface> surfaces_;
    std::list<Mark> marks_;
};

} // namespace iw2::game
