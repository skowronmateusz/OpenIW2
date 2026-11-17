// Enhanced rendering mode: shadow casters, exposure, bloom, tone mapping and EDR output (docs/render-enhanced.md). Our own design from
// public techniques; appended after the default library (see shaders_enhanced.metal.inc).
R"MSL(
// ---- sun shadow casters: depth only, alpha-tested surfaces discard below half alpha ----
struct ShadowVU {
    float4x4 viewProj;
    float4x4 model;
    float4 p; // x: alpha test
};
struct ShadowOut {
    float4 position [[position]];
    float2 uv;
    float alpha;
    float test;
};
vertex ShadowOut vs_shadow(VertexIn in [[stage_in]], constant ShadowVU& u [[buffer(0)]]) {
    ShadowOut o;
    o.position = u.viewProj * (u.model * float4(in.position, 1.0));
    o.uv = in.uv;
    o.alpha = in.color.a;
    o.test = u.p.x;
    return o;
}
fragment void fs_shadow(ShadowOut in [[stage_in]], texture2d<float> colorMap [[texture(0)]], sampler s [[sampler(0)]]) {
    if (in.test > 0.5 && colorMap.sample(s, in.uv).a * in.alpha < 0.5) discard_fragment();
}

// ---- shared ----
static float3 enh_srgb_to_linear(float3 c) {
    const float3 a = abs(c);
    const float3 l = select(pow((a + 0.055) / 1.055, float3(2.4)), a / 12.92, a <= 0.04045);
    return sign(c) * l;
}
static float3 enh_linear_to_srgb(float3 l) {
    const float3 a = abs(l);
    const float3 c = select(1.055 * pow(a, float3(1.0 / 2.4)) - 0.055, a * 12.92, a <= 0.0031308);
    return sign(l) * c;
}
static float enh_luma(float3 lin) { return dot(lin, float3(0.2126, 0.7152, 0.0722)); }

// ---- auto exposure: centre-weighted log2 luminance, averaged by the mip chain, then adapted over time ----
fragment float4 fs_enh_lum(GlowVOut in [[stage_in]], texture2d<float> scene [[texture(0)]], sampler s [[sampler(0)]]) {
    const float3 lin = enh_srgb_to_linear(max(scene.sample(s, in.uv).rgb, 0.0));
    const float w = mix(1.0, 0.3, smoothstep(0.15, 0.75, length((in.uv - 0.5) * float2(1.6, 1.0))));
    return float4(w * log2(enh_luma(lin) + 1e-4), w, 0.0, 1.0);
}
struct ExpoU {
    float4 a; // x: key (log2), y: strength, z: min EV, w: max EV
    float4 b; // x: dt seconds, y: tau brighten, z: tau darken, w: 1 = reset to the target
    float4 c; // x: mip level of the 1 x 1 average
};
fragment float4 fs_enh_expo(GlowVOut in [[stage_in]], constant ExpoU& u [[buffer(0)]], texture2d<float> lum [[texture(0)]],
                            texture2d<float> prev [[texture(1)]], sampler s [[sampler(0)]], sampler ps [[sampler(1)]]) {
    const float2 m = lum.sample(s, float2(0.5), level(u.c.x)).rg;
    const float mean = m.x / max(m.y, 1e-6);
    const float target = clamp((u.a.x - mean) * u.a.y, u.a.z, u.a.w);
    const float cur = prev.sample(ps, float2(0.5)).r;
    if (u.b.w > 0.5) return float4(target, 0.0, 0.0, 1.0);
    const float tau = target > cur ? u.b.y : u.b.z;
    return float4(cur + (target - cur) * (1.0 - exp(-u.b.x / max(tau, 1e-3))), 0.0, 0.0, 1.0);
}

// ---- bloom: soft-threshold prefilter, 13-tap downsample chain, tent upsample added back up the chain ----
struct BloomU {
    float4 a; // x, y: source texel size; z: threshold (linear); w: knee
    float4 b; // x: exposure EV bias; y: 1 = use the exposure texture
};
static float3 enh_down13(texture2d<float> t, sampler s, float2 uv, float2 d) {
    const float3 a = t.sample(s, uv + d * float2(-2, -2)).rgb, b = t.sample(s, uv + d * float2(0, -2)).rgb, c = t.sample(s, uv + d * float2(2, -2)).rgb;
    const float3 e = t.sample(s, uv + d * float2(-1, -1)).rgb, f = t.sample(s, uv + d * float2(1, -1)).rgb;
    const float3 g = t.sample(s, uv + d * float2(-2, 0)).rgb, h = t.sample(s, uv).rgb, i = t.sample(s, uv + d * float2(2, 0)).rgb;
    const float3 j = t.sample(s, uv + d * float2(-1, 1)).rgb, k = t.sample(s, uv + d * float2(1, 1)).rgb;
    const float3 l = t.sample(s, uv + d * float2(-2, 2)).rgb, mm = t.sample(s, uv + d * float2(0, 2)).rgb, n = t.sample(s, uv + d * float2(2, 2)).rgb;
    return (e + f + j + k) * 0.125 + (a + b + g + h) * 0.03125 + (b + c + h + i) * 0.03125 + (g + h + l + mm) * 0.03125 + (h + i + mm + n) * 0.03125;
}
fragment float4 fs_enh_bloom_pre(GlowVOut in [[stage_in]], constant BloomU& u [[buffer(0)]], texture2d<float> scene [[texture(0)]],
                                 texture2d<float> expo [[texture(1)]], sampler s [[sampler(0)]], sampler ps [[sampler(1)]]) {
    const float ev = u.b.x + (u.b.y > 0.5 ? expo.sample(ps, float2(0.5)).r : 0.0);
    float3 lin = 0.0;
    const float2 d = u.a.xy;
    for (int k = 0; k < 4; ++k) { // 2 x 2 box of the full-size scene, each tap clamped to keep single bright pixels from flickering
        const float2 o = float2((k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5) * d;
        const float3 c = enh_srgb_to_linear(max(scene.sample(s, in.uv + o).rgb, 0.0)) * exp2(ev);
        lin += c / (1.0 + enh_luma(c) * 0.25);
    }
    lin *= 0.25;
    const float l = enh_luma(lin);
    const float soft = clamp(l - u.a.z + u.a.w, 0.0, 2.0 * u.a.w);
    const float w = max(soft * soft / (4.0 * u.a.w + 1e-5), l - u.a.z) / max(l, 1e-5);
    return float4(lin * max(w, 0.0), 1.0);
}
fragment float4 fs_enh_bloom_down(GlowVOut in [[stage_in]], constant BloomU& u [[buffer(0)]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(enh_down13(src, s, in.uv, u.a.xy), 1.0);
}
fragment float4 fs_enh_bloom_up(GlowVOut in [[stage_in]], constant BloomU& u [[buffer(0)]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    const float2 d = u.a.xy;
    float3 c = src.sample(s, in.uv).rgb * 4.0;
    c += (src.sample(s, in.uv + float2(-d.x, 0)).rgb + src.sample(s, in.uv + float2(d.x, 0)).rgb + src.sample(s, in.uv + float2(0, -d.y)).rgb +
          src.sample(s, in.uv + float2(0, d.y)).rgb) * 2.0;
    c += src.sample(s, in.uv + float2(-d.x, -d.y)).rgb + src.sample(s, in.uv + float2(d.x, -d.y)).rgb + src.sample(s, in.uv + float2(-d.x, d.y)).rgb +
         src.sample(s, in.uv + float2(d.x, d.y)).rgb;
    return float4(c / 16.0, 1.0);
}

// ---- tone mapping: exposure and bloom on linear light, back to display encoding, highlight shoulder ----
struct ToneU {
    float4 a; // x: 1 = shoulder (else clip at white), y: white (display encoding), z: bloom intensity, w: 1 = use the exposure texture
    float4 b; // x: exposure bias EV, y: 1 = bloom present, z: knee, w: 1 = motion-vector debug view
};
fragment float4 fs_enh_tonemap(GlowVOut in [[stage_in]], constant ToneU& u [[buffer(0)]], texture2d<float> scene [[texture(0)]],
                               texture2d<float> bloom [[texture(1)]], texture2d<float> expo [[texture(2)]],
                               sampler s [[sampler(0)]], sampler bs [[sampler(1)]], sampler ps [[sampler(2)]]) {
    const float3 c = max(scene.sample(s, in.uv).rgb, 0.0);
    const float ev = u.b.x + (u.a.w > 0.5 ? expo.sample(ps, float2(0.5)).r : 0.0);
    float3 g = c;
    if (ev != 0.0 || u.b.y > 0.5) {
        float3 lin = enh_srgb_to_linear(c) * exp2(ev);
        if (u.b.y > 0.5) lin += bloom.sample(bs, in.uv).rgb * u.a.z;
        g = enh_linear_to_srgb(lin);
    }
    const float white = u.a.y, knee = u.b.z;
    if (u.a.x > 0.5) {
        const float span = max(white - knee, 1e-4);
        g = select(knee + span * (1.0 - exp(-(g - knee) / span)), g, g <= knee);
    } else {
        g = min(g, white);
    }
    return float4(g, 1.0);
}

// ---- motion vectors of the static scene (camera motion), from $floatz ----
struct VelU {
    float4x4 invViewProj;
    float4x4 prevViewProj;
    float4 z; // x, y: clip z = x * w + y; z: $floatz clear value (sky) threshold
};
fragment float4 fs_enh_velocity(GlowVOut in [[stage_in]], constant VelU& u [[buffer(0)]], texture2d<float> floatZ [[texture(0)]], sampler s [[sampler(0)]]) {
    const float2 ndc = float2(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    float cz = floatZ.sample(s, in.uv).r;
    float w = (cz - u.z.y) / u.z.x;
    if (cz >= u.z.z) { // sky: a far point
        w = 65536.0;
        cz = u.z.x * w + u.z.y;
    }
    const float4 wp = u.invViewProj * float4(ndc * w, cz, w);
    const float4 pc = u.prevViewProj * float4(wp.xyz / wp.w, 1.0);
    return float4(ndc - pc.xy / pc.w, 0.0, 1.0);
}
// Debug view of the motion vectors (r_enhMotionDebug 1): red / green = screen motion in x / y, grey = none.
fragment float4 fs_enh_motion_view(GlowVOut in [[stage_in]], texture2d<float> vel [[texture(0)]], sampler s [[sampler(0)]]) {
    const float2 v = vel.sample(s, in.uv).rg;
    return float4(saturate(0.5 + v.x * 10.0), saturate(0.5 + v.y * 10.0), 0.5, 1.0);
}

// ---- final pass to the window: the r_gamma ramp on the SDR part, then extended linear sRGB for the EDR swapchain ----
fragment float4 fs_enh_present(GlowVOut in [[stage_in]], constant float4& u [[buffer(0)]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    float3 c = max(src.sample(s, in.uv).rgb, 0.0);
    if (u.x != 1.0) { // same ramp as fs_gamma on the 0..1 part; the part above 1 is kept
        const float3 lo = round(saturate(c) * 255.0) / 255.0;
        c = round(65535.0 * pow(lo, float3(u.y))) / 65535.0 + max(c - 1.0, 0.0);
    }
    if (u.z > 0.5) c = enh_srgb_to_linear(c);
    return float4(c, 1.0);
}
)MSL"
