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
    return failures ? 1 : 0;
}
