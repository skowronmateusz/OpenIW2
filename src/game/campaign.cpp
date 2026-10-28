// Objective and level-change built-ins following CoD2SP_s.exe 1.3 (docs/spec/campaign.md).
#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace iw2::game {

using namespace script;

namespace {

const char* const kStates[6] = {"empty", "active", "invisible", "done", "current", "failed"};

int stateIndex(const Value& v) {
    std::string s = v.toString();
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < 6; ++i)
        if (s == kStates[i]) return i;
    throw std::runtime_error("Illegal objective state \"" + v.toString() +
                             "\". Valid states are \"empty\", \"active\", \"invisible\", \"done\", \"current\", \"failed\"");
}

std::string textOf(const Value& v) {
    if (std::holds_alternative<LocalizedString>(v.v)) return std::get<LocalizedString>(v.v).key;
    return v.toString();
}

} // namespace

// State message (FUN_004f10b0): active/current → obj_update "<text>"; done → obj_complete
// "GAME_OBJECTIVECOMPLETED"; failed → obj_update "GAME_OBJECTIVEFAILED". A current message within 100 ms of the
// previous one is dropped.
void World::objectiveMessage(int state, const std::string& text) {
    if (state == 4 && timeMs_ - lastObjectiveMessageMs_ < 100) return;
    lastObjectiveMessageMs_ = timeMs_;
    if (state == 1 || state == 4) clientCommands_.push_back({"obj_update", text});
    else if (state == 3) clientCommands_.push_back({"obj_complete", "GAME_OBJECTIVECOMPLETED"});
    else if (state == 5) clientCommands_.push_back({"obj_update", "GAME_OBJECTIVEFAILED"});
}

void World::registerCampaignBuiltins() {
    auto index = [](const Value& v) {
        const int i = v.asInt();
        if (i < 0 || i >= 16)
            throw std::runtime_error("index " + std::to_string(i) + " is an illegal objective index. Valid indexes are 0 to 15");
        return i;
    };
    // Ring pulse start (parse 0x4c0a42 / 0x4c0b8c): cg time when the state becomes 4 (current) or the "ring" value changes.
    auto setState = [this](Objective& o, int st) {
        if (st == 4 && o.state != 4) o.ringStartMs = timeMs_;
        o.state = st;
    };
    vm_.registerBuiltin("objective_add", [this, index, setState](CallContext& c) {
        if (c.args.size() < 2) throw std::runtime_error("objective_add needs at least 2 parameters");
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        setState(o, stateIndex(c.arg(1)));
        if (c.args.size() > 2) o.text = textOf(c.arg(2));
        if (c.args.size() > 3 && c.arg(3).isVector()) o.positions[0] = c.arg(3).asVector();
        if (c.args.size() > 4) o.icon = c.arg(4).toString();
        clientCommands_.push_back({"menu_show_notify", "2"});
        objectiveMessage(o.state, c.args.size() > 2 ? o.text : std::string("GAME_OBJECTIVESUPDATED"));
        return Value();
    });
    vm_.registerBuiltin("objective_state", [this, index, setState](CallContext& c) {
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        setState(o, stateIndex(c.arg(1)));
        objectiveMessage(o.state, "GAME_OBJECTIVESUPDATED");
        return Value();
    });
    // TODO(spec): objective_string's message (FUN_004f18c0) is not decompiled; the text is stored only.
    auto setString = [this, index, setState](CallContext& c) {
        // The further arguments (the "&&N" values, formatted by 0x4eb9b0 like iprintln's) follow after \x1f.
        std::string text = textOf(c.arg(1));
        for (size_t i = 2; i < c.args.size(); ++i) text += "\x1f" + c.arg(i).toString();
        objectives_[size_t(index(c.arg(0)))].text = text;
        return Value();
    };
    vm_.registerBuiltin("objective_string", setString);
    vm_.registerBuiltin("objective_string_nomessage", setString);
    vm_.registerBuiltin("objective_icon", [this, index, setState](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))].icon = c.arg(1).toString();
        return Value();
    });
    vm_.registerBuiltin("objective_position", [this, index, setState](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))].positions[0] = c.arg(1).asVector();
        return Value();
    });
    vm_.registerBuiltin("objective_additionalposition", [this, index, setState](CallContext& c) {
        const int n = c.arg(1).asInt();
        if (n < 0 || n > 7) throw std::runtime_error("objective_additionalposition: position index must be 0 to 7");
        objectives_[size_t(index(c.arg(0)))].positions[size_t(n)] = c.arg(2).asVector();
        return Value();
    });
    vm_.registerBuiltin("objective_current", [this, index, setState](CallContext& c) {
        std::vector<int> listed;
        for (const auto& a : c.args) listed.push_back(index(a));
        for (int i = 0; i < 16; ++i) {
            const bool in = std::find(listed.begin(), listed.end(), i) != listed.end();
            if (in) setState(objectives_[size_t(i)], 4);
            else if (objectives_[size_t(i)].state == 4) setState(objectives_[size_t(i)], 1);
        }
        objectiveMessage(4, "GAME_OBJECTIVESUPDATED");
        return Value();
    });
    vm_.registerBuiltin("objective_additionalcurrent", [this, index, setState](CallContext& c) {
        for (const auto& a : c.args) setState(objectives_[size_t(index(a))], 4);
        return Value();
    });
    vm_.registerBuiltin("objective_ring", [this, index, setState](CallContext& c) {
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        o.ring = !o.ring;
        o.ringStartMs = timeMs_; // the "ring" configstring value changed (0x4c0b8c)
        return Value();
    });
    vm_.registerBuiltin("objective_delete", [this, index, setState](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))] = Objective{};
        return Value();
    });

    // getkeybinding(command) (0x4ecd60, 0x4cdb30): array with "count" and "key1"/"key2" key names; an unknown or
    // unbound command gives count 0 and key1 "KEY_UNBOUND".
    vm_.registerBuiltin("getkeybinding", [this](CallContext& c) {
        auto arr = std::make_shared<Array>();
        const auto it = keyBindings.find(c.arg(0).toString());
        const std::vector<std::string> keys = it == keyBindings.end() ? std::vector<std::string>{} : it->second;
        arr->set(ArrayKey{true, 0, "key1"}, Value(keys.empty() ? std::string("KEY_UNBOUND") : keys[0]));
        arr->set(ArrayKey{true, 0, "key2"}, Value(keys.size() > 1 ? keys[1] : std::string()));
        arr->set(ArrayKey{true, 0, "count"}, Value(int32_t(std::min<size_t>(keys.size(), 2))));
        return Value(arr);
    });
    // iprintln / iprintlnbold (0x4ebc50 / 0x4ebc60 → 0x4ebc00): the arguments are formatted (0x4eb9b0) and sent to the
    // client as the command `gm "<text>"` / `gmb "<text>"` (format 0x587bd8 = `%s "%s"`); the client shows them in the game
    // message window (type 1) or the bold message window (type 2) (0x4a6bff / 0x4a6c3f). The text here is the first
    // argument (a localized reference stays a key), the further arguments follow after \x1f for the "&&N" substitution.
    // TODO(spec): the exact formatting of 0x4eb9b0 (number and localized-string arguments).
    auto printMessage = [this](const char* command) {
        return [this, command](CallContext& c) {
            std::string text = c.args.empty() ? std::string() : textOf(c.arg(0));
            for (size_t i = 1; i < c.args.size(); ++i) text += "\x1f" + c.arg(i).toString();
            clientCommands_.push_back({command, text});
            if (std::string(command) == "gm") c.vm.print(c.args.empty() ? std::string() : c.arg(0).toString());
            return Value();
        };
    };
    vm_.registerBuiltin("iprintln", printMessage("gm"));
    vm_.registerBuiltin("iprintlnbold", printMessage("gmb"));
    // The level change / cinematic request (0x4f3f00, 0x4f4220 -> 0x4d9da0): the exit time (seconds, >= 0, 0x1c60fec) sets how long the
    // screen takes to fade out. The client is sent "scr_fade 1 <now+250> <exit+750>" and "snd_fade 0 <exit+1000>" and the change falls due
    // exit + 1000 ms after the request (0x1c60f54); only with a live player (0x13c(entity) > 0).
    auto scheduleChange = [this]() {
        if (!levelChange_.pending) return;
        levelChange_.atMs = timeMs_ + exitMs_ + 1000;
        clientCommands_.push_back({"scr_fade", "1 " + std::to_string(timeMs_ + 250) + " " + std::to_string(exitMs_ + 750)});
        clientCommands_.push_back({"snd_fade", "0 " + std::to_string(exitMs_ + 1000)});
    };
    // changelevel(map [, persistent [, exitTime]]): only with a live player; exitTime in seconds >= 0.
    auto change = [this, scheduleChange](CallContext& c, bool fromMissionSuccess) {
        if (!player_ || !isAlive(*player_)) return Value();
        if (fromMissionSuccess && c.args.size() > 2) throw std::runtime_error("missionsuccess: too many parameters");
        if (!fromMissionSuccess && c.args.size() > 2) {
            const float exitTime = c.arg(2).asFloat();
            if (exitTime < 0) throw std::runtime_error("exitTime cannot be negative");
            exitMs_ = int(exitTime * 1000.0f + 0.5f);
        }
        levelChange_ = {true, false, c.arg(0).toString().substr(0, 63), c.args.size() > 1 && c.arg(1).truthy(), 0};
        scheduleChange();
        return Value();
    };
    // cinematic(name [, exit time seconds]) (0x4f4220): stores the name (63 characters) and the exit time (the screen fade before the
    // movie, see above; a negative value is a script error, 0x586138). The movie plays when the change falls due, before the next level
    // loads. TODO(spec): the dvar at 0x1c148c4 that disables the request when non-zero.
    vm_.registerBuiltin("cinematic", [this, scheduleChange](CallContext& c) {
        if (c.args.size() > 1) {
            const float exitTime = c.arg(1).asFloat();
            if (exitTime < 0) throw std::runtime_error("cinematic: negative timeout");
            exitMs_ = int(exitTime * 1000.0f + 0.5f);
        }
        cinematic_ = c.arg(0).toString().substr(0, 63);
        scheduleChange();
        return Value();
    });
    vm_.registerBuiltin("changelevel", [change](CallContext& c) { return change(c, false); });
    vm_.registerBuiltin("missionsuccess", [change](CallContext& c) { return change(c, true); });
    // missionfailed() (0x4f4080): when a player entity is in use (0x4d7850 finds the classname 0x1c7f0ac at +0x118)
    // the death screen opens (0x4e8e10) and 0x1c60fe8 is set, which turns a pending changelevel into
    // loadgame_continue (0x4d9f90).
    vm_.registerBuiltin("missionfailed", [this](CallContext& c) {
        print("missionfailed at " + std::to_string(timeMs_) + " ms from" + vm_.backtrace(c.thread));
        if (player_ && !player_->obj->removed) {
            openDeathScreen();
            if (levelChange_.pending && !levelChange_.failed) levelChange_ = {true, true, mapName_, false, levelChange_.atMs, true};
        }
        return Value();
    });
}

// 0x4e8e10: once per level (g_reloading set to 1), schedules loadgame_continue g_deathDelay ms (default 3000, real
// time) from now and sends the client "snd_fade 0 <delay>", "clear_blur", "scr_blur <delay> 32 1 2",
// "time_slow <delay> 1 0.1" and, when the dvar ui_deadquote exists, "opendeadscreen <ui_deadquote>".
void World::openDeathScreen() {
    if (reloading_) return;
    reloading_ = true;
    int delay = 3000;
    if (auto it = cvars.find("g_deathDelay"); it != cvars.end()) delay = std::max(0, std::atoi(it->second.c_str()));
    loadGameAtRealMs_ = realNowMs() + delay;
    const std::string d = std::to_string(delay);
    clientCommands_.push_back({"snd_fade", "0 " + d});
    clientCommands_.push_back({"clear_blur", ""});
    clientCommands_.push_back({"scr_blur", d + " 32.000000 1 2"});
    clientCommands_.push_back({"time_slow", d + " 1.000000 0.100000"});
    if (auto it = cvars.find("ui_deadquote"); it != cvars.end()) clientCommands_.push_back({"opendeadscreen", it->second});
    print("death screen at " + std::to_string(timeMs_) + " ms, loadgame_continue in " + d + " ms");
}

// Per frame: the client think opens the death screen when the player is dead and the level time is past the
// respawn time (0x4e7eb7: pm_type >= 4 and level time > client +0x866c); the server frame runs loadgame_continue
// when its real-time deadline passes (0x4d9f90).
// loadgame_continue loads sv_lastSaveGame (0x447190): the front end does that for a change with loadContinue set and restarts
// the level when there is no such save.
void World::runDeathScreen() {
    if (player_ && playerRespawnMs_ >= 0 && !isAlive(*player_) && timeMs_ > playerRespawnMs_) openDeathScreen();
    if (loadGameAtRealMs_ >= 0 && realNowMs() >= loadGameAtRealMs_) {
        loadGameAtRealMs_ = -1;
        levelChange_ = {true, true, mapName_, false, timeMs_, true};
    }
}

} // namespace iw2::game
