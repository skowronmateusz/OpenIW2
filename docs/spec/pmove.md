# Player movement (pmove)

Source: CoD2SP_s.exe 1.3. Decompiled call tree from `Pmove` 0x50cc10. Dvar defaults were read from their registration calls (the
pushes of `name, default, min, max, flags`), and the dvar pointers were mapped to their names.

## Settings

| dvar | default | range | global |
|---|---|---|---|
| g_speed (int) | 190 | | 0x1c14898 |
| g_gravity | 800 | 1.. | |
| friction | 5.5 | 0–100 | 0x1c7efe8 |
| stopspeed | 100 | 0–1000 | 0x1c7efac |
| jump_height | 39 | 0–128 | 0x1cbac4c |
| jump_stepSize | 18 | 0–64 | |
| jump_slowdownEnable | false | | 0x1cbac44 |
| jump_spreadAdd | 64 | 0–512 | 0x1cbac40 |
| player_backSpeedScale | 0.7 | 0–20 | 0x1c7eff8 |
| player_strafeSpeedScale | 0.8 | 0–20 | 0x1c7f02c |
| bg_bobMax | 8 | 0–36 | |
| player_view_pitch_up / _down | 85 | 0–90 | |
| bg_prone_yawcap | 85 | 0–360 | |

Player box at spawn (0x4e9100, constants at 0x5a5bdc): (−15, −15, 0)–(15, 15, 70); view height 60 (0x4e91ae).

## Frame

`Pmove` (0x50cc10) splits the command time into steps of at most 66 ms; `PmoveSingle`
(0x50c4a0) clamps a step to 1–200 ms.

## Stances (0x509d10, 0x509840)

- View heights: stand 60, crouch 40, prone 11; box tops: stand 70 (player maxs), crouch 50, prone 30.
- A change of target view height starts a transition. Duration (0x5096a0): to prone 400 ms, to crouch 200 ms from
  standing and 400 ms from prone, to stand 200 ms. The view height follows keyframes {percent, height}
  (0x5096d0 over tables 0x5a9088, 0x5a8fa8, 0x5a9130, 0x5a9018):
  - to prone: 0→40, 11→38, 22→33, 34→25, 45→16, 50→15, 55→16, 70→18, 90→17, 100→11
  - to crouch from stand: 0→60, 1→59.5, 4→58.5, 30→56, 80→44, 90→41.5, 95→40.5, 100→40
  - to crouch from prone: 0→11, 5→10, 30→21, 50→25, 67→31, 83→34, 100→40
  - to stand: 0→40, 5→40.5, 10→41.5, 20→44, 70→56, 96→58.5, 99→59.5, 100→60
- Standing up traces the taller box first. TODO(spec): the prone surface check (0x4fc000), the
  crouch/prone button logic (cmd bits 0x100/0x200). Mantling: see "Mantle" below.

## Movement

- Friction (0x507c80): speed ignores z when walking; below 1 the velocity is zeroed; on the ground
  control = max(speed, stopspeed), drop = control · friction · frametime (×0.3 with ps flag 0x200; landing
  slowdown factor with flag 0x80000), velocity scaled by max(speed − drop, 0)/speed.
- Command scale (0x508140 walking, 0x5080a0 in the air): g_speed ·
  max(|forward| (×player_backSpeedScale when negative, walking only), |right| · player_strafeSpeedScale) /
  (sqrt(forward² + right²) · 127); ×0.4 with ps flag 0x100 or a nonzero ps+0x4c; ×3 / ×6 for pm_type 2 / 3 (ClientThink sets pm_type: noclip flag 2, ufo flag 3, else health > 0: linked 1 / 0, health <= 0: 4 + linked; `pmClientType`);
  walking: ×0.15 prone, ×0.65 crouched, blended during crouch/prone transitions (0x509760), × the weapon's
  `moveSpeedScale` (weapon def +0x264) when > 0; ×0.4 with command flag 0x800.
- Accelerate (0x507eb0): addspeed = wishspeed − v·wishdir; if > 0, accelspeed =
  min(addspeed, max(wishspeed, stopspeed) · frametime · accel), limited by `inertiaMax` (0x507e50).
- Walk move (0x5086f0): accel 9 standing, 12 crouched, 19 prone, 1 on slick surfaces; ×0.25 with
  ps flag 0x200. Air move (0x5085b0): accel 1.
- Jump (0x5416b0, 0x5414d0): needs the jump button newly pressed, at least 500 ms
  since the previous jump, a standing view height; vertical speed = sqrt(2 · jump_height · gravity); sets the
  landing-slowdown flag (applied by 0x541300 only when `jump_slowdownEnable`).

Implemented in `src/game/pmove.cpp`. Not implemented yet (TODO there): inertiaMax, landing damage/slowdown
(0x508fe0), movement-direction yaw (0x508390). Ladders: see the Ladder section below.

## Ground, slide and step

PmoveSingle for a normal player: ground trace, stance, pm_time countdown (at 0 it clears pm_flags 0x80000, 0x400,
0x200), walk move when walking else air move, ground trace again.

- Player trace (0x507b00): G_Trace (0x4db7f0 → 0x44bf00) with pm->tracemask, the player's clip mask 0x281c011, and
  the player as pass entity; if it starts solid in contents 0x200c000 (bodies) those bits leave the mask for the rest
  of the move and the trace is repeated.
- Ground trace (0x509480): from origin + 0.25 to origin − 0.25 z (DAT_00591fe0). All solid →
  stuck-in-solid escape (0x5092f0): position tests at origin + the 26 offsets of the table at
  0x56f920 ((0,0,1), (−1,0,1), (0,−1,1), (1,0,1), (0,1,1), the 4 sides at 0, (0,0,−1), the 4 sides
  at −1, then the 12 diagonals); the first free one becomes the origin, dropped by a 1-unit down trace that becomes
  the ground trace; none free → no ground, jump flag and ps+0x74 cleared, done. Start solid → retried from
  origin z − 0.001 to the first end; still start solid → no ground. Fraction 1 → no ground. Velocity z > 0 and
  velocity·normal > 10 → no ground (a jump). Normal z < 0.7 → ground plane but not walking, jump flag cleared. Else
  walking; the origin is never snapped to the trace end.
- Walk move (0x5086f0): jump check (a jump goes to the air move), friction, forward/right flattened, clipped against
  the ground normal and normalised; acceleration 9/12/19, 1 on slick ground (surface flag 2, which also applies
  gravity); then the velocity is clipped against the ground normal and keeps its speed when the clip did not turn it
  around; step slide move without gravity when there is horizontal velocity.
- Air move (0x5085b0): friction, horizontal wish direction, acceleration 1, clipped against a steep ground plane,
  step slide move with gravity.
- Clip (0x507c20): v − n·(d − |d|·0.001), d = v·n.
- Slide move (0x54eac0): with gravity the end vertical speed is v − g·t, the move uses their average
  and the velocity is clipped against the ground plane. Planes: the ground normal, the normalised velocity, then each
  hit (at most 8; more stops the player). Up to 4 traces of the remaining time; all solid → vertical speed 0, done.
  A hit on a plane with normal·plane > 0.999 adds the normal to the clipped velocity. Otherwise the planes are
  sorted by velocity·normal (0x54ea10, insertion sort, ascending); when the smallest is below 0.1 the velocity (and
  the end velocity) are clipped against that plane, then against each further plane they still face (< 0.1); when
  that turns them into the first plane they follow the crease (cross product), and a third plane still faced stops
  the player. With gravity the end velocity becomes the velocity; with pm_time the start velocity is kept.
- Step slide move (0x54f2e0): step 18, 10 prone. After the slide, without ground only a jump may step
  (0x541270: jump_stepSize, limited to the jump's apex ps+0x74 + jump_height). After a hit the slide is repeated from
  up to step + 1 units higher (a trace up; kept when ≥ 1 unit). On a ground plane or after a step a trace goes down
  by the step plus 9 (DAT_00591e88) on ground: hit id (trace+0x1c, a word compared with 1 at 0x54f5e2) 0 → plain
  slide result. G_Trace (0x44bf00) writes 0x7ff there when the fraction is 1 and 0x7fe for the world,
  and an entity hit writes the entity number, so this only happens on a hit on entity 0. Fraction 1 → the origin
  goes down by the step height only; normal z < 0.3 → plain slide result; else land there and clip the velocity. The step is dropped for the plain slide (then stuck to
  ground 9 units below) unless it got further along the velocity (+0.001), or a jump step went above the apex; a
  jump step caps the vertical speed at sqrt(2·g·(apex − z)) (0x541430). On a ground plane a height change of more
  than 0.5 against the plain slide scales the velocity by (1 − |z − start z|/step)·0.8 + 0.2.
- Jump (0x5414d0): also sets ps+0x74 = origin z, pm_time 0, pm_flags 0x80000. The walk move calls 0x541300 while the
  flag is set: pm_time 1200 when landing at least 18 above the jump's origin, else 1800; above 1800 it clears the
  flag; the 0.5 / 0.65 speed factors apply only with jump_slowdownEnable.

## Lean (0x50ade0, 0x440380)

- `+leanleft` / `+leanright` set usercmd button bits 0x40 / 0x80 (CL_CmdButtons near 0x409bfb).
- leanf (ps+0x4c, also a player-state netfield "leanf") targets −1, +1 or 0; leaning needs the ground (or
  pm_type 1). Limit 0.5, or 0.25 at view height 11. It moves towards the limit by ms · limit · (1/350) and back
  to 0 by ms · limit · (1/280). A cube (−8..8) traced from the eye to the leaned eye limits |leanf| to
  1 − sqrt(1 − fraction). A nonzero leanf scales movement by 0.4 (0x508140).
- View offset (0x440380, called by cgame at 0x4ae770 with 16 and 20): g = (2 − |leanf|)·leanf, roll = 16·g
  degrees, offset = 20·g·(cos(roll)·right − sin(roll)·up); the view angles are not rolled.
- View weapon: position + (0, −cos r, −sin r)·(1 − adsFrac)·g·1.6 with r = −2g degrees while not fully aimed
  (0x4b61d0), roll −2g (0x4b6420).
- TODO(spec): the ps flags that forbid leaning (0x8000, 0x2000000 / 0x4000000, 0x20300 in ps+0x7c).

## Aiming (0x4dd020, 0x50bd00, 0x50c5c7)

pm_flags 0x40 = aiming: the aim command (0x1000 or weapon state 0x13/0x14) with a weapon that may aim (0x4dcf70:
aimDownSight, weapon state not 1, 2, 10–16); prone only when the aim was just pressed or not moving (+ pm_flags
0x800). Prone and aiming from the previous step clears forward/right. pm_flags 0x100 = aiming, not prone, weapon
state not 5–9: the ×0.4 of cmdScale.

## Mantle (0x5257f0 check, 0x5259b0 move, 0x524bd0 delta, 0x525b60 yaw cap)

Mantle brushes are found with contents
0x1000000 and surface flags 0x2000000 (MANTLE_ON) / 0x4000000 (MANTLE_OVER); the move follows the delta translation
of player_mantle_up_* (+ player_mantle_over_*) rotated by the mantle yaw.

## Ladder (0x50bd90 check, 0x50c070 move, 0x541560 jump, 0x54f409 step, 0x4de91b weapon)

- Grab (0x50bd90): a box 6 units narrower than the player (feet raised to z +8) is swept 8 units (walking) or 30
  units (airborne) along the flat facing, or straight into the stored ladder normal when already climbing in the
  air. A hit on a surface with flag 0x8 sets pm_flags 0x20 and stores the hit normal (ps+0x64). A walking player
  must press forward. No grab while pm_flags 0x40000 (ladder just left) is set, prone, or within 300 ms of a jump.
  Losing the surface clears 0x20 and sets 0x40000; 0x40000 is dropped once walking again.
- Move (0x50c070): first the jump check; if it jumps, the air move runs instead. Otherwise forward climbs with a
  speed scaled by how far the view looks up (looking down climbs down), strafe runs along the ladder plane,
  acceleration 9; without forward the vertical speed decays by gravity, in the air the speed into the wall is
  replaced by a 50 units/s push into the ladder for a mostly vertical climb; then the step slide move.
- Jump on a ladder (0x5416b0 -> 0x5414d0, 0x541640, 0x541560): the normal jump gate applies (newly pressed, 500 ms
  since the last jump, standing view height, pm_flags 0x1000 / 0x4 clear, pm_type below 4); there is no ground
  requirement. The normal jump start runs (vertical speed sqrt(2 · jump_height · gravity)) and the event is the
  ladder jump 0x5b. Then, because pm_flags 0x20 is still set: the vertical speed is multiplied by 0.75 (0x591e38);
  the view forward with z zeroed and normalized is the push direction; when it points into the ladder (dot with
  the ladder normal below 0, 0x5415a5) it is mirrored off the ladder plane (d - 2 (d·n) n, 0x59216c = -2) and
  normalized again; the horizontal velocity becomes direction × `jump_ladderPushVel` (dvar at 0x1cbac3c,
  registered at 0x54122c: default 128, range 0..1024). pm_flags 0x20 is cleared directly (0x54162c), without
  0x40000, so the ladder can be grabbed again 300 ms later. Space therefore jumps the player away from the ladder;
  it does not climb.
- Step slide on a ladder (0x54f2f8, 0x54f409..0x54f421): on a ladder the ground plane counts as absent (no stick to
  ground, no stairs event) and the jump flag 0x80000 is cleared. Airborne, the step-up is normally only tried for a
  jump step; with pm_flags 0x20 set and an upward vertical speed after the slide it is tried as well: a blocked
  slide is repeated from up to 18 units higher (10 prone) and lowered back by the same amount. This is how the
  player gets off at the top: the push into the ladder hits the top edge, the step-up lifts the box over it and
  the lowered position lands on the floor above.
- Weapon (0x4de880, branch 0x4de91b..0x4de947): while pm_flags 0x20 is set the weapon change goes to weapon 0
  (the weapon is put away), the same path as mantling and vehicles; it is raised again when the flag is gone.
  The busy weapon states that skip the whole change check (0x4de883..0x4de8ea) apply as for mantling.
- TODO(spec): the landing-time adjustment of the jump speed inside 0x5414d0 (0x541490, pm_flags 0x80000 and
  pm_time <= 1800) and the pm_flags 0x1000 / 0x4 jump gates are not modelled by the port's jump check.
