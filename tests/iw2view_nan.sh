#!/bin/sh
# Non-finite values in the enhanced renderer's temporal filter (docs/render-enhanced.md "Non-finite values"), end to end: a shot
# with IW2_ENH_INJECT_NAN writes NaN into the resolve's input or into the history for the first frames of a bench, and the
# non-finite guard (IW2_ENH_NANCHECK) counts the final image. Checks, with our TAA and (where the build has it) MetalFX:
#   1. NaN in the input never reaches the output (no history reset needed, the final image has none);
#   2. NaN in the history is found by the guard, which resets the history, and the final image has none.
#   iw2view_nan.sh <iw2view> <main-dir> <map>
set -u
VIEW=$1 MAIN=$2 MAP=$3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; for f in "$TMP"/*.out; do echo "--- $f"; grep -E 'nan|MetalFX|error' "$f" | tail -20; done; exit 1; }

run() { # run <name> <inject spec> [args...]
  name=$1 inj=$2; shift 2
  IW2_HOME="$TMP/home" IW2_ENH_NANCHECK=1 IW2_ENH_INJECT_NAN=$inj IW2_BENCH=16 "$VIEW" "$MAIN" "$MAP" --no-audio --no-intro --size 480x270 \
    --sim-seconds 2 --cvar r_enhanced=1 --cvar r_enhPreset=2 --cvar r_enhTAA=1 "$@" --shot "$TMP/$name.png" >"$TMP/$name.out" 2>&1 \
    || fail "$name: iw2view exited with $?"
  grep -q '^nan inject: ' "$TMP/$name.out" || fail "$name: nothing injected"
  grep -q '^nan check final: 0 non-finite' "$TMP/$name.out" || fail "$name: non-finite pixels in the final image"
}
mkdir -p "$TMP/home"
for mode in taa mfx; do
  extra="--cvar r_enhMetalFX=0"
  [ $mode = mfx ] && extra="--cvar r_enhMetalFX=1 --cvar r_enhRenderScale=0.77"
  # shellcheck disable=SC2086
  run ${mode}_input 40,30,120,80,4,0 $extra
  if [ $mode = mfx ] && ! grep -q '^MetalFX scaler' "$TMP/${mode}_input.out"; then echo "no MetalFX in this build"; break; fi
  grep -q '^nan guard: ' "$TMP/${mode}_input.out" && fail "${mode}_input: NaN in the input reached the output"
  # shellcheck disable=SC2086
  run ${mode}_history 40,30,120,80,4,1 $extra
  grep -q '^nan guard: 9600 non-finite pixels, history reset' "$TMP/${mode}_history.out" || fail "${mode}_history: the guard did not find the NaN"
done
echo "iw2view nan: ok"
