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
