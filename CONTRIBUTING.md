# Contributing

Thanks for your interest. OpenIW2 is a clean-room reimplementation, and the rules below keep it that way. A pull
request that breaks them cannot be merged.

## The rules

1. **No game files.** No game data (images, sounds, maps, models, scripts, menus, localized text, shaders), no
   executables or DLLs, no screenshots with the game's logos.
2. **No copied code.** No disassembly, no decompiler output (even rewritten line by line), no quoted shader or
   script code. Describe behavior in your own words.
3. **Spec first.** A new behavior gets a section in [`docs/spec/`](docs/spec) that describes it and cites where
   it was observed: an address in the patch 1.3 executable or DLL, or a shipped file by `file:line`. See
   [docs/reverse-engineering.md](docs/reverse-engineering.md). Mark guesses `TODO(spec)`.
4. **Code from the spec.** Implement from the spec, not from the binary.
5. **Tests.** Add or extend a test in `tests/`. `ctest` must pass.
6. **Your own work.** By submitting, you confirm that you wrote the contribution, or that you have the right to
   submit it, under the project licence (GPL-2.0-or-later).

## Practicalities

- Build and test: see the [README](README.md#build-and-test). Tests that need game data use `-DIW2_TEST_MAIN=...`
  or the `IW2_MAIN` environment variable.
- Keep commits focused, with a one-line summary that says what changed and why.
- Match the style of the surrounding code: C++20, the existing naming, comments that cite spec sections or addresses.

## Issues

Bug reports are welcome: say which map, what happened, what you expected, and your macOS version. Requests for game
files, or for help obtaining them, will be closed.
