#!/bin/sh
# The user home of iw2view (src/ui/userhome.h), end to end: a shot started with the game folder on the command line must not
# read or change the profile kept next to that folder. A fake base folder gets a `main` link to the game data and a profile
# players/default/config.cfg that turns the enhanced renderer and MetalFX on. Checks:
#   1. a plain shot uses a temporary home: no "enhanced rendering:" line, the profile and the base folder unchanged, the
#      temporary home removed at exit;
#   2. the same shot with --home <base> reads that profile (the enhanced line with MetalFX on), so check 1 is not vacuous;
#   3. IW2_HOME=<dir> is used as the home, and --home wins over it.
#   iw2view_home.sh <iw2view> <main-dir> <map>
set -u
VIEW=$1 MAIN=$2 MAP=$3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; for f in "$TMP"/*.out; do echo "--- $f"; grep -v '^\[script\]' "$f" | tail -40; done; exit 1; }

BASE="$TMP/base"
mkdir -p "$BASE/players/default"
ln -s "$MAIN" "$BASE/main"
printf 'seta r_enhanced "1"\nseta r_enhPreset "2"\nseta r_enhMetalFX "1"\nseta r_enhTAA "1"\n' >"$BASE/players/default/config.cfg"
cp "$BASE/players/default/config.cfg" "$TMP/config.orig"
listing() { (cd "$BASE" && find . -print | sort); }
listing >"$TMP/listing.orig"

shot() { # shot <name> [args...]
  name=$1; shift
  "$VIEW" "$BASE/main" "$MAP" --no-audio --no-intro --size 320x180 --sim-seconds 0.5 --shot "$TMP/$name.png" "$@" >"$TMP/$name.out" 2>&1 \
    || fail "$name: iw2view exited with $?"
  [ -s "$TMP/$name.png" ] || fail "$name: no shot written"
}

# 1. temporary home
shot plain
grep -q '^home (temporary): ' "$TMP/plain.out" || fail "plain: no temporary home line"
grep -q '^enhanced rendering:' "$TMP/plain.out" && fail "plain: the profile's r_enhanced was read"
HOMEDIR=$(sed -n 's/^home (temporary): //p' "$TMP/plain.out" | head -n 1)
[ -n "$HOMEDIR" ] && [ ! -e "$HOMEDIR" ] || fail "plain: temporary home $HOMEDIR not removed"
cmp -s "$BASE/players/default/config.cfg" "$TMP/config.orig" || fail "plain: profile changed"
listing | cmp -s - "$TMP/listing.orig" || fail "plain: files written next to the game folder"

# 2. --home: the same profile is read (positive control)
shot home --home "$BASE"
grep -q '^home (--home): ' "$TMP/home.out" || fail "home: no --home line"
grep -q '^enhanced rendering: .*MetalFX on' "$TMP/home.out" || fail "home: the profile was not read"

# 3. IW2_HOME, and --home over IW2_HOME
mkdir -p "$TMP/envhome"
IW2_HOME="$TMP/envhome" shot env
grep -q "^home (IW2_HOME): $TMP/envhome" "$TMP/env.out" || fail "env: IW2_HOME not used"
grep -q '^enhanced rendering:' "$TMP/env.out" && fail "env: a profile was read from an empty home"
IW2_HOME="$TMP/envhome" shot both --home "$BASE"
grep -q '^home (--home): ' "$TMP/both.out" || fail "both: --home did not win over IW2_HOME"

echo "iw2view home: ok"
