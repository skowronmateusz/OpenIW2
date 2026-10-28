# Animation trees and blending

## Data (observed in the shipped files)

`animtrees/<name>.atr` is text. Each name is a node; `{ … }` holds a node's children; `// …` and `/* … */` comments;
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

Specified from the executable in `docs/spec/xanim.md` ("Blending", "Weights and time", "Anim delta").
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

## Child order

The executable lists a node's children in the reverse of their order in the .atr file. The parser inserts each child
at the head of its parent's script-variable list (0x4527fd..0x452841, the same head insertion as 0x4643a0) and the
tree builder 0x4530c0 numbers the children walking from that head (blend node entry: 0x475090 writes the child count at
+0xc and the first child at +0x12; leaves 0x475000). Behavioural check: `standMG42gunner_aim_15down / _level / _15up`
have delta heights −32.9 / −40.7 / −48.4, ascending only in the reversed order the turret pose step (0x549ba0) probes.
OpenIW2 keeps the file order in `AnimTree` (node indices are part of the savegame) and only the order-sensitive consumer,
`World::turretPoseStep`, maps child j to `children[count − 1 − j]`. TODO(spec): flip the parser if another
order-sensitive consumer appears.
