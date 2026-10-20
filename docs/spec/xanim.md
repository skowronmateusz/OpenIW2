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

## Posing (implemented in `src/assets/xanim.cpp`; partly inferred)

For a bone named in the animation, the track gives the bone's local rotation and, if present, its local
translation relative to the parent. Other bones keep the model's base local transform. Keys are interpolated
linearly; rotations use normalized lerp. Posing `character_german_afrca_body` with `crouch2crouchrun_forward`
gives a plausible crouched running pose.
