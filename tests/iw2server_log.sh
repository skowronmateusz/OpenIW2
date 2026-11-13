#!/bin/sh
# The dedicated server's session log (docs/logging.md), end to end: iw2server with IW2_LOG=1 and IW2_LOG_DIR in a temp
# folder, a bot, a protocol client and rcon with a password. Checks the pass-through on stdout (the start line, the
# log path), the file name with the bound port, the header, the line prefix, the rcon password redaction, the event
# lines (once per transition, a bounded rate, off with iw2_logEvents=0) and that nothing is written without IW2_LOG.
#   iw2server_log.sh <iw2server> <iw2netclient> <iw2netinfo> <main-dir>
set -u
SERVER=$1 CLIENT=$2 INFO=$3 MAIN=$4
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
LOGS="$TMP/logs"
fail() { echo "FAIL: $*"; echo "--- stdout"; grep -v '^\[script\]' "$TMP/server.out"; echo "--- log"; cat "$LOGS"/iw2server-*.log 2>/dev/null | grep -v '\] \[script\]' | head -200; exit 1; }
retry() {
  n=$(($1 * 5)); shift
  while ! "$@"; do n=$((n - 1)); [ $n -le 0 ] && return 1; sleep 0.2; done
}

mkfifo "$TMP/stdin"
IW2_LOG=1 IW2_LOG_DIR="$LOGS" "$SERVER" "$MAIN" mp_toujane dm --port 0 --bots 1 --seconds 120 \
  +set rcon_password s3cretpw <"$TMP/stdin" >"$TMP/server.out" 2>&1 &
SPID=$!
exec 3>"$TMP/stdin"
i=0
until grep -q "^iw2server: log " "$TMP/server.out"; do
  kill -0 $SPID 2>/dev/null || fail "server exited"
  i=$((i + 1)); [ $i -gt 900 ] && fail "server did not start"; sleep 0.1
done
PORT=$(sed -n 's/^iw2server: .* on UDP \([0-9][0-9]*\)$/\1/p' "$TMP/server.out" | head -n 1)
[ -n "$PORT" ] || fail "no start line on stdout"
LOG=$(ls "$LOGS"/iw2server-*-"$PORT".log 2>/dev/null | head -n 1)
[ -n "$LOG" ] || fail "no iw2server-<time>-$PORT.log in the log folder: $(ls "$LOGS")"
grep -q "^iw2server: log .*iw2server-[0-9]\{8\}-[0-9]\{6\}-$PORT.log$" "$TMP/server.out" || fail "the log path line"

"$CLIENT" 127.0.0.1:"$PORT" 6 -f -c "say zebramarker42 hello" >"$TMP/client.out" 2>&1 || fail "client: $(tail -5 "$TMP/client.out")"
rcon_status() { "$INFO" rcon 127.0.0.1:"$PORT" s3cretpw status >"$TMP/r1" 2>&1 && grep -q "map: mp_toujane" "$TMP/r1"; }
retry 30 rcon_status || fail "rcon status"
wrong_pw() { sleep 0.6; "$INFO" rcon 127.0.0.1:"$PORT" wrongpw2 status >"$TMP/r2" 2>&1 && grep -q "Invalid password." "$TMP/r2"; }
retry 30 wrong_pw || fail "wrong rcon password"
echo "map_restart" >&3
sleep 6
echo "quit" >&3
exec 3>&-
wait $SPID || fail "server exit code"

# the terminal copy is unchanged
grep -q '^rcon "s3cretpw" status$' "$TMP/server.out" || fail "stdout lost the raw rcon line"
grep -q "\[ev\]" "$TMP/server.out" && fail "event lines on stdout"
# header and prefix
head -n 1 "$LOG" | grep -q "^OpenIW2 log, iw2server " || fail "header line"
grep -q "^binary: development build" "$LOG" || fail "binary line"
grep -q "^system: " "$LOG" || fail "system line"
grep -q "^flags: .*IW2_LOG=1" "$LOG" || fail "flags line"
grep -q "^\[[0-9][0-9]*:[0-5][0-9]\.[0-9][0-9][0-9]\] iw2server: .* on UDP $PORT$" "$LOG" || fail "prefixed start line"
# redaction
grep -q "s3cretpw\|wrongpw2" "$LOG" && fail "an rcon password in the log"
grep -q '\] rcon "\*\*\*" status$' "$LOG" || fail "redacted rcon line"
grep -q '\] Bad rcon from 127.0.0.1:[0-9]*:$' "$LOG" || fail "the bad rcon line"
[ -n "${HOME:-}" ] && grep -q "$HOME/" "$LOG" && fail "the home folder in the log"
grep -q "zebramarker42" "$LOG" && fail "chat text in the log"
# event lines: lifecycle once per transition (the console map_restart loads the map again)
[ "$(grep -c '\] \[ev\] life level mp_toujane dm$' "$LOG")" = 2 ] || fail "level load lines (start and map_restart)"
grep -q '\] \[ev\] net connect slot [0-9]* "openiw2probe", mp client [0-9]*$' "$LOG" || fail "connect line"
[ "$(grep -c '\] \[ev\] net connect slot' "$LOG")" -le 2 ] || fail "more connect lines than connects"
grep -q '\] \[ev\] net drop slot [0-9]*: EXE_' "$LOG" || fail "drop line with its reason"
grep -q '\] \[ev\] conn server frames [0-9]*, late [0-9]* (over 10 ms)' "$LOG" || fail "server summary"
grep '\] \[ev\] conn server' "$LOG" | sed 's/^\[\([0-9]*\):\([0-9]*\)\.\([0-9]*\)\].*/\1 \2 \3/' |
  awk '{ t = $1 * 60000 + $2 * 1000 + $3; if (NR > 1 && t - p < 9990) bad = 1; p = t } END { exit bad }' ||
  fail "server summaries closer than 10 s"
# a bounded rate: the whole run stays far below the budget of 40 lines per category and 10 s
EV=$(grep -c '\] \[ev\] ' "$LOG")
[ "$EV" -lt 200 ] || fail "$EV event lines"
grep -q 'lines suppressed' "$LOG" && fail "an event budget was exceeded"

# without IW2_LOG a development build writes nothing
N=$(ls "$LOGS" | wc -l)
env -u IW2_LOG IW2_LOG_DIR="$LOGS" "$SERVER" "$MAIN" mp_toujane dm --port 0 --seconds 1 >"$TMP/plain.out" 2>&1 </dev/null
[ "$(ls "$LOGS" | wc -l)" = "$N" ] || fail "a log without IW2_LOG"
grep -q "^iw2server: log " "$TMP/plain.out" && fail "a log path line without IW2_LOG"
# iw2_logEvents=0: a log without event lines; traces.txt in the log folder switches a trace on: its lines go to the
# trace recorder (not the log, not stdout) and are written to a trace file at the exit
mkdir -p "$TMP/logs2"
printf 'IW2_NET_TRACE=2, IW2_BOGUS=1 # a comment\n' >"$TMP/logs2/traces.txt"
IW2_LOG=1 IW2_LOG_DIR="$TMP/logs2" "$SERVER" "$MAIN" mp_toujane dm --port 0 --seconds 2 --cvar iw2_logEvents=0 >"$TMP/plain2.out" 2>&1 </dev/null
L2=$(ls "$TMP"/logs2/iw2server-*.log 2>/dev/null | grep -v -- -trace- | head -n 1)
[ -n "$L2" ] || fail "no log with iw2_logEvents=0"
grep -q "\[ev\]" "$L2" && fail "event lines with iw2_logEvents=0"
grep -q "^traces: IW2_NET_TRACE=2$" "$L2" || fail "the trace switch in the header"
grep -q "^traces: ignored IW2_BOGUS=1 (not a trace switch)$" "$L2" || fail "the ignored switch in the header"
grep -q "NETTRACE frame" "$L2" && fail "trace lines in the log file"
grep -q "^NETTRACE frame t=" "$TMP/plain2.out" || fail "trace lines no longer on stdout"
T2=$(ls "$TMP"/logs2/iw2server-*-trace-*.log 2>/dev/null | head -n 1)
[ -n "$T2" ] || fail "no trace file at the exit"
head -n 1 "$T2" | grep -q "^trace dump (exit) of iw2server-" || fail "trace file header"
grep -q "^\[[0-9]*:[0-9][0-9]\.[0-9][0-9][0-9]\] NETTRACE frame t=" "$T2" || fail "trace lines in the trace file"
grep -q "\] trace dump (exit): " "$L2" || fail "the dump's line in the log"
# the default of a 0.x version: on when the binary runs from an app bundle (also when started by a relative path)
VER=$(head -n 1 "$LOG" | sed -n 's/^OpenIW2 log, iw2server \([^ ]*\).*/\1/p')
mkdir -p "$TMP/T.app/Contents/MacOS"
cp "$SERVER" "$TMP/T.app/Contents/MacOS/iw2server"
env -u IW2_LOG IW2_LOG_DIR="$TMP/logs3" "$TMP/T.app/Contents/MacOS/iw2server" "$MAIN" mp_toujane dm --port 0 --seconds 1 >"$TMP/b1.out" 2>&1 </dev/null
(cd "$TMP/T.app/Contents/MacOS" && env -u IW2_LOG IW2_LOG_DIR="$TMP/logs4" ./iw2server "$MAIN" mp_toujane dm --port 0 --seconds 1 >"$TMP/b2.out" 2>&1 </dev/null)
case "$VER" in
  v0.*|0.*)
    for d in logs3 logs4; do
      B=$(ls "$TMP/$d"/iw2server-*.log 2>/dev/null | head -n 1)
      [ -n "$B" ] || fail "no log from the app bundle ($d)"
      grep -q "^binary: app bundle, .*/T.app/Contents/MacOS/iw2server$" "$B" || fail "binary line of the app bundle start ($d)"
    done ;;
  *)
    [ -z "$(ls "$TMP/logs3" "$TMP/logs4" 2>/dev/null)" ] || fail "a log from the app bundle of version $VER" ;;
esac
echo "iw2server log: ok ($EV event lines, $(wc -c <"$LOG" | tr -d ' ') bytes)"
