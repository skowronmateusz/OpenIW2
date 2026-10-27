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
- TODO(spec): occlusion, reverb mixing, `speakermap`, per-channel voice priority; `lfe percentage`.

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
- TODO(spec): `snd_setEnvironmentEffects` /
  `snd_deactivateEnvironmentEffects` handlers (0x441260 / 0x4413d0), `setreverb` (0x539380): reverb mixing and occlusion are not
  decoded; no shipped script calls `setreverb` or `setsoundblend`.
- `playloopsound` / `stoploopsound` store the handle in the entity as before; the exe-side loop entity bookkeeping was not re-verified.
