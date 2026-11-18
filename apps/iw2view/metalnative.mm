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

} // namespace iw2::metal
