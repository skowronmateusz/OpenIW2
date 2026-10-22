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
