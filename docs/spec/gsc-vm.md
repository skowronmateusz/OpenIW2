# GSC virtual machine

This describes the behaviour OpenIW2 implements. Items marked *inferred* come from how the shipped scripts use
the language, not from analysis of the original VM. They still have to be checked against the executable.

## Values

`undefined`, int (32-bit), float, string, localized string, vector (3 floats), object, array, function pointer,
animation reference, animation tree.

- Objects (`level`, `game`, `anim`, `spawnstruct()` results, entities) are reference types.
- Arrays have value semantics (copy on write). Keys are ints or strings; `.size` is the element count.
  Assigning `undefined` removes an element. Indexing `undefined` in an assignment creates a new array
  (*inferred*, from `level.strings["x"] = …` on a fresh field).
- `obj["key"]` on an object reads or writes the field `key` (used as `game["allies"] = …`).
- Identifiers, field names and function names are case-insensitive (*inferred*: the scripts mix `triggerOff` and
  `triggeroff`).

## Operators

- `/` always produces a float (`131/255` is used as a colour component in `toujane.gsc`). `%` is an integer remainder.
- `+` concatenates when either side is a string.
- Vectors support `+ -`, multiplication and division by a scalar, and `v[0..2]`.
- `&&` and `||` short-circuit and produce 0 or 1.

## Threads

- `f()` runs to completion in the caller's thread. `thread f()` creates a thread that starts running immediately
  until it first suspends; then the caller continues. The thread call's value is the thread's return value when
  the thread ends before suspending, otherwise undefined (VM 0x4690c0: the new thread's frame base is the caller's
  stack top; end/return writes the slot above it and the caller resumes with that slot pushed).
  `_fx.gsc` relies on it: `ent = level thread createfx_showOrigin(...)`.
- `wait s` suspends for `s` seconds of game time. `waittillframeend` resumes after the other threads of the current
  frame.
- `obj waittill("e", a, b)` suspends until `obj notify("e", x, y)`; the notify arguments are stored in `a`, `b`.
  `waittillmatch("e", m)` also requires the first notify argument to equal `m`.
- `obj endon("e")` ends the function call that registered it when `obj` is notified with `e`, together with the
  calls it made; in the thread's first function that ends the thread, otherwise the caller continues as if the call
  returned undefined. A call's endons are dropped when it returns (*inferred*: `animscripts/shared.gsc`
  `DoNoteTracksForTime` ends a looping notetrack wait with `ent endon("stop_notetracks")` and `run.gsc` keeps going
  after it). Threads are ended before the waiters of the same notify run (*inferred* ordering).
- A runtime error stops only the failing thread and is reported. The original prints a script runtime error and,
  in developer mode, halts.

- A script function called without an object (`f()`, `thread f()`, `path::f()`) runs with the caller's `self`
  (*inferred*: `aitype/*.gsc` calls `character\x::main()`, which then sets fields on `self`).

## Name resolution

An unqualified call looks in the current file, then in the files named by `#include` (not transitively), then
in the engine built-ins. `path\file::f` loads that file on demand. `::f` resolves like an unqualified script
function.

## Implementation

`src/script/vm.cpp`: the compiler turns the syntax tree into stack bytecode per function; the interpreter runs
threads cooperatively. All 621 valid shipped scripts compile (`iw2script <main> compile`).

## Notify queueing, `wait 0`, frame lists (exe evidence)
- `notify` does not run the waiters. Script variable lists: 0x4643a0 links a new child at the head (parent +0xe) and 0x4643f0 at the
  tail; the first entry's +2 leads to a sentinel whose +2 is the last entry, so the idiom "+0xe, +2, +2, +0" (0x46cb08..0x46cb26) gives
  the entry before a variable, or the last child when applied to the parent.
  VM_Notify (0x46ca60, from Scr_NotifyNum 0x46d0f0) uses that idiom: it starts at the event's list, so it takes the LAST entry, the
  oldest registration (a waittill is linked at the head: 0x46afb3..0x46afdb -> 0x465730 -> 0x4643a0). After every handled entry it
  restarts from the list; a waittillmatch whose arguments do not match stays and the walk moves on to the entry before it. Endon
  entries (type 0) are handled in the same pass (0x46ccc4). A thread's endon is older than its waittill, so handling endons first
  gives the same result.
  Each woken thread is filed under the current frame counter 0x9bef20 by an inlined head insertion (0x46ce73..0x46ceba). Oldest
  walked first and head filing give: the NEWEST registration of a notify runs first, and threads woken by a later notify run before
  those of an earlier one. Behaviour agrees: beltot.gsc:1762-1793 (the later waiter truck_pause_node must beat the generic trigger
  thread of _vehicle.gsc :527) and moscow.gsc:1396-1399.
- The frame's list is one stack ordered by filing. `wait` files with the same head insertion (0x46a2e9 -> 0x4643a0) under counter +
  frames (mod 0xfffd, found with 0x463f20), and VM_RunList (0x46d560), the only caller of the interpreter 0x4690c0, runs the head and
  re-reads the head until the list is empty. So what a running thread wakes runs right after it yields, before older due threads,
  and a notify between runs lands above every timed thread already in the list. `thread f()` runs f at once.
- `waittillframeend` (0x46a495 -> 0x4643f0) links the thread at the TAIL of the current frame's list. Frame-end threads therefore run
  last, oldest first, in the same list run; what one of them wakes goes to the head and runs before the next frame-end thread.
- When the list runs: G_RunFrame stage 3 runs it after each trigger pass (0x4dac2a), also with no queued touches (0x4dab1b), then
  removes it. So timed threads run before the animation time step of stage 4, not after. Stage 4 runs a list filed by notetracks
  (0x4dadc3) and stage 5 what is left (0x4daf3d), then 0x467150, then counter + 1. Scr_RunFrame (0x46f5c0) has the same tail for the
  callers outside G_RunFrame. 0x467150 is not a frame-end step: it empties the freed object list [0x9bef38] (see "Freeing a script
  object").
- `wait s` (0x46a1fc..0x46a2e9): frames = fistp(float(s * 20) + 2^-30) (constant 0x591f20), which rounds to nearest with ties up, not
  a truncation; a non-zero s that rounds to 0 becomes 1. An integer argument gives value * 20. `wait 0` files under the current
  counter, at the head of the list being run.
- TODO(spec): the three points above are decoded but NOT implemented; a trial changed mission runs and is held back. Ours still: `VM::notify` queues
  the woken threads newest registration first, `VM::runFrame` runs in stage 5 with the due timed threads first (newest filed first), then the queue, then the
  frame-end threads in thread order, each pass followed by a queue drain. `VM::drainReady` runs only the queue. A `wait` wakes at the first frame at or after s seconds, so it can differ from the exe's rounding by a frame.
- Consequence for init: threads woken or left in `wait 0` by main() run in the first frame list; `wait 0.05` threads are in the
  next one. World::start drains the queue and runs one game frame (matmata: the start trigger_multiple fires and spawns
  the jeep before ridein's `wait 0.05` calls getent("jeep")). Evidenced: the server spawn (0x4488b0) runs the frame start
  0x44a6a0 (level.time += 50) at 0x448985, then 0x4da470: entity spawn, main thread (0x4da50e), one full G_RunFrame(0, 0)
  (0x4da524); only then is the level loaded (0x448aab -> 0x4da540 clears 0x1c61048). So main() and the first frame run
  at level time 50, as in the port (runLevelMain adds the 50 ms before starting main).
- Engine-side notifies relative to the script frame: G_RunFrame stages (docs/spec/ai.md "Server frame order"): the player's
  trigger touches (stage 1) are delivered as "trigger" notifies in stage 3, the animation notetracks in stage 4, both before
  the script frame (stage 5, 0x467150).
- Spawn (`dospawn` 0x4edff0 arg3=0, `stalingradspawn` 0x4ee120 arg3=1, core 0x532200): see docs/spec/ai.md "Spawn checks"
  (telefrag, player sight, one spawn per spawner per level time, optional targetname, 32 actor slots, forcespawn eviction).

## Freeing a script object (entity delete)
VAs in the patch 1.3 SP executable. `delete()` on an entity notifies "death" (0x46d0f0) and frees the entity (0x4d7c30), which
hands its script object to 0x4670b0. The object is not released there: it is marked (type 0x14), referenced once more and pushed on
a list (head 0x9bef38). The list is drained when script execution returns to the engine (0x467150), so threads started in the same
frame still see the object. Each queued object goes through 0x46d1f0, which takes entries off the object's notify list (key
0x1fffe; entry name = the registering call's local id + 0x10000) until it is empty:
- An endon registration on the object (any type but 0xa): it is removed from the call's endon table (0x46c640; global table
  0x9bef28, key local id + 0x10000, dropped when empty) and the entry becomes the dead type 0x17 (0x46d384). The thread that owns
  the call (found by climbing the type 0x12 parent links, 0x46d2ec..0x46d32c) is only acted on when it has a 0x1ffff entry
  (lookup on the thread at 0x46d335..0x46d35a). The only writer of 0x1ffff entries is 0x46c2e0 called with 0 (0x46c488, see
  below); an ordinary `waittill` keeps its paused stack in the notify list of the object it waits on (0x46c6f0 takes it from there
  when the wait-object field of the thread is non-zero). So a thread in a `waittill` on another object, in a timed `wait` (state
  0x11), running or ready goes on unchanged, only without that endon (the object can never be notified again).
- A `waittill` paused on the freed object itself (entry type 0xa, value = the paused stack): the registration is removed (0x46c640)
  and 0x46c2e0 is called with 0. 0x46c2e0 walks the paused stack from the top, releasing its values. At each code position (the
  return point into the calling function) it asks whether the call above it still holds any endon (lookup of local id + 0x10000
  in 0x9bef28, 0x46c38b..0x46c3bd):
  - no: that call is ended (0x464990, 0x464ea0) and the walk goes on with its caller;
  - yes: the walk stops and the stack is cut there (0x46c431..0x46c442). With 0 the thread is parked again with state 0x10 and a
    wait-object field of 0 (0x46c464) and the remaining stack filed under 0x1ffff on the thread (0x46c488). Nothing can notify
    it any more; only one of the endons still held ends it (0x46c6f0 with field 0 -> 0x46c130: unwind to the registering call,
    the caller goes on);
  - the stack runs out: the thread ends (its variable and stack are released, 0x46c3f1..0x46c428).
  The first call of a thread has no code position below it, so it is never asked.
- A thread parked by such an earlier free (with its 0x1ffff entry) whose endon on the freed object was just removed is walked the
  same way with 1 (0x46d369): calls that no longer hold an endon are ended; if none is left, the 0x1ffff entry is removed
  (0x46c3fd) and the thread ends; otherwise it stays parked.
- The order of the entries does not change the result: an endon of the freed object is gone before any walk could stop at it.
- Behavioural consequence, shipped `maps/_spawner.gsc` `flood_think`: with no antispawn trigger `flood_trigger` deletes its
  `flood_spawner` trigger in the same frame in which it started the `flood_think` threads (`kill_trigger`), and every one of them
  holds `trigger endon("antispawn")`. Those threads keep running: a refused `dospawn` sleeps in `wait(2)` and retries (trainyard
  `waittill_aigroupcleared("station")`), and a successful spawn waits for the AI's `death` and spawns the next one.
- A paused stack never starts with a code position, so the first call of a thread is never asked. A thread call (0x46a75c..0x46a7a3)
  marks the slot below the arguments as the thread's start (its type is saved in the function frame and replaced by type 8,
  "before the first code position"); a wait or waittill (0x46a2e9, 0x46a3f4, 0x46afa8) archives top minus start entries
  through 0x46be00, which copies from the top down and stops above that slot. Code positions (type 7) enter the stack only
  from calls made inside the thread, one per nested call. Restoring (0x46bfb0) opens one function frame for the stack's
  saved position and one more per type 7 entry, which agrees with that layout. So when only the first call holds an endon,
  0x46c2e0 runs out of stack and the thread ends (0x46c3f1..0x46c428), whatever object that endon is on.
- Every entity free goes through this path, not only `delete()`: G_FreeEntity (0x4d7c30) calls 0x4670b0 at 0x4d7ea7 with no
  test of the entity kind (the actor, turret and vehicle parts before it are conditional, the script object free is not).
  So the frees the engine does itself (an exploded grenade or rocket, a picked-up item, a `trigger_once` after it fired, a
  `trigger_damage` with its remove flag) end the waits on the entity in the same way. Shipped `maps/_load.gsc:1688`
  (`doAutoSpawn`, a thread on a trigger_once/_multiple/... that waits for its `trigger` and holds endons only in its first
  call) therefore ends when a `trigger_once` frees itself, instead of waiting forever. `maps/_spawner.gsc:1366`
  (`start_tracker`, no endons, waits on its spawner) ends only when that entity is freed: a scripted `delete`, or a map-start
  placed actor that finds the actor pool full (freed through G_FreeEntity at 0x4fc6aa). A real spawner whose count runs out
  is not freed (0x5325ca..0x5325d8 only decrement it), and a placed actor that spawns stays the same entity (0x4fc640 turns
  it into the actor), so its waits go on on the actor.
- TODO(spec): the port spawns a new entity for a placed actor and only marks the placed one removed (src/game/world.cpp
  spawnActor), so threads waiting on the placed entity (duhoc_defend `start_tracker`) stay parked instead of following the actor.

