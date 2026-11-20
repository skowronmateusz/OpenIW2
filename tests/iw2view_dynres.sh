#!/bin/sh
# The 3D output scale and dynamic resolution of the enhanced renderer (docs/render-enhanced.md "3D output scale", "Dynamic
# resolution"), end to end:
#   1. r_enhOutputScale 0.75: the 3D output is 75 % of the window and the shot keeps the window's size, with no non-finite pixel;
#      with our TAA and, where the build and GPU have it, with MetalFX at that output size;
#   2. a --shot / IW2_BENCH run with dynamic resolution on keeps the upper bound (no change of the scale);
#   3. with MetalFX: a render scale below the upper bound (IW2_DYNRES_FORCE) goes to the scaler as content inside its larger input
#      (a "dynamic content" scaler, never our TAA instead);
#   4. with MetalFX: live play drawn offscreen with IW2_DYNRES_CYCLE steps the scale down and up during play without a GPU error.
#   iw2view_dynres.sh <iw2view> <main-dir> <map>
set -u
VIEW=$1 MAIN=$2 MAP=$3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; for f in "$TMP"/*.out; do echo "--- $f"; grep -E 'GPU|gputrace|MetalFX|3D output|dynamic|offscreen|nan|error' "$f" | tail -20; done; exit 1; }
mkdir -p "$TMP/home"
ENH="--cvar r_enhanced=1 --cvar r_enhPreset=2"

bench() { # bench <name> <env...> -- <cvars...>
  name=$1; shift
  envs=""
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs="$envs $1"; shift; done
  [ $# -gt 0 ] && shift
  # shellcheck disable=SC2086
  env IW2_HOME="$TMP/home" IW2_ENH_NANCHECK=1 IW2_BENCH=20 $envs "$VIEW" "$MAIN" "$MAP" --no-audio --no-intro --size 640x360 --sim-seconds 1 \
    $ENH "$@" --shot "$TMP/$name.png" >"$TMP/$name.out" 2>&1 || fail "$name: iw2view exited with $?"
  grep -q '^nan check final: 0 non-finite' "$TMP/$name.out" || fail "$name: non-finite pixels in the final image"
}

# 1. the 3D output at 75 %
bench taa -- --cvar r_enhMetalFX=0 --cvar r_enhOutputScale=0.75
grep -q '^3D output 480x270 of 640x360$' "$TMP/taa.out" || fail "taa: the 3D output is not 480x270"
file "$TMP/taa.png" | grep -q '640 x 360' || fail "taa: the shot is not at the window size"
bench native -- --cvar r_enhMetalFX=0
grep -q '^3D output ' "$TMP/native.out" && fail "native: a 3D output below the window at r_enhOutputScale 1"
MFX=0
grep -q 'MetalFX yes' "$TMP/taa.out" && MFX=1
if [ $MFX = 1 ]; then
  bench mfx -- --cvar r_enhMetalFX=1 --cvar r_enhRenderScale=0.67 --cvar r_enhDynRes=0 --cvar r_enhOutputScale=0.75
  grep -q '^3D output 480x270 of 640x360$' "$TMP/mfx.out" || fail "mfx: the 3D output is not 480x270"
  grep -q '^MetalFX scaler 322x181 -> 480x270' "$TMP/mfx.out" || fail "mfx: MetalFX does not write the 3D output size"
  grep -q 'our TAA instead' "$TMP/mfx.out" && fail "mfx: the scaler could not be made"
fi

# 2. a bench holds the scale; 3. a forced scale is content inside the scaler's input
bench held -- --cvar r_enhMetalFX=1 --cvar r_enhRenderScale=0.67 --cvar r_enhDynRes=1
grep -q '^dynamic resolution: ' "$TMP/held.out" && fail "held: the scale changed in a bench"
if [ $MFX = 1 ]; then
  grep -q '^MetalFX scaler 429x241 -> 640x360.*dynamic content 429x241' "$TMP/held.out" || fail "held: no dynamic scaler at the upper bound"
  bench forced IW2_DYNRES_FORCE=0.55 -- --cvar r_enhMetalFX=1 --cvar r_enhRenderScale=0.67 --cvar r_enhDynRes=1
  grep -q '^MetalFX scaler 429x241 -> 640x360.*dynamic content 352x198' "$TMP/forced.out" || fail "forced: the content is not 352x198 in a 429x241 input"
  grep -q 'our TAA instead' "$TMP/forced.out" && fail "forced: the scaler could not be made"

  # 4. live play: the scale steps through the ladder
  # shellcheck disable=SC2086
  env IW2_HOME="$TMP/home" IW2_OFFSCREEN=640x360 IW2_GPU_TRACE=1 IW2_EXIT_AFTER=25 IW2_MENU_KEYS=enter IW2_DYNRES_CYCLE=10 "$VIEW" "$MAIN" "$MAP" \
    --no-audio --no-intro --size 640x360 --cvar ui_autoContinue=1 $ENH --cvar r_enhMetalFX=1 --cvar r_enhRenderScale=0.67 --cvar r_enhDynRes=1 \
    >"$TMP/live.out" 2>&1 || fail "live: iw2view exited with $?"
  grep -q '^offscreen: 640x360' "$TMP/live.out" || fail "live: not drawn offscreen"
  grep -q '^dynamic resolution: 0.67 -> 0.65' "$TMP/live.out" || fail "live: the scale never stepped down"
  grep -q '^dynamic resolution: 0.50 -> 0.55' "$TMP/live.out" || fail "live: the scale never stepped up from the lower bound"
  grep -q 'our TAA instead' "$TMP/live.out" && fail "live: the scaler could not be made"
  [ "$(grep -c '^MetalFX scaler ' "$TMP/live.out")" -le 1 ] || fail "live: the scaler was reported more than once"
  grep '^gputrace ' "$TMP/live.out" | grep -v -q ' errors 0 ' && fail "live: GPU errors"
  grep -q -E '^GPU (error|errors|guard)' "$TMP/live.out" && fail "live: a GPU error or a hold"
fi
echo "iw2view dynamic resolution: ok"
