// Profile config text round trip.
#include "ui/menurun.h"
#include "ui/profile.h"

#include <iostream>

int main() {
    int failures = 0;
    auto check = [&](bool c, int line) { if (!c) { std::cerr << "line " << line << " failed\n"; ++failures; } };
    std::map<std::string, std::string> cv{{"sensitivity", "7"}, {"m_pitch", "-0.022"}, {"snd_volume", "0.5"}, {"mis_03", "1"}, {"ui_background", "x"}, {"sv_running", "1"}};
    const std::string text = iw2::ui::formatConfig(cv);
    const auto back = iw2::ui::parseConfig(text);
    check(back.size() == 4, __LINE__);
    check(back.at("m_pitch") == "-0.022" && back.at("sensitivity") == "7" && back.at("mis_03") == "1", __LINE__);
    check(back.count("ui_background") == 0 && back.count("sv_running") == 0, __LINE__);
    check(iw2::ui::parseConfig("bind A x\nseta snd_volume \"0.3\"\r\n").at("snd_volume") == "0.3", __LINE__);
    // Mission progress (mis_NN, mis_none, mis_difficulty) is archived and defaults as registered by the exe.
    std::string prog(50, '0');
    prog[0] = '2';
    const auto pb = iw2::ui::parseConfig(iw2::ui::formatConfig({{"mis_01", "10"}, {"mis_none", "0"}, {"mis_difficulty", prog}}));
    check(pb.size() == 3 && pb.at("mis_01") == "10" && pb.at("mis_difficulty") == prog, __LINE__);
    check(iw2::ui::registeredDefault("mis_difficulty") == std::string(50, '0') && iw2::ui::registeredDefault("mis_10") == "0", __LINE__);
    // sv_lastSaveGame is archived (flags 0x1001, 0x449d78): the last committed save survives a restart, so loadgame_continue finds it.
    const auto ls = iw2::ui::parseConfig(iw2::ui::formatConfig({{"sv_lastSaveGame", "autosave/hill400_defend-6"}, {"sv_running", "1"}}));
    check(ls.size() == 1 && ls.at("sv_lastSaveGame") == "autosave/hill400_defend-6", __LINE__);
    // r_gamma (the brightness slider) and r_ignorehwgamma are archived renderer dvars (gfx 0x1000ab8a / 0x1000abc0): saved to the profile,
    // registered defaults 1 and 0 (the slider needs r_ignorehwgamma == 0 to show).
    const auto gm = iw2::ui::parseConfig(iw2::ui::formatConfig({{"r_gamma", "1.8"}, {"r_ignorehwgamma", "0"}, {"r_picmip", "1"}}));
    check(gm.size() == 2 && gm.at("r_gamma") == "1.8" && gm.at("r_ignorehwgamma") == "0", __LINE__);
    check(iw2::ui::registeredDefault("r_gamma") == "1" && iw2::ui::registeredDefault("r_ignorehwgamma") == "0", __LINE__);
    check(iw2::ui::registeredDefault("ui_multiplayer") == "0", __LINE__);
    check(iw2::ui::registeredDefault("cg_hudGrenadeIconInScope") == "0" && iw2::ui::isArchivedCvar("cg_hudGrenadeIconInScope"), __LINE__);
    // Option dvars archived in the profile (flags 0x1001 / 0x2021) and r_zFeather not (0x2000); vsync on by default (deviation).
    for (const char* n : {"cg_blood", "cg_drawCrosshair", "cg_subtitles", "m_filter", "cl_freelook", "r_swapInterval"})
        check(iw2::ui::isArchivedCvar(n), __LINE__);
    check(!iw2::ui::isArchivedCvar("r_zFeather") && iw2::ui::registeredDefault("r_zFeather") == "1", __LINE__);
    check(iw2::ui::registeredDefault("r_swapInterval") == "1" && iw2::ui::registeredDefault("m_filter") == "0", __LINE__);
    check(iw2::ui::registeredDefault("cl_yawspeed") == "140" && iw2::ui::registeredDefault("cl_anglespeedkey") == "1.5", __LINE__);
    // The texture filter is archived (flags 0x2001) and its menu row takes effect without the Apply button.
    const auto tf = iw2::ui::parseConfig(iw2::ui::formatConfig({{"r_texturemode", "trilinear"}, {"r_anisotropy", "8"}, {"ui_r_texturemode", "trilinear"}}));
    check(tf.size() == 2 && tf.at("r_texturemode") == "trilinear" && tf.at("r_anisotropy") == "8", __LINE__);
    check(iw2::ui::immediateMenuDvar("ui_r_textureMode") == "r_texturemode" && iw2::ui::immediateMenuDvar("ui_r_picmip").empty(), __LINE__);
    return failures ? 1 : 0;
}
