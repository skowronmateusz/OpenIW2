// map_rotate, implemented from the spec in docs/spec/mp.md ("End of the map").
#include "game/maprotation.h"

#include <cctype>

namespace iw2::game {

namespace {
bool sameNoCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && !b[i];
}
} // namespace

// TODO(spec): the engine's token reader may also skip comments; the shipped server configs use plain words only.
bool rotationToken(const std::string& text, std::string& token, std::string& rest) {
    size_t i = 0;
    while (i < text.size() && static_cast<unsigned char>(text[i]) <= ' ') ++i;
    if (i >= text.size()) {
        token.clear();
        rest.clear();
        return false;
    }
    token.clear();
    if (text[i] == '"') {
        ++i;
        while (i < text.size() && text[i] != '"') token += text[i++];
        if (i < text.size()) ++i; // the closing quote
    } else {
        while (i < text.size() && static_cast<unsigned char>(text[i]) > ' ') token += text[i++];
    }
    rest = text.substr(i);
    return true;
}

MapRotateResult mapRotate(const RotationGet& get, const RotationSet& set) {
    MapRotateResult r;
    r.console.push_back("map_rotate...\n");
    r.console.push_back("\"sv_mapRotation\" is:\"" + get("sv_mapRotation") + "\"\n");
    r.console.push_back("\"sv_mapRotationCurrent\" is:\"" + get("sv_mapRotationCurrent") + "\"\n");
    // The next token of sv_mapRotationCurrent; the rest is written back (empty at the end).
    auto next = [&](std::string& tok) {
        std::string rest;
        const bool ok = rotationToken(get("sv_mapRotationCurrent"), tok, rest);
        set("sv_mapRotationCurrent", rest);
        return ok;
    };
    if (get("sv_mapRotationCurrent").empty()) set("sv_mapRotationCurrent", get("sv_mapRotation"));
    std::string tok;
    bool have = next(tok);
    if (!have) { // the current list is used up: start over from sv_mapRotation
        set("sv_mapRotationCurrent", get("sv_mapRotation"));
        have = next(tok);
    }
    auto restart = [&](const char* why) {
        r.console.push_back(why);
        r.kind = MapRotateResult::Kind::Restart;
        return r;
    };
    while (true) {
        if (!have) return restart("No map specified in sv_mapRotation - forcing map_restart.\n");
        if (sameNoCase(tok, "gametype")) {
            std::string gt;
            if (!next(gt)) return restart("No gametype specified after 'gametype' keyword in sv_mapRotation - forcing map_restart.\n");
            r.console.push_back("Setting g_gametype: " + gt + ".\n");
            // A different gametype on the running server turns the persist flag off (the clients' pers is not kept).
            if (!sameNoCase(gt, get("g_gametype").c_str())) r.gametypeChanged = true;
            set("g_gametype", gt);
        } else if (sameNoCase(tok, "map")) {
            std::string map;
            if (!next(map)) return restart("No map specified after 'map' keyword in sv_mapRotation - forcing map_restart.\n");
            r.console.push_back("Setting map: " + map + ".\n");
            r.kind = MapRotateResult::Kind::Map;
            r.map = map;
            return r;
        } else {
            r.console.push_back("Unknown keyword '" + tok + "' in sv_mapRotation.\n");
        }
        have = next(tok);
    }
}

} // namespace iw2::game
