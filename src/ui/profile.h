// Player profile config: players/<name>/config.cfg holds the archived cvars as `seta <name> "<value>"` lines
// (exe strings `players/%s/`, `config.cfg`, `seta %s "%s"`).
#pragma once

#include <map>
#include <string>

namespace iw2::ui {

// True for the cvars the menus persist. The exe writes every cvar registered with the archive flag; the port has no cvar registry
// with flags, so the list is the archived cvars that its menus and levels use (flags cited in profile.cpp).
bool isArchivedCvar(const std::string& name);

// The dvar a menu write to `name` also sets at once, or "" when there is none. The texture filter row writes ui_r_texturemode, which
// only the Apply button copies to r_texturemode, and the shipped page hides Apply while a level runs. openiw2 applies the filter at
// the next frame without a restart, so the row takes effect directly (deliberate deviation, docs/spec/material.md).
std::string immediateMenuDvar(const std::string& name);

// Parses `seta`/`set` lines of a config file into name -> value (names kept as written).
std::map<std::string, std::string> parseConfig(const std::string& text);
// The config text for the archived cvars of `cvars`.
std::string formatConfig(const std::map<std::string, std::string>& cvars);

} // namespace iw2::ui
