// Metal ray tracing for the lighting re-bake (rebake.h, docs/render-rebake.md). Our own kernels.
#include "rebake.h"

#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
#import <Metal/Metal.h>
#endif

namespace iw2::rebake {

#ifdef SDL_GPU_OPENIW2_METAL_HANDLES

namespace {

// Texel points are 12 floats each (pos, normal, tangent, binormal: game::rebake::TexelPoint); the kernels read them as plain floats.
// The same sampling as the CPU reference tracer (game::rebake::makeCpuTracer), which the tests and IW2_REBAKE_VERIFY compare with.
const char* kSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
using namespace raytracing;
struct BakeU {
    float4 sun;      // xyz: towards the sun, w: tan of the cone's half angle
    float4 basis[4]; // the four lightmap basis directions in tangent space
    uint4 p;         // x: first texel of this dispatch, y: texel count of the buffer, z: rays, w: lamp count
    float4 q;        // x: ray origin offset along the normal, y: longest ray, z: lamp size
    float4 gbasis[8]; // the light grid's eight directions (world space)
};
static float hash01(uint k) {
    k = (k ^ 61u) ^ (k >> 16);
    k *= 9u;
    k = k ^ (k >> 4);
    k *= 0x27d4eb2du;
    k = k ^ (k >> 15);
    return float(k & 0xffffffu) / 16777216.0;
}
static float3 f3(const device float* t, uint o) { return float3(t[o], t[o + 1], t[o + 2]); }
// Alpha test of a candidate triangle: the table row of its geometry is (first triangle, mask layer or ~0 for opaque, function).
static bool alpha_pass(uint geom, uint prim, float2 bary, const device uint* idx, const device float* uv, const device uint4* tab,
                       texture2d_array<float> masks) {
    const uint4 t = tab[geom];
    if (t.y == 0xffffffffu) return true;
    const uint base = (t.x + prim) * 3u;
    const uint i0 = idx[base], i1 = idx[base + 1], i2 = idx[base + 2];
    const float2 c = float2(uv[i0 * 2], uv[i0 * 2 + 1]) * (1.0 - bary.x - bary.y) + float2(uv[i1 * 2], uv[i1 * 2 + 1]) * bary.x +
                     float2(uv[i2 * 2], uv[i2 * 2 + 1]) * bary.y;
    constexpr sampler ms(filter::nearest, address::repeat);
    const float a = rint(saturate(masks.sample(ms, c, t.y).r) * 255.0);
    return t.z == 1u ? a >= 128.0 : t.z == 2u ? a > 0.0 : t.z == 3u ? a < 128.0 : true;
}
static bool occluded(primitive_acceleration_structure as, float3 o, float3 d, float len, const device uint* idx, const device float* uv,
                     const device uint4* tab, texture2d_array<float> masks) {
    ray r(o, d, 0.0, len);
    intersection_query<triangle_data> q;
    intersection_params qp;
    qp.assume_geometry_type(geometry_type::triangle);
    qp.accept_any_intersection(true);
    q.reset(r, as, qp);
    while (q.next())
        if (alpha_pass(q.get_candidate_geometry_id(), q.get_candidate_primitive_id(), q.get_candidate_triangle_barycentric_coord(), idx, uv, tab, masks))
            q.commit_triangle_intersection();
    return q.get_committed_intersection_type() != intersection_type::none;
}
#define SCENE_ARGS primitive_acceleration_structure as [[buffer(3)]], const device uint* idx [[buffer(4)]], const device float* uv [[buffer(5)]], \
    const device uint4* tab [[buffer(6)]], texture2d_array<float> masks [[texture(0)]]
#define OCC(o, d, len) occluded(as, o, d, len, idx, uv, tab, masks)

kernel void rebake_sun(const device float* tx [[buffer(0)]], device float* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                       uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12), N = f3(tx, i * 12 + 3);
    const float3 sd = u.sun.xyz;
    if (dot(N, sd) <= 0.0) { out[i] = 0.0; return; } // facing away: no direct sun (the runtime multiplies N.L anyway)
    const float3 t1 = normalize(cross(sd, abs(sd.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0))), t2 = cross(sd, t1);
    const float rot = hash01(i) * 6.2831853;
    const float3 o = P + N * u.q.x;
    uint lit = 0;
    for (uint k = 0; k < u.p.z; ++k) {
        const float rr = sqrt((float(k) + 0.5) / float(u.p.z)) * u.sun.w, a = float(k) * 2.3999632 + rot;
        const float3 d = normalize(sd + (t1 * cos(a) + t2 * sin(a)) * rr);
        lit += OCC(o, d, u.q.y) ? 0u : 1u;
    }
    out[i] = float(lit) / float(u.p.z);
}
kernel void rebake_sky(const device float* tx [[buffer(0)]], device float4* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                       uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12), N = f3(tx, i * 12 + 3), T = f3(tx, i * 12 + 6), B = f3(tx, i * 12 + 9);
    const float3 o = P + N * u.q.x;
    const float rot = hash01(i * 2u + 1u) * 6.2831853;
    float4 acc = float4(0.0);
    for (uint k = 0; k < u.p.z; ++k) {
        const float z = (float(k) + 0.5) / float(u.p.z), rxy = sqrt(max(0.0, 1.0 - z * z)), a = float(k) * 2.3999632 + rot;
        const float3 l = float3(rxy * cos(a), rxy * sin(a), z); // uniform over the hemisphere (equal steps in z)
        if (OCC(o, T * l.x + B * l.y + N * l.z, u.q.y)) continue;
        acc += max(float4(dot(l, u.basis[0].xyz), dot(l, u.basis[1].xyz), dot(l, u.basis[2].xyz), dot(l, u.basis[3].xyz)), float4(0.0));
    }
    out[i] = acc * (2.0 / float(u.p.z));
}
// Lamps: three float4 each (position + radius, colour + cos outer, direction + cos inner), game::mapLightFalloff.
static float lamp_falloff(float3 pos, float radius, float3 dir, float cosOuter, float cosInner, float3 p) {
    const float3 v = pos - p;
    const float d = length(v);
    if (d >= radius) return 0.0;
    float f = (1.0 - d / radius) * (1.0 - d / radius);
    if (cosOuter >= -1.0 && d > 0.0) {
        const float c = -dot(v, dir) / d;
        const float t = clamp((c - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
        f *= t * t * (3.0 - 2.0 * t);
    }
    return f;
}
kernel void rebake_lamps(const device float* tx [[buffer(0)]], device float* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                         const device float4* lamps [[buffer(7)]], uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12), N = f3(tx, i * 12 + 3), T = f3(tx, i * 12 + 6), B = f3(tx, i * 12 + 9);
    const float3 o = P + N * u.q.x;
    float acc[12];
    for (int e = 0; e < 12; ++e) acc[e] = 0.0;
    for (uint li = 0; li < u.p.w; ++li) {
        const float4 a = lamps[li * 3], b = lamps[li * 3 + 1], c = lamps[li * 3 + 2];
        const float3 v = a.xyz - P;
        const float d = length(v);
        if (d >= a.w || d < 1.0) continue;
        const float f = lamp_falloff(a.xyz, a.w, c.xyz, b.w, c.w, P);
        const float3 dir = v / d;
        if (dot(dir, N) <= 0.0 || f <= 0.0) continue;
        float vis = 0.0;
        for (uint r = 0; r < u.p.z; ++r) {
            const uint h = i * 977u + li * 131u + r * 7u;
            const float3 j = float3(hash01(h + 1u), hash01(h + 2u), hash01(h + 3u)) * 2.0 - 1.0;
            const float3 to = a.xyz + j * u.q.z - o;
            const float len = length(to);
            vis += OCC(o, to / len, len - 1.0) ? 0.0 : 1.0;
        }
        vis /= float(u.p.z);
        for (int k = 0; k < 4; ++k) {
            const float3 nk = T * u.basis[k].x + B * u.basis[k].y + N * u.basis[k].z;
            const float w = f * vis * max(0.0, dot(nk, dir));
            acc[k] += b.x * w, acc[4 + k] += b.y * w, acc[8 + k] += b.z * w;
        }
    }
    for (int e = 0; e < 12; ++e) out[i * 12 + uint(e)] = acc[e];
}
// One bounce: the closest hit of each hemisphere ray, its albedo times the irradiance cache at its lightmap coordinates.
kernel void rebake_bounce(const device float* tx [[buffer(0)]], device float* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                          const device float* tris [[buffer(7)]], const device float4* albedo [[buffer(8)]], const device float4* cache [[buffer(9)]],
                          uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12), N = f3(tx, i * 12 + 3), T = f3(tx, i * 12 + 6), B = f3(tx, i * 12 + 9);
    const float3 o = P + N * u.q.x;
    const float rot = hash01(i * 2u + 7u) * 6.2831853;
    float acc[12];
    for (int e = 0; e < 12; ++e) acc[e] = 0.0;
    for (uint k = 0; k < u.p.z; ++k) {
        const float z = (float(k) + 0.5) / float(u.p.z), rxy = sqrt(max(0.0, 1.0 - z * z)), a = float(k) * 2.3999632 + rot;
        const float3 l = float3(rxy * cos(a), rxy * sin(a), z);
        ray r(o, T * l.x + B * l.y + N * l.z, 0.0, u.q.y);
        intersection_query<triangle_data> q;
        intersection_params qp;
        qp.assume_geometry_type(geometry_type::triangle);
        q.reset(r, as, qp);
        while (q.next())
            if (alpha_pass(q.get_candidate_geometry_id(), q.get_candidate_primitive_id(), q.get_candidate_triangle_barycentric_coord(), idx, uv, tab, masks))
                q.commit_triangle_intersection();
        if (q.get_committed_intersection_type() == intersection_type::none) continue;
        const uint tri = tab[q.get_committed_geometry_id()].x + q.get_committed_primitive_id();
        const float2 bc = q.get_committed_triangle_barycentric_coord();
        const device float* t = tris + tri * 12u;
        const int lm = as_type<int>(t[6]), tex = as_type<int>(t[7]);
        float3 e = float3(t[8], t[9], t[10]);
        if (lm >= 0) {
            const float w0 = 1.0 - bc.x - bc.y;
            const float2 c = float2(t[0], t[1]) * w0 + float2(t[2], t[3]) * bc.x + float2(t[4], t[5]) * bc.y;
            const int x = clamp(int(c.x * 512.0), 0, 511), y = clamp(int(c.y * 512.0), 0, 511);
            e = cache[(uint(lm) * 512u + uint(y)) * 512u + uint(x)].rgb;
        }
        float3 alb = tex >= 0 ? albedo[tex].rgb : float3(0.5);
        if (u.sun.x != 1.0) alb = pow(max(alb, float3(0.0)), float3(u.sun.x)); // game::rebake::shapeAlbedo
        alb = float3(dot(alb, float3(0.2126, 0.7152, 0.0722))) + (alb - float3(dot(alb, float3(0.2126, 0.7152, 0.0722)))) * u.sun.y;
        const float3 rad = alb * e;
        const float4 pj = max(float4(dot(l, u.basis[0].xyz), dot(l, u.basis[1].xyz), dot(l, u.basis[2].xyz), dot(l, u.basis[3].xyz)), float4(0.0));
        for (int c = 0; c < 3; ++c)
            for (int j = 0; j < 4; ++j) acc[c * 4 + j] += rad[c] * pj[j];
    }
    for (int e = 0; e < 12; ++e) out[i * 12 + uint(e)] = acc[e] * (2.0 / float(u.p.z));
}
// The light grid: the same terms over the whole sphere around a free point (sphere directions: equal steps in height), eight basis directions.
static float3 sphere_dir(uint k, uint n, float rot) {
    const float z = 1.0 - 2.0 * (float(k) + 0.5) / float(n), r = sqrt(max(0.0, 1.0 - z * z)), a = float(k) * 2.3999632 + rot;
    return float3(r * cos(a), r * sin(a), z);
}
kernel void rebake_grid_sky(const device float* tx [[buffer(0)]], device float* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                            uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12);
    const float rot = hash01(i * 2u + 11u) * 6.2831853;
    float acc[8];
    for (int e = 0; e < 8; ++e) acc[e] = 0.0;
    for (uint k = 0; k < u.p.z; ++k) {
        const float3 d = sphere_dir(k, u.p.z, rot);
        if (d.z <= 0.0 || OCC(P, d, u.q.y)) continue; // the sky is a dome: nothing below the horizon
        for (int j = 0; j < 8; ++j) acc[j] += max(0.0, dot(d, u.gbasis[j].xyz));
    }
    for (int j = 0; j < 8; ++j) out[i * 8 + uint(j)] = acc[j] * (4.0 / float(u.p.z));
}
kernel void rebake_grid_lamps(const device float* tx [[buffer(0)]], device float* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                              const device float4* lamps [[buffer(7)]], uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12);
    float acc[24];
    for (int e = 0; e < 24; ++e) acc[e] = 0.0;
    for (uint li = 0; li < u.p.w; ++li) {
        const float4 a = lamps[li * 3], b = lamps[li * 3 + 1], c = lamps[li * 3 + 2];
        const float3 v = a.xyz - P;
        const float d = length(v);
        if (d >= a.w || d < 1.0) continue;
        const float f = lamp_falloff(a.xyz, a.w, c.xyz, b.w, c.w, P);
        if (f <= 0.0) continue;
        const float3 dir = v / d;
        float vis = 0.0;
        for (uint r = 0; r < u.p.z; ++r) {
            const uint h = i * 977u + li * 131u + r * 7u;
            const float3 j = float3(hash01(h + 1u), hash01(h + 2u), hash01(h + 3u)) * 2.0 - 1.0;
            const float3 to = a.xyz + j * u.q.z - P;
            const float len = length(to);
            vis += OCC(P, to / len, len - 1.0) ? 0.0 : 1.0;
        }
        vis /= float(u.p.z);
        for (int k = 0; k < 8; ++k) {
            const float w = f * vis * max(0.0, dot(u.gbasis[k].xyz, dir));
            acc[k] += b.x * w, acc[8 + k] += b.y * w, acc[16 + k] += b.z * w;
        }
    }
    for (int e = 0; e < 24; ++e) out[i * 24 + uint(e)] = acc[e];
}
kernel void rebake_grid_bounce(const device float* tx [[buffer(0)]], device float* out [[buffer(1)]], constant BakeU& u [[buffer(2)]], SCENE_ARGS,
                               const device float* tris [[buffer(7)]], const device float4* albedo [[buffer(8)]], const device float4* cache [[buffer(9)]],
                               uint gid [[thread_position_in_grid]]) {
    const uint i = u.p.x + gid;
    if (i >= u.p.y) return;
    const float3 P = f3(tx, i * 12);
    const float rot = hash01(i * 2u + 13u) * 6.2831853;
    float acc[24];
    for (int e = 0; e < 24; ++e) acc[e] = 0.0;
    for (uint k = 0; k < u.p.z; ++k) {
        const float3 d = sphere_dir(k, u.p.z, rot);
        ray r(P, d, 0.0, u.q.y);
        intersection_query<triangle_data> q;
        intersection_params qp;
        qp.assume_geometry_type(geometry_type::triangle);
        q.reset(r, as, qp);
        while (q.next())
            if (alpha_pass(q.get_candidate_geometry_id(), q.get_candidate_primitive_id(), q.get_candidate_triangle_barycentric_coord(), idx, uv, tab, masks))
                q.commit_triangle_intersection();
        if (q.get_committed_intersection_type() == intersection_type::none) continue;
        const uint tri = tab[q.get_committed_geometry_id()].x + q.get_committed_primitive_id();
        const float2 bc = q.get_committed_triangle_barycentric_coord();
        const device float* t = tris + tri * 12u;
        const int lm = as_type<int>(t[6]), tex = as_type<int>(t[7]);
        float3 e = float3(t[8], t[9], t[10]);
        if (lm >= 0) {
            const float w0 = 1.0 - bc.x - bc.y;
            const float2 c = float2(t[0], t[1]) * w0 + float2(t[2], t[3]) * bc.x + float2(t[4], t[5]) * bc.y;
            const int x = clamp(int(c.x * 512.0), 0, 511), y = clamp(int(c.y * 512.0), 0, 511);
            e = cache[(uint(lm) * 512u + uint(y)) * 512u + uint(x)].rgb;
        }
        float3 alb = tex >= 0 ? albedo[tex].rgb : float3(0.5);
        if (u.sun.x != 1.0) alb = pow(max(alb, float3(0.0)), float3(u.sun.x));
        alb = float3(dot(alb, float3(0.2126, 0.7152, 0.0722))) + (alb - float3(dot(alb, float3(0.2126, 0.7152, 0.0722)))) * u.sun.y;
        const float3 rad = alb * e;
        for (int j = 0; j < 8; ++j) {
            const float w = max(0.0, dot(d, u.gbasis[j].xyz));
            acc[j] += rad.x * w, acc[8 + j] += rad.y * w, acc[16 + j] += rad.z * w;
        }
    }
    for (int e = 0; e < 24; ++e) out[i * 24 + uint(e)] = acc[e] * (4.0 / float(u.p.z));
}
// The colour maps: albedo (mean of an 8 x 8 grid at a small mip level, weighted by alpha, game::rebake::meanAlbedo) and the 64 x 64 alpha masks.
kernel void rebake_albedo(texture2d<float> src [[texture(0)]], device float4* out [[buffer(0)]], constant uint& slot [[buffer(1)]],
                          uint gid [[thread_position_in_grid]]) {
    if (gid > 0u) return;
    constexpr sampler s(filter::linear, mip_filter::linear, address::repeat);
    const float lod = max(log2(float(max(src.get_width(), src.get_height()))) - 3.0, 0.0);
    float3 acc = float3(0.0);
    float w = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const float4 c = src.sample(s, (float2(x, y) + 0.5) / 8.0, level(lod));
            acc += c.rgb * c.a, w += c.a;
        }
    out[slot] = float4(w > 0.0 ? acc / w : float3(0.0), 1.0);
}
kernel void rebake_mask(texture2d<float> src [[texture(0)]], texture2d_array<float, access::write> dst [[texture(1)]], constant uint& layer [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= 64u || gid.y >= 64u) return;
    constexpr sampler s(filter::linear, mip_filter::linear, address::repeat);
    const float lod = max(log2(float(src.get_width()) / 64.0), 0.0);
    dst.write(float4(src.sample(s, (float2(gid) + 0.5) / 64.0, level(lod)).a), gid, layer);
}
)MSL";

struct BakeU {
    float sun[4];
    float basis[4][4];
    uint32_t p[4];
    float q[4];
    float gbasis[8][4];
};

class MetalTracer final : public game::rebake::Tracer {
public:
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLAccelerationStructure> as = nil;
    id<MTLComputePipelineState> sunPipe = nil, skyPipe = nil, lampPipe = nil, bouncePipe = nil, albedoPipe = nil, maskPipe = nil, gskyPipe = nil, glampPipe = nil, gbouncePipe = nil;
    id<MTLBuffer> idx = nil, uv = nil, tab = nil, lampBuf = nil, triBuf = nil, albedoBuf = nil, cacheBuf = nil;
    id<MTLTexture> masks = nil;
    game::rebake::Config cfg;
    uint32_t lampCount = 0;
    std::string lastError;

    bool sun(const std::vector<game::rebake::TexelPoint>& pts, const float towardsSun[3], std::vector<float>& vis) override {
        BakeU u = base(pts.size(), unsigned(std::max(cfg.sunRays, 1)));
        const float c = std::tan(cfg.sunConeDegrees * 3.14159265f / 180.0f);
        float l = std::sqrt(towardsSun[0] * towardsSun[0] + towardsSun[1] * towardsSun[1] + towardsSun[2] * towardsSun[2]);
        if (l < 1e-6f) l = 1.0f;
        u.sun[0] = towardsSun[0] / l, u.sun[1] = towardsSun[1] / l, u.sun[2] = towardsSun[2] / l, u.sun[3] = c;
        vis.assign(pts.size(), 0.0f);
        return run(sunPipe, pts, sizeof(float), u, vis.data(), 32768, {});
    }
    bool sky(const std::vector<game::rebake::TexelPoint>& pts, std::vector<float>& v4) override {
        BakeU u = base(pts.size(), unsigned(std::max(cfg.skyRays, 1)));
        v4.assign(pts.size() * 4, 0.0f);
        return run(skyPipe, pts, 4 * sizeof(float), u, v4.data(), 32768, {});
    }
    bool lamps(const std::vector<game::rebake::TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 12, 0.0f);
        if (!lampCount) return true;
        BakeU u = base(pts.size(), unsigned(std::max(cfg.lampRays, 1)));
        u.p[3] = lampCount;
        u.q[2] = cfg.lampSize;
        return run(lampPipe, pts, 12 * sizeof(float), u, out.data(), 16384, {lampBuf});
    }
    bool gridSky(const std::vector<game::rebake::TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 8, 0.0f);
        BakeU u = base(pts.size(), unsigned(std::max(cfg.skyRays, 1)));
        return run(gskyPipe, pts, 8 * sizeof(float), u, out.data(), 32768, {});
    }
    bool gridLamps(const std::vector<game::rebake::TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 24, 0.0f);
        if (!lampCount) return true;
        BakeU u = base(pts.size(), unsigned(std::max(cfg.lampRays, 1)));
        u.p[3] = lampCount;
        u.q[2] = cfg.lampSize;
        return run(glampPipe, pts, 24 * sizeof(float), u, out.data(), 16384, {lampBuf});
    }
    bool gridBounce(const std::vector<game::rebake::TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 24, 0.0f);
        if (!cacheBuf) return lastError = "no irradiance cache", false;
        BakeU u = base(pts.size(), unsigned(std::max(cfg.bounceRays, 1)));
        u.sun[0] = cfg.albedoGamma, u.sun[1] = cfg.albedoSat;
        return run(gbouncePipe, pts, 24 * sizeof(float), u, out.data(), 8192, {triBuf, albedoBuf, cacheBuf});
    }
    void setCache(const std::vector<float>& c) override {
        cacheBuf = [dev newBufferWithBytes:c.data() length:std::max<size_t>(c.size() * 4, 16) options:MTLResourceStorageModeShared];
    }
    bool bounce(const std::vector<game::rebake::TexelPoint>& pts, std::vector<float>& out) override {
        out.assign(pts.size() * 12, 0.0f);
        if (!cacheBuf) return lastError = "no irradiance cache", false;
        BakeU u = base(pts.size(), unsigned(std::max(cfg.bounceRays, 1)));
        u.sun[0] = cfg.albedoGamma, u.sun[1] = cfg.albedoSat;
        return run(bouncePipe, pts, 12 * sizeof(float), u, out.data(), 8192, {triBuf, albedoBuf, cacheBuf});
    }

private:
    BakeU base(size_t n, unsigned rays) const {
        BakeU u{};
        const auto basis = game::rebake::lightmapBasis();
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) u.basis[k][a] = basis[size_t(k)][size_t(a)];
        const auto gb = game::rebake::gridBasis();
        for (int k = 0; k < 8; ++k)
            for (int a = 0; a < 3; ++a) u.gbasis[k][a] = gb[size_t(k)][size_t(a)];
        u.p[1] = uint32_t(n), u.p[2] = rays;
        u.q[0] = 1.0f;     // origin offset along the normal (world units)
        u.q[1] = 20000.0f; // longest ray: further than any map is wide
        return u;
    }
    // Chunks of the texel range, one command buffer each (no single submission runs long enough for the GPU watchdog). `extra` are the
    // kernel's buffers from index 7 on.
    bool run(id<MTLComputePipelineState> pipe, const std::vector<game::rebake::TexelPoint>& pts, size_t stride, BakeU u, float* out, size_t chunk,
             const std::vector<id<MTLBuffer>>& extra) {
        if (pts.empty()) return true;
        @autoreleasepool {
            id<MTLBuffer> in = [dev newBufferWithBytes:pts.data() length:pts.size() * sizeof(game::rebake::TexelPoint) options:MTLResourceStorageModeShared];
            id<MTLBuffer> ob = [dev newBufferWithLength:pts.size() * stride options:MTLResourceStorageModeShared];
            if (!in || !ob) return lastError = "buffer allocation failed", false;
            id<MTLCommandBuffer> last = nil;
            for (size_t first = 0; first < pts.size(); first += chunk) {
                u.p[0] = uint32_t(first);
                const size_t count = std::min(chunk, pts.size() - first);
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:pipe];
                [enc setBuffer:in offset:0 atIndex:0];
                [enc setBuffer:ob offset:0 atIndex:1];
                [enc setBytes:&u length:sizeof u atIndex:2];
                [enc setAccelerationStructure:as atBufferIndex:3];
                [enc useResource:as usage:MTLResourceUsageRead];
                [enc setBuffer:idx offset:0 atIndex:4];
                [enc setBuffer:uv offset:0 atIndex:5];
                [enc setBuffer:tab offset:0 atIndex:6];
                [enc setTexture:masks atIndex:0];
                for (size_t e = 0; e < extra.size(); ++e) [enc setBuffer:extra[e] offset:0 atIndex:7 + e];
                const NSUInteger tw = std::min<NSUInteger>(pipe.maxTotalThreadsPerThreadgroup, 64);
                [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
                [enc endEncoding];
                [cb commit];
                if (last) { // keep at most two chunks in flight
                    [last waitUntilCompleted];
                    if (last.status != MTLCommandBufferStatusCompleted) return lastError = last.error.localizedDescription.UTF8String, false;
                }
                last = cb;
            }
            [last waitUntilCompleted];
            if (last.status != MTLCommandBufferStatusCompleted) return lastError = last.error.localizedDescription.UTF8String, false;
            std::memcpy(out, ob.contents, pts.size() * stride);
        }
        return true;
    }
};

} // namespace

bool available(SDL_GPUDevice* dev) {
    id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(dev);
    return d && d.supportsRaytracing && !std::getenv("IW2_NO_REBAKE");
}

std::unique_ptr<game::rebake::Tracer> makeTracer(SDL_GPUDevice* sdl, const game::rebake::TraceScene& sc, const game::rebake::Config& cfg, std::string& error) {
    id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(sdl);
    if (!d || !d.supportsRaytracing) return error = "no Metal ray tracing", nullptr;
    if (sc.indices.size() < 3 || sc.positions.empty()) return error = "no occluders", nullptr;
    auto t = std::make_unique<MetalTracer>();
    t->dev = d;
    t->cfg = cfg;
    t->queue = [d newCommandQueue];
    NSError* err = nil;
    id<MTLLibrary> lib = [d newLibraryWithSource:[NSString stringWithUTF8String:kSource] options:nil error:&err];
    if (!lib) return error = std::string("kernel compile: ") + (err ? err.localizedDescription.UTF8String : "?"), nullptr;
    auto pipe = [&](NSString* name) { return [d newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&err]; };
    t->sunPipe = pipe(@"rebake_sun"), t->skyPipe = pipe(@"rebake_sky"), t->lampPipe = pipe(@"rebake_lamps"), t->bouncePipe = pipe(@"rebake_bounce");
    t->albedoPipe = pipe(@"rebake_albedo"), t->maskPipe = pipe(@"rebake_mask");
    t->gskyPipe = pipe(@"rebake_grid_sky"), t->glampPipe = pipe(@"rebake_grid_lamps"), t->gbouncePipe = pipe(@"rebake_grid_bounce");
    if (!t->sunPipe || !t->skyPipe || !t->lampPipe || !t->bouncePipe || !t->albedoPipe || !t->maskPipe || !t->gskyPipe || !t->glampPipe || !t->gbouncePipe)
        return error = std::string("kernel pipeline: ") + (err ? err.localizedDescription.UTF8String : "?"), nullptr;
    @autoreleasepool {
        // the geometry groups (one opaque group over everything when the caller gave none)
        std::vector<game::rebake::GeomRange> ranges = sc.ranges;
        if (ranges.empty()) ranges.push_back({0, uint32_t(sc.indices.size()), -1, 1, -1});
        id<MTLBuffer> vb = [d newBufferWithBytes:sc.positions.data() length:sc.positions.size() * 4 options:MTLResourceStorageModeShared];
        t->idx = [d newBufferWithBytes:sc.indices.data() length:sc.indices.size() * 4 options:MTLResourceStorageModeShared];
        std::vector<float> uvs = sc.vertexUv;
        uvs.resize(std::max<size_t>(uvs.size(), sc.positions.size() / 3 * 2), 0.0f);
        t->uv = [d newBufferWithBytes:uvs.data() length:uvs.size() * 4 options:MTLResourceStorageModeShared];
        std::vector<uint32_t> tab;
        NSMutableArray<MTLAccelerationStructureTriangleGeometryDescriptor*>* geoms = [NSMutableArray array];
        int32_t layers = 0;
        for (const auto& r : ranges) {
            if (r.count < 3) continue;
            MTLAccelerationStructureTriangleGeometryDescriptor* g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            g.vertexBuffer = vb;
            g.vertexStride = 12;
            g.vertexFormat = MTLAttributeFormatFloat3;
            g.indexBuffer = t->idx;
            g.indexBufferOffset = NSUInteger(r.first) * 4;
            g.indexType = MTLIndexTypeUInt32;
            g.triangleCount = r.count / 3;
            g.opaque = r.layer < 0 ? YES : NO;
            [geoms addObject:g];
            tab.insert(tab.end(), {r.first / 3, r.layer < 0 ? 0xffffffffu : uint32_t(r.layer), r.func, 0u});
            layers = std::max(layers, r.layer + 1);
        }
        t->tab = [d newBufferWithBytes:tab.data() length:tab.size() * 4 options:MTLResourceStorageModeShared];
        MTLPrimitiveAccelerationStructureDescriptor* desc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        desc.geometryDescriptors = geoms;
        const MTLAccelerationStructureSizes sz = [d accelerationStructureSizesWithDescriptor:desc];
        t->as = [d newAccelerationStructureWithSize:sz.accelerationStructureSize];
        id<MTLBuffer> scratch = [d newBufferWithLength:std::max<NSUInteger>(sz.buildScratchBufferSize, 16) options:MTLResourceStorageModePrivate];
        if (!t->as || !scratch) return error = "acceleration structure allocation failed", nullptr;
        id<MTLCommandBuffer> cb = [t->queue commandBuffer];
        id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
        [enc buildAccelerationStructure:t->as descriptor:desc scratchBuffer:scratch scratchBufferOffset:0];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) return error = "acceleration structure build failed", nullptr;

        // per-triangle bounce data, the lamps, the albedo and the alpha masks
        t->triBuf = [d newBufferWithBytes:sc.tris.data() length:std::max<size_t>(sc.tris.size() * sizeof(game::rebake::SceneTri), 16) options:MTLResourceStorageModeShared];
        static_assert(sizeof(game::rebake::SceneTri) == 48, "the bounce kernel reads 12 floats per triangle");
        std::vector<float> lamps;
        for (const auto& l : sc.lamps)
            lamps.insert(lamps.end(), {l.pos[0], l.pos[1], l.pos[2], l.radius, l.color[0], l.color[1], l.color[2], l.cosOuter, l.dir[0], l.dir[1], l.dir[2], l.cosInner});
        t->lampCount = uint32_t(sc.lamps.size());
        t->lampBuf = [d newBufferWithBytes:lamps.data() length:std::max<size_t>(lamps.size() * 4, 16) options:MTLResourceStorageModeShared];
        const size_t slots = std::max<size_t>(sc.textures.size(), 1);
        std::vector<float> albedo(slots * 4, 0.5f);
        t->albedoBuf = [d newBufferWithBytes:albedo.data() length:albedo.size() * 4 options:MTLResourceStorageModeShared];
        MTLTextureDescriptor* md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:64 height:64 mipmapped:NO];
        md.textureType = MTLTextureType2DArray;
        md.arrayLength = std::max<int32_t>(layers, 1);
        md.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        md.storageMode = MTLStorageModePrivate;
        t->masks = [d newTextureWithDescriptor:md];
        id<MTLCommandBuffer> pre = [t->queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [pre computeCommandEncoder];
        std::vector<uint8_t> usedSlot(slots, 0);
        for (const auto& tri : sc.tris)
            if (tri.tex >= 0 && size_t(tri.tex) < slots) usedSlot[size_t(tri.tex)] = 1;
        for (size_t s = 0; s < slots; ++s) {
            if (!usedSlot[s] || !sc.textures[s]) continue;
            id<MTLTexture> src = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(sdl, static_cast<SDL_GPUTexture*>(sc.textures[s]));
            if (!src || src.textureType != MTLTextureType2D) continue;
            const uint32_t slot = uint32_t(s);
            [ce setComputePipelineState:t->albedoPipe];
            [ce setTexture:src atIndex:0];
            [ce setBuffer:t->albedoBuf offset:0 atIndex:0];
            [ce setBytes:&slot length:4 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        }
        std::vector<uint8_t> doneLayer(size_t(std::max<int32_t>(layers, 1)), 0);
        for (const auto& r : ranges) {
            if (r.layer < 0 || r.tex < 0 || size_t(r.tex) >= slots || !sc.textures[size_t(r.tex)] || doneLayer[size_t(r.layer)]) continue;
            id<MTLTexture> src = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(sdl, static_cast<SDL_GPUTexture*>(sc.textures[size_t(r.tex)]));
            if (!src || src.textureType != MTLTextureType2D) continue;
            doneLayer[size_t(r.layer)] = 1;
            const uint32_t layer = uint32_t(r.layer);
            [ce setComputePipelineState:t->maskPipe];
            [ce setTexture:src atIndex:0];
            [ce setTexture:t->masks atIndex:1];
            [ce setBytes:&layer length:4 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(64, 64, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }
        [ce endEncoding];
        [pre commit];
        [pre waitUntilCompleted];
        if (pre.status != MTLCommandBufferStatusCompleted) return error = "albedo and mask kernels failed", nullptr;
        if (std::getenv("IW2_REBAKE_VERIFY")) {
            const float* a = static_cast<const float*>(t->albedoBuf.contents);
            size_t measured = 0, used = 0;
            for (size_t i = 0; i < slots; ++i) used += usedSlot[i], measured += usedSlot[i] && !(a[i * 4] == 0.5f && a[i * 4 + 1] == 0.5f && a[i * 4 + 2] == 0.5f);
            std::fprintf(stderr, "rebake: albedo measured for %zu of %zu textures in use; first used: %f %f %f\n", measured, used, double(a[4]), double(a[5]), double(a[6]));
        }
    }
    return t;
}

#else

bool available(SDL_GPUDevice*) { return false; }
std::unique_ptr<game::rebake::Tracer> makeTracer(SDL_GPUDevice*, const game::rebake::TraceScene&, const game::rebake::Config&, std::string& error) {
    error = "built without the Metal handles";
    return nullptr;
}

#endif

} // namespace iw2::rebake
