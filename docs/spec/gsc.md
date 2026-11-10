# GSC script language

The single-player campaign logic (levels, AI behaviour in `animscripts/`, vehicles, effects) ships as GSC source
text inside the `.iwd` packs: 622 files, 7,092 functions. This grammar is derived from those sources; the original
compiler (in the executable) has not been analyzed yet.

## Files

- `#include path\to\file;` makes another file's functions callable without a qualifier.
- `#using_animtree("name");` selects the animation tree that `%anim` references resolve against.
- A file is a list of functions: `name(param, ...) { statements }`.

## Statements

`{ … }`, `expr;`, `if (c) s [else s]`, `while (c) s`, `for (init; cond; step) s` (any part may be empty),
`switch (e) { case v: … default: … }`, `break;`, `continue;`, `return [e];`, `wait e;`,
`waittillframeend;`, `breakpoint;`, and developer blocks `/# … #/` (compiled only when the bool dvar `developer_script`, default 0, is set: registration 0x42df9d, lexer 0x457820).

## Expressions

- Literals: integers, floats (`.5`), strings `"…"` with `\n \t \\ \"` escapes, localized strings `&"KEY"`,
  vectors `(x, y, z)`, empty array `[]`, `undefined`, `true`, `false`.
- Built-in objects: `self`, `level`, `game`, `anim`.
- Animation references `%name` and the tree reference `#animtree`. `%` after an operand is the modulo operator.
- Members `a.b` (`.size` is the array or string length), indexing `a[i]`.
- Calls: `f(args)`, `path\file::f(args)`, `::f` (function pointer), `[[expr]](args)` (call through a pointer),
  each optionally prefixed by an object (`ent f()`) and/or `thread` (`ent thread f()`).
- Operators, by increasing precedence: `||`, `&&`, `|`, `^`, `&`, `== !=`, `< > <= >=`, `<< >>`, `+ -`,
  `* / %`, unary `! - ~`.
- Assignment statements: `= += -= *= /= %= &= |= ^= <<= >>=`, plus `++` and `--` (prefix and postfix).

## Quirks found in the shipped data

- `animscripts/traverse/stairs_down.gsc` omits `;` at two line ends. The parser accepts a missing `;` before a
  line break; whether the original compiler does is unknown.
- `animscripts/traverse/stairs_up.gsc` line 29 contains `self endnode.origin`, which is not valid in this grammar.
  The original compiler must reject it, so the file is probably unused.

## Implementation

`src/script/parser.cpp` (lexer + recursive descent) builds the tree in `src/script/ast.h`.
`iw2script <main> check` parses every shipped script.

## Developer-only built-ins (`developer`, `developer_script`)

Evidence: patch 1.3 SP executable and patch 1.3 MP executable (MP VAs have the 0x08 prefix).

**Dvars.** Both registrations sit in the same start-up routine in both executables.
- `developer`: integer, range 0..2, default 0, flags 0x1000 (SP 0x42df7b, MP 0x8061e1f). Stored at 0x747ed0 (SP).
- `developer_script`: bool, default 0, flags 0x1000 (SP 0x42df9d, MP 0x8061e40). Stored at 0x74a904 (SP).
- A refresh routine (SP 0x42de90, run again at 0x42e5b3..0x42e5c1 before scripts are loaded; MP 0x8061cf4 -> 0x807ffd8) copies them
  into the script system's flags: "error reporting" (0x9bef12) = `developer != 0` or the `logfile` dvar != 0; "developer compile"
  (0x9bef13) = `developer_script`; "developer" (0xa01895) = `developer != 0`.
  The compile flag is the only gate of developer-only script code. Nothing at call time looks at either dvar.

**Which built-ins are developer-only.** The last word of a built-in table row (docs/spec/builtins.md) marks them. SP functions:
`print`, `println`, `print3d`, `line`, `assert`, `assertex`, `assertmsg`, `getdebugcvar`, `getdebugcvarint`, `getdebugcvarfloat`,
`getentbynum`; SP methods: `setdebugorigin`, `setdebugangles`, `getdebugeye`, `getentnum`. The MP function table (0x8187040..) flags
`print`, `println`, `print3d`, `line`, `assert`, `assertex`, `assertmsg`, `getentbynum` and has no debug-cvar functions or debug
methods. `iprintln` / `iprintlnbold` are NOT developer-only (flag 0; they print the gameplay message).

**Effect: a compile-time gate, not a run-time check.**
- The lookup of a built-in function name (SP 0x4f8b60, called only from the compiler at 0x457a3f) returns the row's flag; the compiler
  records the name as a "codepos" (script variable type 7) or, for a developer-only row, a "developer codepos" (type 12).
- The lexer treats `/# ... #/` as a block that is compiled only when "developer compile" is set; with it clear the tokens up to `#/`
  are skipped (0x457820). A call of a developer-only built-in found outside such a block opens an implicit block at the name
  (0x457810) that ends after the call (0x457870): with developer_script 0 the call and its arguments are skipped the same way, with
  developer_script 1 they are compiled.
- A developer-only call outside a block whose return value is used (an expression, not a statement) is a compile error:
  "return value of developer command can not be accessed if not in a /# ... #/ comment" (0x457acd, 0x457cf1).
- Consequences with developer_script 0: `println` / `print` / `print3d` / `line` print or draw nothing, `assert` / `assertex` /
  `assertmsg` do nothing (a false condition never raises an error, the argument expressions never run), and the `/# #/` blocks do not
  exist. Most developer-only calls in the shipped scripts stand outside a block (506 `assert`, 304 `assertex`, 402 `println` against 3 / 5 / 37
  inside one), so the implicit block is the normal case, not an exception.
- With developer_script 1 the built-ins are ordinary calls. `assert` / `assertex` / `assertmsg` (SP 0x4ebe60 / 0x4ebe90 /
  0x4ebee0, MP 0x8110c08 / 0x8110c2e / 0x8110c6c) raise a script error "assert fail" / "assert fail: <text>" when the condition is
  false (`assertmsg` always) through the common error routine (0x468ed0).
- What the error does afterwards depends on the error-reporting flag: the report routine (0x460030) prints nothing at all unless
  `developer` (or `logfile`) is non-zero or the error is one of the "severe" kind (flag 0xa01896); otherwise it prints the script stack
  and raises Com_Error code 4 (code 5 for a severe error), see 0x4600d1. TODO(spec): the Com_Error 4 / 5 behaviour (whether the
  original ends the whole level in developer mode; the spec above, "A runtime error stops only the failing thread", is the
  behaviour openiw2 keeps); and the printing of runtime errors with developer 0.

**openiw2.** `developer` and `developer_script` are cvars with the original defaults (World constructor). `developer_script` is read at
every script-file compile (`VM::setDeveloperScript`); a statement that is a call of a developer-only name and every `/# #/` block
compile to nothing when it is 0. With it 1 they compile and run; a failed assert ends the failing thread and is logged as a script
error (the existing runtime-error report). `--cvar developer_script=1` turns the developer script code on (add `--cvar developer=1`
for the original's full developer mode); it has to be set before the first script of the level is compiled, which all tools do.
Deliberate leniency: a developer-only call whose value is used outside a block (the original's compile error above) gives undefined
here with developer_script 0 and runs with 1, because the parser accepts a missing `;` at a line end and so reads an `assert(...)` on
the next line of `animscripts/traverse/stairs_down.gsc` (lines 14-17) as a method call on the previous value; that file must keep
loading. A function between a file-scope `/#` and `#/` (`animscripts/utility.gsc:990`, for example) is declared and compiled only with
developer_script 1 (a call to it from non-developer code then fails at run time, as in the original, where the function does not exist).
Tests: tests/test_vm.cpp (developer_script 0 / 1, value use, file-scope developer functions).
