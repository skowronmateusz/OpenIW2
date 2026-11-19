// The user home of iw2view (src/ui/userhome.h): which folder holds the profiles, saves and screenshots, and the temporary home of
// a shot. The end-to-end check that a shot ignores the profile next to the game folder is tests/iw2view_home.sh.
#include "ui/userhome.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    int failures = 0;
    auto check = [&](bool c, int line) { if (!c) { std::cerr << "line " << line << " failed\n"; ++failures; } };
    using iw2::ui::HomeSource;
    using iw2::ui::chooseHome;
    const std::filesystem::path normal = "/normal";
    // A start for playing uses the normal folder.
    {
        const auto h = chooseHome("", nullptr, false, normal);
        check(h.source == HomeSource::Normal && h.dir == normal, __LINE__);
        check(chooseHome("", "", false, normal).source == HomeSource::Normal, __LINE__); // an empty IW2_HOME is not set
    }
    // A shot or a bench never uses the normal folder: a temporary home unless one is given.
    {
        const auto h = chooseHome("", nullptr, true, normal);
        check(h.source == HomeSource::Temporary && h.dir.empty(), __LINE__);
        check(chooseHome("", "", true, normal).source == HomeSource::Temporary, __LINE__);
    }
    // IW2_HOME, and --home over it, for both kinds of start.
    for (bool automated : {false, true}) {
        const auto e = chooseHome("", "/env", automated, normal);
        check(e.source == HomeSource::Env && e.dir == "/env", __LINE__);
        const auto f = chooseHome("/flag", "/env", automated, normal);
        check(f.source == HomeSource::Flag && f.dir == "/flag", __LINE__);
        check(chooseHome("/flag", nullptr, automated, normal).dir == "/flag", __LINE__);
    }
    // The temporary home: a new, empty folder, and two of them differ; removed with its contents.
    {
        std::filesystem::path kept;
        {
            iw2::ui::TemporaryHome a, b;
            kept = a.dir();
            check(std::filesystem::is_directory(a.dir()) && std::filesystem::is_empty(a.dir()), __LINE__);
            check(a.dir() != b.dir(), __LINE__);
            check(a.dir().filename().string().rfind("openiw2-home-", 0) == 0, __LINE__);
            std::filesystem::create_directories(a.dir() / "players" / "default");
            std::ofstream(a.dir() / "players" / "default" / "config.cfg") << "seta r_enhanced \"1\"\n";
        }
        check(!std::filesystem::exists(kept), __LINE__);
    }
    for (HomeSource s : {HomeSource::Flag, HomeSource::Env, HomeSource::Temporary, HomeSource::Normal})
        check(std::string(iw2::ui::homeSourceName(s)).size() > 0, __LINE__);
    if (failures) return 1;
    std::cout << "userhome: ok\n";
    return 0;
}
