# Sound

## Files

Shipped sounds are `sound/**.wav` (20,188 files, all RIFF WAVE with 16-bit PCM; 19 are stereo) plus 71 `.mp3`
files (music and long ambient tracks, e.g. `sound/ambient/amb_africa01b.mp3`: MPEG layer 3, 44,100 Hz stereo,
1152 frames per packet, 255.9 s). Checked by scanning every `.wav` in the packs.

### Decoding

- `.wav`: parsed directly (`parseWav`).
- `.mp3`: decoded with the system AudioToolbox framework (`AudioFileOpenWithCallbacks` over the in-memory file +
  `ExtAudioFile` converting to interleaved signed 16-bit PCM at the file's own rate and channel count;
  `src/audio/decode_apple.cpp`). The decoder is chosen by the file extension (`decodeSound`).
- `iw2asset <main> sound-check` decodes every `.wav` and `.mp3` under `sound/`: 20,190 `.wav` and 71 `.mp3`
  decode without errors. `iw2asset <main> sound-info <path>` prints rate, channels and frame count; for the file
  above it reports 11,286,144 frames at 44,100 Hz, matching `afinfo`'s 255.92 s.
- A whole track is decoded on first play and cached as PCM (a 4-minute stereo track is about 45 MB). Streaming is
  not implemented yet.

## Sound aliases (`soundaliases/*.csv`)

CSV text. Rows whose first field is empty or starts with `#` are ignored. The first remaining row names the
columns (`name,sequence,file,vol_min,vol_max,vol_mod,pitch_min,pitch_max,dist_min,dist_max,channel,type,
probability,loop,masterslave,loadspec,subtitle,compression,secondaryaliasname,volumefalloffcurve,startdelay,
speakermap,reverb,lfe percentage`), and columns are found by that name. Several rows may share a name; one of them
is picked at random when the alias plays. `file` is relative to `sound/`.

`loadspec` limits a row to levels: `!a !b` excludes levels `a` and `b`; a plain list includes only the listed
levels; empty means every level.

## Playback (implemented in `src/audio/sound.cpp`; inferred)

- Volume and pitch are drawn uniformly from their min..max ranges.
- `channel local` plays without position. Other sounds fall off linearly from full volume at `dist_min` to
  silence at `dist_max` (10 × `dist_min` when `dist_max` is empty). The `.vfcurve` fall-off curves are not used yet.
- Panning is constant-power by the side component of the direction to the listener.
- `loop looping` repeats the sound until stopped; `secondaryaliasname` starts a second alias with the first.

## Script built-ins

`playsound(alias [, notify])` plays at the entity (without position on the player); with a notify name the
entity gets that notify when the sound ends. Also `playsoundasmaster`, `playlocalsound`, `playloopsound`,
`stoploopsound`, `ambientplay`, `ambientstop`, `musicplay`, `musicstop`. Weapon shots play the weapon's
`fireSoundPlayer` (player) or `fireSound` (others); reloading and rechambering play `reloadSound` and
`rechamberSound`.

## Music, ambient and loadspec (audio survey, exe evidence)

- `musicplay(alias)` (0x4f3400): unknown alias raises "unknown sound alias '%s'"; otherwise the server sends the client command
  `mu_play <alias>` (0x4a6e2b -> 0x443030: starts the alias in the dedicated music slot, replacing the previous one).
- `musicstop([fade])` (0x4f3470): fade seconds * 1000 rounded (+0.5), negative is an error; sends `mu_stop <ms>`
  (0x4a6e60 -> 0x442fb0): ms 0 stops the music slot at once, otherwise its volume falls linearly to zero over ms.
- `ambientplay(alias [, fade])` (0x4f3800) / `ambientstop([fade])` (0x4f3930): argument validation as in the exe; the state goes
  out as configstring 3 (`n\<alias>\t\<levelTime + fade ms>` / `t\<levelTime + fade ms>`). TODO(spec): the client-side
  consumer of configstring 3 (ambient fade-in/out) is not decoded; the engine starts/stops the loop at once.
- `snd_fade <vol> <ms>` is emitted by 0x4f3520 (soundfade-like builtin, format "snd_fade %f %i\n").
- `setsoundblend` and `setreverb` are not called by any shipped script (survey of all .gsc), so their client side stays TODO(spec).
- Alias loadspec (0x428f40): a row whose loadspec equals the mode tag `all_sp` loads in every single-player level
  (the fallback compare of the whole spec with the tag); "menu" and "!list" cases follow the same function.
  Effect: bullet_small_flesh (and every other `all_sp` row) was previously dropped.

## Round 2: ambient, soundfade, volumes, dvars (exe evidence)

- Slot layout of the client sound code (0x442df0 play-into-slot, 0x442fb0 stop/fade-out, table 0x793ce0 current/rate per slot):
  music is slot 0 (`mu_play` / `mu_stop`); ambient uses slots 1/2 alternately (0x443080, `3 - current`). A new ambient alias
  fades the old slot out and itself in over the fade time (rate = volume / ms); the same alias keeps playing. The engine models
  configstring 3 as the client commands `ambient_play <alias> <fade ms>` / `ambient_stop <fade ms>`.
- `soundfade(volume [, seconds])` (0x4f3520) sends `snd_fade <volume> <ms>` (ms = int(seconds * 1000), 0 without the argument).
- Ambient / music aliases must be `streamed` and must not use a 3D channel type (load-time errors at 0x442e47 / 0x442e72).
- Master volume = `snd_volume` (float, default 1.0, range 0..1, archived, 0x4449d6) x `snd_fade` volume x 0.75 (0x443ac4).
  The engine's former default 0.8 was replaced by 0.75.
- `vol_mod` column: group volumes come from `soundaliases/volumemodgroups.def`; the post-parse check (0x429783) multiplies
  `vol_min` and `vol_max` by the group volume and clamps to 0..1; an unknown group gives 0 (0x428c2c). Implemented.
- `dist_max` 0 means 5 x `dist_min` (0x429701); the engine used 10 x (guess) before.
- Registered dvars (0x444900): `snd_khz` (default 44, 0x1021), `snd_bits` (16), `snd_stereo` (1), `snd_volume`,
  `snd_slaveFadeTime` (int, default 500, 0..5000, 0x1081), `snd_enable2D/3D/Stream/Reverb` (default 1, 0x1080),
  `snd_touchStreamFilesOnLoad` (0). `snd_channelvolprio_shellshock/pain/holdbreath` and `snd_enveffectsprio_level/shellshock`
  are priority names (0x506f61..0x506fbc) accepted by `snd_setEnvironmentEffects` (0x444be9) and the script `setreverb`.
- `masterslave` column: `master` sets alias flag 0x2, a number in 0..1 sets slave flag 0x4 with that percentage (+0x134,
  error text "SlavePercentage"). 3141 rows are masters (dialogue); 0.7 / 0.5 slaves are ducked while a master plays, fading
  over `snd_slaveFadeTime` ms (see Round 3).
- Occlusion: the exe imports no Miles occlusion/obstruction call (mss32 imports checked: only `AIL_set_3D_sample_effects_level`, `set_*_reverb_levels`, `set_*_room_type`) and the voice path has no geometry trace, so there is no occlusion to port (evidence by absence). See Round 5 for reverb.
- `speakermap` / `lfe percentage` / voice priority: see Round 7.

## Round 3: ducking, falloff curves

- Duck state (0x793cfc, updated by 0x443bb0 with the elapsed ms): rises by dt / `snd_slaveFadeTime` while any master voice plays,
  falls by the same rate otherwise, clamped 0..1; with `snd_slaveFadeTime` 0 it jumps to 1 or 0. A voice of an alias with
  slave flag 0x4 that is not itself a master is scaled by `1 - duck * (1 - slavePct)` (0x4a0cf0 and the start path 0x4a0ef0).
  Implemented in `Mixer::mix`.
- Distance gain (0x441590, called from 0x49f0b4, 0x49f7da and the blend voices at 0x4a0e8b): `t = dist - dist_min`; `t <= 0` gives 1;
  `dist_min == dist_max` gives 0; `t /= dist_max - dist_min`; `t >= 1` gives 0; otherwise the alias curve is evaluated at t
  (0x420b40: first knot i >= 1 with `t <= x[i]`, linear interpolation between knot i-1 and i). The result multiplies the voice volume.
  Implemented as `Mixer::falloff`.
- `soundaliases/<name>.vfcurve` (parser 0x42a9f0 / 0x42a6e0, 15 curves max): `SNDCURVE`, knot count 2..8, then `x y` pairs, x in 0..1.
  The first knot is forced to (0,1) and the last to (1,0) (the exe prints a warning). The alias `volumefalloffcurve` column names the
  file; an empty column keeps the default curve (0,1)-(1,0) installed at 0x428d22 (alias +0x130, runtime +0x40). Blend voices
  use the curve of the low alias (0x4a0e85).
- Pan law (0x49f000, level setter 0x49ff80 -> backend `set levels(handle, left, right)`): `a` = volume x falloff. A stereo wave
  (channel count 0x14 of the sound == 2) plays `(a, a)`. A mono wave plays `(a (1-p), a p)` with `p = (1 - dot) / 2`, `dot` = unit
  direction to the source (0x4210d0, zero vector stays zero) . listener axis[1] (the left axis, 0x793d94); a mono wave without a
  position uses `p = 0.5` (0x49f5b9 / 0x4a0d22), so a centred mono voice is half as loud per channel. Implemented in `Mixer::gains`.
- Reverb/environment effects: see Round 5.
- `playloopsound` / `stoploopsound` store the handle in the entity as before; the exe-side loop entity bookkeeping was not re-verified.

## Round 4: loop sounds, reverb (exe evidence)

- `playloopsound(alias)` (0x4ef3f0) only writes the alias' configstring index (type 0x11d, 256 entries) into the entity state (ent+6) and
  clears flag bit 0 of ent+0xa2; an unknown alias prints "unknown sound alias '%s'" (0x587118). `stoploopsound` (0x4ef4c0) writes 0.
  The client plays one loop per entity, so the same alias again does not restart it (the engine now matches this; before, it
  restarted the loop).
- `setreverb(priority, roomtype [, dry [, wet [, fade]]])` (0x539380; 2..5 arguments, "Incorrect number of parameters" otherwise;
  defaults dry 1.0, wet 0.5, fade 0): the priority must be `snd_enveffectsprio_level` (1) or `snd_enveffectsprio_shellshock` (2),
  otherwise "priority must be ...". It sends the client command `reverb %i "%s" %g %g %g` (priority, room, dry, wet, fade).
  The client-side `snd_setEnvironmentEffects <priority> <roomstring> <dry> <wet> <fade>` (0x441260, exactly 6 tokens) checks dry
  and wet against 0..1 and the fade as an int, then hands them to the sound backend (0x443ec0); see Round 5.
- Occlusion is not present in the exe (see Round 2).

## Round 5: environment effects (reverb) state machine (exe evidence)

- Server: `setreverb` sends `reverb %i "%s" %g %g %g` (priority 1 = `snd_enveffectsprio_level`, 2 = `..._shellshock`; room, dry default 1.0,
  wet default 0.5, fade seconds default 0; args order in the command is prio, room, dry, wet, fade; 0x539380). `deactivatereverb(prio [, fade])`
  (0x539560, 1..2 args) sends `deactivatereverb %i %g`. Both are player-only. (The earlier comment "dry 0.5, wet 1.0" was a misreading.)
- Client `reverb` -> CG_ReverbCmd 0x4a6350: prio = atoi, dry/wet = atof, fade ms = max(0, round(fade s x 1000)) -> 0x443ec0.
  `deactivatereverb` -> 0x4a6500 -> 0x443fd0.
- Room table (0x5a02e8, index = Miles room type): generic, paddedcell, room, bathroom, livingroom, stoneroom, auditorium, concerthall, cave,
  arena, hangar, carpetedhallway, hallway, stonecorridor, alley, forest, city, mountains, quarry, plain, parkinglot, sewerpipe, underwater,
  drugged, dizzy, psychotic (26 = the EAX presets). Unknown name: error listing the names (0x4411e0).
- State: 3 slots of 0x20 bytes at 0x793d00 (room, cur dry, tgt dry, dry rate, cur wet, tgt wet, wet rate, on); slot 0 is the default
  (generic, dry 1, wet 0, on, 0x444aa4); `0x793d60` points at the active slot. Activate (0x443ec0): the slot stores room and targets, its current
  values continue from the active slot's, rates = (target - cur) / max(fade ms, 1). If the slot is the active one the room type is re-applied
  (0x49fe90); else it becomes active only when no higher slot is on. Deactivate (0x443fd0): clears `on`; if it was the active slot, the next lower
  `on` slot becomes active, inheriting the old current values and ramping to its own targets over the fade. Per frame (0x444290..): cur += dt x rate,
  clamped at the target (rate then 0).
- Application: room type -> `AIL_set_digital_master_room_type` + `AIL_set_3D_room_type`; wet -> `AIL_set_digital_master_reverb_levels(drv, 1.0, wet)`
  and `AIL_set_sample_reverb_levels(handle, 1.0, wet)` for every playing sample/stream. A voice gets wet 0 when `snd_enableReverb` is 0 or its alias has
  flag 0x10 (`reverb` csv column contains `nowetlevel`, parser 0x429130; `fulldrylevel` sets flag 0x8). The dry level of a slot is tracked but
  every consumer reads only the wet level (and Miles' dry arg is the constant 1.0).
- Implemented: `audio/reverb.h` (state machine, tested in tests/test_sound.cpp), `Mixer::reverbCommand/deactivateReverbCommand/voiceWetLevel`,
  alias `noWetLevel`/`fullDryLevel`, iw2view `reverb`/`deactivatereverb` client commands. TODO(spec): the reverb DSP itself is inside mss32.dll
  (EAX/software presets), so the mixer does not render a reverb tail; `fulldrylevel` (flag 0x8) has no consumer found; no shipped script calls
  `setreverb`, so nothing is audible either way.

## Round 6: audit (exe evidence)

- `snd_fade` per-frame step: 0x4437a0 receives the elapsed ms as an int (`fildl 4(%esp)`) and the master fade does `vol += dt * rate` (0x443a21), clamps
  at the target (0x443a47..0x443a76, rate cleared); same dt as the ambient slots and reverb ramps. Implemented: `Mixer::mix` calls `fadeStep`
  (it was never stepped before, so `soundfade` / the mission-end `snd_fade 0` never changed the volume).
- Still TODO(spec): looping-alias lifetime (stamp refresh 0x441f80 / sweep 0x443550), `speakermap`, `lfe percentage`,
  per-channel voice priority and the sample slot limits (0x4423d0), the Miles reverb DSP.

## Round 7: slots, priority, loops, columns (exe evidence)

- Alias columns (name table 0x59b2b4, 1-based; parser switch 0x4291c0 -> 0x4295cc): `startdelay` (col 20) is atoi -> alias +0x3c, passed to the 2D / 3D /
  stream start (0x441d79, 0x441dcc, 0x441f28): with a zero start offset the sample is not started now but after the delay (0x49f685..0x49f691);
  the slot end time includes it. `lfe percentage` (col 23) is clamped 0..1 -> alias +0x38 and has no reader anywhere in the sound code (evidence by absence).
  `speakermap` (col 21) is handled by an empty case (0x4295c3): parsed and ignored. `type` (0x428e70): "streamed" (also the string at 0x5757e8) = 2, "loaded" = 1
  (default); 20,909 shipped rows are `streamed` (nearly all dialogue).
- Slot pools (0x49eb80, 0x49ecec): slot indices 0..31 are 2D samples (count from the Miles driver, capped at 32), 32..44 are 13 streams (32..34 are the
  dedicated music / ambient slots of 0x442df0), 45..52 are 8 3D samples. A play picks the pool by `type` (streamed -> stream pool, 0x441e10) and else by channel
  (0x441d1c, table bytes at 0x441dfc): auto, weapon, voice, item, body use 3D (0x49f740, snd_enable3D); auto2d, menu, local, music, announcer, shellshock use 2D (0x49f470).
- Channel numbers (0x59b334): auto 0, auto2d 1, menu 2, weapon 3, voice 4, item 5, body 6, local 7, music 8, announcer 9, shellshock 10. A slot stores owner (+0:
  entity number, 0x7ff / 0x7fe for world sounds), channel (+4, 0x4417d1), loop stamp (+0xc), end time (+0x10).
- Voice priority = the channel number. Free slot first (0x441990 / 0x441a30 / 0x441ad0); else the steal choice 0x441910(base, count, owner, priority): candidates are slots with
  priority <= the new sound's (the stream window is 10 slots: 0x441e32 uses base 0x23, count 13 - 3); a slot of the new sound's own owner wins over another owner's
  (and an owner-matching best is kept); otherwise the lowest priority, then the earlier end time (a loop's end is 0, 0x49f6db). No candidate: the play fails (handle -1 -> 0).
- Generic play 0x4423d0 order: secondary alias first (recursion, 0x442401..0x44243e; an error if the secondary has one itself, 0x578df8); 3D channels are culled when
  dist^2 > dist_max^2 (0x4424e6; 2D channels are not); loop refresh 0x441f80 (a running loop slot with the same owner, alias and second alias name is updated and its stamp
  set to 0x793db8, the call returns 0); for channels other than auto / auto2d / menu and an alias WITHOUT a secondary, 0x441b50 stops the owner's running sounds of the
  same channel in all three pools; then volume/pitch draw (0x442310) and the start.
- Loop lifetime: the per-frame sweep 0x443510 (called from 0x42edfe; skipped while paused, 0x443522) stops every looping slot whose stamp (+0xc) differs from 0x793db8, then
  sets 0x793db8 = 0x793db4 (the sound time in ms). So a loop started through the generic play lives only while its caller keeps re-playing it each frame. Slot stamps start
  as 0x793db8 (0x44182b). Entity loops (`playloopsound`) store a configstring index in the entity state (Round 4) and the client replays them each frame.
  Implemented in `Mixer` as an opt-in: `PlayOptions{owner, dedicated, refresh}` + `sweepLoops(soundTimeMs)`; the port's script/turret loops keep their handle instead.
  The entity-loop re-player is 0x4aaf90 (see "Channel volume slots" below). TODO(spec): the owners passed by the other ~30 callers of 0x4423d0; the port's own callers only
  pass `dedicated` for music / ambient, so pools and exclusivity are exercised by tests, and by live play only through the pool caps and distance culling.

## Channel volume slots (0x793a34 / 0x793cdc) and the entity-loop re-player

- Table 0x793a34: 5 slots of 0x88 bytes. Per channel c a `{cur, target, step}` float triple at c*12; an active flag at +0x84.
  0x793cdc is a pointer to the highest active slot. Setter (fade) 0x4431f0, clear 0x443390, per-frame step 0x4437a0.
- Application: `master(0x793a24) * slot[ch].cur * voice volume`, with `ch = (alias +0x2c >> 7) & 0xf`. Sites: 0x49f574, 0x49f7ed,
  0x49fb8d, 0x49fdff, 0x4a0d10, 0x4a0eed, 0x4a106c.
- Channel order in the exe (shock volumes and registrations at 0x4a22a2..): auto, auto2d, menu, weapon, voice, item, body, local,
  music, announcer, shellshock; defaults auto .1, auto2d .1, menu 1, weapon .5, voice .2, item .1, body .1, the rest 1. The port keeps a
  name-indexed order (`kShockChannelNames`, `channelIndex`); behaviour is identical because lookups are by name.
- 0x4423d0 (generic play) takes 9 stack args: alias, alias2, frac, owner entity, origin ptr, out handle ptr, a7, a8, a9. It does the
  secondary alias, distance cull, loop refresh (0x441f80), exclusivity stop (0x441b50), volume/pitch draw (0x442310) and start (0x441e10).
- Entity-loop re-player: 0x4aaf90, called per entity every frame from the dispatcher 0x4acf30 (eType at cent +0xa0, jump table
  0x4acfb0). It reads cent word +0xa6 (loopSound configstring index), resolves it via 0x62072a / 0x8aed00 (+4 + idx*8), looks up
  (0x428320) and picks (0x428a20) the alias, then 0x4a3610 (play at an origin, owner = word +0xa8, arg9 = 1, sound length through
  0x4a3540) -> 0x4423d0. Origin = cent +0x148, or, when dword +0x120 == 0xffffff, +0x148 plus the brush model centre
  0xf584a0[word +0xae * 12]. ET_SOUND_BLEND (eType 6) is 0x4ac8a0 via 0x442900 (two aliases at cent +0x12c / +0x12e).
- Not decoded (TODO(spec)): the other callers' owners; brass-delay gating (the bolt-action brass is in weapons.md) and the usercmd
  byte +0xd compared with +0x3d in the binocular cancel (0x4dfa8a).

## Channel volume slots and level changes

The sound system keeps five channel-volume slots (set 0x4431f0, clear 0x443390); the highest active slot applies. The stop-all routine 0x444730 clears slots 1..4 (unless its flag 0x10 is set) and runs on level load and disconnect, so the level-end `setchannelvol` (slot 1, victoryscreen.shock) never reaches the next level. The port keeps these in `audio::ChannelVolumeSlots`. VAs in the patch 1.3 SP executable.
