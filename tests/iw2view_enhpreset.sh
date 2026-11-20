#!/bin/sh
# The Enhanced graphics presets in iw2view, end to end (docs/render-enhanced.md "Presets"):
#   1. an older profile with "as the preset" (-1) values is read with the saved preset's values (high: sun shadows 3) and the
#      shot does not rewrite the profile;
#   2. --cvar r_enhPreset picks a preset over the profile's options; a --cvar option on top makes it custom, whatever the order;
#   3. the menu path (IW2_MENU_KEYS set:, as the page's rows): picking Low writes Low's values into the saved profile, then an option
#      changed by hand makes the preset custom and keeps the other values; the saved profile reads back as that custom setting.
#   iw2view_enhpreset.sh <iw2view> <main-dir> <map>
set -u
VIEW=$1 MAIN=$2 MAP=$3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; for f in "$TMP"/*.out; do echo "--- $f"; grep -v '^\[script\]' "$f" | tail -30; done; exit 1; }
HOMEDIR="$TMP/home"
CFG="$HOMEDIR/players/default/config.cfg"
mkdir -p "$HOMEDIR/players/default"
printf 'seta r_enhanced "1"\nseta r_enhPreset "2"\nseta r_enhSunShadows "-1"\nseta r_enhAO "-1"\nseta r_enhRTGI "-1"\n' >"$CFG"
cp "$CFG" "$TMP/config.orig"

shot() { # shot <name> [args...]
  name=$1; shift
  "$VIEW" "$MAIN" "$MAP" --home "$HOMEDIR" --no-audio --no-intro --size 320x180 --sim-seconds 0.3 --shot "$TMP/$name.png" "$@" \
    >"$TMP/$name.out" 2>&1 || fail "$name: iw2view exited with $?"
}
expect() { # expect <name> <preset> <sun shadows>
  grep -q "^enhanced rendering: preset $2, sun shadows $3," "$TMP/$1.out" || fail "$1: expected preset $2, sun shadows $3"
}
saved() { grep -q "^seta $1 \"$2\"\$" "$CFG" || fail "profile: $1 is not \"$2\""; }
menu() { # menu <name> <keys>
  env IW2_MENU_KEY_MS=300 IW2_EXIT_AFTER=30 IW2_MENU_KEYS="$2,wait,quit" "$VIEW" "$MAIN" --menu --home "$HOMEDIR" --no-audio --no-intro \
    --size 320x180 >"$TMP/$1.out" 2>&1 || fail "$1: iw2view exited with $?"
}

# 1. migration
shot old
expect old 2 3
cmp -s "$CFG" "$TMP/config.orig" || fail "old: the shot rewrote the profile"

# 2. --cvar
shot pick --cvar r_enhPreset=0
expect pick 0 1
shot custom --cvar r_enhPreset=3 --cvar r_enhSunShadows=1
expect custom 4 1
shot reversed --cvar r_enhSunShadows=1 --cvar r_enhPreset=3
expect reversed 4 1

# 3. the menu path and the saved profile
menu low set:r_enhPreset=0
saved r_enhPreset 0
saved r_enhSunShadows 1
saved r_enhAO 0
saved r_enhTAA 0
saved r_enhRTGI 0
grep -q '"-1"' "$CFG" && fail "low: an \"as the preset\" value is still saved"
menu hand set:r_enhAO=1
saved r_enhPreset 4
saved r_enhAO 1
saved r_enhSunShadows 1
saved r_enhTAA 0
shot readback
expect readback 4 1

echo "iw2view enhanced presets: ok"
