#pragma once
// The version shown in the corner of the main menu (an openiw2 addition, README "Start"). The build passes
// `git describe --tags --always --dirty` of the checkout as IW2_VERSION (CMakeLists.txt); a release is built from its
// tag, so it shows "0.2.5", a build between tags "0.2.5-3-g1a2b3c4".
#include <string>

namespace iw2::ui {

// "v0.2.5" -> "OpenIW2 0.2.5"; empty -> "OpenIW2".
inline std::string versionLabel(std::string describe) {
    if (describe.size() > 1 && describe[0] == 'v' && describe[1] >= '0' && describe[1] <= '9') describe.erase(0, 1);
    return describe.empty() ? std::string("OpenIW2") : "OpenIW2 " + describe;
}

} // namespace iw2::ui
