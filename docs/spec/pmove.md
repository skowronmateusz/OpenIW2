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
  crouch/prone button logic (cmd bits 0x100/0x200), mantling.

## Movement

- Friction (0x507c80): speed ignores z when walking; below 1 the velocity is zeroed; on the ground
  control = max(speed, stopspeed), drop = control · friction · frametime (×0.3 with ps flag 0x200; landing
  slowdown factor with flag 0x80000), velocity scaled by max(speed − drop, 0)/speed.
- Command scale (0x508140 walking, 0x5080a0 in the air): g_speed ·
  max(|forward| (×player_backSpeedScale when negative, walking only), |right| · player_strafeSpeedScale) /
  (sqrt(forward² + right²) · 127); ×0.4 with ps flag 0x100 or a nonzero ps+0x4c; ×3 / ×6 for pm_type 2 / 3;
  walking: ×0.15 prone, ×0.65 crouched, blended during crouch/prone transitions (0x509760), × the weapon's
  `moveSpeedScale` (weapon def +0x264) when > 0; ×0.4 with command flag 0x800.
- Accelerate (0x507eb0): addspeed = wishspeed − v·wishdir; if > 0, accelspeed =
  min(addspeed, max(wishspeed, stopspeed) · frametime · accel), limited by `inertiaMax` (0x507e50).
- Walk move (0x5086f0): accel 9 standing, 12 crouched, 19 prone, 1 on slick surfaces; ×0.25 with
  ps flag 0x200. Air move (0x5085b0): accel 1.
- Jump (0x5416b0, 0x5414d0): needs the jump button newly pressed, at least 500 ms
  since the previous jump, a standing view height; vertical speed = sqrt(2 · jump_height · gravity); sets the
  landing-slowdown flag (applied by 0x541300 only when `jump_slowdownEnable`).

Implemented in `src/game/pmove.cpp`. Not implemented yet (TODO there): ground trace (0x50bd90), slide move
(0x54f2e0), step (0x508390), inertiaMax, ladders, mantling, lean.

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
