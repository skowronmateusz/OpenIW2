#include "assets/d3dbsp.h"

#include <cctype>
#include <cstring>
#include <stdexcept>

namespace iw2::assets {
namespace {

constexpr uint32_t kVersion = 4;
constexpr size_t kHeaderSize = 8 + BspLump::Count * 8;

// Element sizes from docs/spec/d3dbsp.md (0 = not checked).
constexpr std::array<uint32_t, BspLump::Count> kElementSize = {
    72, D3dBsp::kLightmapBytes, 8, 24, 16, 8, 4, 16, 68, 2, // 0-9
    32, 4, 0, 0, 0, 0, 0, 12, 20, 4,                       // 10-19
    0, 2, 12, 52, 16, 36, 36, 4, 4, 16,                     // 20-29
    56, 72, 28, 12, 32, 48, 0, 0, 0,                        // 30-38
};

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

} // namespace

const std::string* BspEntity::get(const std::string& key) const {
    for (const auto& [k, v] : fields)
        if (k == key) return &v;
    return nullptr;
}

D3dBsp::D3dBsp(std::span<const uint8_t> data) : data_(data) {
    if (data.size() < kHeaderSize || std::memcmp(data.data(), "IBSP", 4) != 0)
        throw std::runtime_error("d3dbsp: not an IBSP file");
    const uint32_t version = rd<uint32_t>(data.data() + 4);
    if (version != kVersion) throw std::runtime_error("d3dbsp: unsupported version " + std::to_string(version));

    for (int i = 0; i < BspLump::Count; ++i) {
        BspLumpInfo& l = lumps_[size_t(i)];
        l.length = rd<uint32_t>(data.data() + 8 + i * 8);
        l.offset = rd<uint32_t>(data.data() + 12 + i * 8);
        if (uint64_t(l.offset) + l.length > data.size())
            throw std::runtime_error("d3dbsp: lump " + std::to_string(i) + " extends past end of file");
        if (l.length && kElementSize[size_t(i)] && l.length % kElementSize[size_t(i)])
            throw std::runtime_error("d3dbsp: funny size for lump " + std::to_string(i));
    }

    auto mats = lump(BspLump::Materials);
    for (size_t p = 0; p + 72 <= mats.size(); p += 72) {
        BspMaterial m;
        const char* name = reinterpret_cast<const char*>(mats.data() + p);
        m.name.assign(name, strnlen(name, 64));
        m.surfaceFlags = rd<uint32_t>(mats.data() + p + 64);
        m.contentFlags = rd<uint32_t>(mats.data() + p + 68);
        materials_.push_back(std::move(m));
    }

    auto verts = lump(BspLump::DrawVertices);
    vertices_.resize(verts.size() / 68);
    for (size_t i = 0; i < vertices_.size(); ++i) {
        const uint8_t* v = verts.data() + i * 68;
        BspVertex& o = vertices_[i];
        std::memcpy(o.position.data(), v, 12);
        std::memcpy(o.normal.data(), v + 12, 12);
        std::memcpy(o.color.data(), v + 24, 4);
        std::memcpy(o.uv.data(), v + 28, 8);
        std::memcpy(o.lightmapUv.data(), v + 36, 8);
        std::memcpy(o.tangent.data(), v + 44, 12);
        std::memcpy(o.binormal.data(), v + 56, 12);
    }

    auto idx = lump(BspLump::DrawIndices);
    indices_.resize(idx.size() / 2);
    std::memcpy(indices_.data(), idx.data(), indices_.size() * 2);

    auto soups = lump(BspLump::TriangleSoups);
    for (size_t p = 0; p + 16 <= soups.size(); p += 16) {
        const uint8_t* s = soups.data() + p;
        BspTriangleSoup t;
        t.material = rd<uint16_t>(s);
        t.lightmap = rd<uint16_t>(s + 2);
        t.firstVertex = rd<uint32_t>(s + 4);
        t.vertexCount = rd<uint16_t>(s + 8);
        t.indexCount = rd<uint16_t>(s + 10);
        t.firstIndex = rd<uint32_t>(s + 12);
        if (t.material >= materials_.size()) throw std::runtime_error("d3dbsp: soup material out of range");
        if (uint64_t(t.firstVertex) + t.vertexCount > vertices_.size() ||
            uint64_t(t.firstIndex) + t.indexCount > indices_.size())
            throw std::runtime_error("d3dbsp: soup range out of bounds");
        for (uint32_t k = 0; k < t.indexCount; ++k)
            if (indices_[t.firstIndex + k] >= t.vertexCount) throw std::runtime_error("d3dbsp: soup index out of range");
        soups_.push_back(t);
    }
}

std::span<const uint8_t> D3dBsp::lump(int index) const {
    const BspLumpInfo& l = lumps_.at(size_t(index));
    return data_.subspan(l.offset, l.length);
}

std::string D3dBsp::entityString() const {
    auto e = lump(BspLump::Entities);
    std::string s(reinterpret_cast<const char*>(e.data()), e.size());
    if (const auto nul = s.find('\0'); nul != std::string::npos) s.resize(nul);
    return s;
}

std::vector<BspEntity> D3dBsp::entities() const { return parseEntities(entityString()); }

std::vector<BspEntity> parseEntities(std::string_view text) {
    std::vector<BspEntity> out;
    size_t i = 0;
    auto skipWs = [&] { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
    auto quoted = [&](std::string& dst) {
        skipWs();
        if (i >= text.size() || text[i] != '"') return false;
        const size_t end = text.find('"', i + 1);
        if (end == std::string_view::npos) throw std::runtime_error("entities: unterminated string");
        dst.assign(text.substr(i + 1, end - i - 1));
        i = end + 1;
        return true;
    };
    for (;;) {
        skipWs();
        if (i >= text.size()) break;
        if (text[i] != '{') throw std::runtime_error("entities: expected '{'");
        ++i;
        BspEntity ent;
        for (;;) {
            skipWs();
            if (i < text.size() && text[i] == '}') { ++i; break; }
            std::string k, v;
            if (!quoted(k) || !quoted(v)) throw std::runtime_error("entities: expected key/value");
            ent.fields.emplace_back(std::move(k), std::move(v));
        }
        out.push_back(std::move(ent));
    }
    return out;
}

} // namespace iw2::assets
