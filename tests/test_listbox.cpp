// LISTBOX and EDITFIELD behaviour of MenuStack (exe 0x4ce280 paint, 0x4c9c60 keys, 0x4c9880 hover, 0x4cb100 mouse, 0x4ca820 text).
#include "ui/frontend.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
} // namespace

int main() {
    using Key = iw2::ui::MenuStack::Key;
    const auto dir = std::filesystem::temp_directory_path() / "iw2_test_listbox";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "ui");
    std::ofstream(dir / "ui/l.menu") << R"({
	menuDef
	{
		name lm
		rect 0 0 640 480
		visible 1
		itemDef
		{
			name lst
			type 6
			rect 100 50 250 260
			elementwidth 120
			elementheight 14
			elementtype 0
			feeder 24
			doubleclick { setdvar dbl 1 }
			visible 1
		}
		itemDef
		{
			name field
			type 16
			dvar "newname"
			rect 400 50 100 20
			maxchars 5
			maxPaintChars 3
			accept { setdvar accepted 1 }
			visible 1
		}
		itemDef
		{
			name btn
			type 1
			rect 400 100 50 20
			visible 1
			action { setdvar pressed 1 }
		}
	}
})";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::ui::MenuLibrary lib(vfs);
    CHECK(lib.loadFile("ui/l.menu"));
    std::map<std::string, std::string> cv;
    int rows = 30;
    std::vector<int> selected;
    iw2::ui::MenuEnv env;
    env.getCvar = [&](const std::string& n) { return cv.count(n) ? cv[n] : std::string(); };
    env.setCvar = [&](const std::string& n, const std::string& v) { cv[n] = v; };
    env.feederCount = [&](int f) { return f == 24 ? rows : 0; };
    env.feederSelect = [&](int f, int r) { if (f == 24) selected.push_back(r); };
    iw2::ui::MenuStack st(lib, env);
    CHECK(st.open("lm"));
    const iw2::ui::Item& lst = *st.top()->items[0];
    const iw2::ui::Menu& menu = *st.top();
    CHECK(lst.type == 6 && lst.feeder == 24);

    // Geometry: 18 rows fit (258 - 18 * 14 = 6 < 14: the partial row ends the loop, padding 8), maxScroll = count - 18 + 1.
    CHECK(st.listVisible(lst) == 18);
    auto lay = st.listLayout(lst);
    CHECK(lay.first == 0 && lay.rows == 18 && lay.endPos == 17 && lay.drawPadding == 8);
    CHECK(st.listMaxScroll(lst) == 13);
    CHECK(st.listThumbY(menu, lst) == 67.0f);
    // Scrollbar zones in the column x + w - 16 = 334..350.
    CHECK(st.listZone(menu, lst, 340, 52) == 0x100);
    CHECK(st.listZone(menu, lst, 340, 305) == 0x200);
    CHECK(st.listZone(menu, lst, 340, 70) == 0x400);
    CHECK(st.listZone(menu, lst, 340, 200) == 0x1000);
    CHECK(st.listZone(menu, lst, 100, 200) == 0);
    // A short list ends at the last row, with endPos one past it.
    rows = 3;
    lay = st.listLayout(lst);
    CHECK(lay.rows == 3 && lay.endPos == 3 && lay.drawPadding == 0 && st.listMaxScroll(lst) == 0);
    rows = 30;

    // Mouse: row = (y - 2 - top) / 14; a click selects and notifies the feeder.
    st.tick(1000);
    st.mouseMove(150, 50 + 2 + 14 * 2 + 1);
    CHECK(st.focus() == &lst);
    CHECK(st.listState(lst).hover == 2);
    CHECK(st.click(150, 50 + 2 + 14 * 2 + 1));
    CHECK(st.listState(lst).cursor == 2 && selected.back() == 2);
    CHECK(cv["dbl"].empty());
    st.tick(1200); // second click within 300 ms on the selected row: doubleclick script
    CHECK(st.click(150, 50 + 2 + 14 * 2 + 1));
    CHECK(cv["dbl"] == "1");
    cv["dbl"].clear();
    st.tick(2000); // a slow second click does not
    CHECK(st.click(150, 50 + 2 + 14 * 2 + 1));
    CHECK(cv["dbl"].empty());
    st.tick(2100); // different row: selection moves, no doubleclick
    CHECK(st.click(150, 50 + 2 + 14 * 5 + 1));
    CHECK(st.listState(lst).cursor == 5 && cv["dbl"].empty());

    // Keys: the selection moves and scrolls to stay visible; End / Home jump.
    st.key(Key::Down);
    CHECK(st.listState(lst).cursor == 6);
    st.key(Key::End);
    CHECK(st.listState(lst).cursor == 29 && st.listState(lst).startPos == 12 && selected.back() == 29);
    st.key(Key::Down);
    CHECK(st.listState(lst).cursor == 29);
    st.key(Key::PageUp);
    CHECK(st.listState(lst).cursor == 11 && st.listState(lst).startPos == 11);
    st.key(Key::Home);
    CHECK(st.listState(lst).cursor == 0 && st.listState(lst).startPos == 0);
    st.key(Key::WheelDown);
    CHECK(st.listState(lst).cursor == 1);

    // Scrollbar buttons: one step per press, then auto-repeat after 500 ms.
    st.tick(3000);
    CHECK(st.click(340, 305)); // down arrow
    CHECK(st.listState(lst).startPos == 1);
    st.tick(3499);
    CHECK(st.listState(lst).startPos == 1);
    st.tick(3500);
    CHECK(st.listState(lst).startPos == 2);
    st.release();
    st.tick(9000);
    CHECK(st.listState(lst).startPos == 2);
    CHECK(st.click(340, 52)); // up arrow
    st.release();
    CHECK(st.listState(lst).startPos == 1);
    CHECK(st.click(340, 200)); // page down: + 18 clamps to maxScroll
    CHECK(st.listState(lst).startPos == 13);
    CHECK(st.click(340, 70)); // thumb is now at the bottom; this is a page up zone: - 18 clamps to 0
    st.release();
    CHECK(st.listState(lst).startPos == 0);

    // Thumb drag: startPos = (y - (top + 17) - 8) * maxScroll / (h - 50).
    CHECK(st.click(340, 70));
    st.mouseMove(340, 172);
    CHECK(st.listState(lst).startPos == 6);
    st.mouseMove(340, 400);
    CHECK(st.listState(lst).startPos == 13);
    st.release();
    st.mouseMove(340, 100);
    CHECK(st.listState(lst).startPos == 13);

    // setFeederSelection: row 0 resets the scroll, a row above the first shown one scrolls up.
    st.setFeederSelection(24, 4);
    CHECK(st.listState(lst).cursor == 4 && st.listState(lst).startPos == 4);
    st.setFeederSelection(24, 0);
    CHECK(st.listState(lst).cursor == 0 && st.listState(lst).startPos == 0);

    // Edit field (VALIDFILEFIELD, maxChars 5): editing starts at focus with the cursor at 0 and overstrike on.
    CHECK(st.click(410, 55));
    CHECK(st.editItem() && st.editItem()->name == "field" && st.editCursor() == 0 && st.editOffset() == 0 && st.editOverstrike());
    st.textInput("a b!c^");
    CHECK(cv["newname"] == "abc");
    st.textInput("defgh");
    CHECK(cv["newname"] == "abcde"); // maxChars 5
    CHECK(st.editCursor() == 5);
    st.key(Key::Left);
    st.key(Key::Left);
    st.textInput("X"); // overstrike replaces
    CHECK(cv["newname"] == "abcXe" && st.editCursor() == 4);
    st.key(Key::Insert);
    st.key(Key::Home);
    st.textInput("Z"); // insert mode with the field full: refused
    CHECK(cv["newname"] == "abcXe");
    st.key(Key::Delete);
    CHECK(cv["newname"] == "bcXe");
    st.textInput("Z");
    CHECK(cv["newname"] == "ZbcXe");
    st.key(Key::End);
    st.key(Key::Backspace);
    CHECK(cv["newname"] == "ZbcX");
    CHECK(st.editOffset() == 2); // maxPaintChars 3: offset = max(offset, cursor - 3) (0x4ca7e0); End made it 5 - 3
    // Colour codes: the cursor steps over a "^N" pair for free (0x4d41c0).
    cv["newname"] = "a^1bc";
    st.key(Key::End);
    st.key(Key::Left);
    st.key(Key::Left);
    CHECK(st.editCursor() == 3); // before b
    st.key(Key::Left); // the pair ^1 is skipped for free, then one step over a
    CHECK(st.editCursor() == 0);
    st.key(Key::Right);
    CHECK(st.editCursor() == 1);
    st.key(Key::Right); // the code at the cursor is skipped, then one character
    CHECK(st.editCursor() == 4);
    st.key(Key::Enter);
    CHECK(cv["accepted"] == "1" && !st.editItem());
    st.textInput("q");
    CHECK(cv["newname"] == "a^1bc"); // not editing any more

    // Tab moves the focus (the list consumes up/down), editing resumes on the field.
    st.setFeederSelection(24, 0);
    st.mouseMove(150, 100);
    CHECK(st.focus() == &lst);
    st.key(Key::Tab);
    CHECK(st.focus()->name == "field" && st.editItem());
    st.key(Key::Down); // while editing: next item
    CHECK(st.focus()->name == "btn" && !st.editItem());
    st.key(Key::Enter);
    CHECK(cv["pressed"] == "1");
    std::filesystem::remove_all(dir);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
