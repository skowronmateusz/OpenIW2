#include "util/screenshot.h"

#include <cstdio>
#include <system_error>

namespace iw2::util {

std::filesystem::path screenshotPath(const std::filesystem::path& dir, const std::string& ext) {
    for (int i = 0; i < 9999; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "shot%04i.%s", i, ext.c_str());
        const std::filesystem::path p = dir / name;
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) return p;
    }
    return dir / ("shot9999." + ext); // no free number: the last one overwrites, as the original does
}

} // namespace iw2::util
