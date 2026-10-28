// Grenade hint nodes (info_grenade_hint): the level's table of hint points and the pure geometry the AI throw code runs
// over it (docs/spec/grenades.md "Grenade hint nodes"). Evidence: CoD2SP_s.exe 1.3 disassembly:
// spawn 0x51e6a0, pick 0x51ece0, MagicGrenade lob fallback 0x51f050, distance comparator 0x51f280 used by 0x51f320.
#pragma once

#include <array>
#include <vector>

namespace iw2::game {

using HintVec = std::array<float, 3>;

constexpr int kMaxGrenadeHints = 512;          // 0x51e6a6: "MAX_GRENADE_HINTS (%i) exceeded"
constexpr float kGrenadeHintArcDefault = 150;  // ai_debugGrenadeHintArc (0x4d918a: default 150, range 0..180)
constexpr float kGrenadeHintJitter = 15.0f;    // 0x5a94b0: the aim lands within ±15 of the picked hint on each axis

// 0x51ece0: the hint a blocked throw from `start` at `target` aims at. Candidates are at least 10 units above the
// target, within 192 units of it (2D), and seen from the target roughly behind the hint from the thrower: the 2D unit
// directions target→hint and start→hint have a dot product ≤ cos(arc/2 + 180°). The one nearest the target (2D) wins;
// ties keep the earlier entry. False without one.
bool grenadeHintPick(const std::vector<HintVec>& hints, const HintVec& start, const HintVec& target, float arcDeg,
                     HintVec& out);

// One candidate of 0x51f050: a lob from `start` that passes over the hint and comes down at the target's horizontal
// distance along the hint's direction. Fails (false) when the hint is behind the throw (3D dot < 0), not strictly
// between in 2D (0 < L < D), when the landing point `land` = start + dir·D (z = target z) is more than 192 units from the
// target (2D), when the hint is not above the straight line, or when the launch speed² exceeds 810000. On success `vel`
// is the launch velocity and `land` the point the arc check (0x51e710) must reach.
bool grenadeHintLob(const HintVec& hint, const HintVec& start, const HintVec& target, float gravity, HintVec& vel,
                    HintVec& land);

// 0x51f320 sorts the table in place (CRT qsort, 12-byte entries, comparator 0x51f280) by squared 3D distance from
// `origin`, nearest first. The CRT qsort is not stable; equal distances keep their order here (TODO(spec): the exact
// order of ties under the MSVC qsort is not reproduced).
void grenadeHintSortByDistance(std::vector<HintVec>& hints, const HintVec& origin);

}  // namespace iw2::game
