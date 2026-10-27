# Cinematics (video/*.roq)

Everything here is read from CoD2SP_s.exe and the shipped files; guesses are marked TODO(spec).

## File format (RoQ)
- Movies are `video/<name>.roq` in the iwds (iw_06, localized iwds for the subtitled ones). Header: `0x1084`, `0xFFFFFFFF`, fps u16. Then chunks: id u16, size u32, argument u16, body.
- `0x1001` info (width, height, 8, 4); `0x1002` codebook (argument high byte = 2x2 cells, low byte = 4x4 vectors; a cell is 4 luma + U + V); `0x1011` VQ frame (argument = motion offset; 2-bit codes 0 unchanged, 1 motion, 2 vector, 3 split, most significant bits first); `0x1020` / `0x1021` DPCM sound (22050 Hz, mono / stereo, signed square steps from a predictor in the argument); `0x1030` signature (ignored).
- Decoder: `src/video/roq.{h,cpp}` (clean-room, test `tests/test_roq.cpp`); `iw2roq <main> <movie> info|frames <dir> <n>...` inspects / dumps frames (atvi and iw_logo are 512x512 @ 30 fps, stereo).

## How the game plays them
- Script builtin `cinematic(name [, timeout])` (table 0x5a622c, function 0x4f4220) only records the name (63 characters); a negative timeout is a script error.
- The movie plays when the level change falls due: the level-change path runs the console command `cinematic <name> 0` (0x4d9f90); when it ends the level load continues (0x49db20 -> 0x4d9f00).
- Any key or mouse button skips it while the skippable byte 0x59a0d0 is 1 (`unskippablecinematic` clears it). Flags of the command: '1' system(5), '2' hold (|2), '3' letterbox (|0x20; rect y 0x69 height 0x10e, otherwise 640x480).
- Scripts: `_briefing.gsc` (gotothelevel), `movie_*.gsc` maps, `_endmission.gsc` (movie_victory), and the start-up chain (atvi, iw_logo, cod_intro) of the front end.

## openiw2
- VM: `cinematic` stores the name (`World::takeCinematic()`); before the next map is loaded iw2view plays it (modal, 640x480 area centred, any key / mouse button skips, audio through its own 22050 Hz stream, then the load continues). Without a window (`--shot`) or with `--no-audio` the movie is skipped / silent.
- Debug: env `IW2_PLAY_CINEMATIC=<name>` plays `video/<name>.roq` once before the level starts (docs/debug-flags.md).
- Not done: letterbox / hold flags, `unskippablecinematic`, the timeout argument (0x1c60fec), the front-end start-up movie chain, the subtitle track of localized movies (TODO(spec)).
