// Menu event scripts: a synthetic yes/no popup with the same kind of actions as the invert-axis popup of moscow.
#include <unistd.h>
#include <string>
#include "ui/menurun.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

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
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_menurun_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "ui");
    std::ofstream(dir / "ui/popup.menu") << R"({
	menuDef
	{
		name test_popup
		rect 204 160 235 160
		popup
		onESC { scriptMenuResponse "popup_no"; close "test_popup"; }
		itemDef
		{
			name invert_enabled
			type 1
			rect 70 90 100 20
			visible 1
			action { play "test_click"; scriptMenuResponse "popup_yes"; execnow "toggle ui_mousePitch"; uiScript update ui_mousePitch; close test_popup; }
		}
		itemDef
		{
			name invert_disabled
			type 1
			rect 70 50 100 20
			visible 1
			action { play "test_click"; scriptMenuResponse "popup_no"; close test_popup; }
		}
		itemDef { name title text "x" rect 0 0 10 10 visible 1 decoration }
	}
})";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::ui::MenuLibrary lib(vfs);
    CHECK(lib.loadFile("ui/popup.menu"));
    const iw2::ui::Menu* m = lib.find("test_popup");
    CHECK(m != nullptr);
    if (!m) return 1;

    std::map<std::string, std::string> cvars;
    std::vector<std::string> log;
    iw2::ui::MenuEnv env;
    env.getCvar = [&](const std::string& n) { return cvars.count(n) ? cvars[n] : std::string(); };
    env.setCvar = [&](const std::string& n, const std::string& v) { cvars[n] = v; };
    env.scriptMenuResponse = [&](const std::string& menu, const std::string& r) { log.push_back("resp " + menu + " " + r); };
    env.close = [&](const std::string& menu) { log.push_back("close " + menu); };
    env.play = [&](const std::string& a) { log.push_back("play " + a); };

    // Start-up: m_pitch missing or positive -> ui_mousePitch 0; negative -> 1.
    iw2::ui::initUiCvars(env);
    CHECK(cvars["ui_mousePitch"] == "0");
    cvars["m_pitch"] = "-0.022";
    iw2::ui::initUiCvars(env);
    CHECK(cvars["ui_mousePitch"] == "1");
    cvars["m_pitch"] = "0.022";
    iw2::ui::initUiCvars(env);

    // Hit test: menu rect (204,160) + item rect (70,90,100,20) = (274,250)-(374,270); decoration items are not clickable.
    const iw2::ui::Item* yes = iw2::ui::itemAt(*m, 300, 255);
    CHECK(yes && yes->name == "invert_enabled");
    const iw2::ui::Item* no = iw2::ui::itemAt(*m, 300, 215);
    CHECK(no && no->name == "invert_disabled");
    CHECK(iw2::ui::itemAt(*m, 205, 161) == nullptr);
    CHECK(iw2::ui::itemAt(*m, 300, 300) == nullptr);
    if (!yes || !no) return 1;

    // "Yes" inverts: the bool flips, m_pitch changes sign, the script gets "popup_yes".
    log.clear();
    iw2::ui::runScript(yes->action, m->name, env);
    CHECK(cvars["ui_mousePitch"] == "1");
    CHECK(cvars["m_pitch"] == "-0.022");
    CHECK((log == std::vector<std::string>{"play test_click", "resp test_popup popup_yes", "close test_popup"}));

    // "No": nothing changes but the response.
    log.clear();
    iw2::ui::runScript(no->action, m->name, env);
    CHECK(cvars["m_pitch"] == "-0.022");
    CHECK((log == std::vector<std::string>{"play test_click", "resp test_popup popup_no", "close test_popup"}));

    // A second toggle restores it (the second popup's "No").
    iw2::ui::runScript(yes->action, m->name, env);
    CHECK(cvars["ui_mousePitch"] == "0");
    CHECK(cvars["m_pitch"] == "0.022");

    // ESC answers popup_no.
    log.clear();
    iw2::ui::runScript(m->onEsc, m->name, env);
    CHECK((log == std::vector<std::string>{"resp test_popup popup_no", "close test_popup"}));

    // execOnDvar*Value (0x4c8ce0..0x4c8d80): string compare is case-insensitive, float compare has a 1e-5 tolerance.
    {
        std::vector<std::string> ran;
        iw2::ui::MenuEnv e2 = env;
        e2.exec = [&](const std::string& t) { ran.push_back(t); };
        cvars["t"] = "AbC";
        iw2::ui::runScript({{"execOnDvarStringValue", "t", "abc", "go1"}, {"execOnDvarStringValue", "t", "x", "no"},
                            {"execNowOnDvarFloatValue", "t2", "0.5", "no2"}}, "m", e2);
        cvars["t2"] = "0.500001";
        iw2::ui::runScript({{"execOnDvarFloatValue", "t2", "0.5", "go2"}, {"execOnDvarFloatValue", "t2", "0.6", "no3"}}, "m", e2);
        CHECK((ran == std::vector<std::string>{"go1", "go2"}));
        cvars["name"] = "Joe";
        iw2::ui::runScript({{"uiScript", "update", "ui_GetName"}}, "m", e2);
        CHECK(cvars["ui_Name"] == "Joe");
    }

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
