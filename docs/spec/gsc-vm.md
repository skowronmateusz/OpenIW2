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
- `notify` does not run the waiters. VM_Notify (0x46ca60, from Scr_NotifyNum 0x46d0f0) files each woken thread in the list of the
  current frame counter (0x9bef20, via 0x46c570 -> 0x46c130). CORRECTION (2026-10-27): the filing is a head insertion, not an
  append. 0x46c130 walks the waiters from the last registered to the first (esi/edi count down from the end, 0x46c151..0x46c1a4)
  and files each through the script-variable child insertion 0x4643a0, which sets the parent's +0xe (the first element) to the new
  entry and links the old first behind it; the runner (0x46d560, 0x46d5b4) and the iterator idiom (0x46def0..0x46dfb0) start at
  the parent's +0xe. So the list behaves as a stack: the waiters of ONE notify run in registration order (double reversal), and
  the threads woken by a LATER notify run BEFORE those of an earlier one. Evidence by behaviour: moscow.gsc:1396-1399, where
  dialogue() must run before friendly_letlev_block's resume_idle restart (else the block loop animscripted cancels lookcompass
  and the chain dies; with the old FIFO the lines moscow_rs1_lookcompass / storeroom never played; with the stack they do).
  `wait` filing (0x46a2e9) uses the same head insertion (0x4643a0) into the wake frame's list (counter + frames mod 0xfffd,
  looked up with 0x463f20) and Scr_RunFrame runs that list through the same VM_RunList, so threads due in one frame run
  newest-filed first (VM::runFrame sorts the due threads by filing sequence, descending). Only VM_RunList (0x46d560) calls the interpreter
  (0x4690c0); it runs the list head and re-reads the head until the list is empty. Scr_RunFrame (0x46f5c0) = run the current list
  (0x46de90), the frame-end threads (0x467150), then `incl 0x9bef20`. `thread f()` runs f at once. Ours: `VM::notify` queues,
  `VM::drainReady` runs the queue as a stack (push at the head) after the timed threads of `runFrame`.
- `wait s` (0x46a1fc..0x46a2e9): frames = (int)(s*20 + 0.5), a non-zero s that rounds to 0 becomes 1, the thread is filed under
  counter + frames. `wait 0` therefore resumes in the SAME frame's list (after what is already queued). Ours: `wait 0` queues the
  thread like a notify wake-up.
- Consequence for init: threads woken or left in `wait 0` by main() run in the first frame list; `wait 0.05` threads are in the
  next one. World::start drains the queue and runs one game frame at time 0 (matmata: the start trigger_multiple fires and spawns
  the jeep before ridein's `wait 0.05` calls getent("jeep")). TODO(spec): the exe's start-up frames (number, time step) are not
  read; only the order is evidenced.
- TODO(spec): where the frame-end threads (0x467150) run what they notify; the order of engine-side notifies (triggers, anim
  notetracks) relative to Scr_RunFrame (we notify before `runFrame`, so they run after that frame's timed threads).
- Spawn (`dospawn` 0x4edff0 arg3=0, `stalingradspawn` 0x4ee120 arg3=1, core 0x532200): dospawn refuses a spawnpoint whose box
  (origin + (-15,-15,0)..(15,15,70)) holds a living body (0x4e8500, contents 0x200c000), "couldn't spawn from %s because spawnpoint
  would telefrag"; both fail with "no free actors" when all 32 actor slots (0x4fc8d0) are in use (corpses free their slot).
  TODO(spec): the player's contents/box height, the spawner flag-2 retry (0x4fcaf0).
