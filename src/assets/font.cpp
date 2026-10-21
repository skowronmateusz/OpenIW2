#include "assets/font.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace iw2::assets {
namespace {

template <typename T>
T rd(std::span<const uint8_t> d, size_t at) {
    if (at + sizeof(T) > d.size()) throw std::runtime_error("font: truncated");
    T v;
    std::memcpy(&v, d.data() + at, sizeof v);
    return v;
}

std::string cstr(std::span<const uint8_t> d, size_t at) {
    std::string s;
    while (at < d.size() && d[at]) s += char(d[at++]);
    if (at >= d.size()) throw std::runtime_error("font: unterminated name");
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// Layout (observed in the shipped fonts): uint32 name offset, uint32 pixel height, uint32 glyph count,
// uint32 material-name offset, then 24-byte glyphs, then the two names.
Font parseFont(std::span<const uint8_t> d) {
    Font f;
    const uint32_t nameOff = rd<uint32_t>(d, 0);
    f.pixelHeight = int(rd<uint32_t>(d, 4));
    const uint32_t count = rd<uint32_t>(d, 8);
    const uint32_t matOff = rd<uint32_t>(d, 12);
    if (count > 4096 || 16 + size_t(count) * 24 > d.size()) throw std::runtime_error("font: bad glyph count");
    f.name = cstr(d, nameOff);
    f.material = cstr(d, matOff);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 16 + size_t(i) * 24;
        Glyph g;
        g.letter = rd<uint16_t>(d, at);
        g.x0 = rd<int8_t>(d, at + 2);
        g.y0 = rd<int8_t>(d, at + 3);
        g.dx = rd<uint8_t>(d, at + 4);
        g.width = rd<uint8_t>(d, at + 5);
        g.height = rd<uint8_t>(d, at + 6);
        g.s0 = rd<float>(d, at + 8);
        g.t0 = rd<float>(d, at + 12);
        g.s1 = rd<float>(d, at + 16);
        g.t1 = rd<float>(d, at + 20);
        f.glyphs.push_back(g);
    }
    return f;
}

const Glyph* Font::find(uint16_t letter) const {
    for (const Glyph& g : glyphs)
        if (g.letter == letter) return &g;
    return nullptr;
}

void Localization::load(const fs::Vfs& vfs, const std::string& language) {
    const std::string want = "LANG_" + upper(language);
    for (const std::string& path : vfs.list("localizedstrings/")) {
        if (path.size() < 4 || path.substr(path.size() - 4) != ".str") continue;
        const auto data = vfs.read(path);
        if (!data) continue;
        std::string file = path.substr(path.rfind('/') + 1);
        file = upper(file.substr(0, file.size() - 4));
        const std::string text(data->begin(), data->end());
        std::string ref, english;
        bool haveWanted = false;
        auto flush = [&]() {
            if (!ref.empty() && !haveWanted && !english.empty()) strings_[file + "_" + ref] = english;
        };
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t k = 0;
            while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) ++k;
            size_t ke = k;
            while (ke < line.size() && !std::isspace(static_cast<unsigned char>(line[ke]))) ++ke;
            const std::string keyword = line.substr(k, ke - k);
            const size_t q0 = line.find('"', ke), q1 = line.rfind('"');
            const std::string value = q0 != std::string::npos && q1 > q0 ? line.substr(q0 + 1, q1 - q0 - 1) : std::string();
            if (keyword == "REFERENCE") {
                flush();
                while (ke < line.size() && std::isspace(static_cast<unsigned char>(line[ke]))) ++ke;
                ref = upper(line.substr(ke));
                while (!ref.empty() && std::isspace(static_cast<unsigned char>(ref.back()))) ref.pop_back();
                english.clear();
                haveWanted = false;
            } else if (keyword == "LANG_ENGLISH") {
                english = value;
            } else if (keyword == want && !ref.empty() && !value.empty()) {
                // "#same" means the English text is used unchanged.
                strings_[file + "_" + ref] = value == "#same" ? english : value;
                haveWanted = true;
            }
        }
        flush();
    }
    // "\n" escapes in the files become line breaks.
    for (auto& [k, v] : strings_) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == 'n') { out += '\n'; ++i; }
            else out += v[i];
        }
        v = out;
    }
}

const std::string* Localization::find(const std::string& key) const {
    auto it = strings_.find(upper(key));
    return it == strings_.end() ? nullptr : &it->second;
}

} // namespace iw2::assets
