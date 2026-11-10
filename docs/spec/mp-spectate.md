# Multiplayer spectating, death view and intermission

How a client that is not playing sees the game: the free spectator camera, following another client, the dead view
before a respawn and the intermission camera at the end of a map. Evidence: VAs in the 1.3 Linux dedicated server
(server side), VAs in the patch 1.3 MP executable (client side and the shared movement code) and the shipped scripts
(`maps/mp/gametypes/*.gsc`). Implementation: `src/game/mpspectate.cpp`, `src/game/pmove.cpp` (`spectatorMove`),
`src/netgame/netmove.cpp`, iw2view, iw2server. Tests: `tests/test_mpspectate.cpp`.

## Session state and movement type

The script field `sessionstate` (docs/spec/mp.md "Client fields") is stored as a number: playing 0, dead 1,
spectator 2, intermission 3. The server gives the client's player state the MP movement type by it: playing 0
(1 while linked), spectator 4, intermission 5, dead 6 (7 while linked). The server's death path sets the type: 7 when the client was at type 1 (linked to an entity), else 6
(0x8101934..0x8101950). These are MP numbers: the SP pmove of this port
uses 4 / 5 for its dead types, so the MP code keeps its own mapping (`mpClientPmType`) and the spectator move is a
separate path of the movement code.

## allowSpectateTeam (0x80fdbe0)

`self allowSpectateTeam(team, allow)` works on a client entity only ("entity %i is not a player", "not an entity").
Each client has a mask of disallowed targets: `none` bit 1, `axis` bit 2, `allies` bit 4, `freelook` bit 0x10. Allowing
clears the bit, disallowing sets it; any other team string is the script error
`team must be "axis", "allies", "none", or "freelook"`. The mask starts empty (everything allowed). A team is allowed
when its bit, at the position of the team number (0 none, 1 axis, 2 allies), is clear; free look is bit 4.
`_spectating.gsc:48-86` sets the mask from `scr_spectatefree` and `scr_spectateenemy` for the team gametypes (own
team always, the enemy only with `scr_spectateenemy`, free look only with `scr_spectatefree`, never `none`); DM does not
call it, so DM spectators may watch anyone and fly freely.

## Spectator think (0x80f4eb2)

Every command of a client in session state spectator:
- The current and previous buttons are kept and the presses accumulated.
- When the script has not forced a target (`spectatorclient` below 0), free look is allowed, the client follows
  someone and button 4 changed since the previous command, the client stops following (below).
- An attack press (button 1) cycles to the next target; otherwise a press of button 0x1000 cycles to the previous
  one.
- While not following (pm_flags 0x400000 clear) the client moves itself: movement type 4, speed 400 with free look
  allowed, else 0 (it cannot move), clip mask 0x800811, and the normal movement code runs.

## Follow cycle (0x80ff4ac)

Only for a client in session state spectator whose `spectatorclient` is below 0. The direction must be 1 or -1 (else
the error `Cmd_FollowCycle_f: bad dir %i`). From the current target (0 when there is none) the client number steps by the
direction, wrapping at the number of client slots, until a client is found whose archived player state exists and whose
team is allowed; that client becomes the target. Back at the start without a match nothing changes. TODO(spec): the
archive test (0x809a0b6); this port accepts an active client that is playing, never the spectator itself.

## Stop following (0x80ff252)

`spectatorclient` and the target become -1. When the client was following, it is placed at the target's eye moved
40 units back along the view direction and 10 units up along the view's up vector, with an 8-unit box traced from the
eye with mask 0x810011 (the trace end is used); its view gets the target's view angles with the pitch 15 degrees
lower; the velocity and the following flags are cleared.

## End of frame (0x80f6916)

For a spectator:
- A `spectatorclient` set by the script (killcam, `_killcam.gsc`) is followed at the archive time
  (src/game/mpkillcam.cpp); the free follow state is left untouched meanwhile.
- Without a target and without free look the client cycles to the next target by itself.
- While following, the target's player state replaces the client's (pm_flags 0x400000 set). A target that is no longer
  valid ends the following (stop following above).
Clients following a client stop when it dies (0x810175a) or disconnects (0x80f94ce).

## Spectator movement (MP pmove, movement type 4)

The movement dispatch (0x51aac1, jump table 0x51af54) sends type 4 to 0x515e60:
- friction (0x515490): no ground friction (the spectator is never on the ground); type 4 adds a drop of
  speed x frametime x 5; below speed 1 the velocity stops;
- command scale (0x5158e0) of forward and right only: the ps speed x the larger absolute move / (127 x length), x 0.4
  while aiming or leaning, and for type 4 x `player_spectateSpeedScale` (default 2, range 0..20, registered at 0x513a16;
  the same factor in the 3-D scale 0x515810);
- the wish is forward x f + right x r along the full view axes times that scale; with a speed above 0 the up (0x80) and
  down (0x40) buttons add a vertical part with the scale of a full up command. TODO(spec): the exact vertical term
  (0x515f20..0x515f4b);
- accelerate with 8, then the slide move with collision and no gravity (0x530ac0).
So a free spectator flies at 800 units/s by default and stops quickly when the keys are released.

## Client side (0x4cbca0, 0x4cba90)

- pm_type 4: the 2D pass skips three draws that the other types make (0x4c9500, 0x4ca8d0, 0x542740). TODO(spec): which
  HUD parts these are; the crosshair names of everyone are shown (docs/spec/mp-hud.md).
- The client also registers the console commands `follow`, `follownext` and `followprev` (0x4c1d35, 0x4c1e21,
  0x4c1e70). TODO(spec): their server side; not implemented here (attack / 0x1000 presses cycle).
- Following (pm_flags 0x400000), while the scoreboard is hidden: the localized `CGAME_FOLLOWING` at y 40 and the
  target's name at y 65, centred, scale 1/3, shadowed. TODO(spec): the colour (0x59d038) and the second hide flag
  (0x151ad94). The followed client's body is not drawn: its view is the camera.
- pm_type 5: only the scoreboard (docs/spec/mp-scoreboard.md).
- View weapon: the MP view weapon function (VAs in the patch 1.3 MP executable 0x4d6390) returns at once for pm_type 4
  and 5 (0x4d6399..0x4d63ad), so a spectator or a client in intermission sees no gun even though its player state
  still carries a weapon. Only then come the other tests (a flag at 0x1513c40, cg_drawGun, the turret eFlags 0x300).
  Implemented: iw2view's network client hides the view weapon by the server's pm_type
  (`netgame::RemoteView::viewWeaponHidden`).

## Dead view

A client in session state dead (`dm.gsc:321`) keeps its body where it died (`cloneplayer`: a corpse entity, entity type 2,
built at 0x52cd00). The MP client does not use the single-player dead view (pitch -15, roll 40): every frame 0x4cfcb0 sets
the third-person flag when `cg_thirdPerson` is set or the movement type is a dead one (6 or more), and the view then goes
through the third-person offset (0x4ce890, chosen at 0x4cfb1e) instead of the first-person one. That offset:

- adds the view height to the view origin (the eye); while dead, the yaw of both the focus and the view becomes the
  player state's dead yaw (an int, facing the killer, docs/spec/mp.md);
- the focus angles are the view angles with the pitch limited to 45; the focus point lies 512 units along them;
- the camera starts 8 units above the eye, the view pitch is halved, and the camera moves back by `cg_thirdPersonRange`
  (default 120) along the view's forward vector, turned by `cg_thirdPersonAngle` (default 0) towards the right vector;
- a box trace (-4..4, mask 0x811, skipping the own client) from the eye to that point; on a hit the camera goes to the
  hit point raised by (1 - fraction) * 32 and a second trace from the eye limits it again;
- the pitch then looks at the focus point (minus the arctangent of the height difference over the horizontal distance,
  at least 1); the yaw is lowered by `cg_thirdPersonAngle`; the roll is left as it was (0).

With the third-person flag the view weapon is not drawn and the local body is (0x4d63b3, 0x4dbbc1); since the dead
client's own entity is not playing, what is seen is the corpse from behind. The view angles do not change while dead
(the command's angles are not applied), so the pitch is the one of the death. It stays
until the gametype respawns the client (`dm.gsc:510-557`). The killcam that some gametypes show first is
docs/spec/mp-hud.md "Killcam".

## Intermission

`endMap` (`dm.gsc:598-684`, `tdm.gsc:711`, ...) sets `game["state"]` to intermission, notifies `intermission`, picks the
winner and puts every client in session state intermission through `spawnIntermission` (`dm.gsc:485-508`): the
client is placed at a random `mp_global_intermission` spawn and looks with its angles. The winner text is the script's:
it sets `cg_objectiveText` on each client, so the scoreboard shows it above the list. With movement type 5 the client
does not move; the 2D pass draws only the scoreboard. The view input is ignored as well: the mouse does not turn the view, which keeps the spawn's angles
(a network client takes them from the server's player state; TODO(spec): the MP executable's check, SP 0x50b12b skips the
command angles for the dead types the same way). After the script's delay `exitLevel` goes to the next map
(docs/spec/mp.md).
