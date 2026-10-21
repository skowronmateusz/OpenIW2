// Objective and level-change built-ins following CoD2SP_s.exe 1.3 (docs/spec/campaign.md).
#include "game/world.h"

#include <algorithm>
#include <cctype>

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
    vm_.registerBuiltin("objective_add", [this, index](CallContext& c) {
        if (c.args.size() < 2) throw std::runtime_error("objective_add needs at least 2 parameters");
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        o.state = stateIndex(c.arg(1));
        if (c.args.size() > 2) o.text = textOf(c.arg(2));
        if (c.args.size() > 3 && c.arg(3).isVector()) o.positions[0] = c.arg(3).asVector();
        if (c.args.size() > 4) o.icon = c.arg(4).toString();
        clientCommands_.push_back({"menu_show_notify", "2"});
        objectiveMessage(o.state, c.args.size() > 2 ? o.text : std::string("GAME_OBJECTIVESUPDATED"));
        return Value();
    });
    vm_.registerBuiltin("objective_state", [this, index](CallContext& c) {
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        o.state = stateIndex(c.arg(1));
        objectiveMessage(o.state, "GAME_OBJECTIVESUPDATED");
        return Value();
    });
    // TODO(spec): objective_string's message (FUN_004f18c0) is not decompiled; the text is stored only.
    auto setString = [this, index](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))].text = textOf(c.arg(1));
        return Value();
    };
    vm_.registerBuiltin("objective_string", setString);
    vm_.registerBuiltin("objective_string_nomessage", setString);
    vm_.registerBuiltin("objective_icon", [this, index](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))].icon = c.arg(1).toString();
        return Value();
    });
    vm_.registerBuiltin("objective_position", [this, index](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))].positions[0] = c.arg(1).asVector();
        return Value();
    });
    vm_.registerBuiltin("objective_additionalposition", [this, index](CallContext& c) {
        const int n = c.arg(1).asInt();
        if (n < 0 || n > 7) throw std::runtime_error("objective_additionalposition: position index must be 0 to 7");
        objectives_[size_t(index(c.arg(0)))].positions[size_t(n)] = c.arg(2).asVector();
        return Value();
    });
    vm_.registerBuiltin("objective_current", [this, index](CallContext& c) {
        std::vector<int> listed;
        for (const auto& a : c.args) listed.push_back(index(a));
        for (int i = 0; i < 16; ++i) {
            const bool in = std::find(listed.begin(), listed.end(), i) != listed.end();
            if (in) objectives_[size_t(i)].state = 4;
            else if (objectives_[size_t(i)].state == 4) objectives_[size_t(i)].state = 1;
        }
        objectiveMessage(4, "GAME_OBJECTIVESUPDATED");
        return Value();
    });
    vm_.registerBuiltin("objective_additionalcurrent", [this, index](CallContext& c) {
        for (const auto& a : c.args) objectives_[size_t(index(a))].state = 4;
        return Value();
    });
    vm_.registerBuiltin("objective_ring", [this, index](CallContext& c) {
        Objective& o = objectives_[size_t(index(c.arg(0)))];
        o.ring = !o.ring;
        return Value();
    });
    vm_.registerBuiltin("objective_delete", [this, index](CallContext& c) {
        objectives_[size_t(index(c.arg(0)))] = Objective{};
        return Value();
    });

    // changelevel(map [, persistent [, exitTime]]): only with a live player; exitTime in seconds ≥ 0.
    auto change = [this](CallContext& c, bool fromMissionSuccess) {
        if (!player_ || !isAlive(*player_)) return Value();
        if (fromMissionSuccess && c.args.size() > 2) throw std::runtime_error("missionsuccess: too many parameters");
        const float exitTime = !fromMissionSuccess && c.args.size() > 2 ? c.arg(2).asFloat() : 0.0f;
        if (exitTime < 0) throw std::runtime_error("exitTime cannot be negative");
        levelChange_ = {true, false, c.arg(0).toString().substr(0, 63), c.args.size() > 1 && c.arg(1).truthy(),
                        timeMs_ + int64_t(exitTime * 1000.0f)};
        return Value();
    };
    vm_.registerBuiltin("changelevel", [change](CallContext& c) { return change(c, false); });
    vm_.registerBuiltin("missionsuccess", [change](CallContext& c) { return change(c, true); });
    // TODO(spec): the original shows the failure screen and waits for the player (opendeadscreen); OpenIW2
    // restarts the level.
    vm_.registerBuiltin("missionfailed", [this](CallContext&) {
        levelChange_ = {true, true, mapName_, false, timeMs_ + 3000};
        return Value();
    });
}

} // namespace iw2::game
