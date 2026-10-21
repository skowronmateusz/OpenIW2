// Bitmap fonts (fonts/<name>) and localized strings (localizedstrings/*.str); see docs/spec/ui.md.
#pragma once

#include "fs/vfs.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace iw2::assets {

struct Glyph {
    uint16_t letter = 0;
    int8_t x0 = 0, y0 = 0;     // pen offset of the bitmap's top-left (y0 negative: above the baseline)
    uint8_t dx = 0;            // advance
    uint8_t width = 0, height = 0;
    float s0 = 0, t0 = 0, s1 = 0, t1 = 0;
};

struct Font {
    std::string name;
    std::string material;      // e.g. fonts/gamefonts
    int pixelHeight = 0;
    std::vector<Glyph> glyphs;
    const Glyph* find(uint16_t letter) const;
};

Font parseFont(std::span<const uint8_t> data);

// Localized strings: key "<FILE>_<REFERENCE>" (upper case) to text in the chosen language. Text stays in the
// files' single-byte code page.
class Localization {
public:
    // Loads every localizedstrings/*.str; `language` like "POLISH" or "ENGLISH" (falls back to English).
    void load(const fs::Vfs& vfs, const std::string& language);
    const std::string* find(const std::string& key) const;
    size_t size() const { return strings_.size(); }

private:
    std::map<std::string, std::string> strings_;
};

} // namespace iw2::assets
