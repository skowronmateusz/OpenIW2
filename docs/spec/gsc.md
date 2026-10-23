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
