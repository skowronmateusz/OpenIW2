#include "ui/profilelist.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace iw2::ui {

namespace fs = std::filesystem;

namespace {
int icmp(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = std::tolower((unsigned char)a[i]), y = std::tolower((unsigned char)b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
} // namespace

void ProfileList::sortIndex() {
    index_.resize(names_.size());
    for (size_t i = 0; i < index_.size(); ++i) index_[i] = int(i);
    std::stable_sort(index_.begin(), index_.end(), [&](int a, int b) {
        const int c = icmp(names_[a], names_[b]);
        return ascending_ ? c < 0 : c > 0; // 0x4c4ab0 negates the compare when the flag is 0
    });
}

void ProfileList::refresh() {
    names_.clear();
    ascending_ = true;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        if (int(names_.size()) >= kMax) break;
        names_.push_back(it->path().filename().string());
    }
    sortIndex();
}

const std::string& ProfileList::name(int row) const {
    static const std::string none;
    return row >= 0 && row < count() ? names_[index_[row]] : none;
}

int ProfileList::rowOf(const std::string& profile) const {
    for (int r = 0; r < count(); ++r)
        if (icmp(names_[index_[r]], profile) == 0) return r;
    return -1;
}

void ProfileList::toggleSort() {
    ascending_ = !ascending_;
    sortIndex();
}

bool ProfileList::exists(const std::string& profile) const {
    if (profile.empty()) return false;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && icmp(it->path().filename().string(), profile) == 0) return true;
    }
    return false;
}

ProfileList::Create ProfileList::create(const std::string& wanted, int& row) {
    row = -1;
    const std::string profile = wanted.substr(0, 31);
    if (profile.empty()) return Create::Empty;
    if (count() == kMax) return Create::TooMany;
    if (rowOf(profile) >= 0) return Create::Exists;
    if (exists(profile)) return Create::Failed; // 0x427850 refuses a directory that already exists ("Profile '%s' already exists")
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (!fs::create_directory(dir_ / profile, ec) || ec) return Create::Failed; // "Unable to create new profile path: %s"
    names_.push_back(profile);
    sortIndex();
    row = rowOf(profile);
    return Create::Ok;
}

bool ProfileList::remove(const std::string& selected, int& row) {
    row = -1;
    if (names_.empty()) return true;
    if (!exists(selected)) return false;
    std::error_code ec;
    fs::remove_all(dir_ / selected, ec);
    if (ec || fs::exists(dir_ / selected, ec)) return false;
    int idx = rowOf(selected);
    if (idx < 0) return true; // TODO(spec): 0x4c4a70 returns -1 for a name that is not in the list and the exe indexes index[-1]
    const int slot = index_[idx];
    names_[slot] = names_.back(); // the last name takes the freed slot (0x4c4ece..0x4c4edb)
    names_.pop_back();
    if (names_.empty()) {
        index_.clear();
        return true;
    }
    if (idx == int(names_.size())) --idx;
    sortIndex();
    row = idx;
    return true;
}

} // namespace iw2::ui
