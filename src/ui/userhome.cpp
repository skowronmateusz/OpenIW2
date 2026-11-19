#include "ui/userhome.h"

#include <cstdlib>
#include <unistd.h> // mkdtemp
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace iw2::ui {

HomeChoice chooseHome(const std::string& flag, const char* env, bool automated, const std::filesystem::path& normal) {
    if (!flag.empty()) return {HomeSource::Flag, std::filesystem::path(flag)};
    if (env && *env) return {HomeSource::Env, std::filesystem::path(env)};
    if (automated) return {HomeSource::Temporary, {}};
    return {HomeSource::Normal, normal};
}

TemporaryHome::TemporaryHome() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) base = "/tmp";
    std::string tmpl = (base / "openiw2-home-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) throw std::runtime_error("cannot create a temporary home under " + base.string());
    dir_ = buf.data();
}

TemporaryHome::~TemporaryHome() {
    std::error_code ec;
    if (!dir_.empty()) std::filesystem::remove_all(dir_, ec);
}

const char* homeSourceName(HomeSource s) {
    switch (s) {
    case HomeSource::Flag: return "--home";
    case HomeSource::Env: return "IW2_HOME";
    case HomeSource::Temporary: return "temporary";
    case HomeSource::Normal: return "normal";
    }
    return "normal";
}

} // namespace iw2::ui
