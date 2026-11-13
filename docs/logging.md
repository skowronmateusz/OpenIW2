# Session logs

An openiw2 addition with no counterpart in the original game: the original keeps its console in memory and writes
`games_mp.log` / `console_mp.log` only on request. This page is the spec of what OpenIW2 writes for bug reports. The
code is `src/util/log.{h,cpp}`; tests are `tests/test_log.cpp` and `tests/iw2server_log.sh`.

## When a log is written

| Start | Log file |
|---|---|
| `OpenIW2.app` from Finder or Terminal (also `.../MacOS/OpenIW2 <main> ...`) of a 0.x version | yes |
| `iw2server` inside the app bundle (`.../OpenIW2.app/Contents/MacOS/iw2server`) of a 0.x version | yes |
| a development build (`build/iw2view`, `build/iw2server`), a 1.x or later version | only on request |

A 0.x version is a version string (`git describe` of the build, also shown in the menu corner) that matches
`^v?0\.`. "In the app bundle" means the executable path contains `.app/Contents/`. Overrides, first match wins:

1. `IW2_LOG=0` / `IW2_LOG=1` in the environment;
2. `--cvar iw2_log=0` / `--cvar iw2_log=1` on the command line (iw2view and iw2server);
3. the default above.

`IW2_LOG_DIR=<folder>` changes the folder (default `~/Library/Logs/OpenIW2/` on macOS,
`$XDG_STATE_HOME/openiw2/` or `~/.local/state/openiw2/` elsewhere). Tests set `IW2_LOG_DIR` to a folder in the build
tree; the ctest binaries are never in an app bundle, so without `IW2_LOG=1` they write no log.

## Files

- Client: `openiw2-YYYYMMDD-HHMMSS.log`, one per start (local time of the start). A dedicated server started from
  the menu runs in the same process and goes on writing the client's file.
- Server: `iw2server-YYYYMMDD-HHMMSS-<port>.log`. The port is known only once the socket is bound (`--port 0`,
  `+set net_port`), so the file is opened as `iw2server-YYYYMMDD-HHMMSS.log` and renamed then; the server prints
  `iw2server: log <path>` after its start line.
- Trace dumps (console command `dumptrace`, a fatal signal; the recorder itself is not used yet):
  `<session name>-trace-HHMMSS.log`, after a fatal signal `<session name>-trace-crash.log`.
- Two starts in the same second: the second name gets `-2` (up to `-9`) before `.log`.
- At each start, before the new file is opened: the newest 10 sessions of the same kind (`openiw2` or `iw2server`)
  are kept with their trace files, older ones are deleted; then, while all such files together exceed 100 MB, the
  oldest session of either kind goes. Only names of exactly these forms are touched, never other files in the
  folder and never the running session. The names of earlier versions (`openiw2.log`, `openiw2-1.log` ...
  `openiw2-9.log`, `openiw2.log.1`) are deleted.
- One file grows to at most 20 MB. Then one line `log capped at 20 MB: from here only errors, warnings and lifecycle
  lines` is written and only lines from standard error and lines containing `error`, `warning`, `fatal` or `failed`
  (any case) are kept, up to 24 MB.

## Capture

Standard output and standard error (file descriptors 1 and 2, so `printf`, `std::cout`, `std::cerr` and libraries
alike) are redirected into two pipes at the very start of `main`. A reader thread:

- passes every byte unchanged and at once to the original descriptor (terminal, pipe or file; skipped when it is
  `/dev/null`, as for a Finder start), so the terminal, `> file`, pipes and the tests see exactly what they saw before;
- assembles lines (a line longer than 16 KB is cut and continued), applies the redaction below and writes each line
  to the log as `[mm:ss.mmm] text` (standard output) or `[mm:ss.mmm]! text` (standard error), the time since the
  start of the session (minutes go on past 59).

The descriptor is opened with `O_APPEND`, so the old problem of two `FILE` positions on one file (standard error
lines overwritten by standard output) cannot happen. `iw2server` makes standard output line buffered as iw2view does.
At exit (`atexit`, so also `quit` and `std::exit`), standard output and error are flushed, the descriptors restored,
and the reader drains what is left and stops; it does not wait for end of file (the dedicated console window holds
its own copy of the descriptor, `netgame::StdoutTee`, which nests inside the capture: it is started later and stopped
earlier).

## Redaction (log file only)

The terminal copy is never changed. In the file:

- the home folder (`$HOME`) is written `~`;
- an rcon request `rcon <password> <command>` keeps `rcon *** <command>` (wherever `rcon ` starts a line or follows a
  space, quote, `]` or `+`); the server prints it on the line after `Rcon from` / `Bad rcon from`, so wrong passwords
  are covered too;
- the value after any cvar whose name contains `password` (any case; `set g_password x`, `rcon_password "x"`,
  `\sv_privatePassword\x` in an info string, `g_password: x`) is written `***`;
- the chat text of a `[chat] ` line (everything after the first `: `) becomes `(N chars)`; the speaker part stays. A
  name that itself contains `: ` only loses its tail, the message never shows. A server console `say` echo
  (`h "..."` / `i "..."`) becomes `h "(N chars)"`;
- an IPv4 address outside loopback (127/8), private (10/8, 172.16/12, 192.168/16), link-local (169.254/16) and
  0.0.0.0 keeps its first two numbers: `83.12.x.x`.

The command line is never written raw (it can hold `+set rcon_password`); the header lists only `IW2_*` variable
names with redacted values.

## Header

The first lines of every file, written straight into the file (not to the terminal):

```
OpenIW2 log, openiw2 <version> (<build type>), session <YYYY-MM-DD HH:MM:SS +ZZZZ>
binary: app bundle | development build, <executable name>
system: macOS <version> (<build>), <model>, <CPU>, <cores> cores, <GB> GB    (Linux: kernel, CPU)
log: <path>
flags: IW2_...=... (or "none")
traces: <trace switch notes>                 (only when trace switches were given)
previous session: crash report <file name>    (only when one exists, see below)
```

Later lines in the same form, written as soon as known: `display:` (desktop mode, refresh, pixel density, window
size), `language:` (game language and system locale), `cvars:` (archived cvars of the profile that differ from their
registered default, once per profile load). No host name, user name or account names are written.

"Previous session": at start, the crash reports `OpenIW2-*.ips` (client) / `iw2server-*.ips` (server) in
`~/Library/Logs/DiagnosticReports/` newer than the newest earlier session file of that kind are named (file name
only, up to 3).

## Fatal errors

Handlers for SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE and SIGTRAP (alternate signal stack, one-shot) do only
async-signal-safe work: give the reader up to 300 ms to empty the pipes, write `[mm:ss.mmm]! fatal: signal <n>
(<name>): the log ends here; the crash report is in ~/Library/Logs/DiagnosticReports`, a backtrace (symbol names),
copy whatever is still in the pipes, write the trace recorder to the crash trace file when it holds anything, restore
descriptors 1 and 2, and raise the signal again so the system writes its crash report. `std::terminate` (an uncaught
exception) prints `fatal: uncaught exception: <what>` to standard error and aborts into the same path.

## Menu

**Options > OpenIW2 > Open log folder** (English, Polish) runs the command `iw2_openlogs`, which opens the log folder
in Finder (`SDL_OpenURL`); the folder is created first when it does not exist.
