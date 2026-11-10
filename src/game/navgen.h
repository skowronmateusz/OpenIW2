// Navigation graph generated from the collision of a map (docs/spec/mp-bots.md, "Navigation graph"). An OpenIW2
// extension: multiplayer maps carry no path nodes, so the bots' graph is sampled from the world brushes and cached
// per map in a format of our own.
#pragma once

#include "game/collision.h"
#include "game/pathnodes.h"
#include "game/pmove.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iw2::game {

namespace NavLinkFlag {
constexpr uint8_t Jump = 1; // a step up higher than jump_stepSize: the bot jumps
constexpr uint8_t Drop = 2; // a fall of more than jump_stepSize on the way (one-way)
constexpr uint8_t Ladder = 4; // climbed on a ladder (up: forward and looking up; down: backward off the top)
constexpr uint8_t Mantle = 8; // a mantle over a ledge (forward with the jump button held)
constexpr uint8_t Leap = 16;  // a standing jump forward with the button held (over a gap, or a jump into a mantle)
constexpr uint8_t Crouch = 32; // only the crouched box (top 50) fits on the way: the bot crouches on the link
constexpr uint8_t Special = Ladder | Mantle | Leap;
} // namespace NavLinkFlag

struct NavLink {
    uint32_t target = 0;
    float distance = 0;
    uint8_t flags = 0;
    float yaw = 0; // Ladder / Mantle: the view yaw (degrees) the traversal is made with
};

struct NavParams {
    float cell = 16.0f;        // grid spacing in x and y
    float stepHeight = 18.0f;  // jump_stepSize (pmove.md)
    float jumpHeight = 36.0f;  // highest ledge a jump reaches with the box clear (jump_height 39 minus a margin)
    float maxDrop = 190.0f;    // below bg_fallDamageMinHeight (200): no fall damage
    float minNormalZ = 0.7f;   // walkable slope (pmove's MIN_WALK_NORMAL)
    float walkStep = 12.0f;    // sub-step of the walk test
    int maxFloorsPerColumn = 8;
    // Ladder and mantle links are found by running pmove with the controller's input (needs the mantle animations for
    // mantles; without them only ladders are linked). Off: walk, jump and drop links only.
    bool special = true;
    bool leaps = true; // standing jumps towards neighbours without a plain link
    const MantleAnims* mantle = nullptr;
};

// The pmove input of a Ladder / Mantle / Leap traversal along `link` (from a node at height `fromZ` to one at `toZ`) for
// the player state `ps`; `first` on its first frame (docs/spec/mp-bots.md "Ladder, mantle and leap links").
PlayerInput navTraverseInput(const NavLink& link, float fromZ, float toZ, const PlayerState& ps, bool first);
// Runs pmove from `start` with the traversal input of a link of kind `flags` / `yaw` for at most `maxSecs`; returns where
// the player stands after the ladder or mantle when one was used, nothing otherwise (exposed for tests).
std::optional<std::array<float, 3>> navSimulateTraverse(const CollisionWorld& world, const std::array<float, 3>& start,
                                                        uint8_t flags, float yaw, bool down, const MantleAnims* mantle,
                                                        float maxSecs = 6.0f, bool* mantled = nullptr);

struct NavGraph {
    std::vector<std::array<float, 3>> nodes;  // where the standing player box rests
    std::vector<std::vector<NavLink>> links;
    float cell = 16.0f;
    std::array<float, 2> gridOrigin{};        // x, y of column (0, 0)
    // Nodes [0, mainCount) reach each other; the rest are exit-only (they lead into the main part, nothing leads back).
    size_t mainCount = 0;

    size_t size() const { return nodes.size(); }
    PathGraph toPathGraph() const;
    // Nearest node within `maxDist` (3D, z weighted double) whose column is near `pos`; -1 when none. `mainOnly`
    // skips the exit-only nodes (for goals).
    int nearest(const std::array<float, 3>& pos, float maxDist = 128.0f, bool mainOnly = false) const;
    // Nodes within `maxDist` (the same measure), nearest first.
    std::vector<int> within(const std::array<float, 3>& pos, float maxDist) const;
    void buildIndex();

private:
    std::unordered_map<int64_t, std::vector<int>> columns_;
    int64_t key(int cx, int cy) const { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cy)); }
};

// Samples the floors of the box `mins`..`maxs` and links neighbours that the player box can walk, step, jump up or
// drop between. When `seeds` is not empty, only the nodes that are reachable from the seeds' nearest nodes and that
// lead back to them are kept (islands and one-way dead ends are removed).
NavGraph generateNavGraph(const CollisionWorld& world, const std::array<float, 3>& mins, const std::array<float, 3>& maxs,
                          const std::vector<std::array<float, 3>>& seeds, const NavParams& params = {});

// Where the standing player box lands when dropped from `pos` (at most 256 units down); `pos` when it does not.
std::array<float, 3> navDropToFloor(const CollisionWorld& world, const std::array<float, 3>& pos);

// The walk test between two floor points (exposed for tests): returns false when the box cannot get from `a` to `b`.
bool navWalkTest(const CollisionWorld& world, const std::array<float, 3>& a, const std::array<float, 3>& b, const NavParams& p,
                 uint8_t& flags);

// Cache file ("IW2NAV", version, map key, nodes, links). `key` identifies the map data the graph was made from.
bool saveNavGraph(const std::string& path, const NavGraph& g, uint64_t key);
std::optional<NavGraph> loadNavGraph(const std::string& path, uint64_t key);
// The cache directory: $IW2_NAV_CACHE, else the user cache directory (~/Library/Caches/openiw2/nav on macOS,
// $XDG_CACHE_HOME/openiw2/nav or ~/.cache/openiw2/nav elsewhere). Empty when none can be determined.
std::string navCacheDir();

} // namespace iw2::game
