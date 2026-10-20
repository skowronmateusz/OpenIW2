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
};

struct VertexUniforms {
    float4x4 viewProj;
    float4 eye;
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float4 color;
    float2 uv;
    float2 lmUv;
};

vertex VertexOut vs_main(VertexIn in [[stage_in]], constant VertexUniforms& u [[buffer(0)]]) {
    VertexOut o;
    o.position = u.viewProj * float4(in.position, 1.0);
    o.normal = in.normal;
    o.color = in.color;
    o.uv = in.uv;
    o.lmUv = in.lmUv;
    return o;
}

struct FragmentUniforms {
    float4 sunDirection;   // xyz: direction towards the sun
    float4 sunColor;       // rgb: sun color * intensity
    float4 lmWeights;      // weights applied to the four lightmap coefficients
    float4 params;         // x: alpha test (1 = on), y: has lightmap, z: lightmap scale, w: fullbright
};

fragment float4 fs_main(VertexOut in [[stage_in]],
                        constant FragmentUniforms& u [[buffer(0)]],
                        texture2d<float> colorMap [[texture(0)]],
                        texture2d_array<float> lmCoeffs [[texture(1)]],
                        texture2d<float> lmSun [[texture(2)]],
                        sampler colorSampler [[sampler(0)]],
                        sampler lmSampler [[sampler(1)]],
                        sampler sunSampler [[sampler(2)]]) {
    float4 base = colorMap.sample(colorSampler, in.uv) * in.color;
    if (u.params.x > 0.5 && base.a < 0.5) discard_fragment();

    float3 light;
    if (u.params.w > 0.5) {
        light = float3(1.0);
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
)MSL"
