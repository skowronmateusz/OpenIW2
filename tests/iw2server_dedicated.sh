#!/bin/sh
# End-to-end check of the dedicated server (docs/spec/dedicated.md): iw2server with a bot, a protocol client,
# rcon with a wrong and a right password, getstatus with live players, a map rotation by rcon that the client
# follows, and quit from the stdin console.
#   iw2server_dedicated.sh <iw2server> <iw2netclient> <iw2netinfo> <main-dir> [port]
# The default port 0 lets the system pick a free one (parallel test runs never collide); the script reads the
# bound port from the server's start line. Every step waits for its condition instead of a fixed delay.
set -u
SERVER=$1 CLIENT=$2 INFO=$3 MAIN=$4 PORT=${5:-0}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# retry <seconds> <command...>: runs the command every 0.2 s until it succeeds.
retry() {
  n=$(($1 * 5)); shift
  while ! "$@"; do n=$((n - 1)); [ $n -le 0 ] && return 1; sleep 0.2; done
}
fail() { echo "FAIL: $*"; echo "--- server"; grep -v '^\[script\]' "$TMP/server.log"; echo "--- client"; cat "$TMP/client.log"; exit 1; }

mkfifo "$TMP/stdin"
"$SERVER" "$MAIN" mp_toujane dm --port "$PORT" --bots 1 --seconds 150 +set rcon_password pw \
  +set sv_mapRotation "gametype dm map mp_carentan" <"$TMP/stdin" >"$TMP/server.log" 2>&1 &
SPID=$!
exec 3>"$TMP/stdin"
i=0
until grep -q "on UDP" "$TMP/server.log"; do
  kill -0 $SPID 2>/dev/null || fail "server exited"
  i=$((i + 1)); [ $i -gt 600 ] && fail "server did not start"; sleep 0.1
done
PORT=$(sed -n 's/^iw2server: .* on UDP \([0-9][0-9]*\)$/\1/p' "$TMP/server.log" | head -n 1)
[ -n "$PORT" ] && [ "$PORT" != 0 ] || fail "no bound port in the start line"

"$CLIENT" 127.0.0.1:"$PORT" 100 -f --until-map mp_carentan >"$TMP/client.log" 2>&1 &
CPID=$!
# rcon replies are rate limited per address (docs/spec/dedicated.md), so repeated asks are spaced by retry.
rcon_status() {
  "$INFO" rcon 127.0.0.1:"$PORT" pw status >"$TMP/r2" 2>&1 && grep -q "map: mp_toujane" "$TMP/r2" \
    && grep -q "bot0" "$TMP/r2" && grep -q "openiw2probe" "$TMP/r2"
}
retry 90 rcon_status || fail "rcon status: $(cat "$TMP/r2")"
wrong_pw() { sleep 0.6; "$INFO" rcon 127.0.0.1:"$PORT" wrong status >"$TMP/r1" 2>&1 && grep -q "Invalid password." "$TMP/r1"; }
retry 60 wrong_pw || fail "wrong rcon password: $(cat "$TMP/r1")"
get_status() {
  "$INFO" query 127.0.0.1:"$PORT" getstatus >"$TMP/st" 2>&1 && grep -q '"bot0"' "$TMP/st" && grep -q '"openiw2probe"' "$TMP/st"
}
retry 60 get_status || fail "getstatus: $(cat "$TMP/st")"
rotate() { sleep 0.6; "$INFO" rcon 127.0.0.1:"$PORT" pw map_rotate >"$TMP/r3" 2>&1 && grep -q "Setting map: mp_carentan." "$TMP/r3"; }
retry 60 rotate || fail "rcon map_rotate: $(cat "$TMP/r3")"
wait $CPID || fail "client exit code"
grep -q 'mapname\\mp_carentan' "$TMP/client.log" || fail "client did not get the new map's gamestate"
echo "status" >&3
echo "quit" >&3
exec 3>&-
wait $SPID || fail "server exit code"
grep -q "map: mp_carentan" "$TMP/server.log" || fail "stdin status"
echo "iw2server dedicated: ok"
