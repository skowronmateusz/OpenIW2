// Native Metal interop (metalnative.h).
#include "metalnative.h"

#include "game/renderenhanced.h"

#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#endif

namespace iw2::metal {

Caps query(SDL_GPUDevice* dev) {
    Caps c;
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(dev);
    if (!d) return c;
    c.handles = true;
    c.gpu = d.name.UTF8String ? d.name.UTF8String : "";
    if (@available(macOS 13.0, *))
        c.metalFx = [MTLFXTemporalScalerDescriptor supportsDevice:d] && !std::getenv("IW2_NO_METALFX");
    c.rayTracing = game::metalRayTracingAllowed(d.supportsRaytracing, [d supportsFamily:MTLGPUFamilyApple9], std::getenv("IW2_NO_METAL_RT") != nullptr);
#else
    (void)dev;
#endif
    return c;
}

#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
struct TemporalScaler::Impl {
    id<MTLFXTemporalScaler> scaler = nil;
    unsigned inW = 0, inH = 0, outW = 0, outH = 0;
    MTLPixelFormat color = MTLPixelFormatInvalid, depth = MTLPixelFormatInvalid, motion = MTLPixelFormatInvalid, out = MTLPixelFormatInvalid;
    bool failed = false;
};
#else
struct TemporalScaler::Impl {};
#endif

TemporalScaler::TemporalScaler() : impl_(new Impl) {}
TemporalScaler::~TemporalScaler() { delete impl_; }

bool TemporalScaler::encode(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* color, SDL_GPUTexture* depth,
                            SDL_GPUTexture* motion, SDL_GPUTexture* output, unsigned inW, unsigned inH, unsigned outW, unsigned outH,
                            float jitterNdcX, float jitterNdcY, bool reset) {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    if (@available(macOS 13.0, *)) {
        id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(dev);
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)SDL_GetGPUCommandBufferMetalCommandBuffer(cmd);
        id<MTLTexture> c = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(dev, color);
        id<MTLTexture> z = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(dev, depth);
        id<MTLTexture> m = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(dev, motion);
        id<MTLTexture> o = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(dev, output);
        if (!d || !cb || !c || !z || !m || !o) return false;
        Impl& I = *impl_;
        if (!I.scaler || I.inW != inW || I.inH != inH || I.outW != outW || I.outH != outH || I.color != c.pixelFormat ||
            I.depth != z.pixelFormat || I.motion != m.pixelFormat || I.out != o.pixelFormat) {
            MTLFXTemporalScalerDescriptor* desc = [MTLFXTemporalScalerDescriptor new];
            desc.colorTextureFormat = c.pixelFormat;
            desc.depthTextureFormat = z.pixelFormat;
            desc.motionTextureFormat = m.pixelFormat;
            desc.outputTextureFormat = o.pixelFormat;
            desc.inputWidth = inW, desc.inputHeight = inH, desc.outputWidth = outW, desc.outputHeight = outH;
            desc.autoExposureEnabled = YES;
            desc.inputContentPropertiesEnabled = NO;
            I.scaler = [desc newTemporalScalerWithDevice:d];
            I.inW = inW, I.inH = inH, I.outW = outW, I.outH = outH;
            I.color = c.pixelFormat, I.depth = z.pixelFormat, I.motion = m.pixelFormat, I.out = o.pixelFormat;
            I.failed = I.scaler == nil;
            reset = true;
        }
        if (!I.scaler) return false;
        I.scaler.colorTexture = c;
        I.scaler.depthTexture = z;
        I.scaler.motionTexture = m;
        I.scaler.outputTexture = o;
        // MetalFX takes the jitter and the motion in input pixels, y down (game::metalFxJitterPixels, game::metalFxMotionScale);
        // the signs were checked with a static camera and a pan (docs/render-enhanced.md "Stage 5").
        const auto j = game::metalFxJitterPixels(jitterNdcX, jitterNdcY, inW, inH);
        const auto ms = game::metalFxMotionScale(inW, inH);
        I.scaler.jitterOffsetX = j[0];
        I.scaler.jitterOffsetY = j[1];
        I.scaler.motionVectorScaleX = ms[0];
        I.scaler.motionVectorScaleY = ms[1];
        I.scaler.depthReversed = NO;
        I.scaler.reset = reset;
        [I.scaler encodeToCommandBuffer:cb];
        return true;
    }
#else
    (void)dev, (void)cmd, (void)color, (void)depth, (void)motion, (void)output, (void)inW, (void)inH, (void)outW, (void)outH;
    (void)jitterNdcX, (void)jitterNdcY, (void)reset;
#endif
    return false;
}

std::string TemporalScaler::describe() const {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    if (@available(macOS 13.0, *)) {
        if (!impl_->scaler) return impl_->failed ? "MetalFX scaler: could not be made" : "MetalFX scaler: none";
        id<MTLFXTemporalScaler> s = impl_->scaler;
        return "MetalFX scaler " + std::to_string(impl_->inW) + "x" + std::to_string(impl_->inH) + " -> " + std::to_string(impl_->outW) +
               "x" + std::to_string(impl_->outH) + ", usage color " + std::to_string(unsigned(s.colorTextureUsage)) + " depth " +
               std::to_string(unsigned(s.depthTextureUsage)) + " motion " + std::to_string(unsigned(s.motionTextureUsage)) + " output " +
               std::to_string(unsigned(s.outputTextureUsage));
    }
#endif
    return "MetalFX: not built";
}

#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
namespace {
id<MTLAccelerationStructure> buildPrimitive(id<MTLDevice> d, id<MTLCommandBuffer> cb, MTLPrimitiveAccelerationStructureDescriptor* desc) {
    const MTLAccelerationStructureSizes sz = [d accelerationStructureSizesWithDescriptor:desc];
    id<MTLAccelerationStructure> as = [d newAccelerationStructureWithSize:sz.accelerationStructureSize];
    id<MTLBuffer> scratch = [d newBufferWithLength:std::max<NSUInteger>(sz.buildScratchBufferSize, 16) options:MTLResourceStorageModePrivate];
    if (!as || !scratch) return nil;
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:as descriptor:desc scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    return as;
}
} // namespace

// Our own kernel (docs/render-enhanced.md "Ray-traced shadows of moving objects").
const char* kShadowSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
using namespace raytracing;
struct RtShadowU {
    float4x4 inv;   // inverse (jittered) view-projection
    float4 z;       // x, y: clip z = x * w + y; z: $floatz sky threshold; w: frame index
    float4 eye;     // xyz
    float4 sun;     // xyz towards the sun; w: tan of the cone's half angle (soft edge)
    float4 lamp[2]; // xyz, w radius (0: none)
    float4 lampW;   // x, y: the lamps' luminance (weights)
    float4 size;    // 1 / w, 1 / h, w, h
    float4 flags;   // x 1 = sun, y contact occlusion ray length (0 = off), z lamp radius (soft)
};
static float rnd(uint2 p, uint k) {
    uint h = p.x * 1973u + p.y * 9277u + k * 26699u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return float((h ^ (h >> 15)) & 0xffffu) / 65535.0;
}
static float3 posAt(texture2d<float> fz, sampler ps, constant RtShadowU& u, float2 uv, thread float& w) {
    const float cz = fz.sample(ps, uv).r;
    w = (cz - u.z.y) / u.z.x;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 wp = u.inv * float4(ndc * w, cz, w);
    return wp.xyz / wp.w;
}
static bool blocked(instance_acceleration_structure as, float3 o, float3 d, float len) {
    ray r;
    r.origin = o;
    r.direction = d;
    r.min_distance = 0.05;
    r.max_distance = len;
    intersector<instancing> isect;
    isect.accept_any_intersection(true);
    isect.assume_geometry_type(geometry_type::triangle);
    return isect.intersect(r, as, 2u).type != intersection_type::none; // the moving objects only
}
kernel void rt_dynamic_shadow(texture2d<float, access::sample> fz [[texture(0)]], texture2d<float, access::write> outMask [[texture(1)]],
                              constant RtShadowU& u [[buffer(0)]], instance_acceleration_structure as [[buffer(1)]],
                              uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uint(u.size.z) || gid.y >= uint(u.size.w)) return;
    constexpr sampler ps(filter::nearest, address::clamp_to_edge);
    const float2 uv = (float2(gid) + 0.5) * u.size.xy;
    if (fz.sample(ps, uv).r >= u.z.z) { outMask.write(float4(1.0), gid); return; }
    float w, wx, wy;
    const float3 P = posAt(fz, ps, u, uv, w);
    const float3 Px = posAt(fz, ps, u, uv + float2(u.size.x, 0.0), wx), Py = posAt(fz, ps, u, uv + float2(0.0, u.size.y), wy);
    float3 n = normalize(cross(Px - P, Py - P));
    if (dot(n, u.eye.xyz - P) < 0.0) n = -n;
    const float3 o = P + n * (1.0 + w * 0.002);
    const uint frame = uint(u.z.w);
    float sun = 1.0;
    if (u.flags.x > 0.5 && dot(n, u.sun.xyz) > 0.0) {
        const float3 t1 = normalize(cross(u.sun.xyz, abs(u.sun.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0))), t2 = cross(u.sun.xyz, t1);
        const float a = rnd(gid, frame * 2u) * 6.2831853, rr = sqrt(rnd(gid, frame * 2u + 1u)) * u.sun.w;
        const float3 d = normalize(u.sun.xyz + (t1 * cos(a) + t2 * sin(a)) * rr);
        sun = blocked(as, o, d, 3000.0) ? 0.0 : 1.0;
    }
    float lampSum = 0.0, lampVis = 0.0;
    for (int i = 0; i < 2; ++i) {
        if (u.lamp[i].w <= 0.0) continue;
        const float3 L = u.lamp[i].xyz - P;
        const float dist = length(L);
        if (dist >= u.lamp[i].w || dist < 1.0) continue;
        const float ndl = dot(n, L / dist);
        if (ndl <= 0.0) continue;
        const float f = 1.0 - dist / u.lamp[i].w;
        const float wgt = f * f * ndl * (i == 0 ? u.lampW.x : u.lampW.y);
        const float3 j = float3(rnd(gid, frame * 3u + 7u), rnd(gid, frame * 3u + 8u), rnd(gid, frame * 3u + 9u)) * 2.0 - 1.0;
        const float3 tgt = u.lamp[i].xyz + j * u.flags.z;
        const float3 dl = tgt - o;
        const float ll = length(dl);
        lampSum += wgt;
        lampVis += wgt * (blocked(as, o, dl / ll, ll - 1.0) ? 0.0 : 1.0);
    }
    const float lamp = lampSum > 1e-6 ? lampVis / lampSum : 1.0;
    float ao = 1.0;
    if (u.flags.y > 0.0) { // two cosine-weighted rays a frame against the moving objects
        const float3 t1 = normalize(cross(n, abs(n.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0))), t2 = cross(n, t1);
        float occ = 0.0;
        for (uint k = 0; k < 2u; ++k) {
            const float a = rnd(gid, frame * 4u + 20u + k * 2u) * 6.2831853, r2 = rnd(gid, frame * 4u + 21u + k * 2u);
            const float3 d = normalize(t1 * (cos(a) * sqrt(r2)) + t2 * (sin(a) * sqrt(r2)) + n * sqrt(1.0 - r2));
            occ += blocked(as, o, d, u.flags.y) ? 0.5 : 0.0;
        }
        ao = 1.0 - occ * 0.8;
    }
    outMask.write(float4(sun, lamp, ao, 1.0), gid);
}
)MSL";

struct SceneRT::Impl {
    id<MTLComputePipelineState> shadowPipe = nil;
    id<MTLAccelerationStructure> world = nil;
    size_t worldTriangles = 0;
    id<MTLAccelerationStructure> placeholderBlas = nil, placeholderTlas = nil;
    id<MTLBuffer> placeholderVerts = nil;
    id<MTLAccelerationStructure> frameTlas = nil;
    NSMutableArray<id<MTLAccelerationStructure>>* frameBlases = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> scratch = nil;
    std::string error;
    struct Item {
        SDL_GPUBuffer* vertices = nullptr;
        uint32_t vertexOffset = 0, stride = 0;
        SDL_GPUBuffer* indices = nullptr;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        float transform[16];
        const void* key = nullptr;
        bool deforming = false;
    };
    std::vector<Item> items;
    struct Cached { id<MTLAccelerationStructure> as; size_t triangles; };
    std::unordered_map<const void*, Cached> rigid;   // bottom levels of models whose vertices never change
    std::unordered_map<const void*, Cached> skinned; // bottom levels of deforming models, refit every frame
    id<MTLBuffer> itemScratch = nil;
    size_t dynamicTriangles = 0, itemCount = 0;
};

namespace {
id<MTLAccelerationStructure> buildInstances(id<MTLDevice> d, id<MTLCommandBuffer> cb, NSArray<id<MTLAccelerationStructure>>* blases,
                                            const std::vector<MTLAccelerationStructureInstanceDescriptor>& inst, id<MTLBuffer> __strong& instBuf,
                                            id<MTLBuffer> __strong& scratch) {
    const NSUInteger bytes = std::max<NSUInteger>(inst.size(), 1) * sizeof(MTLAccelerationStructureInstanceDescriptor);
    if (!instBuf || instBuf.length < bytes) instBuf = [d newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
    std::memcpy(instBuf.contents, inst.data(), inst.size() * sizeof(MTLAccelerationStructureInstanceDescriptor));
    MTLInstanceAccelerationStructureDescriptor* desc = [MTLInstanceAccelerationStructureDescriptor descriptor];
    desc.instancedAccelerationStructures = blases;
    desc.instanceCount = inst.size();
    desc.instanceDescriptorBuffer = instBuf;
    const MTLAccelerationStructureSizes sz = [d accelerationStructureSizesWithDescriptor:desc];
    id<MTLAccelerationStructure> as = [d newAccelerationStructureWithSize:sz.accelerationStructureSize];
    if (!scratch || scratch.length < sz.buildScratchBufferSize)
        scratch = [d newBufferWithLength:std::max<NSUInteger>(sz.buildScratchBufferSize * 2, 256) options:MTLResourceStorageModePrivate];
    if (!as || !scratch) return nil;
    id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:as descriptor:desc scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    return as;
}
MTLAccelerationStructureInstanceDescriptor identityInstance(uint32_t blasIndex, uint32_t mask) {
    MTLAccelerationStructureInstanceDescriptor i{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r) i.transformationMatrix.columns[c].elements[r] = (c == r) ? 1.0f : 0.0f;
    i.options = MTLAccelerationStructureInstanceOptionOpaque;
    i.mask = mask;
    i.intersectionFunctionTableOffset = 0;
    i.accelerationStructureIndex = blasIndex;
    return i;
}
} // namespace
#else
struct SceneRT::Impl {};
#endif

SceneRT::SceneRT() : impl_(new Impl) {}
SceneRT::~SceneRT() { delete impl_; }

bool SceneRT::hasStatic() const {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    return impl_->world != nil;
#else
    return false;
#endif
}

void SceneRT::clearStatic() {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    impl_->world = nil;
    impl_->worldTriangles = 0;
    impl_->rigid.clear();
    impl_->skinned.clear();
#endif
}

void SceneRT::beginItems() {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    impl_->items.clear();
#endif
}

void SceneRT::addItem(SDL_GPUBuffer* vertices, uint32_t vertexOffset, uint32_t vertexStride, SDL_GPUBuffer* indices,
                      const std::vector<std::pair<uint32_t, uint32_t>>& ranges, const float transform[16], const void* key, bool deforming) {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    if (!vertices || !indices || ranges.empty()) return;
    Impl::Item it;
    it.vertices = vertices, it.vertexOffset = vertexOffset, it.stride = vertexStride, it.indices = indices, it.ranges = ranges, it.key = key;
    it.deforming = deforming;
    std::memcpy(it.transform, transform, sizeof it.transform);
    impl_->items.push_back(std::move(it));
#else
    (void)vertices, (void)vertexOffset, (void)vertexStride, (void)indices, (void)ranges, (void)transform, (void)key, (void)deforming;
#endif
}

bool SceneRT::buildStatic(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* vertices, uint32_t vertexStride, SDL_GPUBuffer* indices,
                          const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    Impl& I = *impl_;
    I.world = nil;
    id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(dev);
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)SDL_GetGPUCommandBufferMetalCommandBuffer(cmd);
    id<MTLBuffer> vb = (__bridge id<MTLBuffer>)SDL_GetGPUBufferMetalBuffer(dev, vertices);
    id<MTLBuffer> ib = (__bridge id<MTLBuffer>)SDL_GetGPUBufferMetalBuffer(dev, indices);
    if (!d || !cb || !vb || !ib || ranges.empty()) return false;
    NSMutableArray<MTLAccelerationStructureTriangleGeometryDescriptor*>* geoms = [NSMutableArray array];
    I.worldTriangles = 0;
    for (const auto& [first, count] : ranges) {
        if (count < 3) continue;
        MTLAccelerationStructureTriangleGeometryDescriptor* g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        g.vertexBuffer = vb;
        g.vertexStride = vertexStride;
        g.vertexFormat = MTLAttributeFormatFloat3;
        g.indexBuffer = ib;
        g.indexBufferOffset = NSUInteger(first) * 4;
        g.indexType = MTLIndexTypeUInt32;
        g.triangleCount = count / 3;
        g.opaque = YES;
        [geoms addObject:g];
        I.worldTriangles += count / 3;
    }
    MTLPrimitiveAccelerationStructureDescriptor* desc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    desc.geometryDescriptors = geoms;
    I.world = buildPrimitive(d, cb, desc);
    if (!I.world) I.error = "the world structure could not be made";
    return I.world != nil;
#else
    (void)dev, (void)cmd, (void)vertices, (void)vertexStride, (void)indices, (void)ranges;
    return false;
#endif
}

bool SceneRT::buildFrame(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, bool active) {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    Impl& I = *impl_;
    id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(dev);
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)SDL_GetGPUCommandBufferMetalCommandBuffer(cmd);
    if (!d || !cb) return false;
    if (!I.placeholderTlas) { // one degenerate triangle with mask 0: nothing ever hits it
        const float v[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        I.placeholderVerts = [d newBufferWithBytes:v length:sizeof v options:MTLResourceStorageModeShared];
        MTLAccelerationStructureTriangleGeometryDescriptor* g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        g.vertexBuffer = I.placeholderVerts;
        g.vertexStride = 12;
        g.triangleCount = 1;
        MTLPrimitiveAccelerationStructureDescriptor* pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        pd.geometryDescriptors = @[g];
        I.placeholderBlas = buildPrimitive(d, cb, pd);
        id<MTLBuffer> ib = nil, sc = nil;
        if (I.placeholderBlas) I.placeholderTlas = buildInstances(d, cb, @[I.placeholderBlas], {identityInstance(0, 0)}, ib, sc);
    }
    I.frameTlas = nil;
    I.frameBlases = [NSMutableArray array];
    std::vector<MTLAccelerationStructureInstanceDescriptor> inst;
    if (active && I.world) {
        [I.frameBlases addObject:I.world];
        inst.push_back(identityInstance(0, kMaskStatic));
    }
    I.dynamicTriangles = 0, I.itemCount = 0;
    // Bottom levels of the moving objects: rigid models built once; deforming (skinned) ones built once for refitting and refit
    // every frame (same triangles, new positions). All in one encoder, each with its own part of one persistent scratch buffer.
    struct Op { id<MTLAccelerationStructure> as; MTLPrimitiveAccelerationStructureDescriptor* desc; bool refit; NSUInteger scratch; };
    std::vector<Op> ops;
    std::vector<id<MTLAccelerationStructure>> itemBlas;
    NSUInteger scratchTotal = 0;
    for (const Impl::Item& it : active ? I.items : std::vector<Impl::Item>{}) {
        id<MTLAccelerationStructure> blas = nil;
        auto& cache = it.deforming ? I.skinned : I.rigid;
        const auto c = cache.find(it.key);
        size_t tris = 0;
        for (const auto& r : it.ranges) tris += r.second / 3;
        const bool have = c != cache.end() && c->second.triangles == tris;
        if (have && !it.deforming) {
            blas = c->second.as;
        } else {
            id<MTLBuffer> vb = (__bridge id<MTLBuffer>)SDL_GetGPUBufferMetalBuffer(dev, it.vertices); // after this frame's upload
            id<MTLBuffer> ib = (__bridge id<MTLBuffer>)SDL_GetGPUBufferMetalBuffer(dev, it.indices);
            if (!vb || !ib) continue;
            NSMutableArray<MTLAccelerationStructureTriangleGeometryDescriptor*>* geoms = [NSMutableArray array];
            for (const auto& [first, count] : it.ranges) {
                if (count < 3) continue;
                MTLAccelerationStructureTriangleGeometryDescriptor* g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
                g.vertexBuffer = vb;
                g.vertexBufferOffset = it.vertexOffset;
                g.vertexStride = it.stride;
                g.vertexFormat = MTLAttributeFormatFloat3;
                g.indexBuffer = ib;
                g.indexBufferOffset = NSUInteger(first) * 4;
                g.indexType = MTLIndexTypeUInt32;
                g.triangleCount = count / 3;
                g.opaque = YES;
                [geoms addObject:g];
            }
            if (geoms.count == 0) continue;
            MTLPrimitiveAccelerationStructureDescriptor* pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            pd.geometryDescriptors = geoms;
            if (it.deforming) pd.usage = MTLAccelerationStructureUsageRefit;
            const MTLAccelerationStructureSizes sz = [d accelerationStructureSizesWithDescriptor:pd];
            const bool refit = have && it.deforming;
            if (refit) {
                blas = c->second.as;
            } else {
                blas = [d newAccelerationStructureWithSize:sz.accelerationStructureSize];
                if (!blas) continue;
                cache[it.key] = {blas, tris};
            }
            const NSUInteger need = (refit ? sz.refitScratchBufferSize : sz.buildScratchBufferSize) + 255 & ~NSUInteger(255);
            ops.push_back({blas, pd, refit, scratchTotal});
            scratchTotal += std::max<NSUInteger>(need, 256);
        }
        MTLAccelerationStructureInstanceDescriptor id{};
        for (int cc = 0; cc < 4; ++cc)
            for (int r = 0; r < 3; ++r) id.transformationMatrix.columns[cc].elements[r] = it.transform[cc * 4 + r];
        id.options = MTLAccelerationStructureInstanceOptionOpaque;
        id.mask = kMaskDynamic;
        id.accelerationStructureIndex = uint32_t(I.frameBlases.count);
        [I.frameBlases addObject:blas];
        inst.push_back(id);
        ++I.itemCount;
        I.dynamicTriangles += tris;
    }
    if (!ops.empty()) {
        if (!I.itemScratch || I.itemScratch.length < scratchTotal)
            I.itemScratch = [d newBufferWithLength:scratchTotal + scratchTotal / 2 options:MTLResourceStorageModePrivate];
        id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
        for (const Op& o : ops) {
            if (o.refit) [enc refitAccelerationStructure:o.as descriptor:o.desc destination:nil scratchBuffer:I.itemScratch scratchBufferOffset:o.scratch];
            else [enc buildAccelerationStructure:o.as descriptor:o.desc scratchBuffer:I.itemScratch scratchBufferOffset:o.scratch];
        }
        [enc endEncoding];
    }
    if (!inst.empty()) I.frameTlas = buildInstances(d, cb, I.frameBlases, inst, I.instances, I.scratch);
    return I.frameTlas != nil;
#else
    (void)dev, (void)cmd, (void)active;
    return false;
#endif
}

void SceneRT::bind(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    (void)dev, (void)cmd;
    Impl& I = *impl_;
    id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)SDL_GetGPURenderPassMetalEncoder(pass);
    if (!enc) return;
    id<MTLAccelerationStructure> top = I.frameTlas ? I.frameTlas : I.placeholderTlas;
    if (!top) return;
    [enc setFragmentAccelerationStructure:top atBufferIndex:kSceneBufferIndex];
    [enc useResource:top usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    if (I.frameTlas)
        for (id<MTLAccelerationStructure> b in I.frameBlases) [enc useResource:b usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    else if (I.placeholderBlas)
        [enc useResource:I.placeholderBlas usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
#else
    (void)dev, (void)cmd, (void)pass;
#endif
}

bool SceneRT::shadowPass(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, SDL_GPUTexture* mask, uint32_t w, uint32_t h,
                         const float* u) {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    Impl& I = *impl_;
    if (!I.frameTlas) return false;
    id<MTLDevice> d = (__bridge id<MTLDevice>)SDL_GetGPUDeviceMetalDevice(dev);
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)SDL_GetGPUCommandBufferMetalCommandBuffer(cmd);
    id<MTLTexture> z = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(dev, depth);
    id<MTLTexture> m = (__bridge id<MTLTexture>)SDL_GetGPUTextureMetalTexture(dev, mask);
    if (!d || !cb || !z || !m) return false;
    if (!I.shadowPipe) {
        NSError* err = nil;
        id<MTLLibrary> lib = [d newLibraryWithSource:[NSString stringWithUTF8String:kShadowSource] options:nil error:&err];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"rt_dynamic_shadow"] : nil;
        I.shadowPipe = fn ? [d newComputePipelineStateWithFunction:fn error:&err] : nil;
        if (!I.shadowPipe) {
            I.error = err ? err.localizedDescription.UTF8String : "no shadow pipeline";
            return false;
        }
    }
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:I.shadowPipe];
    [enc setTexture:z atIndex:0];
    [enc setTexture:m atIndex:1];
    [enc setBytes:u length:sizeof(float) * 48 atIndex:0];
    [enc setAccelerationStructure:I.frameTlas atBufferIndex:1];
    [enc useResource:I.frameTlas usage:MTLResourceUsageRead];
    for (id<MTLAccelerationStructure> b in I.frameBlases) [enc useResource:b usage:MTLResourceUsageRead];
    const NSUInteger tw = I.shadowPipe.threadExecutionWidth, th = std::max<NSUInteger>(1, I.shadowPipe.maxTotalThreadsPerThreadgroup / tw);
    [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [enc endEncoding];
    return true;
#else
    (void)dev, (void)cmd, (void)depth, (void)mask, (void)w, (void)h, (void)u;
    return false;
#endif
}

std::string SceneRT::describe() const {
#ifdef SDL_GPU_OPENIW2_METAL_HANDLES
    if (impl_->world)
        return "ray tracing scene: " + std::to_string(impl_->worldTriangles) + " static triangles, " + std::to_string(impl_->itemCount) +
               " moving objects (" + std::to_string(impl_->dynamicTriangles) + " triangles)";
    return "ray tracing scene: none" + (impl_->error.empty() ? std::string() : " (" + impl_->error + ")");
#else
    return "ray tracing scene: not built";
#endif
}

} // namespace iw2::metal
