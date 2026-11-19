// The user home of iw2view: the folder that holds the player profiles (players/), the saves (save/) and the screenshots
// (screenshots/). openiw2 choice, not original behaviour (docs/debug-flags.md "--home").
#pragma once

#include <filesystem>
#include <string>

namespace iw2::ui {

enum class HomeSource {
    Flag,      // --home <dir>
    Env,       // IW2_HOME=<dir>
    Temporary, // an automated run (--shot): a fresh, empty folder that is removed at exit
    Normal,    // the app's preference folder, or next to the game folder for a development build
};

// Which home a start uses. `flag` is the --home value (empty: not given), `env` the IW2_HOME value (null or empty: not set),
// `automated` true for a run that takes a shot (--shot, also the IW2_BENCH runs) and `normal` the folder a start without these
// uses. The flag wins over the variable; an automated run without either never reads or writes the user's own profile, saves or
// screenshots, so a shot or a bench shows the defaults plus its --cvar options whatever the player chose in the menus.
struct HomeChoice {
    HomeSource source = HomeSource::Normal;
    std::filesystem::path dir; // empty for Temporary: made by TemporaryHome
};
HomeChoice chooseHome(const std::string& flag, const char* env, bool automated, const std::filesystem::path& normal);

// A fresh folder `openiw2-home-XXXXXX` under the system temporary folder, removed with everything in it when the object goes away.
class TemporaryHome {
public:
    TemporaryHome();
    ~TemporaryHome();
    TemporaryHome(const TemporaryHome&) = delete;
    TemporaryHome& operator=(const TemporaryHome&) = delete;
    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path dir_;
};

const char* homeSourceName(HomeSource s);

} // namespace iw2::ui
