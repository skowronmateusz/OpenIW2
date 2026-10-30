# Reverse engineering: tools and method

The behavior of OpenIW2 comes from studying the original game: its single-player executable and renderer DLL from
**patch 1.3**, plus the shipped data files. What I learned is written down in [`spec/`](spec) in my own words, with the
addresses where each rule was observed. This page lists the tools and the conventions, so that you can check a claim
or extend a spec with your own, legally obtained copy of the game.

This repository contains no binaries, disassembly, decompiled code or game files. Keep everything your tools produce
**outside your fork** (see [What never goes into the repository](#what-never-goes-into-the-repository)).

## Tools

- **[REA: reverse-engineer-anything](https://github.com/morluto/rea)** (npm package `rea-agents`, version 6.7.0,
  MIT): an MCP server and agent skill for static and runtime analysis. It drives **Ghidra 12.1.4** for analysis and
  decompilation. Setup for Claude Code:

  ```bash
  claude mcp add rea -- npx -y rea-agents@6.7.0 mcp
  ```

- **[LEANN](https://github.com/yichuan-w/LEANN)** (`leann-core` 0.3.8, MIT): a local semantic index. I index the specs,
  so agents search before they read whole files. It is configured with the `ivf` backend
  and the `sentence-transformers/all-MiniLM-L6-v2` embeddings.

  ```bash
  uv tool install leann-core
  claude mcp add leann-server -- leann_mcp
  ```

Thanks to the authors of both tools.

## Method

1. **Two passes per feature.**
   - **Analysis:** study the original and write the behavior into `docs/spec/<topic>.md` in your own words, with
     the addresses it was observed at.
   - **Implementation:** write code from the spec only, without the binary open.

   The two passes are never mixed.
2. **One function at a time.** Ask REA about a single function (pseudocode, instructions, cross-references) instead
   of decompiling whole groups. It is cheaper, and the analysis stays focused.
3. **Evidence first.** Every rule cites where it was observed. Anything inferred is marked `TODO(spec)` until it is
   confirmed.
4. **Tests for every behavior** (`tests/`), plus the headless bot `iw2game --autoplay`, which plays the mission
   scripts and reports where they stall.

## Conventions

- Addresses are lowercase hexadecimal virtual addresses in the patch 1.3 single-player executable (image base
  0x400000) or in the renderer DLL (image base 0x10000000), e.g. `0x4ddb40`.
- `FUN_xxxxxxxx` and `DAT_xxxxxxxx` are Ghidra's automatic labels. They are used only as address references.
- Function names such as `PM_Weapon` are labels I chose. Many follow the naming of the GPL-licensed Quake III Arena
  source, the engine family CoD2 descends from.
- Shipped scripts are cited as `file.gsc:line`. They are not quoted.

## What never goes into the repository

- Game files: `.iwd` packs, images, sounds, maps, scripts, localized text, shaders, executables and DLLs.
- Disassembly listings, decompiler output (even annotated or shortened), memory or string dumps.
- Analysis databases and evidence files from REA or Ghidra, and LEANN indexes built over them.
- Verbatim text from the game's scripts, menus or shaders. Describe it instead.
