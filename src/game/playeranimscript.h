// The multiplayer player animation script (mp/playeranim.script) and the move type of a client
// (docs/spec/mp-players.md). Implemented from the spec.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace iw2::game {

enum class PaBody { Legs, Torso, Both };

struct PaPart {
    PaBody body = PaBody::Both;
    std::string anim; // lower case
};

// One command line: its body parts and modifiers (-1 when absent).
struct PaCommand {
    std::vector<PaPart> parts;
    int durationMs = -1;
    int blendMs = -1;
    bool turretAnim = false;
    std::string sound;
};

struct PaCondition {
    std::string type;                // lower case
    std::vector<std::string> values; // lower case; empty for a flag
};

struct PaItem {
    std::vector<PaCondition> conds; // empty: default
    std::vector<PaCommand> commands;
    int line = 0;
};

// The client's values for the conditions.
struct PaContext {
    std::string playerAnimType = "none";
    std::string weaponClass;
    std::string moveType = "idle";
    std::string weaponPosition = "hip"; // hip / ads
    std::string strafing = "not";       // not / left / right
    bool mounted = false;
    bool firing = false;
};

class PlayerAnimScript {
public:
    // Parses the script text; false with a message (and line) on a syntax error.
    bool parse(std::string_view text, std::string* error = nullptr);

    // The first item of the COMBAT state's block for `moveType` that holds, or nullptr.
    const PaItem* animation(const std::string& moveType, const PaContext& ctx) const;
    // The first item of the event's block that holds, or nullptr.
    const PaItem* event(const std::string& name, const PaContext& ctx) const;

    bool holds(const PaItem& item, const PaContext& ctx) const;
    size_t animationCount() const { return anims_.size(); }
    size_t eventCount() const { return events_.size(); }

private:
    bool valueMatches(const std::string& type, const std::string& value, const std::string& current) const;
    // condition type -> defined name -> values
    std::map<std::string, std::map<std::string, std::vector<std::string>>> defines_;
    std::map<std::string, std::vector<PaItem>> anims_;  // COMBAT state, by move type
    std::map<std::string, std::vector<PaItem>> events_; // by event name
};

// The movement a move type is chosen from (docs/spec/mp-players.md, "Move type from the movement").
struct PaMovement {
    int viewHeightTarget = 60; // 11 prone, 40 crouched
    float xySpeed = 0;
    float forwardMove = 0; // the command's forward / right moves (sign only)
    float rightMove = 0;
    bool slow = false;     // aiming slowdown or leaning: walk instead of run
    // The damage timer (ps+0x90, its start ps+0x94 and the hit's yaw ps+0x98; docs/spec/mp-players.md): the first
    // player_dmgtimer_stumbleTime ms of a hit replace the move type.
    int damageTimerMs = 0, damageDurationMs = 0, stumbleTimeMs = 500;
    float flinchYaw = 0;
};

std::string paMoveType(const PaMovement& m, float moveThreshold = 10.0f);
// TODO(spec): the strafing value (docs/spec/mp-players.md).
std::string paStrafing(const PaMovement& m);

} // namespace iw2::game
