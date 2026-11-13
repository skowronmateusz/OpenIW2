# Multiplayer player models and animation (third person)

How a multiplayer client other than the local one is drawn: the model the gametype gives it, the player animation
script that picks its animations from the movement state, the weapon in its hands and the body left after a death.
Evidence: VAs in the patch 1.3 MP executable (the client; "MP" below), VAs in the 1.3 Linux dedicated server ("lnxded",
as in docs/spec/mp.md) and the shipped files under `main/` cited as `file:line`. Implementation:
`src/game/playeranimscript.*` (the script), `src/game/mpplayers.*` (the per-client driver), the third-person branch of
`apps/iw2view` (drawing).

## Model composition (script side)

The engine does not choose the player's look; the gametype does it with ordinary entity built-ins.
- `_teams.gsc:420-476` (`setPlayerModels`, run from the team module's init, `_teams.gsc:33`) picks one `mptype`
  script per side from `game["allies"]` and the `*_soldiertype` entries, precaches it and stores its `main` as
  `game["allies_model"]` / `game["axis_model"]`.
- `_teams.gsc:478-488` (`model`, called by every gametype at spawn, e.g. `dm.gsc:424`) detaches everything and runs the
  team's function, then stores the result of `_utility.gsc:155` (`saveModel`: model, view model and every attachment
  with its tag) in `pers["savedmodel"]`.
- An `mptype` script (`mptype/american_normandy.gsc:2-30`) picks one of several `character/mp_*` scripts at random;
  those set a `playerbody_*` model (through `codescripts/character.gsc`, `setModelFromArray`) and `attach` a head and a
  hat with an empty tag (`character/mp_american_normandy_braeburn.gsc:4-7`).

So a spawned client entity carries a body model plus attachments exactly as an SP drone or actor does. Attachments with
an empty tag share the body's skeleton (bones of the same name follow the body), as in the SP renderer.

## The weapon in the hands

The world model of the client's current weapon is the weapon file's `worldModel` key (`weapons/mp/thompson_mp`: an
`xmodel/weapon_*` name). The client game attaches it to the body at `tag_weapon_right` (string 0x5b4270, referenced
from the client's entity code at MP 0x4bf475 and 0x513430). It is not one of the script attachments: `detachAll` in
`_teams.gsc:480` does not remove it and `getAttachSize` does not count it. TODO(spec): which of the two sites is the
third-person player body, and the left-hand tag (0x5b4298) users.

## The animation tree

The client resolves, once (MP 0x4fb490 → 0x4fb3c0), the tree `multiplayer` (`animtrees/multiplayer.atr`) and four of its
nodes by name: `root` (0x5af144), `torso` (0x5b73e8), `legs` (0x5b73e0) and `turning` (0x5b73d8).
In the tree file (`multiplayer.atr:9-558`) `main` has two blend children: `torso`, holding the `pt_*` animations, and
`legs`, holding the `pl_*` and the full-body `pb_*` animations (the naming rule is the comment at
`multiplayer.atr:1-7`). A `pt_` animation has tracks for the upper body only (for `pt_stand_shoot`: `j_spine1` and every
bone above it); a `pb_` animation has tracks for the whole skeleton.

Port rule: a "both" command plays its animation on the `legs` node and clears `torso`; a "legs" command plays on `legs`;
a "torso" command plays on `torso`. With both nodes weighted the shared upper-body bones are the weighted mean of the
two, as for any blend node (docs/spec/animtree.md). TODO(spec): how the original weights the torso against the legs
(the per-bone mask, if any); the `turning` node (in-place turns) is not used.

## The player animation script

`mp/playeranim.script` is a text file of three sections; keywords are matched without regard to case (the engine's
keyword tables are upper case, the file is mostly lower case). `//` starts a comment.

1. `DEFINES` (`playeranim.script:25-34`): lines `set <condition type> <name> = <value> AND <value> ...`. A defined name
   may then be used as a value of that condition type and stands for all its values (e.g. a weapon-class name covering
   two classes).
2. `ANIMATIONS`: `STATE <name> { ... }` blocks. Only the `COMBAT` state has content (`playeranim.script:98-833`). Inside,
   one block per move type, each a list of items.
3. `EVENTS` (`playeranim.script:834` on): one block per event name, each a list of items.

An item is a condition list followed by a `{ }` block of commands. The condition list is `default` (always true) or
`<type> <values>` pairs separated by commas; values of one type are joined with `AND` and the condition holds when the
current value is any of them. Flag types (`firing`) take no value. All pairs must hold. Items are tried in file order
and the first one that holds is used (the rule stated at `playeranim.script:39-40`). An empty block matches and does
nothing (`playeranim.script:839-842`: firing on a mounted MG plays no torso animation).

A command line is `<body part> <animation> [modifiers]`, body part `legs`, `torso` or `both` (table at MP 0x5d8c88).
Modifiers seen in the file: `duration <ms>` (how long a torso event holds the torso), `blendtime <ms>`, `turretanim`
(MG42 aim, not used here), `sound <alias>`.
When an item lists several commands, one is chosen at random each time the item is played (lnxded 0x80d96ee:
`rand() % count` over the item's 16-byte commands, then 0x80d92e8 plays it).

Keyword tables (MP, 8-byte entries, string pointer first):
- move types at 0x5d8a90, index 0 unused: idle 1, idlecr 2, idleprone 3, walk 4, walkbk 5, walkcr 6, walkcrbk 7,
  walkprone 8, walkpronebk 9, run 10, runbk 11, runcr 12, runcrbk 13, turnright 14, turnleft 15, turnrightcr 16,
  turnleftcr 17, climbup 18, climbdown 19, the mantle types 20-30, flinch 31-34, stumble 35-40;
- events at 0x5d8be0: pain 0, death 1, fireweapon 2, jump 3, jumpbk 4, land 5, dropweapon 6, raiseweapon 7, climbmount
  8, climbdismount 9, reload 10, crouch_to_prone 11, prone_to_crouch 12, stand_to_crouch 13, crouch_to_stand 14,
  stand_to_prone 15, prone_to_stand 16, meleeattack 17, shellshock 18;
- condition types at 0x5d8d80: playeranimtype, weaponclass, mounted, movetype, underhand, crouching, firing,
  weapon_position, strafing; weapon classes at 0x5d8cf0 (rifle, mg, smg, spread, pistol, grenade, rocketlauncher,
  turret, non-player, item); weapon positions hip / ads (0x5d8d48); strafing not / left / right (0x5d8d60).

The stance of the local (host) client comes from its recorded view state (docs/spec/mp-hud.md, "The recorded view
state": view height target 11 / 40 / 60, velocity, lean), set every frame by the host's front end; bots and network
clients fill their input from their own movement. Without it the host's body would stay standing: its death animation
and its body in a bot's killcam would be the standing ones. The dead state's target (8) is not a stance and is ignored.

Condition values for a client:
- `playerAnimType`: the weapon file's `playerAnimType` key (thompson_mp: `thompson`, colt_mp: `other`), one of the names
  in `mp/playeranimtypes.txt` (the engine reports `Unknown playerAnimType "%s" in "%s"` otherwise, 0x5bdedd). Without
  a weapon: `none`. TODO(spec): the value with no weapon.
- `weaponclass`: the weapon file's `weaponClass` key.
- `movetype`: the current move type (below).
- `weapon_position`: `ads` while aiming down the sights, else `hip`.
- `strafing`: TODO(spec) (computed in the script player 0x4f9190, not decoded): this port uses `left` / `right` when
  the command moves only sideways, else `not`; the file's own note says it is never left or right when moving
  backwards (`playeranim.script:80`).
- `mounted`, `firing`, `underhand`, `crouching`: no MG mounting here; these hold false.

## Move type from the movement (MP 0x518740, the MP footsteps step)

After the move the footsteps step calls the script player (MP 0x4f9190) with the move type (the first argument 3 is
consistent with the `COMBAT` state; TODO(spec)):
- slower than `player_moveThreshhold` (default 10, docs/spec/pmove.md) on the ground: idle by stance from the view
  height target (0x518d1f / 0x518d2d): 11 → `idleprone`, 40 → `idlecr`, else `idle`; TODO(spec) the turn types 14-17
  chosen there;
- moving, prone (0x5189d3): `walkpronebk` when the command's forward move is negative, else `walkprone`;
- moving, crouched (0x5189ff): backwards `walkcrbk` / `runcrbk`, else `walkcr` / `runcr`;
- moving, standing (0x518a98): backwards `walkbk` / `runbk`, else `walk` / `run`.
Walk against run is the same "slow" flag that picks the slower bob factors (docs/spec/pmove.md, bob cycle): aiming
slowdown (pm_flags 0x100) or leaning. A damage timer in the player state drives the hit reactions (VAs in the 1.3 Linux dedicated server): finishPlayerDamage
sets it (docs/spec/mp.md), the move decreases it by the elapsed ms (ps +0x90, 0x80e2700) and the footsteps step compares it with
the duration ps +0x94 minus `player_dmgtimer_stumbleTime` (default 500), at least 0 (0x80e5790). Above that threshold a standing
idle becomes a flinch chosen by the hit's yaw (0x80e5672: 315..45 `flinch_forward`, 45..135 `flinch_left`, 135..225
`flinch_backward`, else `flinch_right`), and a moving client uses the stumble types: `stumble_forward` / `stumble_backward`
for run, `stumble_walk_*` for walk, `stumble_crouch_*` for crouch (types 31-40). Prone has no reaction. The movement scale from the
timer (0x80e1f20) is 1 with the default `player_dmgtimer_minScale` of 0, so it is not implemented.

## Ladder and mantle move types

VAs in the 1.3 Linux dedicated server. The footsteps step (0x80e572c, called by the move at 0x80e9066) picks a move
type and calls the script player (0x80d94a6, arguments: the player state, state 3, the move type, 1) in this order:
- On a map turret (eFlags 0x300, 0x80e57ff): the idle of the stance (`idleprone` with pm_flags 1, `idlecr` with
  pm_flags 2, else `idle`, 0x80e5815..0x80e589d), nothing else.
- In the air (groundEntityNum 1023, 0x80e58b8) with pm_type other than 1 (linked) and pm_flags 0x20 (on a ladder,
  docs/spec/pmove.md): within 300 ms of the last jump (command time minus ps +0x70, at most 299, 0x80e58e9..0x80e58f8)
  the step ends without choosing; otherwise `climbup` (type 18) while the vertical velocity (ps +0x28) is zero or up,
  `climbdown` (19) while it goes down (0x80e5960..0x80e59b6). The climb also advances the bob cycle (docs/spec/pmove.md).
- In the air otherwise, and after the climb choice: the step ends there unless a stance change is under way (pm_flags
  & 3 differ from the stance of the view height target, 0x80e1532: 40 → 2, 11 → 1, else 0; 0x80e5a26..0x80e5a34), so
  the animation that started before the player left the ground goes on.
- On the ground: the idle / walk / run types of the previous section.
The mantle move (0x80de260, while mantling) plays move type 20 + the mantle animation index each step (0x80de357..
0x80de377). The index comes from the mantle transition table (0x8184d00, 12-byte entries {up animation, over animation,
height}, the same table as the single-player one, docs/spec/pmove.md "Mantle") by the mantle's transition (+8 of the
mantle state at ps +0x5bc): the up animation while the mantle time (+4) is at most the up animation's length, then the
over animation (0x80dd1b2). The move types are named by the table at 0x81848c0 (8-byte entries): 20 `mantle_root`,
21..27 `mantle_up_57` .. `mantle_up_21`, 28..30 `mantle_over_high`, `mantle_over_mid`, `mantle_over_low`; the
`playeranim.script` blocks of 18, 19 and 21..30 play `pb_climbup`, `pb_climbdown` and the `mp_mantle_*` animations on both
body parts (`playeranim.script:703-806`). A mantle is in the air, so the footsteps step chooses nothing over it.

openiw2 applies the climb and the mantle types and the 300 ms hold after a jump on a ladder; a client on a turret does
not move, so the ground rule gives it the same idle; the airborne hold (no new type in the air) is not applied
(`TODO(spec)`: it would need the puppets' ground state, which the network client does not get), so a jump shows the
types of the speed it has.

What reaches the other clients: the player state's legs and torso animations (ps +0x7c, ps +0x84) go into the
entity state's legsAnim and torsoAnim (0x80dfe37..0x80dfe4c, the player state to entity state copy), so every client
plays the server's choice. openiw2's clients choose the move type themselves from the entity state (stance flags,
velocity, docs/spec/net.md section 11), which carries nothing about a ladder or a mantle: the server sends in legsAnim
the node number in the multiplayer animation tree of the animation it plays for a climb or mantle move type (0 for
any other), and the client uses the move type whose animation that is. TODO(spec): the 1.3 numbering of legsAnim and
its toggle bit (as for the body). The killcam's archived players carry it from their recorded view state.
TODO(spec): how the client advances a move type's animation (a climb on the spot is still played at the animation's
own rate, as walking is).

## Events

- `fireweapon` when the client fires, `reload` when a reload starts, `jump` on a jump: the first matching item's
  command; a torso command holds the torso for its `duration`, or for the animation's length when it has none.
  A body part whose remaining hold is over 49 ms refuses a new animation unless the call forces it (0x80d90d6). The move-type
  animation is never forced; `fireweapon`, `jump`, `land`, `reload` and the death are forced, so a newer one cuts an older one;
  raising and dropping the weapon and the stance changes are not.
- `death` (lnxded `player_die` 0x810175a → 0x80d96ee, event 1, forced): the event function returns −1 when the player
  is dead already (pm_type above 5) and the event is not death, or when no item matches; otherwise it plays the chosen
  command and returns its duration, which becomes the last argument of `CodeCallback_PlayerKilled`
  (`deathAnimDuration`, docs/spec/mp.md). `dm.gsc:381` passes it to `cloneplayer`. The `DEATH` block
  (`playeranim.script:1264-1316`) chooses by move type: prone, crouched idle, running backwards, running or crouch
  running sideways, crouch running, running, and a standing default, several with random alternatives.
  TODO(spec): the duration returned by 0x80d92e8 (this port: the animation length in ms).

## The body (cloneplayer)

`cloneplayer(deathAnimDuration)` leaves a body entity where the player died (`dm.gsc:381`, `_deathicons.gsc` reads its
origin). Port: the body copies the player's model and attachments and plays the death animation chosen at the death,
holding its last frame; the player entity itself is not drawn while its `sessionstate` is not `playing`.
The body (cloneplayer 0x80fcc76 in the 1.3 Linux dedicated server) is an entity of handler class 12 in entity slot 64 + (counter % 8).
It lives until a later cloneplayer takes its slot; no timer removes it. Its think runs deathAnimDuration ms after the clone
and clears eFlags 0x80000. cloneplayer drops no weapon: a dropped weapon is the gametype script's own call.
The body starts a gravity trajectory (type 5) at the level time with the victim's velocity; a positive horizontal part is capped by
`g_clonePlayerMaxVelocity` (default 80, 0x80fcdd1). It is a physics object run by the item step each frame (0x810a0c8 -> 0x8107de6,
as for items) with the dead client's box and clip mask 0x10001; its ground stays 1023, so after touching the floor it is re-based to
gravity every frame and rests by the trace. Unlike an item it has none of the settle step at the end (entity byte +0xfc is 0).

## Hit locations

Bullets hit a client on the bone hit boxes of its posed body and attachments (docs/spec/collision.md "Hit boxes"), as
actors: Bullet_Fire of the MP server (VAs in the 1.3 Linux dedicated server) passes the same priority maps as the SP
executable (0x8187c1f for rifleBullet weapons, else 0x8187c0c; selected at 0x811ff02 / 0x811ff0e) to its locational
trace (0x810a5cc) with mask 0x2802831, which takes the client contents 0x2000000 (set at 0x80f56eb). The location
name reaches the gametype's damage callback; `dm.gsc:312` makes a "head" kill MOD_HEAD_SHOT. The damage is first
scaled by the attacker weapon's location multiplier (`loc*` keys of the weapon file, weapon +0x5b0; read at 0x8101ae2,
multiplied and truncated at 0x8101bdc..0x8101bf6, before CodeCallback_PlayerDamage 0x81183b8), the same rule as the
SP G_Damage (combat.cpp `hitLocationMultiplier`). TODO(spec): the server site that marks a client entity for hit boxes
(OpenIW2 marks every multiplayer client with a body model). `World::debugBulletLocation` traces one bullet for tests.

## Not covered (TODO(spec))

Torso pitch to the view (the bone list initialised with `J_Spine4` at MP 0x4df0d0; the strings `fTorsoPitch` /
`fWaistPitch` at 0x5aa600 / 0x5aa60c name tunables): the body is drawn with the animation pose only. Leg yaw against
the torso, the turning node, MG42 mounting, flinches, stumbles, the transition events
(stand_to_crouch ...), sounds of commands.
