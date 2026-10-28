// Map viewer shaders (MSL). Written for this project; not derived from the game's shaders.
R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float4 color    [[attribute(2)]];
    float2 uv       [[attribute(3)]];
    float2 lmUv     [[attribute(4)]];
    float3 light    [[attribute(5)]];
};

struct VertexUniforms {
    float4x4 viewProj;
    float4 eye;
    float4x4 model;
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float4 color;
    float2 uv;
    float2 lmUv;
    float3 light;
    float3 eyeVec;
};

vertex VertexOut vs_main(VertexIn in [[stage_in]], constant VertexUniforms& u [[buffer(0)]]) {
    VertexOut o;
    o.position = u.viewProj * (u.model * float4(in.position, 1.0));
    o.position.z += u.eye.w * o.position.w; // polygon offset: constant depth bias (D3DRS_DEPTHBIAS)
    o.normal = (u.model * float4(in.normal, 0.0)).xyz;
    o.color = in.color;
    o.uv = in.uv;
    o.lmUv = in.lmUv;
    o.light = in.light;
    o.eyeVec = (u.model * float4(in.position, 1.0)).xyz - u.eye.xyz;
    return o;
}

struct FragmentUniforms {
    float4 sunDirection;   // xyz: direction towards the sun
    float4 sunColor;       // rgb: sun color * intensity
    float4 lmWeights;      // weights applied to the four lightmap coefficients
    float4 params;
    float4 ambient;        // entity light: rgb ambient from the light grid, w = sun visibility         // x: alpha test (1 = on), y: 1 = lightmap, 2 = vertex light, 3 = entity light, z: lightmap scale, w: fullbright
    float4 scroll;         // x: V offset subtracted from uv (the scroll technique subtracts the game-time value from the V coordinate)
    float4 fogParams;      // x: 1 = fog on, y: scale, z: shift (linear factor = dist * scale + shift, lib/fogcalc.hlsl)
    float4 fogColor;       // rgb
    float4 probe[6];       // light-grid coefficients R0,R1,G0,G1,B0,B1; used when sunDirection.w > 0.5
};

static float probe_smooth(float t) { return (3.0 - 2.0 * t) * t * t; }
// lightprobe.hlsl ProbeLighting(N): cube-weight the eight light-grid corner colours (mirrors assets::LightGrid::weights).
static float3 probe_lighting(float3 n, constant FragmentUniforms& u) {
    const float t0 = n.x * 0.81649655 + n.z * 0.57735026;
    const float t1 = n.x * -0.40824828 + n.y * 0.70710677 + n.z * 0.57735026;
    const float t2 = n.x * -0.40824828 - n.y * 0.70710677 + n.z * 0.57735026;
    const float m = max(max(abs(t0), abs(t1)), max(abs(t2), 1e-6));
    const float a = probe_smooth((t0 / m + 1.0) * 0.5), b = probe_smooth((t1 / m + 1.0) * 0.5);
    const float s0 = probe_smooth(0.5 - t2 / m * 0.5), s1 = probe_smooth((t2 / m + 1.0) * 0.5);
    const float4 w0 = float4((1 - a) * (1 - b), (1 - b) * a, (1 - a) * b, b * a) * s0;
    const float4 w1 = float4((1 - a) * (1 - b), (1 - b) * a, (1 - a) * b, b * a) * s1;
    return float3(dot(w0, u.probe[0]) + dot(w1, u.probe[1]), dot(w0, u.probe[2]) + dot(w1, u.probe[3]), dot(w0, u.probe[4]) + dot(w1, u.probe[5]));
}

static float3 apply_fog(float3 rgb, float dist, constant FragmentUniforms& u) {
    if (u.fogParams.x < 0.5) return rgb;
    const float f = u.fogParams.w != 0.0 ? exp(dist * u.fogParams.w) : dist * u.fogParams.y + u.fogParams.z; // Fog_ExponentialFactor / Fog_LinearFactor
    return mix(u.fogColor.rgb, rgb, clamp(f, 0.0, 1.0)); // Fog_PixelCalc
}

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant FragmentUniforms& u [[buffer(0)]],
                        texture2d<float> colorMap [[texture(0)]],
                        texture2d_array<float> lmCoeffs [[texture(1)]],
                        texture2d<float> lmSun [[texture(2)]],
                        sampler colorSampler [[sampler(0)]],
                        sampler lmSampler [[sampler(1)]],
                        sampler sunSampler [[sampler(2)]],
                        texture2d<float> detailMap [[texture(3)]],
                        sampler detailSampler [[sampler(3)]]) {
    const float2 uv = in.uv - float2(0.0, u.scroll.x);
    float4 tex0 = colorMap.sample(colorSampler, uv);
    // Detail texture (shade library, USE_DETAIL variant): the detail sample, centred on 0.5, is added to the base colour before the vertex colour multiply.
    if (u.scroll.y > 0.5) tex0.rgb += detailMap.sample(detailSampler, in.uv * u.scroll.zw).rgb - 0.5;
    float4 base = tex0 * in.color;
    if (u.params.x > 0.5 && base.a < 0.5) discard_fragment();
    const float outA = u.params.x < -0.5 ? 1.0 : base.a; // params.x -1: USE_ALPHA 0 technique (MatInfo::alphaOne)

    if (u.params.y > 3.5) { // effect_multiply: blends the texture towards white by the vertex alpha, no light
        float4 t = colorMap.sample(colorSampler, in.uv);
        return float4((1.0 - in.color.a) + t.rgb * in.color.rgb * in.color.a, 1.0);
    }
    float3 light;
    if (u.params.w > 0.5) {
        light = float3(1.0);
    } else if (u.params.y > 2.5) {
        float ndl = max(dot(normalize(in.normal), u.sunDirection.xyz), 0.0);
        const float3 amb = u.sunDirection.w > 0.5 ? probe_lighting(normalize(in.normal), u) : u.ambient.rgb;
        light = amb + u.ambient.w * ndl * u.sunColor.rgb;
    } else if (u.params.y > 1.5) {
        light = in.light; // baked per vertex from the light grid
    } else if (u.params.y > 0.5) {
        float4 r = lmCoeffs.sample(lmSampler, in.lmUv, 0);
        float4 g = lmCoeffs.sample(lmSampler, in.lmUv, 1);
        float4 b = lmCoeffs.sample(lmSampler, in.lmUv, 2);
        float3 lm = float3(dot(r, u.lmWeights), dot(g, u.lmWeights), dot(b, u.lmWeights)) * u.params.z;
        float sunVis = lmSun.sample(sunSampler, in.lmUv).r;
        float ndl = max(dot(normalize(in.normal), u.sunDirection.xyz), 0.0);
        light = lm + sunVis * ndl * u.sunColor.rgb;
    } else {
        float ndl = max(dot(normalize(in.normal), u.sunDirection.xyz), 0.0);
        light = float3(0.45) + ndl * u.sunColor.rgb * 0.6;
    }
    return float4(apply_fog(base.rgb * light, length(in.eyeVec), u), outA);
}

struct SkyOut {
    float4 position [[position]];
    float3 dir;
};

vertex SkyOut vs_sky(VertexIn in [[stage_in]], constant VertexUniforms& u [[buffer(0)]]) {
    SkyOut o;
    o.position = u.viewProj * float4(in.position, 1.0);
    o.dir = in.position - u.eye.xyz;
    return o;
}

fragment float4 fs_sky(SkyOut in [[stage_in]], texturecube<float> sky [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(sky.sample(s, in.dir).rgb, 1.0);
}
// water_l_sun.tech / lib/water.hlsl. normalMap = the FFT-generated map (src/game/watersim.cpp), only .r is read.
struct WaterOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float3 eyeVector;
};

vertex WaterOut vs_water(VertexIn in [[stage_in]], constant VertexUniforms& u [[buffer(0)]]) {
    WaterOut o;
    o.position = u.viewProj * (u.model * float4(in.position, 1.0));
    o.color = in.color;
    o.uv = in.uv;
    o.eyeVector = (u.model * float4(in.position, 1.0)).xyz - u.eye.xyz;
    return o;
}

static float water_turbulance(texture2d<float> normalMap, sampler sm, float2 uv) {
    float t = 0.0, scale = 1.0;
    for (int i = 0; i < 3; ++i) { // lacunarity 3.7, scaleRate 0.6
        t += normalMap.sample(sm, uv).r * scale;
        uv *= 3.7;
        scale *= 0.6;
    }
    return t;
}

fragment float4 fs_water(WaterOut in [[stage_in]], constant FragmentUniforms& u [[buffer(0)]],
                         texturecube<float> cubicEnvMap [[texture(0)]], texture2d<float> normalMap [[texture(1)]],
                         sampler s [[sampler(0)]], sampler sm [[sampler(1)]]) {
    float3 eyeDir = normalize(in.eyeVector);
    float2 uv = in.uv;
    float h = 0.5; // one parallax iteration
    float error = h - normalMap.sample(sm, uv * 0.5).x;
    h -= error;
    uv = uv + error * eyeDir.xy * (1.5 / 64.0);
    float h00 = water_turbulance(normalMap, sm, uv);
    float h10 = water_turbulance(normalMap, sm, uv + float2(0.5 / 128.0, 0.0));
    float h01 = water_turbulance(normalMap, sm, uv + float2(0.0, 0.5 / 128.0));
    float3 n = normalize(float3(h10 - h00, h01 - h00, 1.0));
    float3 r = reflect(eyeDir, n);
    float specularDot = max(0.0, dot(r, u.sunDirection.xyz));
    r.z = abs(r.z);
    float4 env = cubicEnvMap.sample(s, r);
    float3 diffuse = u.ambient.rgb * n.z; // waterColor.rgb
    float3 lit = mix(diffuse, env.rgb, env.a);
    lit += pow(specularDot, 64.0) * u.sunColor.rgb;
    return float4(apply_fog(lit, length(in.eyeVector), u), in.color.a);
}
// lmap_s (techniques lmap_s*, shaders lib/lightmap.hlsl + phong.hlsl + normalmap.hlsl with USE_SPECULAR). World geometry only.
// Constants: lightSpecular0 = lightColor0 * r_specularColorScale (default 1).
struct SpecVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float4 color    [[attribute(2)]];
    float2 uv       [[attribute(3)]];
    float2 lmUv     [[attribute(4)]];
    float3 tangent  [[attribute(6)]];
    float3 binormal [[attribute(7)]];
};

struct SpecVertexUniforms {
    float4x4 viewProj;
    float4 eye;
    float4x4 model;
    float4 sun; // lightPosition0 (direction towards the sun)
};

struct SpecVertexOut {
    float4 position [[position]];
    float3 normal;
    float3 tangent;
    float3 binormal;
    float4 color;
    float2 uv;
    float2 lmUv;
    float3 halfAngle;
    float3 eyeVec;
};

vertex SpecVertexOut vs_spec(SpecVertexIn in [[stage_in]], constant SpecVertexUniforms& u [[buffer(0)]]) {
    SpecVertexOut o;
    const float3 wp = (u.model * float4(in.position, 1.0)).xyz;
    o.position = u.viewProj * float4(wp, 1.0);
    o.position.z += u.eye.w * o.position.w;
    o.normal = (u.model * float4(in.normal, 0.0)).xyz;
    o.tangent = (u.model * float4(in.tangent, 0.0)).xyz;
    o.binormal = (u.model * float4(in.binormal, 0.0)).xyz;
    o.color = in.color;
    o.uv = in.uv;
    o.lmUv = in.lmUv;
    o.halfAngle = normalize(normalize(u.eye.xyz - wp) + u.sun.xyz); // Phong_VertexSetup
    o.eyeVec = wp - u.eye.xyz;
    return o;
}

fragment float4 fs_spec(SpecVertexOut in [[stage_in]],
                        constant FragmentUniforms& u [[buffer(0)]],
                        texture2d<float> colorMap [[texture(0)]],
                        texture2d_array<float> lmCoeffs [[texture(1)]],
                        texture2d<float> lmSun [[texture(2)]],
                        texture2d<float> normalMap [[texture(3)]],
                        texture2d<float> specularMap [[texture(4)]],
                        texture2d<float> specularity [[texture(5)]],
                        texture2d<float> lmWeightMap [[texture(6)]],
                        texture2d<float> detailMap [[texture(7)]],
                        sampler colorSampler [[sampler(0)]],
                        sampler lmSampler [[sampler(1)]],
                        sampler sunSampler [[sampler(2)]],
                        sampler normalSampler [[sampler(3)]],
                        sampler specSampler [[sampler(4)]],
                        sampler specularitySampler [[sampler(5)]],
                        sampler lmWeightSampler [[sampler(6)]],
                        sampler detailSampler [[sampler(7)]]) {
    // Textured diffuse with the detail variant: same detail add as above (detail scale in lmWeights.xy, flag in .z)
    float4 tex = colorMap.sample(colorSampler, in.uv);
    if (u.lmWeights.z > 0.5) tex.rgb += detailMap.sample(detailSampler, in.uv * u.lmWeights.xy).rgb - 0.5;
    float4 base = tex * in.color;
    if (u.params.x > 0.5 && base.a < 0.5) discard_fragment();
    const float outA = u.params.x < -0.5 ? 1.0 : base.a; // params.x -1: USE_ALPHA 0 technique (MatInfo::alphaOne)
    if (u.params.w > 0.5) return float4(apply_fog(base.rgb, length(in.eyeVec), u), outA);
    // Normal_GetSample (PC: channels wy = alpha, green) and Normal_DecodeNormalizedWorldSpace: N + (2x-1) T + (2y-1) B.
    const float4 ns = normalMap.sample(normalSampler, in.uv);
    const float3 n = normalize(in.normal + (2.0 * ns.a - 1.0) * in.tangent + (2.0 * ns.g - 1.0) * in.binormal);
    float4 r = lmCoeffs.sample(lmSampler, in.lmUv, 0);
    float4 g = lmCoeffs.sample(lmSampler, in.lmUv, 1);
    float4 b = lmCoeffs.sample(lmSampler, in.lmUv, 2);
    // The three lightmap coefficient weights come from sampling the lightmap-weights texture ($lightmapweights, gfx 0x1000ff10) with the normal-map sample
    const float4 cw = lmWeightMap.sample(lmWeightSampler, float2(ns.a, ns.g));
    const float3 lm = float3(dot(r, cw), dot(g, cw), dot(b, cw)) * u.params.z;
    const float vis = lmSun.sample(sunSampler, in.lmUv).r; // * dynamicShadowSampler (no dynamic shadows: 1)
    const float3 sunDiffuse = vis * max(dot(u.sunDirection.xyz, n), 0.0) * u.sunColor.rgb;
    // Phong_SpecularLighting / Phong_SpecularVisibility.
    const float4 ss = specularMap.sample(specSampler, in.uv);
    const float nh = dot(normalize(in.halfAngle), n);
    const float3 strength = float3(specularity.sample(specularitySampler, float2(ss.a, nh)).r);
    const float3 spec = mix(0.3, 1.0, vis) * (u.sunColor.rgb * ss.rgb * strength) * u.sunDirection.w; // lightSpecular0 = lightColor0 * r_specularColorScale (sunDirection.w, default 1)
    return float4(apply_fog(base.rgb * (lm + sunDiffuse) + spec, length(in.eyeVec), u), outA);
}
// ---- glow / bloom post chain (materials/shaders/glow_setup.hlsl, filter_symmetric_N.hlsl, glow_apply_bloom/sky_bleed.hlsl) ----
struct GlowU {
    float4 a; // setup: xy = dest texel size (inv width/height), blur: xy = step in uv, z = radius, apply: x = bloom intensity, y = sky bleed intensity
    float4 b; // setup: x = cutoff, y = cutoff rescale, z = desaturation, apply: xyz = nearPlaneOrg
    float4 c; // apply: xyz = nearPlaneDx
    float4 d; // apply: xyz = nearPlaneDy
};
struct GlowVOut {
    float4 position [[position]];
    float2 uv;
};
vertex GlowVOut vs_fs(uint vid [[vertex_id]]) {
    GlowVOut o;
    const float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 fs_copy(GlowVOut in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(src.sample(s, in.uv).rgb, 1.0);
}
float4 glowSample(texture2d<float> scene, sampler s, float2 uv, constant GlowU& u) {
    const float4 px = scene.sample(s, uv);
    const float intensity = dot(px.rgb, float3(0.299, 0.587, 0.114));
    float3 rgb = intensity >= u.b.x ? 1.0 + (px.rgb - 1.0) * u.b.y : float3(0.0);
    const float nonSky = saturate(4.0 * px.a); // TODO(spec): max with the post-sun copy's alpha (colorMapPostSunSampler)
    return float4(rgb * nonSky, 1.0 - nonSky);
}
fragment float4 fs_glow_setup(GlowVOut in [[stage_in]], constant GlowU& u [[buffer(0)]], texture2d<float> scene [[texture(0)]], sampler s [[sampler(0)]]) {
    const float2 h = 0.5 * u.a.xy;
    float4 c = glowSample(scene, s, in.uv + float2(-h.x, -h.y), u) + glowSample(scene, s, in.uv + float2(-h.x, h.y), u) +
               glowSample(scene, s, in.uv + float2(h.x, h.y), u) + glowSample(scene, s, in.uv + float2(h.x, -h.y), u);
    c *= 0.25;
    const float intensity = dot(c.rgb, float3(0.299, 0.587, 0.114));
    c.rgb = mix(c.rgb, float3(intensity), u.b.z);
    return c;
}
// TODO(spec): the kernel weights (filterTap constants) are computed by the DLL; a normalised Gaussian with sigma = radius / 2 is used here.
fragment float4 fs_glow_blur(GlowVOut in [[stage_in]], constant GlowU& u [[buffer(0)]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    const float radius = max(u.a.z, 0.5);
    const float sigma = max(radius * 0.5, 0.5);
    float4 sum = float4(0.0);
    float wsum = 0.0;
    for (int i = -16; i <= 16; ++i) {
        if (abs(float(i)) > radius) continue;
        const float w = exp(-float(i * i) / (2.0 * sigma * sigma));
        sum += w * src.sample(s, in.uv + u.a.xy * float(i));
        wsum += w;
    }
    return sum / wsum;
}
// out.a = bleed * skyBleedIntensity, out.rgb = sky * out.a + blurred.rgb * bloomIntensity; blended ONE, ONE_MINUS_SRC_ALPHA (TODO(spec): blend state).
fragment float4 fs_glow_apply(GlowVOut in [[stage_in]], constant GlowU& u [[buffer(0)]], texture2d<float> glow [[texture(0)]], texturecube<float> sky [[texture(1)]],
                              sampler s [[sampler(0)]], sampler cs [[sampler(1)]]) {
    const float4 blurred = glow.sample(s, in.uv);
    const float3 dir = u.b.xyz + (in.uv.x - 0.5) * u.c.xyz + (in.uv.y - 0.5) * u.d.xyz;
    const float3 skyRgb = sky.sample(cs, dir).rgb;
    const float a = saturate(blurred.a * 2.0) * u.a.y;
    return float4(skyRgb * a + blurred.rgb * u.a.x, a);
}

// Dynamic point light, technique l_point (lib/light_point.hlsl): the distance to the light, divided by its radius, indexes the linear falloff ramp;
// colour = diffuse * light colour * N.L * falloff, added to the lit scene.
// TODO(spec): Phong specular and normal maps of the l_point technique, the exact N.L form of Phong_Lighting.
struct DLightU {
    float4 light;  // xyz position, w radius
    float4 color;  // rgb
    float4 params; // x: alpha test
    float4 eye;
    float4 ramp[4]; // images/falloff_linear.iwi texels / 255 (16 x 1, bilinear, clamp)
};
fragment float4 fs_dlight(VertexOut in [[stage_in]], constant DLightU& u [[buffer(0)]], texture2d<float> colorMap [[texture(0)]], sampler s [[sampler(0)]]) {
    const float4 base = colorMap.sample(s, in.uv) * in.color;
    if (u.params.x > 0.5 && base.a < 0.5) discard_fragment();
    const float3 wp = in.eyeVec + u.eye.xyz;
    const float3 delta = (u.light.xyz - wp) / u.light.w;
    const float d = length(delta);
    const float ndl = max(dot(normalize(in.normal), delta / max(d, 1e-5)), 0.0);
    const float x = clamp(saturate(d) * 16.0 - 0.5, 0.0, 15.0);
    const int i0 = int(floor(x)), i1 = min(i0 + 1, 15);
    const float f = mix(u.ramp[i0 >> 2][i0 & 3], u.ramp[i1 >> 2][i1 & 3], x - float(i0));
    return float4(base.rgb * u.color.rgb * (ndl * f), 1.0);
}

// Technique set "objective" (objective_base.tech / objective_base.hlsl): a pulsing gold silhouette of objective models.
// facing = -(view-space normal).z, here -dot(normal, camera forward); colour = mix(min, max, 0.5 - 0.5 sin(2 pi (facing / 2 +
// gameTime.z))) * (luminance + 0.5), luminance = dot(0.333, texture rgb); alpha = texture alpha (added with SRC_ALPHA, ONE).
struct ObjectiveU {
    float4 forward; // xyz: camera forward (world), w: gameTime.z
};
fragment float4 fs_objective(VertexOut in [[stage_in]], constant ObjectiveU& u [[buffer(0)]], texture2d<float> colorMap [[texture(0)]],
                             sampler s [[sampler(0)]]) {
    const float4 tex = colorMap.sample(s, in.uv);
    const float luminance = dot(float3(0.333), tex.rgb);
    const float facing = -dot(normalize(in.normal), u.forward.xyz);
    const float k = 0.5 - sin((facing * 0.5 + u.forward.w) * 3.14159 * 2.0) * 0.5;
    const float4 c = mix(float4(0.25, 0.15, 0.0, 1.0), float4(1.0, 1.0, 0.5, 1.0), k) * (luminance + 0.5);
    return float4(c.rgb, tex.a);
}
)MSL"
