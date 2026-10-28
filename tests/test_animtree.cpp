// Tests for the animtree parser: block comments and the sync flags of synced subtrees (0x479260 / 0x479120), and
// (with IW2_MAIN) the notetrack delivery of a flagged animation (0x476f10).

#include "fs/vfs.h"
#include "game/animtree.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

} // namespace

int main() {
    const iw2::game::AnimTree t("test", R"(
body
{
    loops : loopsync
    {
        walk
        run
        {
            run_f
            run_b
        }
    }
    starts : nonloopsync
    {
        start_a
    }
/*  hidden : complete loopsync
    {
        hidden_leaf
    }
*/
    plain
    {
        idle // : loopsync
    }
}
)");
    auto flags = [&](const char* n) { const int i = t.find(n); return i < 0 ? -1 : t.nodes()[size_t(i)].syncFlags; };
    CHECK(flags("loops") == 5);
    CHECK(flags("run") == 1);
    CHECK(flags("run_f") == 0); // leaves carry an anim, not flags
    CHECK(flags("starts") == 6);
    CHECK(flags("plain") == 0);
    CHECK(t.find("hidden") < 0);
    CHECK(t.find("hidden_leaf") < 0);
    CHECK(t.nodes()[size_t(t.find("idle"))].parent == t.find("plain"));

    // Game data (IW2_MAIN): a rifle reload played the way animscripts/combat_utility.gsc Reload() starts it
    // (setFlaggedAnimKnobAll("reloadanim", %reload_stand_rifle, %body, 1, .1, 1)) sends each of its notetracks once,
    // in time order, to the flag name, among them "reload done" (shared.gsc DoNoteTracks refills the clip there),
    // then "end" (0x476f10 sends the last notetrack, "end", again on every update once the node stands at 1).
    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        iw2::game::AnimLibrary lib(vfs);
        const iw2::game::AnimTree* tree = lib.tree("generic_human");
        CHECK(tree != nullptr);
        if (tree) {
            iw2::game::AnimState st(*tree, lib);
            const int body = tree->find("body"), reload = tree->find("reload_stand_rifle");
            CHECK(body >= 0 && reload >= 0);
            const iw2::assets::XAnim* a = lib.anim("reload_stand_rifle");
            CHECK(a != nullptr && !a->looping);
            if (body >= 0 && reload >= 0 && a) {
                iw2::game::AnimState::SetArgs on; // an actor's %body is already weighted (knob-all stops below it)
                on.blendTime = 0;
                st.set(body, on);
                iw2::game::AnimState::SetArgs args;
                args.blendTime = 0.1f;
                args.knobAllRoot = body;
                args.notify = "reloadanim";
                st.set(reload, args);
                std::vector<std::string> got;
                for (int i = 0; i < 400 && std::count(got.begin(), got.end(), "end") < 2; ++i)
                    st.update(0.05f, [&](const std::string& flag, const std::string& note) {
                        CHECK(flag == "reloadanim");
                        got.push_back(note);
                    });
                std::vector<std::string> want;
                for (const auto& n : a->notes) want.push_back(n.name);
                CHECK(!want.empty() && want.back() == "end");
                CHECK(std::find(want.begin(), want.end(), "reload done") != want.end());
                CHECK(got.size() == want.size() + 1); // every notetrack once, then "end" once more on the next update
                if (got.size() == want.size() + 1) {
                    for (size_t i = 0; i < want.size(); ++i) CHECK(got[i] == want[i]);
                    CHECK(got.back() == "end");
                }
            }
        }
    }
    if (failures) return 1;
    std::cout << "animtree tests passed\n";
    return 0;
}
