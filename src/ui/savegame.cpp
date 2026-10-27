#include "ui/savegame.h"

#include <cstring>

namespace iw2::ui {

std::string saveFileName(const std::string& name, bool autosaveDir) {
    if (name.size() >= 0x40) return {};
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '/' || c == '\\') {
            out += (autosaveDir && !(i == 8 && name.compare(0, 8, "autosave") == 0)) ? '-' : '/';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out += c;
        } else {
            return {};
        }
    }
    return "save/" + out + ".svg";
}

static int32_t dword(const std::vector<uint8_t>& b, size_t off) {
    int32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

bool saveHeaderLoadable(const std::vector<uint8_t>& b) { return b.size() >= kSaveHeaderSize && dword(b, 0) == kSaveVersion; }

bool saveHeaderListable(const std::vector<uint8_t>& b) {
    if (b.size() < kSaveHeaderSize) return false;
    const int32_t v = dword(b, 0);
    return v >= kSaveMinListVersion && v <= kSaveVersion;
}

bool saveSortKey(const std::vector<uint8_t>& b, SaveSortKey& out) {
    if (b.size() < kSaveHeaderSize) return false;
    for (size_t i = 0; i < 9; ++i) out[i] = dword(b, 0x4ac + 4 * i);
    return true;
}

bool saveListBefore(const SaveSortKey& a, const SaveSortKey& b) {
    // dword indices relative to +0x4ac: +0x4c0 = 5, +0x4c8 = 7, +0x4b4 = 2, +0x4b0 = 1, +0x4ac = 0
    for (const size_t i : {size_t(5), size_t(7), size_t(2), size_t(1), size_t(0)})
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

} // namespace iw2::ui
