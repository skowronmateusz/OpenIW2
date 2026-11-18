// Native Metal interop (metalnative.h).
#include "metalnative.h"

#include "game/renderenhanced.h"

#include <SDL3/SDL_gpu.h>
#include <cstdlib>

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

} // namespace iw2::metal
