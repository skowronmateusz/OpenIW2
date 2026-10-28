#include "ui/menurun.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace iw2::ui {

namespace {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Console command text run by "execnow". "toggle <bool cvar>" is 0x433b80 -> 0x433a10: a bool flips, an int cvar
// without a range (min <= 0, max >= 1) goes 0 -> 1 and anything else -> 0. Every other text goes to env.exec (the console).
void execConsole(const std::string& text, MenuEnv& env) {
    std::istringstream in(text);
    std::string cmd, arg;
    in >> cmd >> arg;
    if (lowerStr(cmd) == "toggle" && !arg.empty()) {
        const std::string v = env.getCvar(arg);
        env.setCvar(arg, v.empty() || std::atof(v.c_str()) == 0.0 ? "1" : "0");
    } else if (env.exec) {
        env.exec(text);
    }
}

// "uiScript update <dvar>" (handler 0x4c5118): for ui_mousePitch the bool decides the sign of the cvar m_pitch:
// true -> -0.022 (0xbcb43958), false -> +0.022 (0x3cb43958).
//
// The handler (0x4c5010) compares the argument with ui_SetName, ui_GetName, ui_mousePitch and ui_savegameListAutosave in that
// order and returns silently for anything else (no error is printed). ui_GetName (0x4c5091) copies the dvar "name" into
// "ui_Name" (an empty string when "name" is missing); ui_SetName (0x4c5029) copies "ui_Name" into "name".
// TODO(spec): ui_savegameListAutosave (0x4c5164 -> 0x4c4960 with "autosave" or 0) refreshes the save list and is not ported.
void uiScriptUpdate(const std::string& name, MenuEnv& env) {
    const std::string lname = lowerStr(name);
    if (lname == "ui_setname") { env.setCvar("name", env.getCvar("ui_Name")); return; }
    if (lname == "ui_getname") { env.setCvar("ui_Name", env.getCvar("name")); return; }
    if (lname != "ui_mousepitch") return;
    const std::string v = env.getCvar("ui_mousePitch");
    const bool on = !v.empty() && std::atof(v.c_str()) != 0.0;
    env.setCvar("m_pitch", on ? "-0.022" : "0.022");
}

} // namespace

std::string registeredDefault(const std::string& n) {
    static const char* const names[] = {"mis_none", "mis_01", "mis_02", "mis_03", "mis_04", "mis_05", "mis_06", "mis_07", "mis_08", "mis_09", "mis_10"};
    for (const char* k : names)
        if (n == k) return "0";
    if (n == "mis_difficulty") return std::string(50, '0'); // string default at 0x581aa0 (registered ~0x4d05a3)
    // Dvars the exe registers at startup with a default (register calls 0x431f60 bool / 0x432020 float; arguments name, default,
    // [min, max], flags): the client block at 0x40e270..0x40e620, snd_volume at 0x4449d6, monkeytoy at 0x4037ba.
    static const std::pair<const char*, const char*> defaults[] = {
        {"cg_drawCrosshair", "1"},  // 0x40e400
        {"cg_subtitles", "1"},      // 0x40e418
        {"cg_subtitleMinTime", "3"},          // 0x4a1ea6 (float 0..FLT_MAX)
        {"cg_subtitleWidthStandard", "80"},   // 0x4a1e.. (int 20..256)
        {"cg_subtitleWidthWidescreen", "80"}, // 0x4a1e.. (int 20..256)
        {"cg_subtitlePosX", "120"}, {"cg_subtitlePosY", "400"}, {"cg_subtitleCharHeight", "12"}, // 0xf48cf0 / 0xf48cf4 / 0x1040c78
        {"cg_blood", "1"},          // 0x40e5f9
        {"cg_drawHealth", "0"},     // 0x4a125d (bool, flags 0x1080; gates the health bar ownerdraws 0x4a8800 / 0x4a8ab0)
        {"monkeytoy", "1"},         // 0x4037ba
        {"cl_freelook", "1"},       // 0x40e2e0
        {"cl_mouseAccel", "0"},     // 0x40e2c3 (float 0..100)
        {"cl_showmouserate", "0"},  // 0x40e2f9
        {"r_inGameVideo", "1"},     // 0x40e311
        {"sensitivity", "5"},       // 0x40e2a3 (float 0.01..100)
        {"m_pitch", "0.022"},       // 0x40e337 (float -1..1)
        {"snd_volume", "1"},        // 0x4449d6 (float 0..1)
        // Renderer DLL (gfx_d3d_x86_s.dll) registrations, 0x1000abb6..0x1000bafc: int/bool defaults pushed before the name; enum defaults are
        // an index into the domain table (r_textureMode: nearest, linear, bilinear, trilinear, anisotropic -> 2; r_rendererPreference:
        // none, dx9, dx7 -> 0; r_aspectRatio: auto, standard, wide 16:10, wide 16:9 -> 0).
        {"r_picmip", "0"}, {"r_picmip_bump", "0"}, {"r_picmip_spec", "0"}, {"r_picmip_manual", "0"},
        {"r_texturemode", "bilinear"}, {"r_aspectratio", "auto"}, {"r_rendererpreference", "none"},
        {"r_swapinterval", "0"}, {"r_aasamples", "1"}, {"r_dlightlimit", "4"}, {"r_gamma", "1"},
    };
    for (const auto& [k, v] : defaults)
        if (lowerStr(n) == lowerStr(k)) return v;
    return {};
}

void initUiCvars(MenuEnv& env) {
    // 0x4c30db: ui_mousePitch = m_pitch < 0 (a missing cvar counts as not inverted).
    const std::string p = env.getCvar("m_pitch");
    env.setCvar("ui_mousePitch", !p.empty() && std::atof(p.c_str()) < 0.0 ? "1" : "0");
}

// uiScript openMenuOnDvar / openMenuOnDvarNot / closeMenuOnDvar / closeMenuOnDvarNot <dvar> <value> <menu>
// (exe 0x4c52d0 parses the three arguments, 0x4c53a0 opens, 0x4c5430 closes): the dvar string is compared with the value
// (case-insensitive, 0x43f6d0); the "Not" variants act when they differ. The exe reads the dvar string with 0x4314b0, which gives
// "" for an unknown dvar (no error is printed in the open/close path 0x4c53a0 / 0x4c5430).
static bool menuOnDvar(const std::string& cmd, const std::string& dvar, const std::string& value, const std::string& menu, MenuEnv& env) {
    const bool open = cmd == "openmenuondvar" || cmd == "openmenuondvarnot";
    const bool close = cmd == "closemenuondvar" || cmd == "closemenuondvarnot";
    if (!open && !close) return false;
    const bool wantEqual = cmd == "openmenuondvar" || cmd == "closemenuondvar";
    const bool equal = lowerStr(env.getCvar ? env.getCvar(dvar) : std::string()) == lowerStr(value);
    if (equal == wantEqual) {
        if (open && env.open) env.open(menu);
        if (close && env.close) env.close(menu);
    }
    return true;
}

void runScript(const Script& script, const std::string& menuName, MenuEnv& env) {
    for (const Command& cmd : script) {
        if (cmd.empty()) continue;
        const std::string name = lowerStr(cmd[0]);
        if (name == "play" && cmd.size() > 1) { if (env.play) env.play(cmd[1]); }
        else if (name == "ingameclose") { // UI_SetActiveMenu(0) 0x4c6520: clear the catcher and the paused state, close every menu
            env.setCvar("cl_paused", "0");
            if (env.closeAll) env.closeAll();
        } else if (name == "ingameopen" && cmd.size() > 1) { // UI_SetActiveMenu(2): cl_paused 1, open the menu
            env.setCvar("cl_paused", "1");
            if (env.open) env.open(cmd[1]);
        }
        else if (name == "open" && cmd.size() > 1) { if (env.open) env.open(cmd[1]); }
        else if (name == "setfocus" && cmd.size() > 1) { if (env.setFocus) env.setFocus(cmd[1]); }
        else if (name == "setfocusbydvar" && cmd.size() > 1) { if (env.setFocusByDvar) env.setFocusByDvar(cmd[1]); }
        else if ((name == "savegameshow" || name == "savegamehide") && cmd.size() > 1) {
            // exe 0x4c8fb0/0x4c8fd0 -> 0x4c8f20: valid = the file named by dvar sv_lastSaveGame opens with header word 0x99 (0x4e06e0);
            // the item is shown iff valid == (1 for savegameshow, 0 for savegamehide), hidden otherwise.
            const bool has = env.savegameExists && env.savegameExists();
            if (env.showItem) env.showItem(cmd[1], name == "savegameshow" ? has : !has);
        } else if (name == "restarthide") {
            // The exe has no "restarthide" menu command or string (CoD2SP_s.exe): the line is not a known menu command and does nothing here.
        }
        else if ((name == "show" || name == "hide") && cmd.size() > 1) { if (env.showItem) env.showItem(cmd[1], name == "show"); }
        else if ((name == "execnow" || name == "exec") && cmd.size() > 1) execConsole(cmd[1], env);
        else if (name == "uiscript" && cmd.size() > 4 && menuOnDvar(lowerStr(cmd[1]), cmd[2], cmd[3], cmd[4], env)) {}
        else if (name == "uiscript" && cmd.size() > 2 && lowerStr(cmd[1]) == "update") uiScriptUpdate(cmd[2], env);
        else if (name == "uiscript" && cmd.size() > 1) { if (env.uiScript) env.uiScript(lowerStr(cmd[1]), Command(cmd.begin() + 2, cmd.end())); }
        else if (name == "scriptmenuresponse" && cmd.size() > 1) { if (env.scriptMenuResponse) env.scriptMenuResponse(menuName, cmd[1]); }
        else if (name == "close" && cmd.size() > 1) { if (env.close) env.close(cmd[1]); }
        else if (name == "setdvar" && cmd.size() > 2) env.setCvar(cmd[1], cmd[2]);
        // Menu command table 0x5a4de8 (name, handler pairs): fadein 0x4c8150 / fadeout 0x4c81a0, setitemcolor 0x4c7ba0,
        // execOnDvarIntValue 0x4c8d00 (0x4c8bb0 with the int compare 0x4c8c70: the dvar's int value equal to the argument queues the
        // console text), nextlevel 0x4c8e10 (jumps to 0x4d9f00, the same as the console command).
        else if ((name == "fadein" || name == "fadeout") && cmd.size() > 1) { if (env.fadeItem) env.fadeItem(cmd[1], name == "fadeout"); }
        else if (name == "setitemcolor" && cmd.size() > 6) {
            if (env.setItemColor)
                env.setItemColor(cmd[1], lowerStr(cmd[2]), Color{float(std::atof(cmd[3].c_str())), float(std::atof(cmd[4].c_str())),
                                                                 float(std::atof(cmd[5].c_str())), float(std::atof(cmd[6].c_str()))});
        }
        else if ((name == "execondvarintvalue" || name == "execondvarstringvalue" || name == "execondvarfloatvalue" ||
                  name == "execnowondvarintvalue" || name == "execnowondvarstringvalue" || name == "execnowondvarfloatvalue") &&
                 cmd.size() > 3) {
            // 0x4c8ce0..0x4c8d80 all enter 0x4c8bb0(queueMode, compare): dvar name, value and console text, then the text runs when
            // compare(dvar string, value) holds. Compares: string 0x4c8c50 (case-insensitive equal), int 0x4c8c70 (atoi equal),
            // float 0x4c8ca0 (|atof(a) - atof(b)| < 1e-5, constant 0x592124). The "now" variants only pass queue mode 0 instead of 2.
            const std::string v = env.getCvar ? env.getCvar(cmd[1]) : std::string();
            const bool isInt = name.find("int") != std::string::npos, isFloat = name.find("float") != std::string::npos;
            bool match;
            if (isInt) match = std::atoi(v.c_str()) == std::atoi(cmd[2].c_str());
            else if (isFloat) match = std::fabs(std::atof(v.c_str()) - std::atof(cmd[2].c_str())) < 1e-5;
            else match = lowerStr(v) == lowerStr(cmd[2]);
            if (match && env.exec) env.exec(cmd[3]);
        }
        else if (name == "nextlevel") { if (env.exec) env.exec("nextlevel"); }
        // transition (0x4c8560) and orbit (0x4c8790) are in the command table but no shipped .menu uses them (checked over all ui*/ menus).
    }
}

const Item* itemAt(const Menu& menu, float x, float y) {
    for (const auto& it : menu.items) {
        if (it->type != 1 || it->decoration || !it->visible) continue; // ITEM_TYPE_BUTTON = 1
        const float ix = menu.rect.x + it->rect.x, iy = menu.rect.y + it->rect.y;
        if (x >= ix && x < ix + it->rect.w && y >= iy && y < iy + it->rect.h) return it.get();
    }
    return nullptr;
}

} // namespace iw2::ui
