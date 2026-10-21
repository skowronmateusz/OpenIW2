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
