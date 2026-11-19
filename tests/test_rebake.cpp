// The lighting re-bake's pure parts (src/game/rebake.h, docs/render-rebake.md). The last block runs on toujane when IW2_MAIN is set.
#include "assets/d3dbsp.h"
#include "assets/lightgrid.h"
#include "fs/vfs.h"
#include "game/rebake.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using namespace iw2::game;
using namespace iw2::game::rebake;

namespace {
int bad = 0;
void chk(bool c, int line) {
    if (!c) {
        std::printf("line %d failed\n", line);
        ++bad;
    }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A quad on the plane z = 5 (world x, y in 0..256) mapped to lightmap uv [u0, u1] x [v0, v1], two triangles, normal +z, tangent +x, binormal +y.
std::vector<LmTriangle> quad(float u0, float v0, float u1, float v1, float z = 5.0f, float size = 256.0f) {
    auto vert = [&](float u, float v) {
        LmVertex w{};
        w.pos[0] = (u - u0) / (u1 - u0) * size, w.pos[1] = (v - v0) / (v1 - v0) * size, w.pos[2] = z;
        w.normal[2] = 1, w.tangent[0] = 1, w.binormal[1] = 1;
        w.uv[0] = u, w.uv[1] = v;
        return w;
    };
    LmTriangle a, b;
    a.v[0] = vert(u0, v0), a.v[1] = vert(u1, v0), a.v[2] = vert(u1, v1);
    b.v[0] = vert(u0, v0), b.v[1] = vert(u1, v1), b.v[2] = vert(u0, v1);
    return {a, b};
}

// A tracer with a known answer: the sun sees points with x < 128, the sky every point, with the projections (1, .75, .75, .75).
struct FakeTracer : Tracer {
    int sunCalls = 0, skyCalls = 0;
    bool sun(const std::vector<TexelPoint>& pts, const float*, std::vector<float>& vis) override {
        ++sunCalls;
        vis.clear();
        for (const TexelPoint& p : pts) vis.push_back(p.pos[0] < 128.0f ? 1.0f : 0.0f);
        return true;
    }
    bool sky(const std::vector<TexelPoint>& pts, std::vector<float>& v4) override {
        ++skyCalls;
        v4.clear();
        for (size_t i = 0; i < pts.size(); ++i) v4.insert(v4.end(), {1.0f, 0.75f, 0.75f, 0.75f});
        return true;
    }
};
} // namespace

int main() {
    // The settings: off by default, -1 (as the preset) never turns it on, no preset turns it on, 1 turns it on.
    {
        std::map<std::string, std::string> cv;
        auto get = [&](const char* n) { const auto it = cv.find(n); return it == cv.end() ? std::string() : it->second; };
        chk(!enhancedSettings(get).rebake, __LINE__);
        cv["r_enhanced"] = "1";
        for (const char* p : {"0", "1", "2"}) {
            cv["r_enhPreset"] = p;
            cv["r_enhRebake"] = "-1";
            chk(!enhancedSettings(get).rebake, __LINE__);
            cv["r_enhRebake"] = "0";
            chk(!enhancedSettings(get).rebake, __LINE__);
        }
        cv["r_enhRebake"] = "1";
        chk(enhancedSettings(get).rebake, __LINE__);
    }

    // The basis: the weights of basis direction k are the unit vector k; a flat normal map uses only the first weight.
    {
        const auto basis = lightmapBasis();
        for (int k = 0; k < 4; ++k) {
            const auto& d = basis[size_t(k)];
            chk(near(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f), __LINE__);
            const auto w = lightmapWeights(d[0], d[1]);
            for (int j = 0; j < 4; ++j) chk(near(w[size_t(j)], j == k ? 1.0f : 0.0f, 2e-3f), __LINE__);
        }
        chk(near(basis[1][2], std::sqrt(1.0f / 3.0f)), __LINE__);
        chk(near(basis[1][0], 0.0f, 1e-5f) && near(basis[1][1], std::sqrt(2.0f / 3.0f)), __LINE__); // weight 1 at azimuth 90 degrees
        chk(near(basis[2][0], std::sqrt(2.0f / 3.0f) * std::cos(3.6651914f)) && basis[2][1] < 0, __LINE__); // 210 degrees
        chk(basis[3][0] > 0 && basis[3][1] < 0, __LINE__);                                                  // 330 degrees
        const auto flat = lightmapWeights(0.0f, 0.0f);
        chk(near(flat[0], 1.0f) && near(flat[1] + flat[2] + flat[3], 0.0f), __LINE__);
        const auto edge = lightmapWeights(1.0f, 0.0f); // on the horizon: no weight on the normal, the other three sum to 1
        chk(near(edge[0], 0.0f) && near(edge[1] + edge[2] + edge[3], 1.0f), __LINE__);
    }

    // Half floats: every half survives the round trip, rounding is to nearest even, overflow goes to infinity.
    {
        for (uint32_t h = 0; h < 0x10000; ++h) {
            if ((h & 0x7c00) == 0x7c00 && (h & 0x3ff)) continue; // nan
            chk(floatToHalf(halfToFloat(uint16_t(h))) == h, __LINE__);
        }
        chk(floatToHalf(1.0f) == 0x3c00 && floatToHalf(-2.0f) == 0xc000 && floatToHalf(0.0f) == 0 && halfToFloat(0x3555) > 0.333f, __LINE__);
        chk(floatToHalf(1e6f) == 0x7c00 && floatToHalf(65504.0f) == 0x7bff && floatToHalf(1e-9f) == 0, __LINE__);
        chk(near(halfToFloat(floatToHalf(0.1f)), 0.1f, 1e-4f), __LINE__);
    }

    // Rasterisation: a quad over the middle 128 x 128 texels (uv .25 .. .75) at 256 x 256.
    {
        const TexelSet s = rasterizeTexels(256, quad(0.25f, 0.25f, 0.75f, 0.75f));
        chk(s.inside == 128u * 128u, __LINE__);
        chk(s.edge == 0, __LINE__); // texel borders coincide with the quad edges: no square only touches it
        chk(s.dilated == 4u * 128u + 4u, __LINE__); // one ring of texels around the 128 x 128 block, corners included
        chk(s.shared == 0, __LINE__);
        const Texel& t = s.texels[size_t(64 + 10) * 256 + size_t(64 + 20)]; // texel (84, 74): world x = (20.5 / 128) * 256, y = (10.5 / 128) * 256
        chk(t.state == TexelState::Inside, __LINE__);
        chk(near(t.pos[0], 41.0f, 0.01f) && near(t.pos[1], 21.0f, 0.01f) && near(t.pos[2], 5.0f), __LINE__);
        chk(near(t.n[2], 1.0f) && near(t.t[0], 1.0f) && near(t.b[1], 1.0f), __LINE__);
        const Texel& d = s.texels[size_t(63) * 256 + 100]; // the ring above the block
        chk(d.state == TexelState::Dilated && d.source == int32_t(64 * 256 + 100) && near(d.pos[1], 1.0f, 0.01f), __LINE__);
        chk(s.texels[size_t(10) * 256 + 10].state == TexelState::Unset, __LINE__);
    }
    // Conservative coverage: a triangle smaller than a texel that misses every centre still claims the texel it lies in (Edge), with its
    // own position; one that sits over a centre is Inside.
    {
        LmTriangle tri{};
        const float uv[3][2] = {{0.1f, 0.1f}, {0.1f + 0.1f / 64, 0.1f}, {0.1f, 0.1f + 0.1f / 64}}; // texel (6, 6) at 64: spans 6.4 .. 6.5, the centre is 6.5
        for (int k = 0; k < 3; ++k) {
            tri.v[k].uv[0] = uv[k][0], tri.v[k].uv[1] = uv[k][1];
            tri.v[k].pos[0] = float(k), tri.v[k].pos[1] = float(k * 2), tri.v[k].pos[2] = 7.0f;
            tri.v[k].normal[2] = 1, tri.v[k].tangent[0] = 1, tri.v[k].binormal[1] = 1;
        }
        const TexelSet s = rasterizeTexels(64, {tri});
        chk(s.inside == 0 && s.edge == 1 && s.texels[6 * 64 + 6].state == TexelState::Edge, __LINE__);
        chk(near(s.texels[6 * 64 + 6].pos[2], 7.0f) && s.dilated >= 4, __LINE__);
        // the point is on the triangle: its barycentric weights are in 0..1, so the position lies inside the vertex box
        const float* p = s.texels[6 * 64 + 6].pos;
        chk(p[0] >= -1e-4f && p[0] <= 2.0f + 1e-4f && p[1] >= -1e-4f && p[1] <= 4.0f + 1e-4f, __LINE__);
    }
    // Overlap: the same surface twice keeps one texel; two surfaces on top of each other (different height) make the texel Shared and it
    // stays so; a mirrored chart (opposite normal on the same coordinates) is Shared too.
    {
        auto a = quad(0.0f, 0.0f, 0.5f, 0.5f);
        TexelSet s = rasterizeTexels(32, a);
        chk(s.shared == 0 && s.inside == 16u * 16u, __LINE__);
        auto both = a;
        auto b = quad(0.0f, 0.0f, 0.5f, 0.5f, 5.0f);
        both.insert(both.end(), b.begin(), b.end()); // identical again
        chk(rasterizeTexels(32, both).shared == 0, __LINE__);
        auto up = quad(0.0f, 0.0f, 0.5f, 0.5f, 105.0f);
        both = a;
        both.insert(both.end(), up.begin(), up.end());
        s = rasterizeTexels(32, both);
        chk(s.shared == 16u * 16u && s.inside == 0, __LINE__);
        auto mirrored = quad(0.0f, 0.0f, 0.5f, 0.5f);
        for (LmTriangle& t : mirrored)
            for (LmVertex& v : t.v) v.normal[2] = -1;
        both = a;
        both.insert(both.end(), mirrored.begin(), mirrored.end());
        chk(rasterizeTexels(32, both).shared == 16u * 16u, __LINE__);
        // a degenerate triangle in uv is skipped
        LmTriangle deg = a[0];
        deg.v[1].uv[0] = deg.v[0].uv[0], deg.v[1].uv[1] = deg.v[0].uv[1];
        chk(rasterizeTexels(32, {deg}).inside == 0, __LINE__);
    }

    // The median of ratios: 3 % bins, insensitive to outliers, empty gives 0.
    {
        RatioHistogram h;
        chk(h.median() == 0.0f, __LINE__);
        for (int i = 0; i < 100; ++i) h.add(0.5f);
        for (int i = 0; i < 20; ++i) h.add(30.0f), h.add(0.001f);
        h.add(0.0f), h.add(-1.0f), h.add(INFINITY);
        chk(h.count() == 140 && near(h.median(), 0.5f, 0.5f * 0.03f), __LINE__);
        RatioHistogram g;
        for (float r : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f}) g.add(r);
        chk(near(g.median(), 4.0f, 4.0f * 0.03f), __LINE__);
    }
    {
        MaskOverlap o;
        const uint8_t a[] = {255, 255, 0, 0, 200}, b[] = {255, 0, 0, 255, 100};
        for (int i = 0; i < 5; ++i) o = maskOverlap(a[i], b[i], 127, o);
        chk(o.both == 1 && o.either == 4 && o.onlyA == 2 && o.onlyB == 1 && near(float(o.iou()), 0.25f), __LINE__); // texel 4 is 200 vs 100: only a is sunlit
        chk(MaskOverlap{}.iou() == 1.0, __LINE__);
    }

    // The bake with a tracer of known answers on a quad over the whole lightmap (uv 0..1): sun on the left half, the sky everywhere. The
    // shipped data says the same sun mask and coefficient 0 = 0.5 / 0.25 / 0.125 in the three channels (so the gains are those over 1).
    {
        const size_t cn = size_t(kCoeffRes) * kCoeffRes, sn = size_t(kSunRes) * kSunRes;
        std::vector<uint8_t> c[3] = {std::vector<uint8_t>(cn * 4, 0), std::vector<uint8_t>(cn * 4, 0), std::vector<uint8_t>(cn * 4, 0)};
        const uint8_t level[3] = {128, 64, 32};
        for (int ch = 0; ch < 3; ++ch)
            for (size_t i = 0; i < cn; ++i) c[ch][i * 4] = level[ch], c[ch][i * 4 + 1] = 200, c[ch][i * 4 + 2] = 100, c[ch][i * 4 + 3] = 50;
        std::vector<uint8_t> sun(sn);
        for (size_t i = 0; i < sn; ++i) sun[i] = (i % kSunRes) < kSunRes / 2 ? 255 : 0; // texel centre x = (i + .5) / res * 256 < 128 for i < res / 2
        ShippedLightmap sh;
        for (int ch = 0; ch < 3; ++ch) sh.coeff[ch] = c[ch].data();
        sh.sun = sun.data();
        std::vector<std::vector<LmTriangle>> tris(3); // lightmaps 0 and 2 exist, lightmap 1 has no triangles
        tris[0] = quad(0.0f, 0.0f, 1.0f, 1.0f);
        tris[2] = quad(0.0f, 0.0f, 0.5f, 1.0f, 5.0f, 128.0f); // world x = u * 256 as in lightmap 0; half of the lightmap; the rest keeps the shipped data
        const float towardsSun[3] = {0, 0, 1}, sunCol[3] = {1, 1, 1};
        FakeTracer tr;
        MapLight lamp;
        lamp.pos[0] = 1e6f; // far away: no texel is excluded from the fit
        Config noLm;
        noLm.lightmapGain = false; // (the per-lightmap correction would move the values the checks below read)
        const auto r = bakeMap(tr, tris, {sh, sh, sh}, towardsSun, sunCol, {lamp}, noLm);
        chk(r.has_value(), __LINE__);
        if (r) {
            chk(tr.sunCalls == 4 && tr.skyCalls == 2, __LINE__);
            chk(r->lightmaps.size() == 3 && r->lightmaps[0].baked && !r->lightmaps[1].baked && r->lightmaps[2].baked, __LINE__);
            chk(near(r->stats.sunFacing.iou(), 1.0f) && near(r->stats.sunAll.iou(), 1.0f), __LINE__);
            chk(near(r->stats.gain[0], 0.5019f, 0.02f) && near(r->stats.gain[1], 0.251f, 0.01f) && near(r->stats.gain[2], 0.1255f, 0.005f), __LINE__);
            chk(r->stats.fitTexels > 0, __LINE__);
            chk(near(float(r->stats.lumRatioFit), 1.0f, 0.04f) && near(float(r->stats.lumRatioAll), 1.0f, 0.04f), __LINE__);
            const LightmapResult& l0 = r->lightmaps[0];
            chk(l0.coeffs.size() == cn * 12 && l0.sun.size() == sn, __LINE__);
            // sampled channel order (c2, c1, c0, c3): c0 = gain * 1, c1..c3 = gain * .75
            const uint16_t* px = &l0.coeffs[(0 * cn + 100 * 512 + 100) * 4];
            chk(near(halfToFloat(px[2]), r->stats.gain[0], 1e-3f) && near(halfToFloat(px[1]), 0.75f * r->stats.gain[0], 1e-3f) &&
                    near(halfToFloat(px[0]), 0.75f * r->stats.gain[0], 1e-3f) && near(halfToFloat(px[3]), 0.75f * r->stats.gain[0], 1e-3f),
                __LINE__);
            chk(l0.sun[10 * 1024 + 100] == 255 && l0.sun[10 * 1024 + 900] == 0, __LINE__);
            // lightmap 2: the covered half re-baked, the rest is the shipped data in the sampled order (c2, c1, c0, c3) = (100, 200, 128, 50) / 255
            const LightmapResult& l2 = r->lightmaps[2];
            const uint16_t* in = &l2.coeffs[(0 * cn + 100 * 512 + 100) * 4];
            const uint16_t* out = &l2.coeffs[(0 * cn + 100 * 512 + 400) * 4];
            chk(near(halfToFloat(in[2]), r->stats.gain[0], 1e-3f), __LINE__);
            chk(near(halfToFloat(out[0]), 100.0f / 255.0f, 1e-3f) && near(halfToFloat(out[1]), 200.0f / 255.0f, 1e-3f) &&
                    near(halfToFloat(out[2]), 128.0f / 255.0f, 1e-3f) && near(halfToFloat(out[3]), 50.0f / 255.0f, 1e-3f),
                __LINE__);
            chk(l2.sun[10 * 1024 + 900] == sun[10 * 1024 + 900] && l2.sun[10 * 1024 + 100] == 255, __LINE__);
            // the cache round trip keeps everything
            const std::string path = (std::filesystem::temp_directory_path() / "iw2_rebake_test.bin").string();
            chk(saveCache(path, *r), __LINE__);
            const auto back = loadCache(path);
            chk(back.has_value() && back->lightmaps.size() == 3 && back->lightmaps[0].coeffs == l0.coeffs && back->lightmaps[2].sun == l2.sun &&
                    !back->lightmaps[1].baked && near(back->stats.gain[1], r->stats.gain[1], 0.0f),
                __LINE__);
            { // a damaged file is refused
                std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
                f.seekp(40);
                f.put('\x7f');
            }
            const auto damaged = loadCache(path); // refused (a flipped byte inside the compressed data) or read: never a crash
            chk(!damaged.has_value() || damaged->lightmaps.size() == 3, __LINE__);
            chk(!loadCache(path + ".missing").has_value(), __LINE__);
            std::filesystem::remove(path);
        }
        // a failing tracer fails the bake
        struct Failing : Tracer {
            bool sun(const std::vector<TexelPoint>&, const float*, std::vector<float>&) override { return false; }
            bool sky(const std::vector<TexelPoint>&, std::vector<float>&) override { return false; }
        } failing;
        chk(!bakeMap(failing, tris, {sh, sh, sh}, towardsSun, sunCol, {}).has_value(), __LINE__);
    }

    auto scene = [](const std::vector<float>& pos, const std::vector<uint32_t>& idx) {
        TraceScene sc;
        sc.positions = pos, sc.indices = idx;
        sc.vertexUv.assign(pos.size() / 3 * 2, 0.0f);
        sc.tris.resize(idx.size() / 3);
        return sc;
    };
    // The CPU reference tracer (the kernels' maths): open ground, a roof, a wall that blocks half the sky, a shadow of a wall on the ground.
    {
        const float kBig = 4000.0f;
        auto tri = [](std::vector<float>& pos, std::vector<uint32_t>& idx, const float a[3], const float b[3], const float c[3]) {
            const uint32_t base = uint32_t(pos.size() / 3);
            for (const float* v : {a, b, c}) pos.insert(pos.end(), v, v + 3);
            idx.insert(idx.end(), {base, base + 1, base + 2});
        };
        auto quadAt = [&](std::vector<float>& pos, std::vector<uint32_t>& idx, const float o[3], const float u[3], const float v[3]) {
            const float p1[3] = {o[0] + u[0], o[1] + u[1], o[2] + u[2]}, p2[3] = {o[0] + u[0] + v[0], o[1] + u[1] + v[1], o[2] + u[2] + v[2]},
                        p3[3] = {o[0] + v[0], o[1] + v[1], o[2] + v[2]};
            tri(pos, idx, o, p1, p2);
            tri(pos, idx, o, p2, p3);
        };
        const Config cfg;
        TexelPoint ground{};
        ground.n[2] = 1, ground.t[0] = 1, ground.b[1] = 1; // a point at the origin on a floor
        const float sunUp[3] = {0, 0, 1};
        std::vector<float> v;
        {   // the floor only
            std::vector<float> pos;
            std::vector<uint32_t> idx;
            const float o[3] = {-kBig, -kBig, 0}, u[3] = {2 * kBig, 0, 0}, w[3] = {0, 2 * kBig, 0};
            quadAt(pos, idx, o, u, w);
            auto t = makeCpuTracer(scene(pos, idx), cfg);
            chk(t->sky({ground}, v) && v.size() == 4, __LINE__);
            chk(near(v[0], 1.0f, 0.02f) && near(v[1], 0.789f, 0.03f) && near(v[2], 0.789f, 0.03f) && near(v[3], 0.789f, 0.03f), __LINE__);
            chk(t->sun({ground}, sunUp, v) && near(v[0], 1.0f), __LINE__);
            TexelPoint down = ground;
            down.n[2] = -1;
            chk(t->sun({down}, sunUp, v) && v[0] == 0.0f, __LINE__); // facing away
        }
        {   // a roof above the point: no sun, no sky
            std::vector<float> pos;
            std::vector<uint32_t> idx;
            const float o[3] = {-kBig, -kBig, 100}, u[3] = {2 * kBig, 0, 0}, w[3] = {0, 2 * kBig, 0};
            quadAt(pos, idx, o, u, w);
            auto t = makeCpuTracer(scene(pos, idx), cfg);
            chk(t->sky({ground}, v) && near(v[0], 0.0f, 0.01f) && near(v[1], 0.0f, 0.05f), __LINE__); // (the few rays under 1.5 degrees pass the roof's edge)
            chk(t->sun({ground}, sunUp, v) && v[0] == 0.0f, __LINE__);
        }
        {   // a tall wall at x = 50 (the +x half of the sky is blocked; the basis directions lean to -x, +x/-x, +x/-x)
            std::vector<float> pos;
            std::vector<uint32_t> idx;
            const float o[3] = {50, -kBig, 0}, u[3] = {0, 2 * kBig, 0}, w[3] = {0, 0, 1e5f};
            quadAt(pos, idx, o, u, w);
            auto t = makeCpuTracer(scene(pos, idx), cfg);
            chk(t->sky({ground}, v) && near(v[0], 0.5f, 0.03f), __LINE__);
            // basis 1 points to +y, 2 to -x (210 degrees: cos < 0), 3 to +x (330): the wall hurts 3 most and 2 least
            chk(v[2] > v[1] && v[1] > v[3], __LINE__);
        }
        {   // a wall 100 high at x = 50 and the sun at 45 degrees from that side: shadowed at its foot, lit further away than its height; from the other side always lit
            std::vector<float> pos;
            std::vector<uint32_t> idx;
            const float o[3] = {50, -kBig, 0}, u[3] = {0, 2 * kBig, 0}, w[3] = {0, 0, 100};
            quadAt(pos, idx, o, u, w);
            auto t = makeCpuTracer(scene(pos, idx), cfg);
            const float sunLow[3] = {0.7071f, 0, 0.7071f}, sunAway[3] = {-0.7071f, 0, 0.7071f};
            TexelPoint farP = ground;
            farP.pos[0] = -300;
            chk(t->sun({ground}, sunLow, v) && v[0] == 0.0f, __LINE__);
            chk(t->sun({farP}, sunLow, v) && near(v[0], 1.0f), __LINE__);
            chk(t->sun({ground}, sunAway, v) && near(v[0], 1.0f), __LINE__);
            // the soft edge: a point whose ray to the sun's centre grazes the wall top sees part of the cone
            TexelPoint edgeP = ground;
            edgeP.pos[0] = -50; // the ray to the sun's centre rises 1:1 and reaches the wall (x = 50) at z = 100 + 1: just clear
            chk(t->sun({edgeP}, sunLow, v) && v[0] > 0.0f, __LINE__);
        }
    }

    // Stage 3 on the CPU tracer: lamps, alpha-tested casters and bounces on analytic scenes.
    {
        auto addQuad = [](TraceScene& sc, const float o[3], const float u[3], const float v[3], int32_t tex, const float light[3]) {
            const uint32_t base = uint32_t(sc.positions.size() / 3);
            const float p[4][3] = {{o[0], o[1], o[2]}, {o[0] + u[0], o[1] + u[1], o[2] + u[2]}, {o[0] + u[0] + v[0], o[1] + u[1] + v[1], o[2] + u[2] + v[2]},
                                   {o[0] + v[0], o[1] + v[1], o[2] + v[2]}};
            const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int i = 0; i < 4; ++i) {
                sc.positions.insert(sc.positions.end(), p[i], p[i] + 3);
                sc.vertexUv.insert(sc.vertexUv.end(), uvs[i], uvs[i] + 2);
            }
            sc.indices.insert(sc.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            for (int t = 0; t < 2; ++t) {
                SceneTri st;
                st.tex = tex;
                for (int e = 0; e < 3; ++e) st.light[e] = light[e];
                sc.tris.push_back(st);
            }
        };
        Config cfg;
        cfg.albedoSat = 1.0f; // the box test uses coloured albedos
        const float zero[3] = {0, 0, 0};
        TexelPoint ground{};
        ground.n[2] = 1, ground.t[0] = 1, ground.b[1] = 1;
        std::vector<float> v;
        { // a lamp 100 above the point, radius 300: colour * (1 - 1/3)^2 along the normal, 1/sqrt 3 of it along the tilted basis directions
            TraceScene sc;
            const float o[3] = {-2000, -2000, 0}, u[3] = {4000, 0, 0}, w[3] = {0, 4000, 0};
            addQuad(sc, o, u, w, -1, zero);
            MapLight l;
            l.pos[2] = 100, l.radius = 300, l.color[0] = 1, l.color[1] = 0.5f, l.color[2] = 0.25f;
            sc.lamps = {l};
            auto t = makeCpuTracer(sc, cfg);
            chk(t->lamps({ground}, v) && v.size() == 12, __LINE__);
            const float f = (2.0f / 3.0f) * (2.0f / 3.0f);
            chk(near(v[0], f, 1e-3f) && near(v[4], f * 0.5f, 1e-3f) && near(v[8], f * 0.25f, 1e-3f), __LINE__);
            chk(near(v[1], f * std::sqrt(1.0f / 3.0f), 1e-3f) && near(v[3], v[1], 1e-3f), __LINE__);
            TexelPoint far = ground;
            far.pos[0] = 400; // outside the radius
            chk(t->lamps({far}, v) && v[0] == 0.0f, __LINE__);
            TexelPoint under = ground;
            under.n[2] = -1; // the lamp is behind the surface
            chk(t->lamps({under}, v) && v[0] == 0.0f && v[1] == 0.0f, __LINE__);
            // a spot lamp aimed away lights nothing, aimed down lights as the point lamp
            sc.lamps[0].cosOuter = std::cos(30.0f * 3.14159265f / 180.0f), sc.lamps[0].cosInner = std::cos(10.0f * 3.14159265f / 180.0f);
            sc.lamps[0].dir[2] = 1;
            chk(makeCpuTracer(sc, cfg)->lamps({ground}, v) && v[0] == 0.0f, __LINE__);
            sc.lamps[0].dir[2] = -1;
            chk(makeCpuTracer(sc, cfg)->lamps({ground}, v) && near(v[0], f, 1e-3f), __LINE__);
        }
        { // a roof between the lamp and the point: no light
            TraceScene sc;
            const float o[3] = {-2000, -2000, 50}, u[3] = {4000, 0, 0}, w[3] = {0, 4000, 0};
            addQuad(sc, o, u, w, -1, zero);
            MapLight l;
            l.pos[2] = 100, l.radius = 300, l.color[0] = l.color[1] = l.color[2] = 1;
            sc.lamps = {l};
            chk(makeCpuTracer(sc, cfg)->lamps({ground}, v) && v[0] == 0.0f, __LINE__);
        }
        { // an alpha-tested roof at 100: its left half (u < .5) fails the test (GE128 on alpha 0), its right half blocks the sun
            TraceScene sc;
            const float o[3] = {-100, -100, 100}, u[3] = {200, 0, 0}, w[3] = {0, 200, 0};
            addQuad(sc, o, u, w, 0, zero);
            sc.ranges = {{0, 6, 0, 1, 0}};
            sc.masks = {std::vector<uint8_t>(64 * 64, 0)};
            for (int y = 0; y < 64; ++y)
                for (int x = 32; x < 64; ++x) sc.masks[0][size_t(y) * 64 + size_t(x)] = 255;
            const float up3[3] = {0, 0, 1};
            auto t = makeCpuTracer(sc, cfg);
            TexelPoint a = ground, b = ground;
            a.pos[0] = -50, b.pos[0] = 50; // uv.x .25 and .75
            chk(t->sun({a}, up3, v) && near(v[0], 1.0f, 0.01f), __LINE__);
            chk(t->sun({b}, up3, v) && v[0] == 0.0f, __LINE__);
            sc.ranges = {{0, 6, -1, 1, -1}}; // solid: both halves block
            chk(makeCpuTracer(sc, cfg)->sun({a}, up3, v) && v[0] == 0.0f, __LINE__);
            sc.ranges = {{0, 6, 0, 3, 0}}; // LT128: the passing half is the left one
            chk(makeCpuTracer(sc, cfg)->sun({a}, up3, v) && v[0] == 0.0f, __LINE__);
            chk(makeCpuTracer(sc, cfg)->sun({b}, up3, v) && near(v[0], 1.0f, 0.01f), __LINE__);
        }
        { // a closed box (albedo .6, light 1 everywhere): a bounce gives .6 along the normal and about .79 of that along the tilted directions
            TraceScene sc;
            const float L = 500.0f, e[3] = {1, 1, 1};
            const float o0[3] = {-L, -L, 0}, ux[3] = {2 * L, 0, 0}, uy[3] = {0, 2 * L, 0}, uz[3] = {0, 0, 2 * L};
            const float o1[3] = {-L, -L, 2 * L}, o2[3] = {-L, -L, 0}, o3[3] = {L, -L, 0};
            addQuad(sc, o0, ux, uy, 0, e);
            addQuad(sc, o1, ux, uy, 0, e);
            addQuad(sc, o2, ux, uz, 0, e);
            const float o4[3] = {-L, L, 0};
            addQuad(sc, o4, ux, uz, 0, e);
            addQuad(sc, o2, uy, uz, 0, e);
            addQuad(sc, o3, uy, uz, 0, e);
            sc.albedo = {0.6f, 0.4f, 0.2f};
            auto t = makeCpuTracer(sc, cfg);
            chk(t->bounce({ground}, v) && v.size() == 12, __LINE__);
            chk(near(v[0], 0.6f, 0.02f) && near(v[4], 0.4f, 0.02f) && near(v[8], 0.2f, 0.02f), __LINE__);
            chk(near(v[1], 0.6f * 0.789f, 0.03f) && near(v[2], v[1], 0.03f), __LINE__);
            // with lightmap coordinates: the cache value at the hit's texel (cache 2 everywhere in lightmap 0) replaces the triangle's light
            for (SceneTri& st : sc.tris) st.lm = 0, st.uv[0] = 0.1f, st.uv[1] = 0.1f, st.uv[2] = 0.1f, st.uv[3] = 0.1f, st.uv[4] = 0.1f, st.uv[5] = 0.1f;
            auto t2 = makeCpuTracer(sc, cfg);
            std::vector<float> cache(size_t(kCoeffRes) * kCoeffRes * 4, 2.0f);
            t2->setCache(cache);
            chk(t2->bounce({ground}, v) && near(v[0], 1.2f, 0.04f), __LINE__);
        }
        { // the albedo shaping: gamma, then towards the luminance
            float a[3] = {0.5f, 0.25f, 1.0f};
            shapeAlbedo(a, 1.0f, 1.0f);
            chk(near(a[0], 0.5f) && near(a[1], 0.25f) && near(a[2], 1.0f), __LINE__);
            shapeAlbedo(a, 2.0f, 1.0f);
            chk(near(a[0], 0.25f) && near(a[1], 0.0625f), __LINE__);
            float g[3] = {0.8f, 0.2f, 0.2f};
            const float l = luma709(g[0], g[1], g[2]);
            shapeAlbedo(g, 1.0f, 0.0f);
            chk(near(g[0], l) && near(g[1], l) && near(g[2], l), __LINE__);
        }
        { // the mean albedo is weighted by alpha
            const float px[8] = {1, 0, 0, 1, 0, 1, 0, 0}; // red opaque, green transparent
            float m[3];
            meanAlbedo(px, 2, m);
            chk(near(m[0], 1.0f) && near(m[1], 0.0f), __LINE__);
            const float half[8] = {1, 0, 0, 1, 0, 1, 0, 1};
            meanAlbedo(half, 2, m);
            chk(near(m[0], 0.5f) && near(m[1], 0.5f), __LINE__);
            const float none[4] = {1, 1, 1, 0};
            meanAlbedo(none, 1, m);
            chk(m[0] == 0.0f, __LINE__);
        }
        { // the bake fits the lamp gain on texels the lamps reach and the sky gain on the rest: sun-less quad, sky c0 = 1, lamps c0 = 1 for x >= 128
            struct LampTracer : Tracer {
                bool sun(const std::vector<TexelPoint>& pts, const float*, std::vector<float>& vis) override { vis.assign(pts.size(), 0.0f); return true; }
                bool sky(const std::vector<TexelPoint>& pts, std::vector<float>& v4) override {
                    v4.clear();
                    for (size_t i = 0; i < pts.size(); ++i) v4.insert(v4.end(), {1.0f, 0.75f, 0.75f, 0.75f});
                    return true;
                }
                bool lamps(const std::vector<TexelPoint>& pts, std::vector<float>& o) override {
                    o.assign(pts.size() * 12, 0.0f);
                    for (size_t i = 0; i < pts.size(); ++i)
                        if (pts[i].pos[0] >= 128.0f)
                            for (int c = 0; c < 3; ++c) o[i * 12 + size_t(c * 4)] = 1.0f;
                    return true;
                }
            } lt;
            const size_t cn = size_t(kCoeffRes) * kCoeffRes;
            std::vector<uint8_t> c[3] = {std::vector<uint8_t>(cn * 4, 0), std::vector<uint8_t>(cn * 4, 0), std::vector<uint8_t>(cn * 4, 0)};
            for (size_t i = 0; i < cn; ++i) {
                const bool lit = (i % kCoeffRes) >= kCoeffRes / 2;
                for (int ch = 0; ch < 3; ++ch) c[ch][i * 4] = lit ? 204 : 128; // .8 with a lamp, .5 without
            }
            ShippedLightmap sh;
            for (int ch = 0; ch < 3; ++ch) sh.coeff[ch] = c[ch].data();
            std::vector<std::vector<LmTriangle>> tris(1);
            tris[0] = quad(0.0f, 0.0f, 1.0f, 1.0f);
            const float sunDir[3] = {0, 0, 1}, sunCol[3] = {1, 1, 1};
            const auto r = bakeMap(lt, tris, {sh}, sunDir, sunCol, {});
            chk(r.has_value(), __LINE__);
            if (r) {
                chk(near(r->stats.gain[0], 0.502f, 0.02f) && near(r->stats.lampGain, 0.298f, 0.03f), __LINE__);
                chk(near(float(r->stats.lumRatioAll), 1.0f, 0.03f) && r->stats.within25 > 0.99, __LINE__);
                chk(r->stats.classTexels[0] > 0 && r->stats.classTexels[1] > 0 && near(float(r->stats.classRatio[1]), 1.0f, 0.03f), __LINE__);
            }
        }
    }

    // The light grid (stage 4): the basis, the CPU tracer's sphere terms, and the grid bake's correction.
    {
        const auto g = gridBasis();
        for (int k = 0; k < 8; ++k) {
            chk(near(g[size_t(k)][0] * g[size_t(k)][0] + g[size_t(k)][1] * g[size_t(k)][1] + g[size_t(k)][2] * g[size_t(k)][2], 1.0f), __LINE__);
            for (int e = 0; e < 3; ++e) chk(near(g[size_t(k)][size_t(e)], -g[size_t(7 - k)][size_t(e)]), __LINE__); // opposite corners
            const auto w = iw2::assets::LightGrid::weights(g[size_t(k)]); // the original's weights are 1 at their own corner
            chk(near(w[size_t(k)], 1.0f, 1e-3f), __LINE__);
        }
        chk(near(g[7][2], 1.0f) && near(g[0][2], -1.0f), __LINE__); // all three signs positive: straight up
        TraceScene sc;
        sc.positions = {-4000, -4000, 0, 4000, -4000, 0, 4000, 4000, 0, -4000, 4000, 0};
        sc.vertexUv.assign(8, 0.0f);
        sc.indices = {0, 1, 2, 0, 2, 3};
        sc.tris.resize(2);
        MapLight l;
        l.pos[2] = 200, l.radius = 600, l.color[0] = 1, l.color[1] = 0.5f, l.color[2] = 0.0f;
        sc.lamps = {l};
        Config cfg;
        auto t = makeCpuTracer(sc, cfg);
        TexelPoint p{};
        p.pos[2] = 100;
        std::vector<float> v;
        chk(t->gridSky({p}, v) && v.size() == 8, __LINE__);
        chk(near(v[7], 1.0f, 0.03f) && near(v[0], 0.0f, 0.01f), __LINE__); // over a floor: the upward corner sees all sky, the downward one none
        float sum = 0;
        for (int k = 0; k < 8; ++k) sum += v[size_t(k)];
        chk(sum > 3.0f && sum < 5.0f, __LINE__);
        TexelPoint high = p;
        high.pos[2] = 100000.0f; // far above the floor: the rays down do not reach it, but there is no sky below the horizon either
        chk(t->gridSky({high}, v) && near(v[0], 0.0f, 0.01f) && near(v[7], 1.0f, 0.03f), __LINE__);
        chk(t->gridLamps({p}, v) && v.size() == 24, __LINE__);
        const float f = (1.0f - 100.0f / 600.0f) * (1.0f - 100.0f / 600.0f);
        chk(near(v[7], f, 1e-3f) && near(v[8 + 7], 0.5f * f, 1e-3f) && near(v[16 + 7], 0.0f) && near(v[0], 0.0f), __LINE__); // the lamp is straight above
        // a box around the point with albedo .5 and light 2 everywhere: every ray hits, so each corner gets albedo * light * 1
        TraceScene box;
        const float L = 300.0f;
        const float corners[8][3] = {{-L, -L, -L}, {L, -L, -L}, {L, L, -L}, {-L, L, -L}, {-L, -L, L}, {L, -L, L}, {L, L, L}, {-L, L, L}};
        for (const auto& c : corners) box.positions.insert(box.positions.end(), c, c + 3);
        box.vertexUv.assign(16, 0.0f);
        box.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
        box.tris.resize(12);
        for (SceneTri& st : box.tris) st.light[0] = st.light[1] = st.light[2] = 2.0f, st.tex = 0;
        box.albedo = {0.5f, 0.5f, 0.5f};
        Config bc;
        bc.albedoSat = 1.0f;
        auto tb = makeCpuTracer(box, bc);
        TexelPoint origin{};
        chk(tb->gridBounce({origin}, v) && v.size() == 24, __LINE__);
        for (int k = 0; k < 8; ++k) chk(near(v[size_t(k)], 1.0f, 0.06f), __LINE__);
        // the grid bake: re-baked 0.5 of everything against shipped 0.8: the correction is 1.6, the share within 25 % goes from 0 to 1
        struct Flat : Tracer {
            bool sun(const std::vector<TexelPoint>&, const float*, std::vector<float>&) override { return true; }
            bool sky(const std::vector<TexelPoint>&, std::vector<float>&) override { return true; }
            bool gridSky(const std::vector<TexelPoint>& pts, std::vector<float>& o) override { o.assign(pts.size() * 8, 1.0f); return true; }
        } flat;
        BakeResult r;
        r.terms.valid = true;
        r.terms.sky[0] = r.terms.sky[1] = r.terms.sky[2] = 0.5f;
        std::vector<std::array<float, 3>> pts(100, {0, 0, 0});
        std::vector<std::array<float, 24>> shipped(100);
        for (auto& sh : shipped) sh.fill(0.8f);
        chk(bakeGrid(flat, r, pts, shipped), __LINE__);
        chk(r.grid.size() == 100 && near(r.stats.gridCorrection[0], 1.6f, 0.06f) && near(r.grid[3][5], 0.8f, 0.03f), __LINE__);
        chk(near(float(r.stats.gridRatioBefore), 0.625f, 0.01f) && near(float(r.stats.gridRatio), 1.0f, 0.04f), __LINE__);
        chk(r.stats.gridWithin25Before == 0.0 && r.stats.gridWithin25 == 1.0 && r.stats.gridPoints == 100, __LINE__);
        std::vector<std::array<float, 24>> wrong(99);
        chk(!bakeGrid(flat, r, pts, wrong), __LINE__);
        // the grid goes through the cache file
        const std::string path = (std::filesystem::temp_directory_path() / "iw2_rebake_grid_test.bin").string();
        chk(saveCache(path, r), __LINE__);
        const auto back = loadCache(path);
        chk(back.has_value() && back->grid.size() == 100 && back->grid[7][9] == r.grid[7][9] && back->stats.gridPoints == 100, __LINE__);
        std::filesystem::remove(path);
    }

    // Cache keys: the same inputs give the same key, any change of the map, the settings or the sun gives another.
    {
        const float s1[3] = {0, 0, 1}, s2[3] = {0, 0.1f, 1};
        const Config cfg;
        Config cfg2;
        cfg2.sunConeDegrees = 1.0f;
        const std::string a = cacheKey(42, settingsString(cfg, s1, {})), b = cacheKey(42, settingsString(cfg, s1, {}));
        chk(a == b, __LINE__);
        chk(a != cacheKey(43, settingsString(cfg, s1, {})), __LINE__);
        chk(a != cacheKey(42, settingsString(cfg2, s1, {})), __LINE__);
        chk(a != cacheKey(42, settingsString(cfg, s2, {})), __LINE__);
        MapLight l;
        chk(a != cacheKey(42, settingsString(cfg, s1, {l})), __LINE__);
        chk(a.find('/') == std::string::npos && a.find("-v8") != std::string::npos, __LINE__);
        chk(fnv1a64("a", 1) != fnv1a64("b", 1) && fnv1a64("", 0) == 1469598103934665603ull, __LINE__);
        setenv("IW2_REBAKE_CACHE", "/nonexistent-cache", 1);
        chk(cacheDir() == "/nonexistent-cache", __LINE__);
        unsetenv("IW2_REBAKE_CACHE");
        chk(cacheDir().find("rebake") != std::string::npos || !std::getenv("HOME"), __LINE__);
    }

    // The debug export writes one position and one normal image per lightmap with triangles.
    {
        const auto dir = std::filesystem::temp_directory_path() / "iw2_rebake_dump_test";
        std::filesystem::remove_all(dir);
        std::vector<std::vector<LmTriangle>> tris(2);
        tris[1] = quad(0.25f, 0.25f, 0.75f, 0.75f);
        chk(exportTexelImages(dir.string(), tris, 64) == 1, __LINE__);
        chk(std::filesystem::file_size(dir / "lm01_pos.ppm") == 64u * 64u * 3u + std::strlen("P6\n64 64\n255\n"), __LINE__);
        chk(std::filesystem::exists(dir / "lm01_normal.ppm") && !std::filesystem::exists(dir / "lm00_pos.ppm"), __LINE__);
        std::ifstream f(dir / "lm01_normal.ppm", std::ios::binary);
        std::string head;
        std::getline(f, head);
        chk(head == "P6", __LINE__);
        std::filesystem::remove_all(dir);
    }

    // toujane: the triangles of the world's lightmaps rasterise into texels with unit normals and orthonormal frames; the shared (overlapping)
    // fraction is small; every covered texel's position lies on its chart's surface (checked through the normal: the plane of the texel
    // contains the position of the vertex the texel's triangle started from, not needed here - the unit checks are enough).
    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        const auto data = vfs.read("maps/toujane.d3dbsp");
        chk(data.has_value(), __LINE__);
        if (data) {
            const iw2::assets::D3dBsp bsp(*data);
            const auto tris = collectLightmapTriangles(bsp);
            chk(tris.size() == bsp.lightmapCount() && bsp.lightmapCount() > 0, __LINE__);
            size_t withTris = 0, covered = 0, shared = 0, badFrames = 0;
            for (size_t li = 0; li < tris.size() && withTris < 4; ++li) {
                if (tris[li].empty()) continue;
                ++withTris;
                const TexelSet s = rasterizeTexels(kCoeffRes, tris[li]);
                covered += s.inside + s.edge, shared += s.shared;
                for (const Texel& t : s.texels) {
                    if (t.state == TexelState::Unset || t.state == TexelState::Shared) continue;
                    const float nn = t.n[0] * t.n[0] + t.n[1] * t.n[1] + t.n[2] * t.n[2], tn = t.t[0] * t.n[0] + t.t[1] * t.n[1] + t.t[2] * t.n[2];
                    if (!near(nn, 1.0f, 1e-3f) || std::fabs(tn) > 1e-3f) ++badFrames;
                }
            }
            chk(withTris == 4 && covered > 1000 && badFrames == 0 && shared * 20 < covered, __LINE__);
        }
    }

    std::printf(bad ? "FAILED: %d\n" : "rebake: ok\n", bad);
    return bad ? 1 : 0;
}
