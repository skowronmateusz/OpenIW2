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
  left shrinks by dt. A node is *active* when its weight is nonzero under an active parent, *alive* when active, with
  a nonzero goal or with a live child, *playing* when active with a playing leaf below (a leaf plays when its
  frequency is nonzero). An alive node that is not playing takes the time handed down (inside a synced subtree the
  synced top's time, elsewhere 0); a synced top that is not playing restarts its subtree (0x4768a0, 0x4768e0). Then
  the old time and cycle are set to the time and cycle (they are the base of the time step and of the anim delta).
  A node that is not alive is released (a new set starts from scratch).
- Time (0x476490): nodes with weight 0 do not advance; below a node with goal weight 0 notetracks are not reported.
  A leaf moves from its old time by `frameRate / (keyCount − 1) × rate × dt`, wrapping (and counting a cycle) if
  it loops and otherwise stopping at 1. A plain blend node passes `dt × rate` to its children. A synced blend node
  advances by its frequency (0x475bb0: the average of the weighted children's frequency × rate, weighted by the
  child weights; children with frequency 0 are skipped) × rate × dt, wrapping unless `nonloopsync`, and every
  weighted descendant is set to its time and cycle (0x476370: a descendant out of step first restarts from the
  top's old time). A new time is only taken when it does not go back.
- Sync flags (0x479260 / 0x479120): a blend node with `loopsync` gets flags 1|4, with `nonloopsync` 2|4; every blend
  node below it gets the same 1 or 2 without 4. A synced subtree may hold only looping (loopsync) or non-looping
  (nonloopsync) leaves (TODO(spec): the `void_loop` / `void` replacement of mismatched leaves, 0x479154).
- Restart (0x4793c0, the `…restart` calls): the node or its nearest ancestor with flag 4 restarts that subtree (time
  and cycle 0) unless it already stands at 0; outside a synced subtree only a leaf restarts. `setanimknoball…restart`
  (0x478c40) restarts each ancestor it sets as well.
- The .atr tokenizer (0x43e4f0) skips `//` and `/* … */` comments; the modifiers parsed by 0x452420 are `loopsync`
  (bit 1), `nonloopsync` (bit 2) and `complete` (bit 8).

## Anim delta (0x478550 → 0x477b80, 0x475960)

The root motion of the last time step: each weighted leaf with a delta track contributes the translation from its
old time to its time (if it loops and the time went back, plus one whole cycle, last key − first key), times its
weight, and adds its weight. A node with one weighted child hands it its own weight; several weighted children are
summed with their own weights, then normalized and added with the node's weight (nested), or at the top divided by
the weight sum. The weights are the current ones, or the goal weights when the caller asks (actor byte +0x220). The
xy translation difference is turned by minus the yaw of the new-time delta rotation quaternion (z, w) (0x475960 passes flag 1 to pick the second sample, the one taken at [node+0], the time that the translation difference ends on; 0x4758f0: x' = (1-2z^2/s)x + (2zw/s)y, y' = (1-2z^2/s)y - (2zw/s)x with s = z^2+w^2; length kept). The relative quaternion conj(old)*new (z = w0 z1 - z0 w1, w = w0 w1 + z0 z1) is summed with the weights like the translation (0x475a0b..0x475a35); the yaw of the result (0x423900, atan2(2zw/s, 1-2z^2/s) in degrees; (0,0) gives the identity) is what the actor turn 0x504aa0 adds in anim modes 4-7.

The actor path move (0x504160 anim mode 1, movemode ≠ stop, with a path and no followed actor) takes the horizontal
length of this delta as the frame distance (0x5041ad → 0x505230).

## Implementation

`src/assets/xanim.cpp` (`accumulateAnim`, `mergeBlend`, `finishPose`, `animatePose`, `blendPose`) and
`src/game/animtree.cpp` (`AnimState::calc`, `setWeight`, `updateWeight`, `updateTime`, `syncTo`, `restart`, `delta`). The view model's tree (0x4b2c90)
has the action animation and the aim animation as two active children of the root, so both are added with weight 1
and divided by 2.
