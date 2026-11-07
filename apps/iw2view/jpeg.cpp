#include "jpeg.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <string>

namespace iw2view {

bool writeJpegRgba(const std::filesystem::path& path, const uint8_t* rgba, int width, int height, int qualityPercent) {
    const size_t bytesPerRow = size_t(width) * 4;
    CGDataProviderRef data = CGDataProviderCreateWithData(nullptr, rgba, bytesPerRow * size_t(height), nullptr);
    if (!data) return false;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    // kCGImageAlphaNoneSkipLast: the 4th byte of each pixel is skipped, so the JPEG keeps the RGB the frame was drawn with.
    CGImageRef image = rgb ? CGImageCreate(width, height, 8, 32, bytesPerRow, rgb, kCGImageAlphaNoneSkipLast, data, nullptr,
                                           false, kCGRenderingIntentDefault)
                           : nullptr;
    const std::string p = path.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(p.data()), CFIndex(p.size()), false);
    // "public.jpeg" is the UTI of the JPEG format (kUTTypeJPEG without the CoreServices include).
    CGImageDestinationRef dest = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, nullptr) : nullptr;
    bool ok = false;
    if (image && dest) {
        const float q = float(std::clamp(qualityPercent, 1, 100)) / 100.0f;
        if (CFNumberRef quality = CFNumberCreate(nullptr, kCFNumberFloatType, &q)) {
            const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
            const void* values[] = {quality};
            if (CFDictionaryRef props = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks)) {
                CGImageDestinationAddImage(dest, image, props);
                CFRelease(props);
            } else
                CGImageDestinationAddImage(dest, image, nullptr);
            CFRelease(quality);
        } else
            CGImageDestinationAddImage(dest, image, nullptr);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest) CFRelease(dest);
    if (url) CFRelease(url);
    if (image) CGImageRelease(image);
    if (rgb) CGColorSpaceRelease(rgb);
    CGDataProviderRelease(data);
    return ok;
}

} // namespace iw2view
