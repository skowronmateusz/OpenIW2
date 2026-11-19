// Our own lighting re-bake, the pure parts (src/game/rebake.h, docs/render-rebake.md).
#include "game/rebake.h"

#include "assets/d3dbsp.h"
#include "assets/material.h"

#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace iw2::game::rebake {

// ---- floats ------------------------------------------------------------------------------------------------------------------------

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
    uint32_t man = x & 0x7fffffu;
    if (((x >> 23) & 0xffu) == 0xffu) return uint16_t(sign | 0x7c00u | (man ? 0x200u : 0u)); // inf, nan
    if (exp >= 31) return uint16_t(sign | 0x7c00u);                                          // overflow
    if (exp <= 0) {                                                                          // subnormal or zero
        if (exp < -10) return uint16_t(sign);
        man |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t h = man >> shift;
        const uint32_t rem = man & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h; // round to nearest even; a carry into the exponent is correct
    return uint16_t(sign | h);
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
    uint32_t x;
    if (exp == 0) {
        if (man == 0) x = sign;
        else { // subnormal: normalise
            int e = -1;
            uint32_t m = man;
            do { ++e, m <<= 1; } while (!(m & 0x400u));
            x = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3ffu) << 13;
        }
    } else if (exp == 31) x = sign | 0x7f800000u | man << 13;
    else x = sign | (exp - 15 + 127) << 23 | man << 13;
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// ---- texel set-up ------------------------------------------------------------------------------------------------------------------

namespace {

struct P2 {
    double x, y;
};

double cross2(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

// Closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5), returned as barycentric weights.
void closestBary(P2 p, P2 a, P2 b, P2 c, double w[3]) {
    auto dot = [](P2 u, P2 v) { return u.x * v.x + u.y * v.y; };
    const P2 ab{b.x - a.x, b.y - a.y}, ac{c.x - a.x, c.y - a.y}, ap{p.x - a.x, p.y - a.y};
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return w[0] = 1, w[1] = 0, w[2] = 0, void();
    const P2 bp{p.x - b.x, p.y - b.y};
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return w[0] = 0, w[1] = 1, w[2] = 0, void();
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double v = d1 / (d1 - d3);
        return w[0] = 1 - v, w[1] = v, w[2] = 0, void();
    }
    const P2 cp{p.x - c.x, p.y - c.y};
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return w[0] = 0, w[1] = 0, w[2] = 1, void();
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double v = d2 / (d2 - d6);
        return w[0] = 1 - v, w[1] = 0, w[2] = v, void();
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return w[0] = 0, w[1] = 1 - v, w[2] = v, void();
    }
    const double den = 1.0 / (va + vb + vc), v = vb * den, u = vc * den;
    w[0] = 1 - v - u, w[1] = v, w[2] = u;
}

// Does the triangle overlap the axis aligned square [x, x + 1] x [y, y + 1]? (separating axes: the square's two, the triangle's three edges)
bool triangleTouchesSquare(const P2 v[3], double x, double y) {
    for (int a = 0; a < 2; ++a) {
        double lo = 1e300, hi = -1e300;
        for (int k = 0; k < 3; ++k) lo = std::min(lo, a ? v[k].y : v[k].x), hi = std::max(hi, a ? v[k].y : v[k].x);
        const double s = a ? y : x;
        if (hi < s || lo > s + 1) return false;
    }
    const double area = cross2(v[0], v[1], v[2]);
    for (int k = 0; k < 3; ++k) {
        const P2 a = v[k], b = v[(k + 1) % 3];
        P2 nrm{b.y - a.y, a.x - b.x}; // edge normal
        if (area < 0) nrm = {-nrm.x, -nrm.y};
        const double off = nrm.x * a.x + nrm.y * a.y;
        // the vertex of the square furthest along the outward normal
        const double cx = nrm.x > 0 ? x + 1 : x, cy = nrm.y > 0 ? y + 1 : y;
        if (nrm.x * cx + nrm.y * cy < off - 1e-12) return false;
    }
    return true;
}

template <size_t N>
void normalize3(float (&v)[N]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) v[0] /= l, v[1] /= l, v[2] /= l;
}

void interpolate(const LmTriangle& tri, const double w[3], Texel& t) {
    auto mix = [&](const float(LmVertex::*m)[3], float out[3]) {
        for (int a = 0; a < 3; ++a) out[a] = float(w[0] * (tri.v[0].*m)[a] + w[1] * (tri.v[1].*m)[a] + w[2] * (tri.v[2].*m)[a]);
    };
    mix(&LmVertex::pos, t.pos);
    mix(&LmVertex::normal, t.n);
    mix(&LmVertex::tangent, t.t);
    mix(&LmVertex::binormal, t.b);
    normalize3(t.n);
    // an orthonormal frame around the normal (the vertex frames are interpolated, then made orthogonal)
    float d = t.t[0] * t.n[0] + t.t[1] * t.n[1] + t.t[2] * t.n[2];
    for (int a = 0; a < 3; ++a) t.t[a] -= d * t.n[a];
    normalize3(t.t);
    d = t.b[0] * t.n[0] + t.b[1] * t.n[1] + t.b[2] * t.n[2];
    const float e = t.b[0] * t.t[0] + t.b[1] * t.t[1] + t.b[2] * t.t[2];
    for (int a = 0; a < 3; ++a) t.b[a] -= d * t.n[a] + e * t.t[a];
    normalize3(t.b);
}

bool sameSurface(const Texel& a, const Texel& b) {
    const float dx = a.pos[0] - b.pos[0], dy = a.pos[1] - b.pos[1], dz = a.pos[2] - b.pos[2];
    return dx * dx + dy * dy + dz * dz <= 1.0f && a.n[0] * b.n[0] + a.n[1] * b.n[1] + a.n[2] * b.n[2] >= 0.9f;
}

} // namespace

TexelSet rasterizeTexels(int res, const std::vector<LmTriangle>& tris) {
    TexelSet s;
    s.res = res;
    s.texels.assign(size_t(res) * size_t(res), Texel{});
    for (const LmTriangle& tri : tris) {
        P2 p[3];
        for (int k = 0; k < 3; ++k) p[k] = {double(tri.v[k].uv[0]) * res, double(tri.v[k].uv[1]) * res};
        const double area = cross2(p[0], p[1], p[2]);
        if (std::fabs(area) < 1e-9) continue;
        double lox = 1e300, loy = 1e300, hix = -1e300, hiy = -1e300;
        for (int k = 0; k < 3; ++k) lox = std::min(lox, p[k].x), hix = std::max(hix, p[k].x), loy = std::min(loy, p[k].y), hiy = std::max(hiy, p[k].y);
        const int x0 = std::max(int(std::floor(lox)), 0), x1 = std::min(int(std::floor(hix)), res - 1);
        const int y0 = std::max(int(std::floor(loy)), 0), y1 = std::min(int(std::floor(hiy)), res - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                Texel& dst = s.texels[size_t(y) * size_t(res) + size_t(x)];
                if (dst.state == TexelState::Shared) continue;
                const P2 c{x + 0.5, y + 0.5};
                double w[3] = {cross2(c, p[1], p[2]) / area, cross2(p[0], c, p[2]) / area, cross2(p[0], p[1], c) / area};
                const bool inside = w[0] >= -1e-6 && w[1] >= -1e-6 && w[2] >= -1e-6;
                if (!inside) {
                    if (dst.state != TexelState::Unset || !triangleTouchesSquare(p, x, y)) continue;
                    closestBary(c, p[0], p[1], p[2], w);
                }
                Texel t;
                interpolate(tri, w, t);
                if (inside) {
                    if (dst.state == TexelState::Inside && !sameSurface(dst, t)) {
                        dst = Texel{};
                        dst.state = TexelState::Shared;
                        continue;
                    }
                    if (dst.state == TexelState::Inside) continue; // the same surface twice (shared edge): keep the first
                    t.state = TexelState::Inside;
                } else t.state = TexelState::Edge;
                dst = t;
            }
    }
    // One texel of dilation: unset texels next to a traced one copy it (the four neighbours first).
    static const int kOff[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    std::vector<std::pair<size_t, int32_t>> add;
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            if (s.texels[size_t(y) * size_t(res) + size_t(x)].state != TexelState::Unset) continue;
            for (const auto& o : kOff) {
                const int nx = x + o[0], ny = y + o[1];
                if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
                const size_t ni = size_t(ny) * size_t(res) + size_t(nx);
                if (s.texels[ni].state == TexelState::Inside || s.texels[ni].state == TexelState::Edge) {
                    add.push_back({size_t(y) * size_t(res) + size_t(x), int32_t(ni)});
                    break;
                }
            }
        }
    for (const auto& [i, src] : add) {
        s.texels[i] = s.texels[size_t(src)];
        s.texels[i].state = TexelState::Dilated;
        s.texels[i].source = src;
    }
    for (const Texel& t : s.texels) {
        switch (t.state) {
            case TexelState::Inside: ++s.inside; break;
            case TexelState::Edge: ++s.edge; break;
            case TexelState::Dilated: ++s.dilated; break;
            case TexelState::Shared: ++s.shared; break;
            default: break;
        }
    }
    return s;
}

std::vector<std::vector<LmTriangle>> collectLightmapTriangles(const assets::D3dBsp& bsp) {
    std::vector<std::vector<LmTriangle>> out(bsp.lightmapCount());
    std::vector<uint8_t> moving(bsp.soups().size(), 0);
    const size_t models = bsp.lump(assets::BspLump::Models).size() / 48;
    for (size_t m = 1; m < models; ++m) {
        const auto [first, count] = bsp.modelSoups(int(m));
        for (size_t k = 0; k < count; ++k) moving[first + k] = 1;
    }
    for (size_t si = 0; si < bsp.soups().size(); ++si) {
        const auto& s = bsp.soups()[si];
        if (moving[si] || s.lightmap == assets::D3dBsp::kNoLightmap || s.lightmap >= out.size()) continue;
        for (uint32_t k = 0; k + 2 < s.indexCount; k += 3) {
            LmTriangle tri;
            for (int c = 0; c < 3; ++c) {
                const auto& v = bsp.vertices()[s.firstVertex + bsp.indices()[s.firstIndex + k + uint32_t(c)]];
                LmVertex& o = tri.v[c];
                std::memcpy(o.pos, v.position.data(), 12);
                std::memcpy(o.normal, v.normal.data(), 12);
                std::memcpy(o.tangent, v.tangent.data(), 12);
                std::memcpy(o.binormal, v.binormal.data(), 12);
                std::memcpy(o.uv, v.lightmapUv.data(), 8);
            }
            out[s.lightmap].push_back(tri);
        }
    }
    return out;
}


// ---- the CPU reference tracer --------------------------------------------------------------------------------------------------------

namespace {

// The same per-texel rotation as the kernels (an integer hash of the texel number).
float hash01(uint32_t k) {
    k = (k ^ 61u) ^ (k >> 16);
    k *= 9u;
    k = k ^ (k >> 4);
    k *= 0x27d4eb2du;
    k = k ^ (k >> 15);
    return float(k & 0xffffffu) / 16777216.0f;
}

} // namespace

// The albedo as the bounce uses it: gamma, then towards its luminance (the same as the kernel's).
void shapeAlbedo(float a[3], float gamma, float sat) {
    if (gamma != 1.0f)
        for (int c = 0; c < 3; ++c) a[c] = std::pow(std::max(a[c], 0.0f), gamma);
    const float l = luma709(a[0], a[1], a[2]);
    for (int c = 0; c < 3; ++c) a[c] = l + (a[c] - l) * sat;
}

namespace {

class CpuTracer final : public Tracer {
public:
    CpuTracer(TraceScene s, const Config& c) : sc_(std::move(s)), cfg_(c) {
        triRange_.assign(sc_.indices.size() / 3, -1);
        for (size_t r = 0; r < sc_.ranges.size(); ++r)
            for (uint32_t t = sc_.ranges[r].first / 3; t < (sc_.ranges[r].first + sc_.ranges[r].count) / 3 && t < triRange_.size(); ++t) triRange_[t] = int(r);
        if (sc_.ranges.empty()) triRange_.assign(triRange_.size(), -1); // all opaque
    }

    bool sun(const std::vector<TexelPoint>& pts, const float towardsSun[3], std::vector<float>& vis) override {
        vis.assign(pts.size(), 0.0f);
        const float c = std::tan(cfg_.sunConeDegrees * 3.14159265f / 180.0f), rays = float(std::max(cfg_.sunRays, 1));
        float sd[3] = {towardsSun[0], towardsSun[1], towardsSun[2]};
        normalize3(sd);
        const float a[3] = {0, 0, 1}, b[3] = {1, 0, 0};
        const float* up = std::fabs(sd[2]) < 0.9f ? a : b;
        float t1[3] = {sd[1] * up[2] - sd[2] * up[1], sd[2] * up[0] - sd[0] * up[2], sd[0] * up[1] - sd[1] * up[0]};
        normalize3(t1);
        const float t2[3] = {sd[1] * t1[2] - sd[2] * t1[1], sd[2] * t1[0] - sd[0] * t1[2], sd[0] * t1[1] - sd[1] * t1[0]};
        for (size_t i = 0; i < pts.size(); ++i) {
            const TexelPoint& p = pts[i];
            if (p.n[0] * sd[0] + p.n[1] * sd[1] + p.n[2] * sd[2] <= 0.0f) continue;
            const float rot = hash01(uint32_t(i)) * 6.2831853f;
            const float o[3] = {p.pos[0] + p.n[0] * kBias, p.pos[1] + p.n[1] * kBias, p.pos[2] + p.n[2] * kBias};
            int lit = 0;
            const int n = std::max(cfg_.sunRays, 1);
            for (int k = 0; k < n; ++k) {
                const float rr = std::sqrt((float(k) + 0.5f) / rays) * c, an = float(k) * 2.3999632f + rot;
                float d[3];
                for (int e = 0; e < 3; ++e) d[e] = sd[e] + (t1[e] * std::cos(an) + t2[e] * std::sin(an)) * rr;
                normalize3(d);
                lit += trace(o, d, kMaxDist, true, nullptr) ? 0 : 1;
            }
            vis[i] = float(lit) / rays;
        }
        return true;
    }

    bool sky(const std::vector<TexelPoint>& pts, std::vector<float>& v4) override {
        v4.assign(pts.size() * 4, 0.0f);
        const int n = std::max(cfg_.skyRays, 1);
        for (size_t i = 0; i < pts.size(); ++i) {
            const TexelPoint& p = pts[i];
            const float rot = hash01(uint32_t(i) * 2u + 1u) * 6.2831853f;
            const float o[3] = {p.pos[0] + p.n[0] * kBias, p.pos[1] + p.n[1] * kBias, p.pos[2] + p.n[2] * kBias};
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < n; ++k) {
                float l[3], d[3];
                hemisphere(p, k, n, rot, l, d);
                if (trace(o, d, kMaxDist, true, nullptr)) continue;
                project(l, 1.0f, acc);
            }
            for (int j = 0; j < 4; ++j) v4[i * 4 + size_t(j)] = acc[j] * 2.0f / float(n);
        }
        return true;
    }

    bool lamps(const std::vector<TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 12, 0.0f);
        const auto basis = lightmapBasis();
        const int rays = std::max(cfg_.lampRays, 1);
        for (size_t i = 0; i < pts.size(); ++i) {
            const TexelPoint& p = pts[i];
            const float o[3] = {p.pos[0] + p.n[0] * kBias, p.pos[1] + p.n[1] * kBias, p.pos[2] + p.n[2] * kBias};
            for (size_t li = 0; li < sc_.lamps.size(); ++li) {
                const MapLight& l = sc_.lamps[li];
                float v[3] = {l.pos[0] - p.pos[0], l.pos[1] - p.pos[1], l.pos[2] - p.pos[2]};
                const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (d >= l.radius || d < 1.0f) continue;
                const float f = mapLightFalloff(l, p.pos);
                float dir[3] = {v[0] / d, v[1] / d, v[2] / d};
                if (dir[0] * p.n[0] + dir[1] * p.n[1] + dir[2] * p.n[2] <= 0.0f || f <= 0.0f) continue;
                float vis = 0;
                for (int r = 0; r < rays; ++r) {
                    const float j[3] = {hash01(uint32_t(i * 977 + li * 131 + size_t(r) * 7 + 1)) * 2 - 1, hash01(uint32_t(i * 977 + li * 131 + size_t(r) * 7 + 2)) * 2 - 1,
                                        hash01(uint32_t(i * 977 + li * 131 + size_t(r) * 7 + 3)) * 2 - 1};
                    float to[3] = {l.pos[0] + j[0] * cfg_.lampSize - o[0], l.pos[1] + j[1] * cfg_.lampSize - o[1], l.pos[2] + j[2] * cfg_.lampSize - o[2]};
                    const float len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
                    for (float& e : to) e /= len;
                    vis += trace(o, to, len - 1.0f, true, nullptr) ? 0.0f : 1.0f;
                }
                vis /= float(rays);
                for (int k = 0; k < 4; ++k) {
                    const auto& bk = basis[size_t(k)];
                    float nk[3];
                    for (int e = 0; e < 3; ++e) nk[e] = p.t[e] * bk[0] + p.b[e] * bk[1] + p.n[e] * bk[2];
                    const float c = std::max(0.0f, nk[0] * dir[0] + nk[1] * dir[1] + nk[2] * dir[2]);
                    for (int ch = 0; ch < 3; ++ch) out[i * 12 + size_t(ch * 4 + k)] += l.color[ch] * f * vis * c;
                }
            }
        }
        return true;
    }

    bool gridSky(const std::vector<TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 8, 0.0f);
        const auto g = gridBasis();
        const int n = std::max(cfg_.skyRays, 1);
        for (size_t i = 0; i < pts.size(); ++i) {
            const float rot = hash01(uint32_t(i) * 2u + 11u) * 6.2831853f;
            float acc[8] = {};
            for (int k = 0; k < n; ++k) {
                float d[3];
                sphere(k, n, rot, d);
                if (d[2] <= 0.0f || trace(pts[i].pos, d, kMaxDist, true, nullptr)) continue; // the sky is a dome: nothing below the horizon
                for (int j = 0; j < 8; ++j) acc[j] += std::max(0.0f, d[0] * g[size_t(j)][0] + d[1] * g[size_t(j)][1] + d[2] * g[size_t(j)][2]);
            }
            for (int j = 0; j < 8; ++j) out[i * 8 + size_t(j)] = acc[j] * 4.0f / float(n);
        }
        return true;
    }

    bool gridLamps(const std::vector<TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 24, 0.0f);
        const auto g = gridBasis();
        const int rays = std::max(cfg_.lampRays, 1);
        for (size_t i = 0; i < pts.size(); ++i) {
            const TexelPoint& p = pts[i];
            for (size_t li = 0; li < sc_.lamps.size(); ++li) {
                const MapLight& l = sc_.lamps[li];
                const float v[3] = {l.pos[0] - p.pos[0], l.pos[1] - p.pos[1], l.pos[2] - p.pos[2]};
                const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (d >= l.radius || d < 1.0f) continue;
                const float f = mapLightFalloff(l, p.pos);
                if (f <= 0.0f) continue;
                const float dir[3] = {v[0] / d, v[1] / d, v[2] / d};
                float vis = 0;
                for (int r = 0; r < rays; ++r) {
                    const uint32_t h = uint32_t(i * 977 + li * 131 + size_t(r) * 7);
                    float to[3] = {l.pos[0] + (hash01(h + 1u) * 2 - 1) * cfg_.lampSize - p.pos[0], l.pos[1] + (hash01(h + 2u) * 2 - 1) * cfg_.lampSize - p.pos[1],
                                   l.pos[2] + (hash01(h + 3u) * 2 - 1) * cfg_.lampSize - p.pos[2]};
                    const float len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
                    for (float& e : to) e /= len;
                    vis += trace(p.pos, to, len - 1.0f, true, nullptr) ? 0.0f : 1.0f;
                }
                vis /= float(rays);
                for (int k = 0; k < 8; ++k) {
                    const float w = f * vis * std::max(0.0f, dir[0] * g[size_t(k)][0] + dir[1] * g[size_t(k)][1] + dir[2] * g[size_t(k)][2]);
                    for (int c = 0; c < 3; ++c) out[i * 24 + size_t(c * 8 + k)] += l.color[c] * w;
                }
            }
        }
        return true;
    }

    bool gridBounce(const std::vector<TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 24, 0.0f);
        const auto g = gridBasis();
        const int n = std::max(cfg_.bounceRays, 1);
        for (size_t i = 0; i < pts.size(); ++i) {
            const float rot = hash01(uint32_t(i) * 2u + 13u) * 6.2831853f;
            float acc[24] = {};
            for (int k = 0; k < n; ++k) {
                float d[3];
                sphere(k, n, rot, d);
                Hit h;
                if (!trace(pts[i].pos, d, kMaxDist, false, &h)) continue;
                float rad[3];
                radianceAt(h, rad);
                for (int j = 0; j < 8; ++j) {
                    const float w = std::max(0.0f, d[0] * g[size_t(j)][0] + d[1] * g[size_t(j)][1] + d[2] * g[size_t(j)][2]);
                    for (int c = 0; c < 3; ++c) acc[c * 8 + j] += rad[c] * w;
                }
            }
            for (int j = 0; j < 24; ++j) out[i * 24 + size_t(j)] = acc[j] * 4.0f / float(n);
        }
        return true;
    }

    void setCache(const std::vector<float>& c) override { cache_ = c; }

    bool bounce(const std::vector<TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 12, 0.0f);
        const int n = std::max(cfg_.bounceRays, 1);
        for (size_t i = 0; i < pts.size(); ++i) {
            const TexelPoint& p = pts[i];
            const float rot = hash01(uint32_t(i) * 2u + 7u) * 6.2831853f;
            const float o[3] = {p.pos[0] + p.n[0] * kBias, p.pos[1] + p.n[1] * kBias, p.pos[2] + p.n[2] * kBias};
            float acc[3][4] = {};
            for (int k = 0; k < n; ++k) {
                float l[3], d[3];
                hemisphere(p, k, n, rot, l, d);
                Hit h;
                if (!trace(o, d, kMaxDist, false, &h)) continue;
                const SceneTri& t = sc_.tris[h.tri];
                float e[3] = {t.light[0], t.light[1], t.light[2]};
                if (t.lm >= 0) {
                    const float u = (1 - h.u - h.v) * t.uv[0] + h.u * t.uv[2] + h.v * t.uv[4], v = (1 - h.u - h.v) * t.uv[1] + h.u * t.uv[3] + h.v * t.uv[5];
                    const int x = std::clamp(int(u * kCoeffRes), 0, kCoeffRes - 1), y = std::clamp(int(v * kCoeffRes), 0, kCoeffRes - 1);
                    const size_t at = ((size_t(t.lm) * kCoeffRes + size_t(y)) * kCoeffRes + size_t(x)) * 4;
                    for (int c = 0; c < 3; ++c) e[c] = at + 2 < cache_.size() ? cache_[at + size_t(c)] : 0.0f;
                }
                float alb[3] = {0.5f, 0.5f, 0.5f};
                if (t.tex >= 0 && size_t(t.tex) * 3 + 2 < sc_.albedo.size())
                    for (int c = 0; c < 3; ++c) alb[c] = sc_.albedo[size_t(t.tex) * 3 + size_t(c)];
                shapeAlbedo(alb, cfg_.albedoGamma, cfg_.albedoSat);
                for (int c = 0; c < 3; ++c) {
                    float a4[4] = {0, 0, 0, 0};
                    project(l, alb[c] * e[c], a4);
                    for (int j = 0; j < 4; ++j) acc[c][j] += a4[size_t(j)];
                }
            }
            for (int c = 0; c < 3; ++c)
                for (int j = 0; j < 4; ++j) out[i * 12 + size_t(c * 4 + j)] = acc[c][j] * 2.0f / float(n);
        }
        return true;
    }

private:
    static constexpr float kBias = 1.0f, kMaxDist = 20000.0f;
    struct Hit {
        size_t tri = 0;
        float t = 0, u = 0, v = 0;
    };
    // The k-th of n uniform directions over the sphere (equal steps in height).
    static void sphere(int k, int n, float rot, float d[3]) {
        const float z = 1.0f - 2.0f * (float(k) + 0.5f) / float(n), r = std::sqrt(std::max(0.0f, 1.0f - z * z)), an = float(k) * 2.3999632f + rot;
        d[0] = r * std::cos(an), d[1] = r * std::sin(an), d[2] = z;
    }
    // albedo * irradiance at a hit (the cache at its lightmap coordinates, or the triangle's own light)
    void radianceAt(const Hit& h, float rad[3]) const {
        const SceneTri& t = sc_.tris[h.tri];
        float e[3] = {t.light[0], t.light[1], t.light[2]};
        if (t.lm >= 0) {
            const float u = (1 - h.u - h.v) * t.uv[0] + h.u * t.uv[2] + h.v * t.uv[4], v = (1 - h.u - h.v) * t.uv[1] + h.u * t.uv[3] + h.v * t.uv[5];
            const int x = std::clamp(int(u * kCoeffRes), 0, kCoeffRes - 1), y = std::clamp(int(v * kCoeffRes), 0, kCoeffRes - 1);
            const size_t at = ((size_t(t.lm) * kCoeffRes + size_t(y)) * kCoeffRes + size_t(x)) * 4;
            for (int c = 0; c < 3; ++c) e[c] = at + 2 < cache_.size() ? cache_[at + size_t(c)] : 0.0f;
        }
        float alb[3] = {0.5f, 0.5f, 0.5f};
        if (t.tex >= 0 && size_t(t.tex) * 3 + 2 < sc_.albedo.size())
            for (int c = 0; c < 3; ++c) alb[c] = sc_.albedo[size_t(t.tex) * 3 + size_t(c)];
        shapeAlbedo(alb, cfg_.albedoGamma, cfg_.albedoSat);
        for (int c = 0; c < 3; ++c) rad[c] = alb[c] * e[c];
    }
    // The k-th hemisphere sample of a point: local direction l (tangent frame) and world direction d (uniform, equal steps in height).
    static void hemisphere(const TexelPoint& p, int k, int n, float rot, float l[3], float d[3]) {
        const float z = (float(k) + 0.5f) / float(n), rxy = std::sqrt(std::max(0.0f, 1.0f - z * z)), an = float(k) * 2.3999632f + rot;
        l[0] = rxy * std::cos(an), l[1] = rxy * std::sin(an), l[2] = z;
        for (int e = 0; e < 3; ++e) d[e] = p.t[e] * l[0] + p.b[e] * l[1] + p.n[e] * l[2];
    }
    static void project(const float l[3], float w, float acc[4]) {
        static const auto basis = lightmapBasis();
        for (int j = 0; j < 4; ++j)
            acc[j] += w * std::max(0.0f, l[0] * basis[size_t(j)][0] + l[1] * basis[size_t(j)][1] + l[2] * basis[size_t(j)][2]);
    }
    bool alphaPass(size_t tri, float u, float v) const {
        const int r = triRange_[tri];
        if (r < 0 || sc_.ranges[size_t(r)].layer < 0) return true;
        const GeomRange& g = sc_.ranges[size_t(r)];
        if (size_t(g.layer) >= sc_.masks.size()) return true;
        const uint32_t* ix = &sc_.indices[tri * 3];
        float uv[2];
        for (int e = 0; e < 2; ++e)
            uv[e] = (1 - u - v) * sc_.vertexUv[size_t(ix[0]) * 2 + size_t(e)] + u * sc_.vertexUv[size_t(ix[1]) * 2 + size_t(e)] + v * sc_.vertexUv[size_t(ix[2]) * 2 + size_t(e)];
        const int x = int(std::floor((uv[0] - std::floor(uv[0])) * 64.0f)) & 63, y = int(std::floor((uv[1] - std::floor(uv[1])) * 64.0f)) & 63;
        const int a = sc_.masks[size_t(g.layer)][size_t(y) * 64 + size_t(x)];
        return g.func == 1 ? a >= 128 : g.func == 2 ? a > 0 : g.func == 3 ? a < 128 : true;
    }
    // Brute force Moller-Trumbore with the alpha tests; anyHit: stop at the first hit, else the closest in `h`.
    bool trace(const float o[3], const float d[3], float maxT, bool anyHit, Hit* h) const {
        float best = maxT;
        bool found = false;
        for (size_t t = 0; t < sc_.indices.size() / 3; ++t) {
            const float* a = &sc_.positions[size_t(sc_.indices[t * 3]) * 3];
            const float* b = &sc_.positions[size_t(sc_.indices[t * 3 + 1]) * 3];
            const float* c = &sc_.positions[size_t(sc_.indices[t * 3 + 2]) * 3];
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::fabs(det) < 1e-12f) continue;
            const float inv = 1.0f / det, tv[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
            const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
            const float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
            if (tt < 0.0f || tt > best) continue;
            if (!alphaPass(t, u, v)) continue;
            if (anyHit) return true;
            best = tt, found = true;
            if (h) h->tri = t, h->t = tt, h->u = u, h->v = v;
        }
        return found;
    }
    TraceScene sc_;
    Config cfg_;
    std::vector<int> triRange_;
    std::vector<float> cache_;
};

} // namespace

std::unique_ptr<Tracer> makeCpuTracer(TraceScene scene, const Config& cfg) { return std::make_unique<CpuTracer>(std::move(scene), cfg); }

// ---- calibration -------------------------------------------------------------------------------------------------------------------

void RatioHistogram::add(float ratio) {
    if (!(ratio > 0.0f) || !std::isfinite(ratio)) return;
    // bin = log2(ratio) in [-12, 12), 25 bins per octave
    const int b = std::clamp(int(std::floor((std::log2(ratio) + 12.0f) * 25.0f)), 0, kBins - 1);
    ++bins_[size_t(b)];
    ++n_;
}

float RatioHistogram::median() const {
    if (!n_) return 0.0f;
    uint64_t seen = 0;
    for (int b = 0; b < kBins; ++b) {
        seen += bins_[size_t(b)];
        if (seen * 2 >= n_) return std::exp2((float(b) + 0.5f) / 25.0f - 12.0f);
    }
    return 1.0f;
}

// ---- the bake ----------------------------------------------------------------------------------------------------------------------

namespace {

std::vector<TexelPoint> tracedPoints(const TexelSet& s, std::vector<uint32_t>& index) {
    std::vector<TexelPoint> pts;
    index.clear();
    for (size_t i = 0; i < s.texels.size(); ++i) {
        const Texel& t = s.texels[i];
        if (t.state != TexelState::Inside && t.state != TexelState::Edge) continue;
        TexelPoint p;
        std::memcpy(p.pos, t.pos, 12);
        std::memcpy(p.n, t.n, 12);
        std::memcpy(p.t, t.t, 12);
        std::memcpy(p.b, t.b, 12);
        pts.push_back(p);
        index.push_back(uint32_t(i));
    }
    return pts;
}

// What is kept of one lightmap between the passes (the gains need all lightmaps; the bounces need the cache of all of them).
struct Pending {
    size_t lm = 0;
    std::vector<TexelPoint> pts;     // the traced texels (Inside, Edge) of the coefficient grid
    std::vector<uint32_t> index;     // their texel numbers
    std::vector<uint8_t> state;      // TexelState per texel
    std::vector<int32_t> source;     // Dilated: the texel it copies
    std::vector<float> sky, lamp, bounce, direct; // per traced texel 4, 12, 12 and 3 values
    std::vector<float> sun;          // per traced texel
    std::vector<uint8_t> kind;       // per traced texel: bit 0 Inside (the others are Edge)
};

float lumaOf(const float v[3]) { return luma709(v[0], v[1], v[2]); }

} // namespace

std::optional<BakeResult> bakeMap(Tracer& tracer, const std::vector<std::vector<LmTriangle>>& tris, const std::vector<ShippedLightmap>& shipped,
                                  const float towardsSun[3], const float sunColor[3], const std::vector<MapLight>& lights, const Config& cfg) {
    const auto t0 = std::chrono::steady_clock::now();
    (void)lights;
    BakeResult out;
    out.lightmaps.resize(tris.size());
    out.stats.lightmaps = tris.size();
    std::vector<Pending> pending;
    const size_t cn = size_t(kCoeffRes) * size_t(kCoeffRes), sn = size_t(kSunRes) * size_t(kSunRes);
    std::vector<uint32_t> index;
    auto shipOf = [&](size_t lm, size_t texel, float v[3]) {
        const ShippedLightmap sh = lm < shipped.size() ? shipped[lm] : ShippedLightmap{};
        for (int c = 0; c < 3; ++c) v[c] = sh.coeff[c] ? float(sh.coeff[c][texel * 4]) / 255.0f : 0.0f; // byte 0 = c0
        return sh.coeff[0] && sh.coeff[1] && sh.coeff[2];
    };
    // ---- phase 1: the sun mask on the 1024 grid, then the direct light of the coefficient grid
    for (size_t li = 0; li < tris.size(); ++li) {
        if (tris[li].empty()) continue;
        const ShippedLightmap sh = li < shipped.size() ? shipped[li] : ShippedLightmap{};
        LightmapResult& res = out.lightmaps[li];
        {
            const TexelSet s = rasterizeTexels(kSunRes, tris[li]);
            std::vector<TexelPoint> pts = tracedPoints(s, index);
            std::vector<float> vis;
            if (!pts.empty() && !tracer.sun(pts, towardsSun, vis)) return std::nullopt;
            if (vis.size() != pts.size() && !pts.empty()) return std::nullopt;
            res.sun.assign(sn, 0);
            if (sh.sun) std::memcpy(res.sun.data(), sh.sun, sn);
            for (size_t k = 0; k < index.size(); ++k) {
                const uint8_t b = uint8_t(std::lround(std::clamp(vis[k], 0.0f, 1.0f) * 255.0f));
                if (sh.sun) {
                    const Texel& t = s.texels[index[k]];
                    out.stats.sunAll = maskOverlap(sh.sun[index[k]], b, cfg.maskThreshold, out.stats.sunAll);
                    if (t.state == TexelState::Inside && t.n[0] * towardsSun[0] + t.n[1] * towardsSun[1] + t.n[2] * towardsSun[2] > cfg.facingMin)
                        out.stats.sunFacing = maskOverlap(sh.sun[index[k]], b, cfg.maskThreshold, out.stats.sunFacing);
                }
                res.sun[index[k]] = b;
            }
            for (size_t i = 0; i < s.texels.size(); ++i)
                if (s.texels[i].state == TexelState::Dilated) res.sun[i] = res.sun[size_t(s.texels[i].source)];
            res.inside = s.inside, res.edge = s.edge, res.dilated = s.dilated, res.shared = s.shared;
            out.stats.sunTexels += s.inside + s.edge;
            out.stats.sharedTexels += s.shared;
        }
        const TexelSet s = rasterizeTexels(kCoeffRes, tris[li]);
        Pending pd;
        pd.lm = li;
        pd.pts = tracedPoints(s, pd.index);
        pd.state.resize(cn);
        pd.source.assign(cn, -1);
        for (size_t i = 0; i < cn; ++i) pd.state[i] = uint8_t(s.texels[i].state), pd.source[i] = s.texels[i].source;
        pd.kind.resize(pd.index.size());
        for (size_t k = 0; k < pd.index.size(); ++k) pd.kind[k] = s.texels[pd.index[k]].state == TexelState::Inside;
        if (!pd.pts.empty()) {
            if (!tracer.sky(pd.pts, pd.sky) || pd.sky.size() != pd.pts.size() * 4) return std::nullopt;
            if (!tracer.lamps(pd.pts, pd.lamp) || pd.lamp.size() != pd.pts.size() * 12) return std::nullopt;
            if (!tracer.sun(pd.pts, towardsSun, pd.sun) || pd.sun.size() != pd.pts.size()) return std::nullopt;
        }
        pd.bounce.assign(pd.pts.size() * 12, 0.0f);
        pd.direct.assign(pd.pts.size() * 3, 0.0f);
        pending.push_back(std::move(pd));
    }

    // ---- phase 2: the gains of the sky (per channel) and of the lamps (one), from texels where only that term reaches
    float gs[3] = {1, 1, 1}, gl = 1.0f;
    {
        RatioHistogram hs[3], dirShip[3], dirBake[3];
        for (const Pending& pd : pending)
            for (size_t k = 0; k < pd.index.size(); ++k) {
                if (!pd.kind[k]) continue;
                float ship[3];
                if (!shipOf(pd.lm, pd.index[k], ship)) continue;
                const float lampL = luma709(pd.lamp[k * 12], pd.lamp[k * 12 + 4], pd.lamp[k * 12 + 8]);
                const float v0 = pd.sky[k * 4];
                if (lampL > 1e-4f || v0 < cfg.minBaked || ship[0] < cfg.minShipped || ship[1] < cfg.minShipped || ship[2] < cfg.minShipped) continue;
                for (int c = 0; c < 3; ++c) hs[c].add(ship[c] / v0);
                const ShippedLightmap& sh = shipped[pd.lm];
                for (int d = 1; d < 4; ++d) { // the directional coefficients against coefficient 0 (a check of the basis reading)
                    dirShip[d - 1].add(float(sh.coeff[1][pd.index[k] * 4 + size_t(d)]) / float(sh.coeff[1][pd.index[k] * 4]));
                    dirBake[d - 1].add(pd.sky[k * 4 + size_t(d)] / v0);
                }
            }
        out.stats.fitTexels = hs[0].count();
        for (int c = 0; c < 3; ++c) gs[c] = hs[c].count() >= 16 ? hs[c].median() : 1.0f;
        for (int d = 0; d < 3; ++d) out.stats.shippedDirRatio[d] = dirShip[d].median(), out.stats.bakedDirRatio[d] = dirBake[d].median();
        RatioHistogram hl;
        for (const Pending& pd : pending)
            for (size_t k = 0; k < pd.index.size(); ++k) {
                if (!pd.kind[k]) continue;
                float ship[3];
                if (!shipOf(pd.lm, pd.index[k], ship)) continue;
                const float lampL = luma709(pd.lamp[k * 12], pd.lamp[k * 12 + 4], pd.lamp[k * 12 + 8]);
                if (lampL < cfg.minLamp) continue;
                const float resid = luma709(ship[0] - gs[0] * pd.sky[k * 4], ship[1] - gs[1] * pd.sky[k * 4], ship[2] - gs[2] * pd.sky[k * 4]);
                if (resid > 0) hl.add(resid / lampL);
            }
        out.stats.lampFitTexels = hl.count();
        gl = hl.count() >= 16 ? hl.median() : 1.0f;
    }

    // ---- phase 3: rounds of cache, bounce passes and the overall correction
    float correction[3] = {1, 1, 1}, gb[3] = {1, 1, 1}; // gb: the bounce gain per channel (the albedo is the stored encoding's mean, not a measured reflectance)
    std::vector<float> cache(tris.size() * cn * 4, 0.0f);
    auto buildCache = [&]() {
        for (size_t lm = 0; lm < tris.size(); ++lm)
            for (size_t i = 0; i < cn; ++i) {
                float v[3];
                shipOf(lm, i, v);
                for (int c = 0; c < 3; ++c) cache[(lm * cn + i) * 4 + size_t(c)] = v[c];
            }
        for (Pending& pd : pending) {
            for (size_t k = 0; k < pd.index.size(); ++k) {
                const TexelPoint& p = pd.pts[k];
                const float ndl = std::max(0.0f, p.n[0] * towardsSun[0] + p.n[1] * towardsSun[1] + p.n[2] * towardsSun[2]);
                for (int c = 0; c < 3; ++c)
                    pd.direct[k * 3 + size_t(c)] = sunColor[c] * pd.sun[k] * ndl + gs[c] * pd.sky[k * 4] + gl * pd.lamp[k * 12 + size_t(c * 4)];
            }
        }
    };
    auto writeCache = [&](bool withBounce) {
        for (Pending& pd : pending) {
            for (size_t k = 0; k < pd.index.size(); ++k)
                for (int c = 0; c < 3; ++c)
                    cache[(pd.lm * cn + pd.index[k]) * 4 + size_t(c)] = pd.direct[k * 3 + size_t(c)] + (withBounce ? gb[c] * pd.bounce[k * 12 + size_t(c * 4)] : 0.0f);
            for (size_t i = 0; i < cn; ++i)
                if (TexelState(pd.state[i]) == TexelState::Dilated)
                    for (int c = 0; c < 3; ++c) cache[(pd.lm * cn + i) * 4 + size_t(c)] = cache[(pd.lm * cn + size_t(pd.source[i])) * 4 + size_t(c)];
        }
    };
    const int rounds = std::max(cfg.fitRounds, 1);
    for (int round = 0; round < rounds; ++round) {
        buildCache();
        writeCache(false);
        for (Pending& pd : pending) std::fill(pd.bounce.begin(), pd.bounce.end(), 0.0f);
        for (int pass = 0; pass < cfg.bounces; ++pass) {
            tracer.setCache(cache);
            for (Pending& pd : pending) {
                if (pd.pts.empty()) continue;
                std::vector<float> b;
                if (!tracer.bounce(pd.pts, b) || b.size() != pd.pts.size() * 12) return std::nullopt;
                pd.bounce = std::move(b);
            }
            writeCache(true);
        }
        { // refit the lamp gain (texels a lamp reaches) and the bounce gain (texels no lamp and little sky reach), given the other terms
            RatioHistogram hb[3];
            double lampResid = 0, lampSum = 0;
            size_t lampN = 0;
            for (const Pending& pd : pending)
                for (size_t k = 0; k < pd.index.size(); ++k) {
                    if (!pd.kind[k]) continue;
                    float ship[3];
                    if (!shipOf(pd.lm, pd.index[k], ship)) continue;
                    const float lampL = luma709(pd.lamp[k * 12], pd.lamp[k * 12 + 4], pd.lamp[k * 12 + 8]);
                    const float skyL = luma709(gs[0], gs[1], gs[2]) * pd.sky[k * 4];
                    const float bl = luma709(pd.bounce[k * 12], pd.bounce[k * 12 + 4], pd.bounce[k * 12 + 8]);
                    const float sl = lumaOf(ship);
                    if (lampL >= cfg.minLamp) {
                        const float resid = sl - skyL - luma709(gb[0] * pd.bounce[k * 12], gb[1] * pd.bounce[k * 12 + 4], gb[2] * pd.bounce[k * 12 + 8]);
                        lampResid += std::max(resid, 0.0f), lampSum += lampL, ++lampN; // the ratio of the sums: the mean over the lamp-lit class matches, not the median
                    } else if (pd.sky[k * 4] < 0.5f && sl >= cfg.minShipped && bl > 1e-4f) {
                        for (int c = 0; c < 3; ++c) {
                            const float resid = ship[c] - gs[c] * pd.sky[k * 4], b = pd.bounce[k * 12 + size_t(c * 4)];
                            if (resid > 0 && b > 1e-5f) hb[c].add(resid / b);
                        }
                    }
                }
            if (lampN >= 16 && lampSum > 0 && lampResid > 0) gl = float(lampResid / lampSum);
            for (int c = 0; c < 3; ++c)
                if (hb[c].count() >= 16) gb[c] = std::clamp(hb[c].median(), 0.25f, 8.0f);
            if (!cfg.bounceGainPerChannel) gb[0] = gb[1] = gb[2] = (gb[0] + gb[1] + gb[2]) / 3.0f;
        }
        // the overall correction: the median over the texels where both are measurable
        RatioHistogram hc[3];
        double sumShip = 0, sumNoBounce = 0;
        for (const Pending& pd : pending)
            for (size_t k = 0; k < pd.index.size(); ++k) {
                if (!pd.kind[k]) continue;
                float ship[3];
                if (!shipOf(pd.lm, pd.index[k], ship)) continue;
                float tot[3], nob[3];
                for (int c = 0; c < 3; ++c) {
                    nob[c] = gs[c] * pd.sky[k * 4] + gl * pd.lamp[k * 12 + size_t(c * 4)];
                    tot[c] = nob[c] + gb[c] * pd.bounce[k * 12 + size_t(c * 4)];
                }
                sumShip += lumaOf(ship), sumNoBounce += lumaOf(nob);
                if (ship[0] < cfg.minShipped || ship[1] < cfg.minShipped || ship[2] < cfg.minShipped) continue;
                for (int c = 0; c < 3; ++c)
                    if (tot[c] > 1e-4f) hc[c].add(ship[c] / tot[c]);
            }
        for (int c = 0; c < 3; ++c) correction[c] = hc[c].count() >= 16 ? std::clamp(hc[c].median(), 0.25f, 4.0f) : 1.0f;
        if (round == 0) out.stats.lumRatioNoBounce = sumShip > 0 ? sumNoBounce / sumShip : 0;
        if (round + 1 < rounds) // not the last round: fold the correction into the gains and go round again
        {
            for (int c = 0; c < 3; ++c) gs[c] *= correction[c];
            gl *= (correction[0] + correction[1] + correction[2]) / 3.0f; // the lamp and bounce gains follow the mean of the channel corrections
            for (int c = 0; c < 3; ++c) gb[c] *= cfg.bounceGainPerChannel ? correction[c] : (correction[0] + correction[1] + correction[2]) / 3.0f;
        }
    }
    for (int c = 0; c < 3; ++c) out.stats.correction[c] = correction[c];

    // ---- a last correction per lightmap and channel: the median of shipped / re-baked over the lightmap's texels (clamped to 0.6 .. 1.6)
    std::vector<std::array<float, 3>> lmGain(tris.size(), {1.0f, 1.0f, 1.0f});
    if (cfg.lightmapGain)
        for (const Pending& pd : pending) {
            RatioHistogram h[3];
            for (size_t k = 0; k < pd.index.size(); ++k) {
                if (!pd.kind[k]) continue;
                float ship[3];
                if (!shipOf(pd.lm, pd.index[k], ship) || ship[0] < cfg.minShipped || ship[1] < cfg.minShipped || ship[2] < cfg.minShipped) continue;
                for (int c = 0; c < 3; ++c) {
                    const float tot = correction[c] * (gs[c] * pd.sky[k * 4] + gl * pd.lamp[k * 12 + size_t(c * 4)] + gb[c] * pd.bounce[k * 12 + size_t(c * 4)]);
                    if (tot > 1e-4f) h[c].add(ship[c] / tot);
                }
            }
            for (int c = 0; c < 3; ++c)
                if (h[c].count() >= 64) lmGain[pd.lm][size_t(c)] = std::clamp(h[c].median(), 0.6f, 1.6f);
        }
    // ---- composition: re-baked where traced (times the final correction), the shipped data elsewhere
    for (int c = 0; c < 3; ++c) out.stats.gain[c] = gs[c] * correction[c];
    out.stats.lampGain = gl * (correction[0] + correction[1] + correction[2]) / 3.0f;
    out.stats.bounceGain = (gb[0] + gb[1] + gb[2]) / 3.0f * (correction[0] + correction[1] + correction[2]) / 3.0f;
    double sumShip[3] = {0, 0, 0}, sumBake[3] = {0, 0, 0}, sumShipFit = 0, sumBakeFit = 0;
    size_t within = 0, counted = 0, chromaN = 0;
    double chromaSum = 0;
    for (const Pending& pd : pending) {
        LightmapResult& res = out.lightmaps[pd.lm];
        const ShippedLightmap sh = pd.lm < shipped.size() ? shipped[pd.lm] : ShippedLightmap{};
        res.coeffs.assign(cn * 4 * 3, 0);
        std::vector<float> total(pd.pts.size() * 12, 0.0f);
        std::vector<int32_t> slot(cn, -1);
        for (size_t k = 0; k < pd.index.size(); ++k) {
            slot[pd.index[k]] = int32_t(k);
            for (int c = 0; c < 3; ++c)
                for (int j = 0; j < 4; ++j)
                    total[k * 12 + size_t(c * 4 + j)] = lmGain[pd.lm][size_t(c)] * correction[c] * (gs[c] * pd.sky[k * 4 + size_t(j)] + gl * pd.lamp[k * 12 + size_t(c * 4 + j)] + gb[c] * pd.bounce[k * 12 + size_t(c * 4 + j)]);
        }
        if (cfg.residualView) res.ratio.assign(cn * 3, 0);
        for (size_t i = 0; i < cn; ++i) {
            const TexelState st = TexelState(pd.state[i]);
            const bool traced = st == TexelState::Inside || st == TexelState::Edge || st == TexelState::Dilated;
            const size_t src = st == TexelState::Dilated ? size_t(pd.source[i]) : i;
            const int32_t k = traced ? slot[src] : -1;
            for (int c = 0; c < 3; ++c) {
                uint16_t* o = &res.coeffs[(size_t(c) * cn + i) * 4];
                if (k >= 0) {
                    const float* v = &total[size_t(k) * 12 + size_t(c * 4)];
                    o[0] = floatToHalf(v[2]), o[1] = floatToHalf(v[1]), o[2] = floatToHalf(v[0]), o[3] = floatToHalf(v[3]);
                } else if (sh.coeff[c]) {
                    const uint8_t* b = &sh.coeff[c][i * 4]; // bytes (c0, c1, c2, c3) = B, G, R, A: the sampled channels are (c2, c1, c0, c3)
                    o[0] = floatToHalf(float(b[2]) / 255.0f), o[1] = floatToHalf(float(b[1]) / 255.0f), o[2] = floatToHalf(float(b[0]) / 255.0f),
                    o[3] = floatToHalf(float(b[3]) / 255.0f);
                }
            }
            if (k >= 0 && sh.coeff[0] && sh.coeff[1] && sh.coeff[2]) {
                const float shipV[3] = {float(sh.coeff[0][i * 4]) / 255.0f, float(sh.coeff[1][i * 4]) / 255.0f, float(sh.coeff[2][i * 4]) / 255.0f};
                const float bakeV[3] = {total[size_t(k) * 12], total[size_t(k) * 12 + 4], total[size_t(k) * 12 + 8]};
                const float sl = lumaOf(shipV), bl = lumaOf(bakeV);
                if (st == TexelState::Inside) {
                    const float lampL = luma709(pd.lamp[size_t(k) * 12], pd.lamp[size_t(k) * 12 + 4], pd.lamp[size_t(k) * 12 + 8]);
                    const int cls = lampL >= cfg.minLamp ? 1 : pd.sky[size_t(k) * 4] >= 0.5f ? 0 : 2;
                    sumShip[cls] += sl, sumBake[cls] += bl;
                    ++out.stats.classTexels[cls];
                    ++out.stats.coeffTexels;
                    if (sl >= cfg.minShipped && bl > 1e-6f) {
                        chromaSum += std::fabs(shipV[0] / sl - bakeV[0] / bl) + std::fabs(shipV[2] / sl - bakeV[2] / bl), ++chromaN;
                    }
                    if (sl >= cfg.minShipped) {
                        ++counted;
                        within += std::fabs(bl / sl - 1.0f) <= 0.25f;
                    }
                    if (shipV[0] >= cfg.minShipped && shipV[1] >= cfg.minShipped && shipV[2] >= cfg.minShipped && lampL < 1e-4f && pd.sky[size_t(k) * 4] >= cfg.minBaked)
                        sumShipFit += sl, sumBakeFit += bl;
                }
                if (cfg.residualView && sl > 1e-4f && bl > 1e-6f) {
                    const float t = std::clamp(std::log2(bl / sl) / 2.0f, -1.0f, 1.0f);
                    res.ratio[i * 3] = uint8_t(std::lround(255.0f * (t < 0 ? 1.0f + t : 1.0f)));
                    res.ratio[i * 3 + 1] = uint8_t(std::lround(255.0f * (1.0f - std::fabs(t))));
                    res.ratio[i * 3 + 2] = uint8_t(std::lround(255.0f * (t > 0 ? 1.0f - t : 1.0f)));
                }
            }
        }
        res.baked = true;
        ++out.stats.bakedLightmaps;
    }
    for (int c = 0; c < 3; ++c) out.stats.classRatio[c] = sumShip[c] > 0 ? sumBake[c] / sumShip[c] : 0;
    out.stats.lumRatioFit = sumShipFit > 0 ? sumBakeFit / sumShipFit : 0;
    const double allShip = sumShip[0] + sumShip[1] + sumShip[2], allBake = sumBake[0] + sumBake[1] + sumBake[2];
    out.stats.lumRatioAll = allShip > 0 ? allBake / allShip : 0;
    out.stats.within25 = counted ? double(within) / double(counted) : 0;
    out.stats.chromaErr = chromaN ? chromaSum / double(chromaN) : 0;
    out.terms.valid = true;
    out.terms.cache = std::move(cache);
    for (int c = 0; c < 3; ++c) out.terms.sky[c] = gs[c], out.terms.bounce[c] = gb[c], out.terms.correction[c] = correction[c];
    out.terms.lamp = gl;
    out.stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

bool bakeGrid(Tracer& tracer, BakeResult& r, const std::vector<std::array<float, 3>>& points, const std::vector<std::array<float, 24>>& shipped, const Config& cfg) {
    (void)cfg;
    if (!r.terms.valid || points.empty() || shipped.size() != points.size()) return false;
    std::vector<TexelPoint> pts(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        TexelPoint& p = pts[i];
        std::memcpy(p.pos, points[i].data(), 12);
        p.n[0] = p.n[1] = 0, p.n[2] = 1, p.t[0] = 1, p.t[1] = p.t[2] = 0, p.b[0] = p.b[2] = 0, p.b[1] = 1;
    }
    std::vector<float> sky, lamp, bounce;
    tracer.setCache(r.terms.cache);
    if (!tracer.gridSky(pts, sky) || !tracer.gridLamps(pts, lamp) || !tracer.gridBounce(pts, bounce)) return false;
    if (sky.size() != pts.size() * 8 || lamp.size() != pts.size() * 24 || bounce.size() != pts.size() * 24) return false;
    const auto& T = r.terms;
    r.grid.assign(pts.size(), std::array<float, 24>{});
    RatioHistogram h[3];
    double sumShip = 0, sumBake = 0;
    for (size_t i = 0; i < pts.size(); ++i)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 8; ++k) {
                const size_t j = size_t(c * 8 + k);
                const float tot = T.correction[c] * (T.sky[c] * sky[i * 8 + size_t(k)] + T.lamp * lamp[i * 24 + j] + T.bounce[c] * bounce[i * 24 + j]);
                r.grid[i][j] = tot;
                sumBake += tot, sumShip += shipped[i][j];
                if (shipped[i][j] >= 3.0f / 255.0f && tot > 1e-4f) h[c].add(shipped[i][j] / tot);
            }
    Stats& st = r.stats;
    {
        double openShip = 0, openBake = 0;
        size_t open = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (sky[i * 8 + 7] < 0.9f) continue; // the upward corner sees (nearly) the whole dome
            ++open;
            for (size_t j = 0; j < 24; ++j) openShip += shipped[i][j], openBake += r.grid[i][j];
        }
        st.gridOpenShare = double(open) / double(pts.size());
        st.gridOpenRatio = openShip > 0 ? openBake / openShip : 0;
    }
    st.gridPoints = pts.size();
    st.gridShippedMean = sumShip / double(pts.size() * 24);
    {
        double sk = 0;
        for (float v : sky) sk += v;
        st.gridSkyMean = sk / double(sky.size());
    }
    st.gridRatioBefore = sumShip > 0 ? sumBake / sumShip : 0;
    auto within = [&](const std::vector<std::array<float, 24>>& v) {
        size_t n = 0, ok = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 8; ++k) a[c] += v[i][size_t(c * 8 + k)] / 8.0f, b[c] += shipped[i][size_t(c * 8 + k)] / 8.0f;
            const float sl = luma709(b[0], b[1], b[2]), bl = luma709(a[0], a[1], a[2]);
            if (sl < 3.0f / 255.0f) continue;
            ++n, ok += std::fabs(bl / sl - 1.0f) <= 0.25f;
        }
        return n ? double(ok) / double(n) : 0.0;
    };
    st.gridWithin25Before = within(r.grid);
    for (int c = 0; c < 3; ++c) st.gridCorrection[c] = h[c].count() >= 64 ? std::clamp(h[c].median(), 0.25f, 4.0f) : 1.0f;
    sumBake = 0;
    for (auto& g : r.grid)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 8; ++k) g[size_t(c * 8 + k)] *= st.gridCorrection[c], sumBake += g[size_t(c * 8 + k)];
    st.gridRatio = sumShip > 0 ? sumBake / sumShip : 0;
    st.gridWithin25 = within(r.grid);
    return true;
}

// ---- cache -------------------------------------------------------------------------------------------------------------------------

uint64_t fnv1a64(const void* data, size_t n, uint64_t h) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string settingsString(const Config& cfg, const float towardsSun[3], const std::vector<MapLight>& lights, const std::string& extra) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "cone%.3f rays%d,%d fit%.5f,%.3f lamp%.3f sun%.5f,%.5f,%.5f", double(cfg.sunConeDegrees), cfg.sunRays, cfg.skyRays,
                  double(cfg.minShipped), double(cfg.minBaked), double(cfg.lampMargin), double(towardsSun[0]), double(towardsSun[1]), double(towardsSun[2]));
    uint64_t h = 0;
    for (const MapLight& l : lights) h = fnv1a64(&l, sizeof l, h ? h : 1469598103934665603ull);
    char out[512];
    std::snprintf(out, sizeof out, "%s lights%016llx lamps%d,%.2f bounce%d,%d rounds%d minlamp%.4f alpha%d %s", buf, static_cast<unsigned long long>(h), cfg.lampRays,
                  double(cfg.lampSize), cfg.bounceRays, cfg.bounces, cfg.fitRounds, double(cfg.minLamp), cfg.alphaCasters ? 1 : 0, extra.c_str());
    // (albedo shaping is part of `extra`)
    return out;
}

std::string cacheKey(uint64_t mapHash, const std::string& settings) {
    uint64_t h = fnv1a64(settings.data(), settings.size());
    const uint32_t v = kFormatVersion;
    h = fnv1a64(&v, sizeof v, h);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%016llx-%016llx-v%u", static_cast<unsigned long long>(mapHash), static_cast<unsigned long long>(h), unsigned(v));
    return buf;
}

std::string cacheDir() {
    if (const char* e = std::getenv("IW2_REBAKE_CACHE"); e && *e) return e;
#ifdef __APPLE__
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/Library/Caches/OpenIW2/rebake";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/openiw2/rebake";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/openiw2/rebake";
#endif
    return {};
}

namespace {
constexpr char kMagic[8] = {'I', 'W', '2', 'R', 'B', 'K', '0', '1'};

template <class T>
void put(std::vector<uint8_t>& b, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
template <class T>
bool get(const std::vector<uint8_t>& b, size_t& at, T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}
void putBlob(std::vector<uint8_t>& b, const void* data, size_t n) {
    uLongf cap = compressBound(uLong(n));
    std::vector<uint8_t> z(cap);
    if (compress2(z.data(), &cap, static_cast<const Bytef*>(data), uLong(n), 1) != Z_OK) cap = 0;
    put(b, uint64_t(n));
    put(b, uint64_t(cap));
    b.insert(b.end(), z.begin(), z.begin() + std::ptrdiff_t(cap));
}
bool getBlob(const std::vector<uint8_t>& b, size_t& at, void* data, size_t n) {
    uint64_t raw = 0, zn = 0;
    if (!get(b, at, raw) || !get(b, at, zn) || raw != n || at + zn > b.size() || zn == 0) return false;
    uLongf out = uLongf(n);
    if (uncompress(static_cast<Bytef*>(data), &out, b.data() + at, uLong(zn)) != Z_OK || out != n) return false;
    at += size_t(zn);
    return true;
}
} // namespace

bool saveCache(const std::string& path, const BakeResult& r) {
    std::vector<uint8_t> b;
    b.insert(b.end(), kMagic, kMagic + 8);
    put(b, uint32_t(kFormatVersion));
    put(b, r.stats);
    put(b, uint32_t(r.lightmaps.size()));
    const size_t cn = size_t(kCoeffRes) * size_t(kCoeffRes), sn = size_t(kSunRes) * size_t(kSunRes);
    for (const LightmapResult& l : r.lightmaps) {
        const bool full = l.baked && l.coeffs.size() == cn * 12 && l.sun.size() == sn;
        put(b, uint8_t(full));
        if (!full) continue;
        put(b, uint64_t(l.inside)), put(b, uint64_t(l.edge)), put(b, uint64_t(l.dilated)), put(b, uint64_t(l.shared));
        putBlob(b, l.coeffs.data(), l.coeffs.size() * 2);
        putBlob(b, l.sun.data(), l.sun.size());
    }
    put(b, uint64_t(r.grid.size()));
    if (!r.grid.empty()) putBlob(b, r.grid.data(), r.grid.size() * sizeof(std::array<float, 24>));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

std::optional<BakeResult> loadCache(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t at = 0;
    if (b.size() < 8 || std::memcmp(b.data(), kMagic, 8) != 0) return std::nullopt;
    at = 8;
    uint32_t ver = 0, n = 0;
    BakeResult r;
    if (!get(b, at, ver) || ver != kFormatVersion || !get(b, at, r.stats) || !get(b, at, n) || n > 4096) return std::nullopt;
    const size_t cn = size_t(kCoeffRes) * size_t(kCoeffRes), sn = size_t(kSunRes) * size_t(kSunRes);
    r.lightmaps.resize(n);
    for (LightmapResult& l : r.lightmaps) {
        uint8_t full = 0;
        if (!get(b, at, full)) return std::nullopt;
        if (!full) continue;
        uint64_t v[4];
        for (uint64_t& x : v) if (!get(b, at, x)) return std::nullopt;
        l.inside = size_t(v[0]), l.edge = size_t(v[1]), l.dilated = size_t(v[2]), l.shared = size_t(v[3]);
        l.coeffs.resize(cn * 12);
        l.sun.resize(sn);
        if (!getBlob(b, at, l.coeffs.data(), l.coeffs.size() * 2) || !getBlob(b, at, l.sun.data(), l.sun.size())) return std::nullopt;
        l.baked = true;
    }
    uint64_t gn = 0;
    if (!get(b, at, gn) || gn > (1u << 24)) return std::nullopt;
    r.grid.resize(size_t(gn));
    if (gn && !getBlob(b, at, r.grid.data(), r.grid.size() * sizeof(std::array<float, 24>))) return std::nullopt;
    return r;
}

// ---- debug export ------------------------------------------------------------------------------------------------------------------

size_t exportTexelImages(const std::string& dir, const std::vector<std::vector<LmTriangle>>& tris, int res) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& lm : tris)
        for (const LmTriangle& t : lm)
            for (const LmVertex& v : t.v)
                for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v.pos[a]), hi[a] = std::max(hi[a], v.pos[a]);
    size_t written = 0;
    for (size_t li = 0; li < tris.size(); ++li) {
        if (tris[li].empty()) continue;
        const TexelSet s = rasterizeTexels(res, tris[li]);
        for (int kind = 0; kind < 2; ++kind) {
            char name[64];
            std::snprintf(name, sizeof name, "/lm%02zu_%s.ppm", li, kind ? "normal" : "pos");
            std::ofstream f(dir + name, std::ios::binary | std::ios::trunc);
            if (!f) return written;
            f << "P6\n" << res << " " << res << "\n255\n";
            std::vector<uint8_t> px(size_t(res) * size_t(res) * 3, 0);
            for (size_t i = 0; i < s.texels.size(); ++i) {
                const Texel& t = s.texels[i];
                float rgb[3] = {0, 0, 0};
                if (t.state == TexelState::Shared) rgb[0] = rgb[2] = 1.0f; // magenta
                else if (t.state != TexelState::Unset) {
                    for (int a = 0; a < 3; ++a)
                        rgb[a] = kind ? t.n[a] * 0.5f + 0.5f : (hi[a] > lo[a] ? (t.pos[a] - lo[a]) / (hi[a] - lo[a]) : 0.0f);
                    if (t.state == TexelState::Dilated) for (float& c : rgb) c *= 0.5f;
                }
                for (int a = 0; a < 3; ++a) px[i * 3 + size_t(a)] = uint8_t(std::lround(std::clamp(rgb[a], 0.0f, 1.0f) * 255.0f));
            }
            f.write(reinterpret_cast<const char*>(px.data()), std::streamsize(px.size()));
        }
        ++written;
    }
    return written;
}

} // namespace iw2::game::rebake
