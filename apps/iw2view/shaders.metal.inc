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
    return o;
}

struct FragmentUniforms {
    float4 sunDirection;   // xyz: direction towards the sun
    float4 sunColor;       // rgb: sun color * intensity
    float4 lmWeights;      // weights applied to the four lightmap coefficients
    float4 params;
    float4 ambient;        // entity light: rgb ambient from the light grid, w = sun visibility         // x: alpha test (1 = on), y: 1 = lightmap, 2 = vertex light, 3 = entity light, z: lightmap scale, w: fullbright
    float4 scroll;         // x: V offset subtracted from uv (the scroll technique subtracts the game-time value from the V coordinate)
};

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant FragmentUniforms& u [[buffer(0)]],
                        texture2d<float> colorMap [[texture(0)]],
                        texture2d_array<float> lmCoeffs [[texture(1)]],
                        texture2d<float> lmSun [[texture(2)]],
                        sampler colorSampler [[sampler(0)]],
                        sampler lmSampler [[sampler(1)]],
                        sampler sunSampler [[sampler(2)]]) {
    const float2 uv = in.uv - float2(0.0, u.scroll.x);
    float4 base = colorMap.sample(colorSampler, uv) * in.color;
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
    return float4(base.rgb * light, base.a);
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
// water_l_sun.tech / lib/water.hlsl with a flat normalMap (R = 0.5): the parallax error is 0 and the three-octave
// turbulence cancels in the finite differences, so localNormal = (0, 0, 1) exactly.
// TODO(spec): the FFT-generated normalMap (ripples) and the texcoord0 scale are not implemented.
struct WaterOut {
    float4 position [[position]];
    float4 color;
    float3 eyeVector;
};

vertex WaterOut vs_water(VertexIn in [[stage_in]], constant VertexUniforms& u [[buffer(0)]]) {
    WaterOut o;
    o.position = u.viewProj * (u.model * float4(in.position, 1.0));
    o.color = in.color;
    o.eyeVector = (u.model * float4(in.position, 1.0)).xyz - u.eye.xyz;
    return o;
}

fragment float4 fs_water(WaterOut in [[stage_in]], constant FragmentUniforms& u [[buffer(0)]],
                         texturecube<float> cubicEnvMap [[texture(0)]], sampler s [[sampler(0)]]) {
    float3 eyeDir = normalize(in.eyeVector);
    float3 n = float3(0.0, 0.0, 1.0);
    float3 r = reflect(eyeDir, n);
    float specularDot = max(0.0, dot(r, u.sunDirection.xyz));
    r.z = abs(r.z);
    float4 env = cubicEnvMap.sample(s, r);
    float3 diffuse = u.ambient.rgb * n.z; // waterColor.rgb
    float3 lit = mix(diffuse, env.rgb, env.a);
    lit += pow(specularDot, 64.0) * u.sunColor.rgb;
    return float4(lit, in.color.a);
}
)MSL"
