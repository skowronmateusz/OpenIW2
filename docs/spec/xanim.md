# `xanim/<name>` animations (version 14)

Source: the loader `FUN_0047a750` in `CoD2SP_s.exe` (1.3), with helpers `FUN_0047a5a0`
(full quaternion), `FUN_0047a630` (yaw quaternion) and `FUN_0047a680`
(notetracks). All 3,669 shipped animations parse and are consumed to the last byte.

## Layout

| Field | Type |
|---|---|
| version | uint16 = 14 ("out of date (version %d, expecting %d)") |
| frame count | uint16 |
| bone count | uint16 |
| flags | uint8: bit 0 looping, bit 1 delta (root motion) |
| frame rate | int16 |
| delta tracks | only if the delta flag is set: a yaw rotation track, then a translation track |
| bone bitsets | if bones > 0: two bitsets of `((bones − 1) >> 3) + 1` bytes: **flip** (negate the first quaternion) and **yaw-only** (quaternions store only z) |
| bone names | `bones × char[]` |
| per bone | a rotation track, then a translation track |
| notetracks | `uint8 count`, then `count × { char name[]; uint16 frame }`; the loader appends an `end` note at time 1.0 |

The **key count** is `frames + 1` for looping animations, otherwise `frames`.

### Tracks

Each track starts with `uint16 count`:

- `0`: no data.
- `1`: one constant key.
- otherwise: if `count < keyCount`, `count` frame numbers follow (`uint8` when `keyCount ≤ 256`, else `uint16`);
  when `count ≥ keyCount` there is one key per frame. Then `count` keys follow.

Rotation keys are `int16 x, y, z` (full) or `int16 z` (yaw-only); `w` is rebuilt as
`round(sqrt(32767² − Σ components²))` and every component is scaled by 1/32767. After loading, consecutive keys
are sign-adjusted into the same hemisphere. Translation keys are `float x, y, z`.

## Sampling (0x479980, 0x479ac0, 0x475530)

The frame for a normalized time `t` is `t × (keyCount − 1)`. Keys are located with a binary search over the frame
numbers (0x47a3e0) and interpolated linearly, quaternions component-wise without normalizing (0x474b40); the loader has already put each key in the same hemisphere as the previous one. At `t = 1` the last
key is used (0x475530). The two samplers differ only in the frame-number size (byte below 256 keys, else uint16).

## Blending (0x4771e0, 0x4782d0, 0x472af0)

Each model bone has an accumulator: quaternion sum, translation sum, weight sum. The tree is calculated
recursively from the root with weight 1:

- **Leaf** (an animation, with the weight it was given): for every bone the animation names, add the sampled
  quaternion × weight (a bone without rotation keys adds identity, i.e. `w += weight`), the sampled translation ×
  weight (nothing if it has no translation keys) and `weight` to the weight sum. The bone is marked animated.
- **Node with one active child** (weight ≠ 0): the child is calculated with the node's weight; the child's own
  weight is not used.
- **Node with several active children**: each child is calculated with its own weight into one buffer (leaves add
  their raw samples). For the first such node on the way down from the root the buffer is the output and every
  bone's sums are divided by its weight sum. Deeper nodes use a temporary buffer and add their result to the
  parent: the normalized quaternion × the node's weight and, if the translation weight is not 0, the translation ×
  (node weight / translation weight), adding the node's weight to the parent's weight sum.
- Buffers are 0x20 bytes per bone, at most 0x200 bones of temporaries ("MAX_CALC_ANIM_BUFFER exceeded").

After the calc, bones that no animation names take the model's base rotation (identity for root bones) and a zero
translation. The skeleton (0x472af0) then adds each non-root bone's **base offset** to its translation, so animation
translations are offsets from the base pose (`crouch_aim_straight` moves `j_mainroot` by −21 from its base height
37), builds rotations from the possibly unnormalized quaternion with scale `2 / |q|²` (a zero quaternion becomes
identity) and composes each bone with its parent. Two quaternion products are used for the composition (0x4724d0 and 0x472560, opposite operand order), selected by a per-bone mask built from the
model; which bones take the second order is not yet specified (OpenIW2 uses parent × local for all bones).

## Weights and time (0x478f10, 0x476960, 0x476490)

- Setting a node (goal weight, blend time, rate): blend time 0 sets the weight to the goal at once; otherwise a node
  at weight 0 starts at goal × 0.001. Time left: going to 0, the blend time (if the old goal was also 0, the smaller
  of the time left and the blend time); going down, `(weight − goal) / weight × blendTime`; going up,
  `(goal − weight) / goal × blendTime`. The root always has weight 1, blend time 0 and rate 1.
- Each frame, top-down: a node whose parent is inactive, or whose time left is ≤ dt, jumps to its goal; otherwise
  `weight += (goal − weight) / timeLeft × dt` (if the result is below 1e-6 it becomes goal × 0.001) and the time
  left shrinks by dt. A node with weight and goal 0 and no live children is released (a new set starts at time 0).
- Time: nodes with weight 0 do not advance. A leaf advances by `frameRate / (keyCount − 1) × rate × dt`, wrapping
  if it loops and otherwise stopping at 1. A blend node passes `dt × rate` to its children. Synced nodes
  (`loopsync`/`nonloopsync`, 0x475bb0, 0x476370) are not specified yet.

## Implementation

`src/assets/xanim.cpp` (`accumulateAnim`, `mergeBlend`, `finishPose`, `animatePose`, `blendPose`) and
`src/game/animtree.cpp` (`AnimState::calc`, `setWeight`, `updateWeight`, `updateTime`). The view model's tree (0x4b2c90)
has the action animation and the aim animation as two active children of the root, so both are added with weight 1
and divided by 2.
