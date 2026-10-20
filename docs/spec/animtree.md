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

## OpenIW2 blending (implemented; inferred until checked against the executable)

Defaults are weight 1, blend time 0.2 s and rate 1. Weights move linearly to their goal over the blend time.
A node's effective weight is the product, up the tree, of its weight divided by its siblings' total (or 1 if the
total is below 1). Leaf poses are blended with weighted normalized quaternions; any remaining weight falls back to
the model's base pose. AI use the `generic_human` tree.

## Engine animscripts (placeholder)

After `init` ends, the actor runs `animscripts/stop::main()`. Before each switch the engine notifies
`"killanimscript"`. The real state machine (move, combat, cover, pain, death) is still to be specified.
