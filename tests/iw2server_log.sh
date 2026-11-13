#!/bin/sh
# The dedicated server's session log (docs/logging.md), end to end: iw2server with IW2_LOG=1 and IW2_LOG_DIR in a temp
# folder, a bot, a protocol client and rcon with a password. Checks the pass-through on stdout (the start line, the
# log path), the file name with the bound port, the header, the line prefix, the rcon password redaction and that
# nothing is written without IW2_LOG.
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

"$CLIENT" 127.0.0.1:"$PORT" 6 -f >"$TMP/client.out" 2>&1 || fail "client: $(tail -5 "$TMP/client.out")"
rcon_status() { "$INFO" rcon 127.0.0.1:"$PORT" s3cretpw status >"$TMP/r1" 2>&1 && grep -q "map: mp_toujane" "$TMP/r1"; }
retry 30 rcon_status || fail "rcon status"
wrong_pw() { sleep 0.6; "$INFO" rcon 127.0.0.1:"$PORT" wrongpw2 status >"$TMP/r2" 2>&1 && grep -q "Invalid password." "$TMP/r2"; }
retry 30 wrong_pw || fail "wrong rcon password"
echo "map_restart" >&3
sleep 3
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
# without IW2_LOG a development build writes nothing
N=$(ls "$LOGS" | wc -l)
IW2_LOG_DIR="$LOGS" "$SERVER" "$MAIN" mp_toujane dm --port 0 --seconds 1 >"$TMP/plain.out" 2>&1 </dev/null
[ "$(ls "$LOGS" | wc -l)" = "$N" ] || fail "a log without IW2_LOG"
grep -q "^iw2server: log " "$TMP/plain.out" && fail "a log path line without IW2_LOG"
echo "iw2server log: ok ($(wc -c <"$LOG" | tr -d ' ') bytes)"
