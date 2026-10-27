// Player profile config: players/<name>/config.cfg holds the archived cvars as `seta <name> "<value>"` lines
// (exe strings `players/%s/`, `config.cfg`, `seta %s "%s"`).
#pragma once

#include <map>
#include <string>

namespace iw2::ui {

// True for the cvars the menus persist (TODO(spec): the exe archives every CVAR_ARCHIVE cvar; only the ones the
// menus and levels here use are written).
bool isArchivedCvar(const std::string& name);

// Parses `seta`/`set` lines of a config file into name -> value (names kept as written).
std::map<std::string, std::string> parseConfig(const std::string& text);
// The config text for the archived cvars of `cvars`.
std::string formatConfig(const std::map<std::string, std::string>& cvars);

} // namespace iw2::ui
