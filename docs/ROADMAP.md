# Roadmap

Each milestone ships a runnable tool or demo. Every format and behavior gets a spec in `docs/spec/`
backed by evidence first, then an implementation with tests.

| # | Milestone | Done when |
|---|---|---|
| M0 | Asset filesystem (`.iwd` ZIP packs, search order, case-insensitive paths) | `iw2fs` lists and reads every file of a retail install ✅ |
| M1 | Asset decoders: `.iwi` textures, `.d3dbsp` maps, materials/techsets, `xmodel*`, `xanim` | `iw2asset` converts each type to an open format (PNG, glTF) for every shipped file |
| M2 | Renderer + map viewer: SDL3 window, Metal via SDL_GPU, MSL shaders reimplemented from the behaviour of the shipped HLSL shaders (own code, not a line-by-line translation) | Fly through any single-player map with lightmaps at 60+ fps |
| M3 | Models and animation | Characters and weapons animate in the viewer |
| M4 | GSC script compiler + VM, with the built-in function table | `maps/*.gsc` compile; level scripts run headless |
| M5 | Game simulation: entities, collision, player movement, weapons | Walk and shoot through a map |
| M6 | AI: pathnodes, animscripts (GSC), squads | Enemies fight back as in the original |
| M7 | Audio (wav/mp3, sound aliases), UI menus (`.menu`), cinematics (`.roq`) | Main menu → mission start works |
| M8 | Save/load, objectives, the full campaign | The campaign can be finished |

The original assets are mostly in readable formats: scripts (`.gsc`), shaders (`.hlsl`, `.tech`, `.techset`),
menus (`.menu`), tables (`.csv`). That makes M4–M7 far more tractable than a pure binary port.
