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
    float4 b; // x: exposure bias EV, y: 1 = bloom present, z: knee, w: frame counter (grain)
    float4 c; // xy: texel size, z: sharpening after TAA (0 = none), w: grade (1 = period film)
};
fragment float4 fs_enh_tonemap(GlowVOut in [[stage_in]], constant ToneU& u [[buffer(0)]], texture2d<float> scene [[texture(0)]],
                               texture2d<float> bloom [[texture(1)]], texture2d<float> expo [[texture(2)]],
                               sampler s [[sampler(0)]], sampler bs [[sampler(1)]], sampler ps [[sampler(2)]]) {
    float3 c = max(scene.sample(s, in.uv).rgb, 0.0);
    if (u.c.z > 0.0) { // unsharp mask against the TAA softening (display encoding, clamped to the neighbourhood)
        const float3 n0 = scene.sample(s, in.uv + float2(u.c.x, 0)).rgb, n1 = scene.sample(s, in.uv - float2(u.c.x, 0)).rgb;
        const float3 n2 = scene.sample(s, in.uv + float2(0, u.c.y)).rgb, n3 = scene.sample(s, in.uv - float2(0, u.c.y)).rgb;
        const float3 mn = min(min(n0, n1), min(n2, n3)), mx = max(max(n0, n1), max(n2, n3));
        c = clamp(c + u.c.z * (c - 0.25 * (n0 + n1 + n2 + n3)), min(mn, c), max(mx, c));
    }
    const float ev = u.b.x + (u.a.w > 0.5 ? expo.sample(ps, float2(0.5)).r : 0.0);
    float3 g = c;
    if (ev != 0.0 || u.b.y > 0.5) {
        float3 lin = enh_srgb_to_linear(c) * exp2(ev);
        if (u.b.y > 0.5) lin += bloom.sample(bs, in.uv).rgb * u.a.z;
        g = enh_linear_to_srgb(lin);
    }
    if (u.c.w > 0.5) { // r_enhGrade 1, "period film" (game::periodFilmGrade): desaturate, S-curve, warm shadows, grain
        const float l = dot(g, float3(0.299, 0.587, 0.114));
        g = l + (g - l) * 0.8;
        const float3 x = saturate(g);
        const float3 sc = select(1.0 - 0.5 * pow(2.0 - 2.0 * x, 1.15), 0.5 * pow(2.0 * x, 1.15), x < 0.5);
        g = sc + (g - x);
        const float warm = 0.03 * (1.0 - saturate(l * 2.0));
        g += float3(warm, 0.0, -warm);
        const float n = fract(sin(dot(in.position.xy + fract(u.b.w * 0.618034) * 131.0, float2(12.9898, 78.233))) * 43758.5453); // b.w: frame
        g += (n - 0.5) * 0.035 * (1.0 - l * 0.5);
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

// ---- ambient occlusion (GTAO, Jimenez et al. 2016) and contact shadows, half resolution, from $floatz ----
struct AOU {
    float4x4 invViewProj;
    float4x4 viewProj;
    float4 z;      // x, y: clip z = x * w + y; z: $floatz sky threshold; w: frame index (noise)
    float4 eye;    // xyz; w: focal length in pixels (full size)
    float4 sun;    // xyz towards the sun; w: 1 = contact shadows
    float4 params; // x: AO radius (world), y: 1 = AO, z: contact ray length (world), w: contact thickness (world)
    float4 size;   // xy: 1 / full size, zw: full size
};
static float3 enh_world_at(float2 uv, constant AOU& u, texture2d<float> fz, sampler s, thread float& w) {
    float cz = fz.sample(s, uv).r;
    if (cz >= u.z.z) { w = 1e9; return float3(1e9); }
    w = (cz - u.z.y) / u.z.x;
    const float4 p = u.invViewProj * float4(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * w, cz, w);
    return p.xyz / p.w;
}
static float enh_ign(float2 px, float frame) { // interleaved gradient noise
    px += 5.588238 * frame;
    return fract(52.9829189 * fract(dot(px, float2(0.06711056, 0.00583715))));
}
fragment float4 fs_enh_ao(GlowVOut in [[stage_in]], constant AOU& u [[buffer(0)]], texture2d<float> fz [[texture(0)]], sampler s [[sampler(0)]]) {
    float w;
    const float3 P = enh_world_at(in.uv, u, fz, s, w);
    if (w > 1e8) return float4(1.0, 1.0, 0.0, 1.0);
    // normal from the nearer neighbour on each axis
    const float2 tx = float2(u.size.x, 0.0), ty = float2(0.0, u.size.y);
    float wa, wb, wc, wd;
    const float3 pr = enh_world_at(in.uv + tx, u, fz, s, wa), pl = enh_world_at(in.uv - tx, u, fz, s, wb);
    const float3 pd = enh_world_at(in.uv + ty, u, fz, s, wc), pu = enh_world_at(in.uv - ty, u, fz, s, wd);
    const float3 dx = abs(wa - w) < abs(wb - w) ? pr - P : P - pl;
    const float3 dy = abs(wc - w) < abs(wd - w) ? pd - P : P - pu;
    float3 N = normalize(cross(dy, dx));
    const float3 V = normalize(u.eye.xyz - P);
    if (dot(N, V) < 0.0) N = -N;
    const float2 px = in.uv * u.size.zw;
    float ao = 1.0;
    if (u.params.y > 0.5) {
        const float radius = u.params.x;
        const float pixRadius = clamp(radius * u.eye.w / w, 4.0, 96.0); // projected radius in pixels (eye.w: focal length in pixels)
        const float noise = enh_ign(px, u.z.w), jitter = enh_ign(px.yx + 17.0, u.z.w);
        float sum = 0.0, wsum = 0.0;
        for (int sl = 0; sl < 2; ++sl) {
            const float phi = (float(sl) + noise) * (3.14159265 / 2.0);
            const float2 dir = float2(cos(phi), sin(phi));
            // the slice plane: the view vector and the world direction of the screen direction at P
            float wq;
            const float3 Q = enh_world_at(in.uv + dir * u.size.xy * 4.0, u, fz, s, wq);
            float3 dW = (Q - P) - V * dot(Q - P, V);
            if (dot(dW, dW) < 1e-8) continue;
            dW = normalize(dW);
            const float3 axis = normalize(cross(dW, V));
            const float3 projN = N - axis * dot(N, axis);
            const float projLen = length(projN);
            if (projLen < 1e-4) continue;
            const float cosN = clamp(dot(projN / projLen, V), -1.0, 1.0);
            const float n = sign(dot(projN, dW)) * acos(cosN);
            float h[2];
            for (int side = 0; side < 2; ++side) {
                const float sgn = side == 0 ? -1.0 : 1.0;
                float maxCos = -1.0;
                for (int j = 0; j < 6; ++j) {
                    const float t = (float(j) + jitter) / 6.0;
                    const float2 suv = in.uv + sgn * dir * (t * t * pixRadius + 1.0) * u.size.xy;
                    float ws;
                    const float3 S = enh_world_at(suv, u, fz, s, ws);
                    const float3 d = S - P;
                    const float len = length(d);
                    if (len < 1e-3 || ws > 1e8) continue;
                    if (dot(d, N) < 0.1 * len + 0.75) continue; // on or under the tangent plane: no occlusion (flat surfaces stay open)
                    const float fall = saturate(1.0 - len / radius);
                    maxCos = max(maxCos, mix(-1.0, dot(d / len, V), fall));
                }
                h[side] = sgn * acos(clamp(maxCos, -1.0, 1.0)); // side 0: h0 <= 0, side 1: h1 >= 0
            }
            const float h0 = n + max(h[0] - n, -1.5707963), h1 = n + min(h[1] - n, 1.5707963);
            const float vis = 0.25 * (-cos(2.0 * h0 - n) + cos(n) + 2.0 * h0 * sin(n)) + 0.25 * (-cos(2.0 * h1 - n) + cos(n) + 2.0 * h1 * sin(n));
            sum += projLen * vis;
            wsum += projLen;
        }
        ao = wsum > 0.0 ? saturate(sum / wsum) : 1.0;
    }
    float contact = 1.0;
    if (u.sun.w > 0.5 && dot(N, u.sun.xyz) > 0.0) { // march towards the sun in $floatz
        const float noise = enh_ign(px, u.z.w + 3.0);
        const float3 o = P + N * 1.5;
        for (int j = 0; j < 12; ++j) {
            const float t = 2.0 + (float(j) + noise) / 12.0 * (u.params.z - 2.0);
            const float4 c = u.viewProj * float4(o + u.sun.xyz * t, 1.0);
            if (c.w <= 0.0) break;
            const float2 suv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
            if (any(suv < 0.0) || any(suv > 1.0)) break;
            float ws;
            enh_world_at(suv, u, fz, s, ws);
            const float diff = c.w - ws;
            if (diff > 1.0 && diff < u.params.w) { contact = 1.0 - saturate(1.0 - t / u.params.z); break; }
        }
        contact = mix(contact, 1.0, saturate((w - 1200.0) / 600.0)); // near the camera only
    }
    return float4(ao, contact, w, 1.0);
}
// Depth-aware 5 x 5 blur of the half-size AO / contact image (z holds the view depth).
fragment float4 fs_enh_ao_blur(GlowVOut in [[stage_in]], constant float4& u [[buffer(0)]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    const float4 c = src.sample(s, in.uv);
    float2 sum = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            const float4 t = src.sample(s, in.uv + float2(x, y) * u.xy);
            const float wgt = 1.0 / (1.0 + abs(t.z - c.z) / max(c.z * 0.02, 1.0) * 4.0);
            sum += t.xy * wgt;
            wsum += wgt;
        }
    return float4(sum / wsum, c.z, 1.0);
}

// Debug view (r_enhMotionDebug 2): AO in red, contact shadow in green.
fragment float4 fs_enh_ao_view(GlowVOut in [[stage_in]], texture2d<float> ao [[texture(0)]], sampler s [[sampler(0)]]) {
    const float2 a = ao.sample(s, in.uv).rg;
    return float4(a.x, a.y, a.x, 1.0);
}

// ---- TAA: $floatz with the motion vectors as a second target, then the history resolve (Karis 2014) ----
struct FloatzVelIn {
    float3 position [[attribute(0)]];
    float4 color    [[attribute(2)]];
    float2 uv       [[attribute(3)]];
    float3 prevPos  [[attribute(9)]]; // the previous frame's model-space position (the same vertex of the previous skinning)
};
struct FloatzVelVU {
    float4x4 viewProj;   // jittered, as the scene pass
    float4 eye;          // w: depth bias
    float4x4 model;
    float4x4 curVP;      // without jitter
    float4x4 prevVP;     // the previous frame's, without jitter
    float4x4 prevModel;
};
struct FloatzVelOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float dist;
    float4 cur;
    float4 prev;
};
vertex FloatzVelOut vs_floatz_vel(FloatzVelIn in [[stage_in]], constant FloatzVelVU& u [[buffer(0)]]) {
    FloatzVelOut o;
    o.position = world_clip(u.viewProj, u.model, in.position, u.eye.w);
    o.color = in.color;
    o.uv = in.uv;
    o.dist = o.position.z - u.eye.w * o.position.w;
    o.cur = u.curVP * (u.model * float4(in.position, 1.0));
    o.prev = u.prevVP * (u.prevModel * float4(in.prevPos, 1.0));
    return o;
}
struct FloatzVelFOut {
    float4 z [[color(0)]];
    float4 v [[color(1)]];
};
fragment FloatzVelFOut fs_floatz_vel(FloatzVelOut in [[stage_in]], constant float4& p [[buffer(0)]], texture2d<float> colorMap [[texture(0)]],
                                     sampler cs [[sampler(0)]]) {
    if (p.x > 0.5 && (in.color * colorMap.sample(cs, in.uv)).a < 128.0 / 255.0) discard_fragment();
    FloatzVelFOut o;
    o.z = float4(in.dist);
    o.v = float4(in.cur.xy / in.cur.w - in.prev.xy / in.prev.w, 0.0, 1.0);
    return o;
}

struct TaaU {
    float4x4 invViewProj;  // jittered current
    float4x4 prevViewProj; // previous, without jitter
    float4 z;              // x, y: clip z line; z: $floatz sky threshold; w: 1 = history valid
    float4 size;           // xy: 1 / size; zw: jitter (NDC) of this frame
};
// Catmull-Rom history sample from 5 bilinear taps (the corner taps of the 4 x 4 kernel dropped), so the history stays sharp when it
// is resampled every frame.
static float3 enh_history(texture2d<float> t, sampler ls, float2 uv, float2 texSize) {
    const float2 pos = uv * texSize, c = floor(pos - 0.5) + 0.5, f = pos - c;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2, o12 = w2 / w12;
    const float2 t0 = (c - 1.0) / texSize, t3 = (c + 2.0) / texSize, t12 = (c + o12) / texSize;
    float3 r = t.sample(ls, float2(t12.x, t0.y)).rgb * (w12.x * w0.y) + t.sample(ls, float2(t0.x, t12.y)).rgb * (w0.x * w12.y) +
               t.sample(ls, t12).rgb * (w12.x * w12.y) + t.sample(ls, float2(t3.x, t12.y)).rgb * (w3.x * w12.y) +
               t.sample(ls, float2(t12.x, t3.y)).rgb * (w12.x * w3.y);
    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}
// MetalFX inputs (docs/render-enhanced.md "Stage 5"): the motion of every pixel as the TAA takes it (the sky's from the camera),
// and the device depth (clip z / w) from $floatz, 1 for the sky.
static float2 enh_pixel_motion(constant TaaU& u, float2 uv, texture2d<float> vel, sampler vs, float fzv) {
    if (fzv >= u.z.z) {
        const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        const float w = 65536.0, cz = u.z.x * w + u.z.y;
        const float4 wp = u.invViewProj * float4(ndc * w, cz, w);
        const float4 pc = u.prevViewProj * float4(wp.xyz / wp.w, 1.0);
        return (ndc - u.size.zw) - pc.xy / pc.w;
    }
    return vel.sample(vs, uv).rg;
}
fragment float4 fs_enh_mfx_motion(GlowVOut in [[stage_in]], constant TaaU& u [[buffer(0)]], texture2d<float> vel [[texture(0)]],
                                  texture2d<float> fz [[texture(1)]], sampler vs [[sampler(0)]], sampler zs [[sampler(1)]]) {
    return float4(enh_pixel_motion(u, in.uv, vel, vs, fz.sample(zs, in.uv).r), 0.0, 1.0);
}
fragment float4 fs_enh_mfx_depth(GlowVOut in [[stage_in]], constant TaaU& u [[buffer(0)]], texture2d<float> fz [[texture(0)]],
                                 sampler zs [[sampler(0)]]) {
    const float cz = fz.sample(zs, in.uv).r;
    if (cz >= u.z.z) return float4(1.0);
    const float w = max((cz - u.z.y) / u.z.x, 1e-3);
    return float4(saturate(cz / w));
}
static float3 enh_tonemap_weight(float3 c) { return c / (1.0 + max(max(c.r, c.g), c.b)); }
static float3 enh_ycocg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
static float3 enh_rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
fragment float4 fs_enh_taa(GlowVOut in [[stage_in]], constant TaaU& u [[buffer(0)]], texture2d<float> cur [[texture(0)]],
                           texture2d<float> hist [[texture(1)]], texture2d<float> vel [[texture(2)]], texture2d<float> fz [[texture(3)]],
                           sampler ps [[sampler(0)]], sampler ls [[sampler(1)]], sampler vs [[sampler(2)]], sampler zs [[sampler(3)]]) {
    // neighbourhood of the current frame (in a compressed space, Karis' luminance weighting), and the nearest depth for the velocity
    float3 mn = 1e9, mx = -1e9, m1 = 0.0, m2 = 0.0;
    float3 c0 = 0.0;
    float nearest = 1e30;
    float2 nearestUv = in.uv;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float2 q = in.uv + float2(x, y) * u.size.xy;
            const float3 c = enh_ycocg(enh_tonemap_weight(max(cur.sample(ps, q).rgb, 0.0)));
            if (x == 0 && y == 0) c0 = c;
            mn = min(mn, c), mx = max(mx, c);
            m1 += c, m2 += c * c;
            const float d = fz.sample(zs, q).r;
            if (d < nearest) nearest = d, nearestUv = q;
        }
    // variance clip box, tightened to min / max
    const float3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));
    mn = max(mn, mu - 1.25 * sigma), mx = min(mx, mu + 1.25 * sigma);
    float2 v;
    if (nearest >= u.z.z) { // sky: camera motion of a far point
        const float2 ndc = float2(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
        const float w = 65536.0, cz = u.z.x * w + u.z.y;
        const float4 wp = u.invViewProj * float4(ndc * w, cz, w);
        const float4 pc = u.prevViewProj * float4(wp.xyz / wp.w, 1.0);
        v = (ndc - u.size.zw) - pc.xy / pc.w;
    } else {
        v = vel.sample(vs, nearestUv).rg;
    }
    const float2 prevUv = in.uv - float2(v.x * 0.5, -v.y * 0.5);
    const float3 current = cur.sample(ps, in.uv).rgb;
    if (u.z.w < 0.5 || any(prevUv < 0.0) || any(prevUv > 1.0)) return float4(current, 1.0);
    const float3 h = enh_ycocg(enh_tonemap_weight(enh_history(hist, ls, prevUv, 1.0 / u.size.xy)));
    const float3 hc = clamp(h, mn, mx);
    // blend in the compressed space, back to linear
    const float3 outW = enh_rgb(mix(c0, hc, 0.9));
    const float m = max(max(outW.r, outW.g), outW.b);
    return float4(max(outW / max(1.0 - m, 1e-3), 0.0), 1.0);
}

// ---- atmosphere: sun shafts (light scattered in the air), half size, added to the scene before TAA ----
struct VolU {
    float4x4 invViewProj;
    float4 z;      // x, y: clip z line; z: $floatz sky threshold; w: frame index
    float4 eye;    // xyz; w: maximum march distance
    float4 sun;    // xyz towards the sun; w: strength
    float4 color;  // rgb: sun colour times the map's fog tint; w: phase asymmetry g
};
fragment float4 fs_enh_volume(GlowVOut in [[stage_in]], constant EnhFragU& eu [[buffer(0)]], constant VolU& v [[buffer(1)]],
                              texture2d<float> fz [[texture(0)]], depth2d<float> shadowMap [[texture(1)]], depth2d_array<float> worldShadow [[texture(2)]],
                              sampler ps [[sampler(0)]], sampler ss [[sampler(1)]], sampler ws [[sampler(2)]]) {
    const float cz = fz.sample(ps, in.uv).r;
    const float2 ndc = float2(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    float w = cz >= v.z.z ? 1e6 : (cz - v.z.y) / v.z.x;
    const float4 farP = v.invViewProj * float4(ndc * 1000.0, v.z.x * 1000.0 + v.z.y, 1000.0);
    const float3 dir = normalize(farP.xyz / farP.w - v.eye.xyz);
    // distance along the ray to the surface: w is the depth along the view axis; the ray reaches it at w / cos
    const float4 midP = v.invViewProj * float4(ndc * 100.0, v.z.x * 100.0 + v.z.y, 100.0);
    const float cosA = 100.0 / max(length(midP.xyz / midP.w - v.eye.xyz), 1e-3);
    const float dist = min(w / max(cosA, 1e-3), v.eye.w);
    const float noise = fract(52.9829189 * fract(dot(in.position.xy + 5.588238 * v.z.w, float2(0.06711056, 0.00583715))));
    const int steps = 12;
    float lit = 0.0;
    for (int i = 0; i < steps; ++i) {
        const float t = (float(i) + noise) / float(steps) * dist;
        const float3 p = v.eye.xyz + dir * t;
        const float2 wv = enh_world_sun(p, float3(0.0, 0.0, 1.0), eu, worldShadow, ws, 1); // one tap per step: the jitter and TAA blur it
        float vis = mix(1.0, wv.x, wv.y);
        if (vis > 0.0) vis *= enh_sun_shadow(p, float3(0.0, 0.0, 1.0), eu, shadowMap, ss, 1);
        lit += vis;
    }
    lit *= dist / float(steps) / max(v.eye.w, 1.0); // fraction of the maximum path that is sunlit
    const float g = v.color.w, ct = dot(dir, v.sun.xyz);
    const float d = 1.0 + g * g - 2.0 * g * ct;
    const float phase = (1.0 - g * g) / (4.0 * 3.14159265 * d * sqrt(d)) * 4.0 * 3.14159265; // 1 for isotropic
    return float4(v.color.rgb * lit * phase * v.sun.w, 1.0);
}
// The volume image added to the scene (additive blend pipeline).
fragment float4 fs_enh_volume_apply(GlowVOut in [[stage_in]], texture2d<float> vol [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(vol.sample(s, in.uv).rgb, 1.0);
}

// ---- motion blur (r_enhMotionBlur): 8 taps along the motion vector, after TAA, before the tone curve ----
fragment float4 fs_enh_motion_blur(GlowVOut in [[stage_in]], constant float4& u [[buffer(0)]], texture2d<float> scene [[texture(0)]],
                                   texture2d<float> vel [[texture(1)]], sampler s [[sampler(0)]], sampler ps [[sampler(1)]]) {
    const float2 v = vel.sample(ps, in.uv).rg * float2(0.5, -0.5) * u.x; // NDC -> uv, times the amount
    float3 c = 0.0;
    for (int i = 0; i < 8; ++i) c += scene.sample(s, in.uv - v * ((float(i) + 0.5) / 8.0 - 0.5)).rgb;
    return float4(c / 8.0, 1.0);
}

// ---- depth of field while aiming (game::aimDofRadius), after TAA ----
fragment float4 fs_enh_dof(GlowVOut in [[stage_in]], constant float4& u [[buffer(0)]], texture2d<float> scene [[texture(0)]],
                           texture2d<float> fz [[texture(1)]], sampler s [[sampler(0)]], sampler ps [[sampler(1)]]) {
    // u.x, u.y: clip z line; u.z: ads * strength; u.w: $floatz sky threshold; texel size from the texture
    const float2 tex = 1.0 / float2(scene.get_width(), scene.get_height());
    const float cz = fz.sample(ps, in.uv).r;
    const float w = cz >= u.w ? 1e6 : (cz - u.y) / u.x;
    const float r = 2.5 * u.z * saturate((w - 40.0) / 400.0);
    const float3 c0 = scene.sample(s, in.uv).rgb;
    if (r < 0.25) return float4(c0, 1.0);
    float3 sum = c0;
    float wsum = 1.0;
    for (int i = 0; i < 12; ++i) {
        const float2 o = kEnhPoisson[i] * r * tex;
        const float cz2 = fz.sample(ps, in.uv + o).r;
        const float w2 = cz2 >= u.w ? 1e6 : (cz2 - u.y) / u.x;
        if (w2 < 40.0) continue; // never bleed the sharp weapon into the background
        sum += scene.sample(s, in.uv + o).rgb;
        wsum += 1.0;
    }
    return float4(sum / wsum, 1.0);
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
