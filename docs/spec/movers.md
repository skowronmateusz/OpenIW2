# Movers and linking

Evidence: addresses in CoD2SP_s.exe 1.3. Implemented in
`src/game/movers.cpp`.

## Movers

- Only `script_brushmodel`, `script_model` and `script_origin` entities move; others raise
  "entity %i is not a script_brushmodel, script_model, or script_origin".
- Time arguments (0x533510): total > 0 ("total time must be positive"), accel and decel ≥ 0 (default 0),
  accel + decel ≤ total. Durations are `int(seconds × 1000)`.
- Each entity has a position and an angle trajectory `{type, start, duration, base, delta}`; evaluation (0x4fae80),
  t = now − start (clamped to the duration for types 3, 7, 8), ts = t / 1000:
  0/1/6 stationary; 2 and 3 `base + delta·ts`; 4 `base + delta·sin(t/duration·2π)`; 5 `base + delta·ts`,
  z −= 400·ts²; 7 `base + dir(delta)·(|delta|/durS)·ts²/2`; 8 `base + delta·ts − dir(delta)·(|delta|/durS)·ts²/2`.
- Setup (0x532ad0) to target T over total, accel a, decel d: with a = d = 0 a type 3 move for the total time with
  delta = (T − start)/total. Otherwise lin = total − a − d, speed = 2|T − start| / (2·total − a − d),
  velocity = dir·speed; a > 0: type 7 for a; a = 0: type 3 for lin (type 8 for d when lin = 0). A running move
  starts from its current evaluated position.
- Phase step (0x5327d0) when a phase ends: 7 with lin > 0 → 3 from the end of the acceleration; 3/7 with d > 0 →
  8 towards T at `speed`; otherwise the entity rests at T and the move is done.
- When done (0x532960): position → notify `movedone`; angles are normalised to [−180, 180] → notify `rotatedone`.
- `movex/y/z` and `rotatepitch/yaw/roll` add the argument to one axis. `rotateto` adds the shortest angle
  difference per axis (0x423f50: d = a − b, then −360 while > 180 and +360 while < −180).
- `movegravity(velocity, time)`: type 5 from the current origin.

## Not specified yet (TODO in code)

`rotatevelocity` accel/decel phases (0x532e20), `vibrate` (0x533b90), the per-frame mover runner and linked
entity update order, the body of `linkto` (0x4ed770: tag offsets), `istouching` shape tests (0x4f9f50,
0x447830), and collision of moving brush models (submodel traces).
OpenIW2's linkto keeps the child at the parent (or tag) plus the given offsets, rotated by the parent's yaw
(inferred), and istouching compares bounding boxes (inferred).
