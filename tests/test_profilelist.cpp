// Profile list rules proven from the exe (src/ui/profilelist.h).
#include "ui/profilelist.h"

#include <filesystem>
#include <fstream>
#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";    \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    using iw2::ui::ProfileList;
    const fs::path dir = fs::temp_directory_path() / "iw2_test_profilelist";
    fs::remove_all(dir);
    fs::create_directories(dir / "Bravo");
    fs::create_directories(dir / "alpha");
    fs::create_directories(dir / "charlie");
    std::ofstream(dir / "active.txt") << "alpha"; // a file, not a profile

    ProfileList pl(dir);
    pl.refresh();
    CHECK(pl.count() == 3);
    CHECK(pl.name(0) == "alpha" && pl.name(1) == "Bravo" && pl.name(2) == "charlie"); // case-insensitive ascending
    CHECK(pl.name(3).empty() && pl.name(-1).empty());
    CHECK(pl.rowOf("BRAVO") == 1 && pl.rowOf("zulu") == -1);
    CHECK(pl.exists("CHARLIE") && !pl.exists("") && !pl.exists("active.txt"));

    pl.toggleSort();
    CHECK(!pl.ascending() && pl.name(0) == "charlie" && pl.name(2) == "alpha");
    pl.toggleSort();
    CHECK(pl.ascending() && pl.name(0) == "alpha");

    int row = 0;
    CHECK(pl.create("", row) == ProfileList::Create::Empty);
    CHECK(pl.create("bravo", row) == ProfileList::Create::Exists);
    CHECK(pl.create("Delta", row) == ProfileList::Create::Ok && row == 3 && pl.name(3) == "Delta" && pl.count() == 4);
    CHECK(fs::is_directory(dir / "Delta"));
    // the name is cut to 31 characters
    CHECK(pl.create("0123456789012345678901234567890123", row) == ProfileList::Create::Ok);
    CHECK(fs::is_directory(dir / "0123456789012345678901234567890"));

    // delete: the last slot moves into the freed one and the old row stays selected (one less past the end)
    CHECK(pl.remove("alpha", row) && row == 1 && !fs::exists(dir / "alpha") && pl.count() == 4);
    CHECK(pl.rowOf("alpha") == -1);
    CHECK(pl.remove("charlie", row) && pl.count() == 3);
    while (pl.count() > 0) {
        const std::string n = pl.name(0);
        CHECK(pl.remove(n, row));
    }
    CHECK(row == -1 && pl.count() == 0);
    CHECK(pl.remove("none", row)); // empty list: nothing happens

    // a full list refuses new profiles
    for (int i = 0; i < ProfileList::kMax; ++i) {
        CHECK(pl.create("p" + std::to_string(i), row) == ProfileList::Create::Ok);
    }
    CHECK(pl.create("one_more", row) == ProfileList::Create::TooMany);

    fs::remove_all(dir);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
