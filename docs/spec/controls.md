# Player controls

Sources: CoD2SP_s.exe 1.3 (objdump listing and REA Ghidra decompiles),
`default.cfg` from the game's iw_00.iwd. Implemented in `src/game/clientinput.{h,cpp}`,
`src/game/playerweapon.{h,cpp}` and `src/game/playeractions.cpp`.

## Bindings

The client execs `default.cfg` (strings "default.cfg", "default_localize.cfg"; the Polish `default_localize.cfg` differs
only in the console key). Its binds are the defaults: W/S/A/D `+forward` `+back` `+moveleft` `+moveright`, Q/E
`+leanleft` `+leanright`, SHIFT `+melee_breath`, MOUSE1 `+attack`, MOUSE2 `toggleads`, 1 and 2 `weapnext`, MOUSE3
and G `+frag`, 4 `+smoke`, B `+binoculars`, F `+activate`, R `+reload`, SPACE `+gostand`, CTRL `goprone`, C
`gocrouch`. OpenIW2 reads the file from the game data at start; nothing is hard-coded.

## Client commands (registration 0x409fe4…0x40a3e4, handlers 0x408b10…0x4093e0)

- `+x` / `-x` press and release a key button (IN_KeyDown 0x408b50 / IN_KeyUp 0x408bd0): up to two keys hold it, the
  first press sets *active* and *was pressed*.
- `+melee_breath` presses `+melee` and `+holdbreath`; `+breath_binoculars` presses `+holdbreath` and `+binoculars`.
- Stance (client value 0 stand, 1 crouch, 2 prone): `gocrouch` → 1, `goprone` → 2, `togglecrouch` 1 ↔ 0,
  `toggleprone` 2 ↔ 0, `lowerstance` +1, `raisestance` −1, all ignored while `+prone` or `+movedown` is held.
  `+gostand` presses its own button; standing it also presses jump, otherwise it sets stand. `+moveup` steps up
  one stance (prone → crouch → stand) and jumps only when standing.
- `toggleads` flips a flag, `leaveads` clears it; the flag is cleared at map load (0x40e821).
- `weapnext` / `weapprev` (cgame 0x4b6fd0 / 0x4b7030 → 0x4b7410): select the weapon in the other of the two weapon
  slots (primary, primaryb) when it has one; rounds do not count (both commands pass 0). Ignored while `cl_paused` is set, with
  pm_flags 0x8000, in a vehicle (0x20000), and sooner than `cg_weaponCycleDelay` ms (int, default 0, archived) after the last
  accepted cycle (stamp 0xf796e4). The walk over other owned weapons (0x4b72f0) skips slotted, offhand and item-class weapons,
  which leaves turret and non-player classes; no shipped SP script gives the player one, so it is not implemented.

## Blocked stance changes
A rise with no room (pmove 0x509d10) adds, unless usercmd bit 0x2000 is set, event 0x8d parm 1 from crouch, or 0x8e from prone
with parm 2 (crouch blocked) or 1 (neither stand nor crouch fit). A stand request from prone that only fits crouched rises to
crouch with no event. The cgame (0x4bac40 / 0x4bac60) sets the client stance to the event's stance, unless `+prone` or `+movedown`
is held, then starts hint state 4 (parm 1) or 5 (parm 2) through 0x4ba880. So the wish becomes the blocked stance and the hint
is raised once per stand attempt, for cg_invalidCmdHintDuration (1800 ms); it does not repeat while the player stays under cover.
Releasing a held `+movedown` under cover is such an attempt, since the client stance is then still stand.

## usercmd buttons (CL_CmdButtons 0x409af0, stance 0x409500)

Held or pressed since the last command: attack 0x1, melee 0x4, activate 0x8, reload 0x10, usereload 0x20, leanleft
0x40, leanright 0x80, jump 0x400, binoculars 0x4000, holdbreath 0x8000, frag 0x10000, smoke 0x20000. In a vehicle
(pm_type 2/3, or pm_flags 0x20000 without 0x80000) the `+gostand` button also counts as jump. Stance: `+prone` held
→ 0x100 | 0x2000, else `+movedown` held → 0x200 | 0x2000, else client stance 1 → 0x200 (crouch), 2 → 0x100
(prone). Aim 0x1000 = `+speed` held XOR the `toggleads` flag, or always while the weapon state is 0x13 / 0x14.

## freezecontrols (pm_flags 0x8000)

`freezecontrols(on)` (0x5392e0) stores the flag in the client record (+0x8630); an absolute `playerlinkto` sets another byte (+0x8698).
ClientThink (0x4e7bc0) sets ps pm_flags 0x8000 while either is set and clears it otherwise. Consumers of the flag (VAs in the patch 1.3 SP executable):
- the client mouse function (test at 0x409871) applies no mouse delta, so the view cannot be turned;
- Pmove (0x50cc10) clears the command's forward and right bytes and keeps only the buttons 0x300 (prone, crouch); PmoveSingle (0x50c4a0,
  test at the top) clears them again, keeps 0x2300 of the buttons and sets the velocity (ps+0x20..0x28) to zero before anything else in the step.
  Gravity then adds one step's worth, so the player stands still and falls slowly; no fire, reload, aim, jump, lean or melee button reaches PM_Weapon;
- the stance logic (0x509d10) is skipped outside a turret (0x509e59 jumps to 0x50a37e), so the stance does not change at all, with or
  without a stance button (a crouched player stays crouched);
- lean input is cancelled (0x50adf1), a driven vehicle takes no command (0x510da5) and the turret fire is not started (0x51096b).
Scripts still see the held buttons (`attackbuttonpressed` etc. read the raw record), so a briefing can wait for a key.
`PlayerInput::controlsFrozen` carries the flag (freezecontrols or absolute link) into `moveStep` and the mouse gate; the front end strips the weapon buttons
only for freezecontrols (`controlsFrozen()`), not for an absolute link (TODO(spec): no shipped script fires during an absolute link, so this is untested).

## Weapon states (PM_Weapon 0x4dfc30)

Player-state fields: weapon +0xb0, state +0xb4, weaponTime +0x34, weaponDelay +0x38, grenade fuse +0x3c, offhand
+0xac, view animation +0x5ac (bit 0x200 toggles on every change). State changes run when the delay just ran out or
both timers are 0. View animation numbers paired with their timers: 2 fire, 4 rechamber, 8 melee, 9 drop,
10 raise, 15 altDrop, 16 altRaise, 17 quickDrop, 18 quickRaise, 19 holdFire.

- **Dead player** (0x4dfc30, start): with pm_flags 0x1000 clear and pm_type ≥ 4 (health ≤ 0) PM_Weapon only sets the
  player's weapon (+0xb0) to 0 and returns: no fire, reload, melee, offhand or weapon change runs, so no shot, sound,
  event or ammo use follows (the port clears the weapon at once, without a putaway). The view code (CG_CalcFov 0x4ae7b0) takes the zoom from the definition of the player's weapon, which is 0 after death, so the field of view is the unzoomed cg_fov and the scope overlay is gone (the port zeroes the aim fraction). TODO(spec): the pm_flags 0x1000 branch.
- **Weapon change** (0x4de880 → 0x4ddb40): not in states 12–22; with the timer running only a rechamber or reload
  (states 4–9) may be interrupted, firing (3) and melee (10, 11) or a running delay may not. A command weapon
  different from the player's starts the drop: state 2 with `altDropTime` (view 15) when it is the current
  weapon's `altWeapon`, else `dropTime` (view 9); without a current weapon at once. At the end (state 2) the command
  weapon becomes the player's: state 1 with `altRaiseTime` (view 16) for the alt weapon, else `raiseTime` (view 10);
  state 1 ends in 0. TODO(spec): the aim flag check (ps+0xc 0x8000) that skips the change.
- **Melee** (0x4def20): not in 12–22; needs `meleeDamage`, button 4 newly pressed (latch ps+0xc 0x2000 until
  released), no delay running (or a reload), and aim fraction ≤ 0 or no `adsOverlayReticle` (with a scope the key
  holds the breath instead, 0x4dd350). Not in states 1, 2, 10, 11. View 8, event 0xa3; `meleeDelay` 0 → the blow at
  once, else state 10 with weaponTime `meleeTime` and delay `meleeDelay`. The blow (0x4dee30): weaponTime ≥
  `meleeTime − meleeDelay`, event 0xa4, state 11; state 11 ends in 0.
- **Turret and vehicle** (proven): PM_Weapon (0x4dfc30) returns before any state change when player-state eFlags (+0x7c) & 0x300
  (a turret; 0x4dfc59), so nothing advances while mounted: no offhand, no fuse countdown. The offhand step (0x4df470) itself tests
  eFlags & 0x20300 (turret or vehicle 0x20000; 0x4df473) and returns, so a vehicle never starts a grenade either. There is no latch:
  the original starts a grenade on the first PM_Weapon after getting off if frag is still held (it throws on release, as it never
  cooks). openiw2 deviation, only with iw2_grenadeCook on: frag / smoke held while blocked is ignored until released, so getting off
  a turret / flak88 with G down does not leave a primed, cooking grenade (PlayerWeapon::blockOffhand, tests/test_offhandturret.cpp).
- **Offhand** (0x4df470): not with pm_flags 0x20300 or in states 12–15 or 17–22. +frag (class 1) / +smoke (class 2)
  held → the first weapon of that `offhandClass` with clip + stock ≠ 0 (0x4dc6d0); none → events 0x94 and 0xc9 /
  0xca. With a weapon (and not in state 16): state 12, `quickDropTime` of the weapon, view 17, then state 13; else
  state 13 at once: `holdFireTime` of the grenade, view 19, flag 0x10 (the view model is the offhand's), event 0xa5.
  13 → 14: grenade fuse = `fuseTime`. 14 waits while the frag/smoke button is held in this and the previous command;
  then 15: `fireTime`, view 2, event 0xa6 (the throw), one round off the grenade's clip (event 0x94 when the last).
  15 → 16: the weapon's `quickRaiseTime`, view 18 (no weapon: delay 1); 16 → 0 (0x4df0a0). Cooking (0x4df5a0): only
  with `cookOffHold` the fuse counts down while flag 0x10 is set; at 0 event 200 (the grenade goes off in the hand:
  game side 0x4e775d: unless god / demigod the client fuse and health become 0 and player_die 0x527d80 runs; no grenade entity,
  no radius damage in the original. Client side, case 200 of 0x4ba8d0: the sound at 0xf5b7a4 (grenade_explode, surface default)
  at the player's origin (0x4bb1a7) and the grenade_explode impact effect of surface 0, direction up (media +0x33c, 0x4853f0), whether or
  not the player died. openiw2 adds, for cooked frags only, the weapon's radius damage from the hand (MOD_GRENADE_SPLASH,
  player excluded; `World::playerGrenadeInHand`, tests/test_grenadecook.cpp). The weapon field is at +0x33c of the weapon definition (field table entry
  at 0x5a8708). No shipped weapon file (single-player or multiplayer grenades) sets `cookOffHold` (all are 0), and
  0x4df5a0 is the only decrement of ps+0x3c found (TODO(spec): no exhaustive search of every writer), so in the shipped game a held grenade does not cook: the fuse stays at
  `fuseTime` and the throw gives the grenade `fuseTime` whatever the hold time. This is not a defect of the port
  (tests/test_grenadecook.cpp, tests/test_playerweapon.cpp `cooking`); a mod weapon with `cookOffHold 1` does cook.
  openiw2 option (deliberate deviation from the shipped data): the cvar `iw2_grenadeCook` (default 1, settable with `--cvar` or
  from config) makes frag-class offhand grenades (offhandClass "Frag Grenade") behave as if `cookOffHold` were 1 in both
  places that test +0x33c (the fuse burn 0x4df5a0 and the crosshair size term 0x4bd2c0: the size grows by (fuse ms mod 1000) · 0.01
  pixels-before-scale, a sawtooth). Smoke grenades only cook with the file field. With `iw2_grenadeCook 0` the shipped behaviour applies.
  Port: `PlayerWeapon::cooking()`, tests/test_grenadecook.cpp.

Firing, rechambering and reloading are still the inferred machine of `docs/spec/weapons.md`.

## Player events in the game (event switch 0x4e769f)

- 0x9e / 0x9f / 0xa0 / 0xaf–0xb1: FireWeapon (0x531c40).
- 0xa4 melee (0x531da0 → 0x530f80 → 0x530de0): from the eye (0x4e8b70: origin + view height, lean offset) along the
  view axes (0x531b90: the vehicle view angles in a vehicle), locational traces (mask 0x280e831, priority map
  0x5a9584, skipping the player) to eye + forward · `player_meleeRange` (64) + right · `player_meleeWidth` (10) · a
  + up · `player_meleeHeight` (10) · b for (a, b) of table 0x58f7c4: (0, 0), (1, 1), (1, −1), (−1, 1), (−1, −1);
  only the centre when width or height ≤ 0. The first trace that ends before its end on a non-sky surface hits; an
  entity that takes damage gets `meleeDamage` + rand() % 5 (Q3 LCG at 0x5a56f4, seed 0x89abcdef, `>> 17`), no
  flags, MOD_MELEE (7), along the forward axis. TODO(spec): the centre trace's impact (0x5295f0), events 0xad / 0xae.
- 0xa6 offhand throw (0x531d60 → 0x531990): velocity v = forward · `projectileSpeed` + (0, 0, `projectileSpeedUp`),
  grenade entity (0x5275f0) from the eye with `fuseTime`; then v is normalised and the trajectory delta gets
  v̂ · (v̂ · player velocity).
- 0xa8–0xac notify the player with script strings; 0xc8 (200) the cooked grenade.

## Use key and turrets

The use scan's line-of-sight test (0x534490, second pass; sorted candidates get +10000 and drop off when blocked) calls 0x44c1b0 with
the zero box, the eye as start, the candidate's absbox centre (a turret: its tag_player) as end, the player's entity as the pass
entity (ps+0xa8), 0x7ff as the second pass entity and content mask 0x11. That is a sight trace (zero extent takes the point path 0x41ada0; TODO(spec): that it is the
same walk as the AI sight trace the port's sightBlocked implements), not a clip trace: the sight walk keeps no 0.125 surface margin
(DAT_00591f80), so a target lying within 0.125 of a face (an item or key on a surface) stays visible, where a clip trace of the
same line is blocked. The port used the clip trace; it now calls the sight trace (tests/test_staticmodelhit.cpp).

The use scan (0x534d40 → 0x534490) also offers turrets (entity type 8) through 0x51dac0: turret info flag 0x1000
(set by the turret setup with 1|2: info+8 = 0x1003 at 0x51e4ec), not in use (+0x112), 0x51d8a0, no grenade fuse
and the player on the ground. 0x51d8a0 compares the flat arc-centre direction (yaw + right arc + (|right| +
|left|)/2) with the flat player → turret direction (0x51d962: turret +0xdc − player +0xdc), so the player has to
stand behind the gun, within half the total arc. The use (0x5342b0 / 0x534350 → 0x5341a0) calls the entity
handler's use function: turrets have handlers 20 (think 0x51ced0) and 21 (think 0x51d250), use 0x51db50 (no
Ghidra procedure: carve it). On a turret, pressing use again sets +0x112 to 2 when pm_flags & 0x300. The full
player-turret use, view clamp, firing and release are in src/game/playeruse.cpp;
on the client side (iw2view) PmoveSingle skips movement and PM_Weapon (0x50c918), the eye is the turret's
tag_player (0x4e8b70), and no view model is drawn (0x4b6d40 returns with eFlags & 0x300).

## Look and turn options (CL_CreateCmd 0x409da0)

Each command runs CL_AdjustAngles (0x409400), CL_KeyMove (0x409500) and CL_MouseMove (0x409720), then limits the pitch change of
the command (0x409ddd).
- CL_AdjustAngles: speed = frame msec * 0.001, times `cl_anglespeedkey` (default 1.5) while the +speed button is held. Without
  +strafe, +right / +left turn the yaw by speed * `cl_yawspeed` (140) times their key state; +lookup / +lookdown change the
  pitch by speed * `cl_pitchspeed` (140). No frozen-controls test.
- CL_KeyMove: with +strafe held, +right / +left add +-127 to the side move.
- CL_MouseMove: mouse deltas are summed per frame into one of two slots; `m_filter` (bool, default 0) uses the mean of this and
  the previous frame, else this frame's sum. With +strafe the x motion is the side move (round(x * `m_side`), 0.25), else the
  yaw. The y motion pitches the view while +mlook or `cl_freelook` (default 1) is on and +strafe is not held, else it is the
  forward move (-round(y * `m_forward`), 0.25). Command bytes are clamped to -128..127.
- `centerview` (0x409240) levels the pitch; -mlook (0x408b20) does the same when `cl_freelook` is 0.
- Implemented in iw2view (per-frame mouse pass) and ClientInput; tested in test_clientinput.

## Other option dvars

- `cg_blood` (cgame, bool, default 1, archived): off, flesh hits play the no-blood impact effect with no reflect (0x4b823d).
- `cg_drawCrosshair` (bool, default 1, archived): off, no hand crosshair (0x4bd129); turret and vehicle reticles ignore it.
- `r_zFeather` (renderer, bool, default 1, flags 0x2000, not archived): off, effects use the unfeathered emissive technique;
  the graphics menu sets it when it closes.
- `r_swapInterval` (renderer, bool, archived, latched): the presentation interval is one frame when set, else immediate
  (gfx 0x100115a3); applied at window creation and on vid_restart. The DLL default is 0 and `com_maxfps` defaults to 0 (no
  frame limit), so the original draws as fast as it can; openiw2 deliberately defaults r_swapInterval to 1.

## Screenshots

The commands live in the renderer DLL (gfx_d3d_x86_s.dll), not the exe. Sources: string area near file offset 0x1924a8
(command names) and 0x1970bc (file naming) of the DLL, the shipped `default.cfg` binding.

- The command names are `screenshotJpeg` and `screenshot` (string area near file offset 0x1924a8). The shipped
  `default.cfg` binds F12 to `screenshotJpeg`.
- Both write into `screenshots/` under the game's writable base folder, named `shot%04i.<ext>` with the first free
  number 0000..9999; when none is free, `shot9999` is used again, overwriting it (string near file offset 0x1970bc).
- After a successful write the console prints `Wrote %s`; a file that cannot be created prints
  `ScreenShot: Couldn't create a file`.

Port implementation (iw2view): the base folder is the app's writable preference folder (the same one that keeps
profiles and saves; `--shot` runs use the executable's folder), never the game's `main` folder. The capture is the
frame as displayed — scene and HUD at the window size — drawn a second time into an offscreen target, because the
swapchain texture cannot be read back. `screenshotJpeg` encodes JPEG with macOS ImageIO (`CGImageDestination`,
apps/iw2view/jpeg.cpp, quality 95). `screenshot` writes PNG instead of the original's TGA: the port has no TGA writer
and PNG is lossless and natively viewable on macOS, which serves the command's purpose better. Deliberate deviation
for the key: OpenIW2 uses F12 for its frame-rate counter, so whenever loaded bindings sit `screenshotJpeg` on F12 (the
shipped default), the port moves that binding to F11 and keeps F12 for the counter; the key can be rebound in the
controls menu (the screenshot row of the interact page). The naming is implemented in `src/util/screenshot.{h,cpp}`
and tested in test_screenshotname.

