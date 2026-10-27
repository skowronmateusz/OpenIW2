// Player profile list behind FEEDER_PLAYER_PROFILES (feeder 0x18) and the profile uiScripts of the exe
// (addPlayerProfiles 0x4c4b80, sortPlayerProfiles 0x4c5a06, createPlayerProfile 0x4c4d20, deletePlayerProfile 0x4c4e50,
// profile exists 0x4273c0, create dir 0x427850, delete dir 0x4278f0). Profiles are the sub-directories of `players/`.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace iw2::ui {

class ProfileList {
public:
    static constexpr int kMax = 64; // names array capacity (0x11071b4..0x11072b4 = 64 pointers; create refuses at 0x40)
    enum class Create { Ok, Empty, TooMany, Exists, Failed };

    explicit ProfileList(std::filesystem::path playersDir) : dir_(std::move(playersDir)) {}

    // addPlayerProfiles: count = 0, ascending flag = 1, lists the directories of players/ (the exe's FS_ListFiles(
    // "players", "/")), sorts them case-insensitively. TODO(spec): the exe does not cap the add loop at 64 names; this does.
    void refresh();
    int count() const { return int(names_.size()); }
    // Feeder text of a row: names[index[row]] ("" when out of range).
    const std::string& name(int row) const;
    // 0x4c4a70: the row whose name equals `profile` (case-insensitive), -1 when there is none.
    int rowOf(const std::string& profile) const;
    // sortPlayerProfiles uiScript: flips the ascending flag and re-sorts (the selection then goes to row 0).
    void toggleSort();
    bool ascending() const { return ascending_; }
    // True when players/<name> exists as a directory (0x4273c0: non-empty name matched case-insensitively in the listing).
    bool exists(const std::string& profile) const;

    // createPlayerProfile (0x4c4d20): the name is cut to 31 characters; empty = nothing happens; a full list, a name already in
    // the list and a failing mkdir map to the popups profile_create_too_many_popmenu, profile_exists_popmenu and
    // profile_create_fail_popmenu. On success the name is added, the list re-sorted and `row` is the new profile's row.
    Create create(const std::string& profile, int& row);
    // deletePlayerProfile (0x4c4e50) of the profile named `selected`; false = the directory removal failed
    // (profile_delete_fail_popmenu). Success: the last slot moves into the freed one and `row` is the row to select
    // (the old row, one less when it was the last), -1 when the list is empty. An empty list does nothing and returns true.
    bool remove(const std::string& selected, int& row);

private:
    void sortIndex();
    std::filesystem::path dir_;
    std::vector<std::string> names_; // exe names array (slots)
    std::vector<int> index_;         // row -> slot
    bool ascending_ = true;
};

} // namespace iw2::ui
