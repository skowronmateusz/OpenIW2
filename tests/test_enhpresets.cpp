// The Enhanced graphics quality presets (docs/render-enhanced.md "Presets"): picking a preset fills the options, an option changed by
// hand makes the preset custom, an older config with "as the preset" values keeps its picture, and the profile round trip.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "game/renderenhanced.h"
#include "ui/iw2options.h"
#include "ui/menurun.h"
#include "ui/profile.h"

using namespace iw2;
using Map = std::map<std::string, std::string>;

static int failures = 0;
static void chk(bool ok, int line) {
    if (!ok) {
        std::cerr << "test_enhpresets.cpp:" << line << ": check failed\n";
        ++failures;
    }
}

static game::EnhancedSettings settingsOf(const Map& m) {
    return game::enhancedSettings([&](const char* n) { return game::enhMapGet(m, n); });
}
// Everything a preset or a page row decides (the picture), apart from the preset number itself.
static bool samePicture(const game::EnhancedSettings& a, const game::EnhancedSettings& b) {
    return a.enabled == b.enabled && a.tonemap == b.tonemap && a.bloom == b.bloom && a.autoExposure == b.autoExposure && a.edr == b.edr &&
           a.sunShadows == b.sunShadows && a.worldShadows == b.worldShadows && a.ao == b.ao && a.contactShadows == b.contactShadows &&
           a.taa == b.taa && a.ggx == b.ggx && a.ssr == b.ssr && a.lightShadows == b.lightShadows && a.mapLightSpec == b.mapLightSpec &&
           a.probes == b.probes && a.volumetric == b.volumetric && a.grade == b.grade && a.metalFx == b.metalFx && a.rtShadows == b.rtShadows &&
           a.rtLightShadows == b.rtLightShadows && a.playerShadow == b.playerShadow && a.rtBeyondView == b.rtBeyondView &&
           a.rtContactAO == b.rtContactAO && a.rtGI == b.rtGI && a.rtReflections == b.rtReflections && a.rebake == b.rebake &&
           a.renderScale == b.renderScale && a.dof == b.dof && a.motionBlur == b.motionBlur && a.gridPerPixel == b.gridPerPixel;
}
static bool holdsPreset(const Map& m, int preset) {
    for (const auto& o : game::enhPresetOptions())
        if (std::fabs(std::atof(game::enhMapGet(m, o.cvar).c_str()) - std::atof(o.values[size_t(preset)])) > 1e-4) return false;
    return true;
}

int main() {
    // The table covers exactly the page's rows (all but the master and the preset row), each preset value is a choice of its row,
    // and the registered default of each option is the medium preset's value.
    {
        std::set<std::string> rows;
        for (const ui::EnhancedRow& r : ui::enhancedRows()) {
            const std::string c = r.cvar;
            if (r.master || c == "r_enhPreset") continue;
            bool taste = false;
            for (const char* t : game::enhTasteOptions()) taste = taste || c == t;
            if (taste) {
                chk(game::enhPresetOption(r.cvar) == nullptr, __LINE__); // not part of a preset
                continue;
            }
            rows.insert(c);
            const game::EnhPresetOption* o = game::enhPresetOption(r.cvar);
            chk(o != nullptr, __LINE__);
            if (!o) continue;
            for (const char* v : o->values) {
                bool offered = false;
                if (r.kind == ui::EnhancedRow::OnOff) offered = std::string(v) == "0" || std::string(v) == "1";
                for (const auto& [label, value] : r.choices) offered = offered || std::atof(value.c_str()) == std::atof(v);
                chk(offered, __LINE__);
            }
            for (const auto& [label, value] : r.choices) chk(std::atof(value.c_str()) >= 0.0, __LINE__); // no "as the preset" value
            chk(ui::registeredDefault(r.cvar) == o->values[1], __LINE__);
        }
        chk(rows.size() == game::enhPresetOptions().size(), __LINE__);
        const ui::EnhancedRow* preset = nullptr;
        for (const ui::EnhancedRow& r : ui::enhancedRows())
            if (std::string(r.cvar) == "r_enhPreset") preset = &r;
        chk(preset && preset->choices.size() == 5 && preset->choices[4].first == "@IW2_ENH_CUSTOM" && preset->choices[4].second == "4", __LINE__);
        chk(ui::registeredDefault("r_enhPreset") == "1", __LINE__);
    }

    // Picking a preset writes its values into every option; the picture is the preset's (as before the redesign).
    {
        Map m;
        game::enhMapResolve(m); // a new profile: medium, every option written
        game::enhMapSet(m, "r_enhanced", "1");
        chk(game::enhMapGet(m, "r_enhPreset") == "1" && holdsPreset(m, 1), __LINE__);
        for (int p = 0; p < 4; ++p) {
            game::enhMapSetOption(m, "r_enhPreset", std::to_string(p));
            chk(game::enhMapGet(m, "r_enhPreset") == std::to_string(p) && holdsPreset(m, p), __LINE__);
        }
        game::enhMapSetOption(m, "r_enhPreset", "0");
        game::EnhancedSettings s = settingsOf(m);
        chk(s.preset == 0 && s.sunShadows == 1 && s.worldShadows == 0 && !s.ao && !s.contactShadows && !s.taa && !s.ssr && !s.lightShadows &&
                !s.autoExposure && !s.mapLightSpec && !s.probes && s.bloom && s.playerShadow && !s.rtGI, __LINE__);
        game::enhMapSetOption(m, "r_enhPreset", "2");
        s = settingsOf(m);
        chk(s.sunShadows == 3 && s.worldShadows == 2 && s.volumetric == 0.35f && s.ao && s.taa && !s.rtGI, __LINE__);
        chk(s.metalFx == (std::string(game::enhPresetOption("r_enhMetalFX")->values[2]) == "1"), __LINE__); // the table's value
        game::enhMapSetOption(m, "r_enhPreset", "3");
        const game::EnhancedSettings u = settingsOf(m);
        game::EnhancedSettings h = s;
        h.rtGI = true;
        chk(u.rtGI && samePicture(u, h), __LINE__); // ultra = high + RT global illumination
    }

    // An option changed by hand makes the preset custom; the options keep their values. A value equal to the preset's changes nothing.
    {
        Map m;
        game::enhMapResolve(m);
        game::enhMapSetOption(m, "r_enhPreset", "2");
        game::enhMapSetOption(m, "r_enhAO", "1"); // the high preset's own value
        chk(game::enhMapGet(m, "r_enhPreset") == "2", __LINE__);
        game::enhMapSetOption(m, "r_enhAO", "0");
        chk(game::enhMapGet(m, "r_enhPreset") == "4" && game::enhMapGet(m, "r_enhAO") == "0", __LINE__);
        chk(game::enhMapGet(m, "r_enhSunShadows") == "3" && game::enhMapGet(m, "r_enhVolumetric") == "0.35", __LINE__); // the rest stays high
        const game::EnhancedSettings c = settingsOf(m);
        chk(c.preset == 4 && !c.ao && c.sunShadows == 3 && c.worldShadows == 2, __LINE__);
        game::enhMapSetOption(m, "r_enhSSR", "0"); // further changes keep it custom
        chk(game::enhMapGet(m, "r_enhPreset") == "4" && game::enhMapGet(m, "r_enhAO") == "0", __LINE__);
        game::enhMapSetOption(m, "r_enhPreset", "4"); // picking Custom by hand changes no option
        chk(game::enhMapGet(m, "r_enhAO") == "0" && game::enhMapGet(m, "r_enhSSR") == "0", __LINE__);
        game::enhMapSetOption(m, "r_enhPreset", "1"); // a preset picked again replaces the hand-made values
        chk(holdsPreset(m, 1) && game::enhMapGet(m, "r_enhPreset") == "1", __LINE__);
        game::enhMapSetOption(m, "r_enhTonemap", "0"); // not a page row: no preset change
        chk(game::enhMapGet(m, "r_enhPreset") == "1", __LINE__);
        // The taste options: changing one keeps the preset, a pick leaves them as they are.
        for (const char* t : game::enhTasteOptions()) {
            const std::string v = std::string(t) == "r_enhEdr" ? "0" : "1";
            game::enhMapSetOption(m, t, v);
            chk(game::enhMapGet(m, "r_enhPreset") == "1" && game::enhMapGet(m, t) == v, __LINE__);
        }
        game::enhMapSetOption(m, "r_enhPreset", "3");
        for (const char* t : game::enhTasteOptions()) chk(game::enhMapGet(m, t) == (std::string(t) == "r_enhEdr" ? "0" : "1"), __LINE__);
        const game::EnhancedSettings taste = settingsOf(m);
        chk(taste.grade == 1 && taste.motionBlur == 1.0f && taste.dof == 1.0f && !taste.edr && taste.preset == 3, __LINE__);
        // MetalFX and its resolution are part of the presets: a pick sets them, a change by hand makes it custom.
        game::enhMapSetOption(m, "r_enhPreset", "2");
        chk(game::enhMapGet(m, "r_enhMetalFX") == game::enhPresetOption("r_enhMetalFX")->values[2] &&
                game::enhMapGet(m, "r_enhRenderScale") == game::enhPresetOption("r_enhRenderScale")->values[2], __LINE__);
        game::enhMapSetOption(m, "r_enhRenderScale", "0.5");
        chk(game::enhMapGet(m, "r_enhPreset") == "4", __LINE__);
        game::enhMapSetOption(m, "r_enhPreset", "1");
        // the menu path: enhPresetAfterSet with a lookup that already sees the new value
        Map n = m;
        game::enhMapSet(n, "r_enhSSR", "0");
        const auto w = game::enhPresetAfterSet("r_enhSSR", [&](const char* k) { return game::enhMapGet(n, k); });
        chk(w.size() == 1 && w[0].first == "r_enhPreset" && w[0].second == "4", __LINE__);
        // a --cvar r_enhPreset first, then an option (iw2view applies them in this order)
        Map cli;
        game::enhMapResolve(cli);
        game::enhMapSetOption(cli, "r_enhpreset", "3");
        game::enhMapSetOption(cli, "r_enhRenderScale", "0.5");
        chk(game::enhMapGet(cli, "r_enhPreset") == "4" && game::enhMapGet(cli, "r_enhRTGI") == "1" && game::enhMapGet(cli, "r_enhRenderScale") == "0.5", __LINE__);
        chk(cli.count("r_enhPreset") == 1 && cli.count("r_enhpreset") == 0, __LINE__); // the page's spelling
    }

    // Migration: a config written before the redesign (-1 = as the preset, options missing) keeps its picture.
    {
        for (int p = 0; p < 4; ++p) { // every old preset with every option left to it
            Map old{{"r_enhanced", "1"}, {"r_enhPreset", std::to_string(p)}};
            for (const auto& o : game::enhPresetOptions())
                if (!o.unsetBefore) old[o.cvar] = "-1"; // MetalFX and its resolution had no "as the preset" value
            old["r_enhRebake"] = "-1";
            const game::EnhancedSettings before = settingsOf(old);
            Map now = old;
            game::enhMapResolve(now);
            // the picture of the earlier version: MetalFX and its resolution unset there were off and native
            game::EnhancedSettings was = before;
            was.metalFx = false, was.renderScale = 1.0f;
            const game::EnhancedSettings after = settingsOf(now);
            chk(samePicture(was, after) && !after.metalFx && after.renderScale == 1.0f, __LINE__);
            const bool presetHad = std::string(game::enhPresetOption("r_enhMetalFX")->values[size_t(p)]) == "0" &&
                                   std::string(game::enhPresetOption("r_enhRenderScale")->values[size_t(p)]) == "1";
            chk(game::enhMapGet(now, "r_enhPreset") == (presetHad ? std::to_string(p) : "4") && (presetHad ? holdsPreset(now, p) : true), __LINE__);
            chk(game::enhMapGet(now, "r_enhRebake") == "0", __LINE__);
        }
        // an old high config with its own choices: the picture stays, the preset becomes custom
        const std::string text = "seta r_enhanced \"1\"\nseta r_enhPreset \"2\"\nseta r_enhSunShadows \"-1\"\nseta r_enhAO \"0\"\n"
                                 "seta r_enhRTGI \"-1\"\nseta r_enhgrade \"1\"\nseta r_enhVolumetric \"-1\"\n";
        const Map old = ui::parseConfig(text);
        game::EnhancedSettings before = settingsOf(old);
        before.metalFx = false, before.renderScale = 1.0f; // unset in the earlier version: off, native
        Map now = old;
        game::enhMapResolve(now);
        const game::EnhancedSettings after = settingsOf(now);
        chk(samePicture(before, after) && after.preset == 4 && !after.ao && after.grade == 1 && after.sunShadows == 3 && after.volumetric == 0.35f, __LINE__);
        chk(now.count("r_enhGrade") == 1 && now.count("r_enhgrade") == 0, __LINE__);
        // an old low config (MetalFX off and native there too) stays on that preset
        Map exact{{"r_enhPreset", "0"}, {"r_enhAO", "0"}, {"r_enhTAA", "0"}};
        game::enhMapResolve(exact);
        chk(game::enhMapGet(exact, "r_enhPreset") == "0" && holdsPreset(exact, 0), __LINE__);
        // a new profile (no enhanced setting saved): the medium preset with all its values, MetalFX included
        Map fresh{{"sensitivity", "5"}};
        game::enhMapResolve(fresh);
        chk(game::enhMapGet(fresh, "r_enhPreset") == "1" && holdsPreset(fresh, 1), __LINE__);
        // an old config that saved an option of the page only: its MetalFX was off and native
        Map none{{"r_enhAO", "-1"}};
        game::enhMapResolve(none);
        chk(game::enhMapGet(none, "r_enhMetalFX") == "0" && game::enhMapGet(none, "r_enhRenderScale") == "1", __LINE__);
        Map tasteOnly{{"r_enhGrade", "1"}};
        game::enhMapResolve(tasteOnly);
        chk(game::enhMapGet(tasteOnly, "r_enhMetalFX") == "0" && game::enhMapGet(tasteOnly, "r_enhGrade") == "1", __LINE__);
        // a custom config with an unresolved option takes the medium value there
        Map custom{{"r_enhPreset", "4"}, {"r_enhAO", "0"}, {"r_enhSunShadows", "-1"}};
        game::enhMapResolve(custom);
        chk(game::enhMapGet(custom, "r_enhPreset") == "4" && game::enhMapGet(custom, "r_enhSunShadows") == "2" && game::enhMapGet(custom, "r_enhAO") == "0", __LINE__);
        // out-of-range presets: clamped
        chk(game::enhPresetFrom("9") == 4 && game::enhPresetFrom("-1") == 1 && game::enhPresetFrom("") == 1 && game::enhPresetFrom("x") == 1, __LINE__);
    }

    // Save and load: the profile's config.cfg written after a pick or a custom change reads back to the same options and picture,
    // and a resolved config needs no more writes.
    {
        for (const char* step : {"pick", "custom"}) {
            Map m{{"r_enhanced", "1"}, {"sensitivity", "5"}};
            game::enhMapResolve(m);
            game::enhMapSetOption(m, "r_enhPreset", "3");
            if (std::string(step) == "custom") game::enhMapSetOption(m, "r_enhVolumetric", "0.7");
            game::enhMapSetOption(m, "r_enhGrade", "1"); // a taste option goes through the round trip too
            for (const auto& o : game::enhPresetOptions()) chk(ui::isArchivedCvar(o.cvar), __LINE__);
            Map back = ui::parseConfig(ui::formatConfig(m));
            chk(game::enhPresetResolve([&](const char* n) { return game::enhMapGet(back, n); }).empty(), __LINE__);
            game::enhMapResolve(back);
            for (const auto& o : game::enhPresetOptions()) chk(game::enhMapGet(back, o.cvar) == game::enhMapGet(m, o.cvar), __LINE__);
            chk(game::enhMapGet(back, "r_enhGrade") == "1", __LINE__);
            chk(game::enhMapGet(back, "r_enhPreset") == (std::string(step) == "pick" ? "3" : "4"), __LINE__);
            chk(samePicture(settingsOf(m), settingsOf(back)), __LINE__);
        }
    }

    if (failures) return 1;
    std::cout << "enhpresets: ok\n";
    return 0;
}
