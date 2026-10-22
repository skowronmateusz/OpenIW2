# Animation trees and blending

## Data (observed in the shipped files)

`animtrees/<name>.atr` is text. Each name is a node; `{ … }` holds a node's children; `// …` comments;
`: complete`, `: loopsync` and `: nonloopsync` modifiers follow a name. Leaves are names of `xanim/` files.
Scripts select a tree with `#using_animtree("name")` / `useanimtree(#animtree)` and refer to nodes as `%name`.
Every tree has an implicit `root` node (scripts use `%root`).

## Script interface (from usage and comments in the shipped animscripts)

- `setanim(anim, goalWeight, goalTime, rate)`, plus `knob` variants that also send the node's siblings to weight 0,
  `knoball(anim, rootAnim, …)` that does the same for every ancestor up to `rootAnim`, and `restart` variants
  that reset the node's time.
- `setflagged…(notifyName, …)`: the entity is notified `notifyName` with each notetrack name as it passes, and
  with `"end"` when a non-looping animation finishes. Scripts wait with `waittillmatch(notifyName, "end")`.
- `clearanim(anim, goalTime)`, `getanimtime(anim)`, `getanimlength(anim)`, `animhasnotetrack(anim, note)`.

## Blending

Specified from the executable in `docs/spec/xanim.md` ("Blending", "Weights and time"). `AnimState::moveSpeed`
still uses an inferred effective weight (a node's weight over its siblings' total, multiplied up the tree).
AI use the `generic_human` tree.

## Engine animscripts (placeholder)

After `init` ends, the actor runs `animscripts/stop::main()`. Before each switch the engine notifies
`"killanimscript"`. The real state machine (move, combat, cover, pain, death) is still to be specified.

## Animation start placement

`getstartorigin(origin, angles, anim)` (0x53be50) and `getstartangles(origin, angles, anim)` (0x53bf80) take the
animation's delta at time 0 (0x478740 → 0x475aa0, weight 1; an animation without delta gives yaw quaternion (0, 1)
and no translation). The origin is `origin + forward·t.x + left·t.y + up·t.z` in the axes of `angles`. The angles
come from the delta yaw `atan2(2zw, 1 − 2z²)` (normalized by z² + w², 0x423900) turned into an axis (0x424b20) and
expressed in the axes of `angles` (0x422450), then back to angles (0x424b70).
