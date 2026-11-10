// Text dump of decoded wire structures (docs/spec/net.md section 9).
#include "net/wiredump.h"

#include <cstdio>
#include <cstring>
#include <set>

#include "net/netfields.h"

namespace iw2::net {

namespace {
bool isTimeName(const char* n) {
    const size_t l = std::strlen(n);
    return l >= 4 && (std::strcmp(n + l - 4, "Time") == 0 || std::strcmp(n + l - 4, "time") == 0);
}

std::string fieldValue(const uint8_t* s, const NetField& f, int timeBase) {
    char b[64];
    if (f.bits == kFieldFloat || f.bits == kFieldSmallFloat || f.bits == kFieldAngle) {
        float v;
        std::memcpy(&v, s + f.offset, 4);
        std::snprintf(b, sizeof b, "%.6g", static_cast<double>(v));
    } else {
        int32_t v;
        std::memcpy(&v, s + f.offset, 4);
        if (timeBase && v && isTimeName(f.name)) v -= timeBase;
        std::snprintf(b, sizeof b, "%d", v);
    }
    return b;
}
} // namespace

std::string dumpFields(const uint8_t* s, const uint8_t* base, const NetField* fields, int count, int timeBase) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        if (std::memcmp(s + fields[i].offset, base + fields[i].offset, 4) == 0) continue;
        out += " ";
        out += fields[i].name;
        out += "=";
        out += fieldValue(s, fields[i], timeBase);
    }
    return out;
}

std::string dumpPlayerState(const uint8_t* ps, int timeBase) {
    static const uint8_t zero[kPlayerStateSize] = {};
    std::string out = dumpFields(ps, zero, kPlayerStateFields, 105, timeBase);
    std::set<int> covered;
    for (const NetField& f : kPlayerStateFields) covered.insert(f.offset);
    for (int off = 0; off + 4 <= kPlayerStateSize; off += 4) {
        if (covered.count(off)) continue;
        int32_t v;
        std::memcpy(&v, ps + off, 4);
        if (!v) continue;
        char b[48];
        std::snprintf(b, sizeof b, " @%d=%d", off, v);
        out += b;
    }
    return out;
}

} // namespace iw2::net
