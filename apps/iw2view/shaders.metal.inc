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
};

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

    if (u.params.y > 3.5) { // effect_multiply: blends the texture towards white by the vertex alpha, no light
        float4 t = colorMap.sample(colorSampler, in.uv);
        return float4((1.0 - in.color.a) + t.rgb * in.color.rgb * in.color.a, 1.0);
    }
    float3 light;
    if (u.params.w > 0.5) {
        light = float3(1.0);
    } else if (u.params.y > 2.5) {
        float ndl = max(dot(normalize(in.normal), u.sunDirection.xyz), 0.0);
        light = u.ambient.rgb + u.ambient.w * ndl * u.sunColor.rgb;
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
    return float4(apply_fog(base.rgb * light, length(in.eyeVec), u), base.a);
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
    if (u.params.w > 0.5) return float4(apply_fog(base.rgb, length(in.eyeVec), u), base.a);
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
    return float4(apply_fog(base.rgb * (lm + sunDiffuse) + spec, length(in.eyeVec), u), base.a);
}
)MSL"
