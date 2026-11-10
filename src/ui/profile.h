// Player profile config: players/<name>/config.cfg holds the archived cvars as `seta <name> "<value>"` lines
// (exe strings `players/%s/`, `config.cfg`, `seta %s "%s"`).
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

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

// The multiplayer player name for a connection: `menuName` (the "name" dvar) when set, else the `name` dvar of the active profile's
// config (players/active.txt names the profile), else the profile's own name (openiw2 choice, TODO(spec): the original's source of
// a default name), else "player". Read from the files so a +connect start, which joins before the menus load, gets it too.
std::string profilePlayerName(const std::filesystem::path& playersDir, const std::string& menuName);

// True when the executable's folder (SDL_GetBasePath) is inside an application bundle (".app/Contents/").
bool runsInAppBundle(std::string_view basePath);

} // namespace iw2::ui
