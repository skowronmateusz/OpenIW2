#!/bin/sh
# GPU errors in the enhanced renderer (docs/render-enhanced.md "GPU errors"), end to end:
#   1. the ray tracing kernels build (a kernel that does not build is reported once and never retried every frame);
#   2. a command buffer counted as hung (IW2_GPU_FAKE_ERROR) is reported and the GPU guard holds the ray tracing and MetalFX,
#      lets them on again after the hold (IW2_GPU_GUARD_MS) and holds them for good after the second error; the picture
#      stays finite across the switches;
#   3. a run without errors reports none and never holds;
#   4. live play drawn offscreen (IW2_OFFSCREEN) runs with MetalFX, and its trace (IW2_GPU_TRACE) counts no error;
#   5. no frame shader or kernel takes triangle candidates from a ray query (the source), and the view that locked the GPU up
#      (the bunker's ladder hatch in elalamein: reflection rays into alpha-tested surfaces, at full size) runs 300 frames with
#      the ray tracing on and no GPU error. Only where the build and the GPU trace rays.
#   iw2view_gpuguard.sh <iw2view> <main-dir> <map>
set -u
VIEW=$1 MAIN=$2 MAP=$3
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; for f in "$TMP"/*.out; do echo "--- $f"; grep -E 'GPU|gputrace|MetalFX|kernel|offscreen|nan|error' "$f" | tail -20; done; exit 1; }
mkdir -p "$TMP/home"
ENH="--cvar r_enhanced=1 --cvar r_enhPreset=3 --cvar r_enhTAA=1 --cvar r_enhMetalFX=1 --cvar r_enhRenderScale=0.77"

bench() { # bench <name> [env assignments...]
  name=$1; shift
  # shellcheck disable=SC2086
  env IW2_HOME="$TMP/home" IW2_ENH_NANCHECK=1 IW2_BENCH=60 "$@" "$VIEW" "$MAIN" "$MAP" --no-audio --no-intro --size 480x270 --sim-seconds 2 \
    $ENH --shot "$TMP/$name.png" >"$TMP/$name.out" 2>&1 || fail "$name: iw2view exited with $?"
  grep -q '^nan check final: 0 non-finite' "$TMP/$name.out" || fail "$name: non-finite pixels in the final image"
}

bench faulty IW2_GPU_FAKE_ERROR=20,45 IW2_GPU_GUARD_MS=30
if grep -q 'ray tracing yes' "$TMP/faulty.out"; then
  grep -q '^ray tracing kernel rt_dynamic_shadow: built' "$TMP/faulty.out" || fail "the shadow kernel was not built"
  grep -q '^ray tracing kernel rt_gi: built' "$TMP/faulty.out" || fail "the global illumination kernel was not built"
  [ "$(grep -c '^ray tracing kernel ' "$TMP/faulty.out")" = 2 ] || fail "a kernel was built more than once"
fi
grep -q 'not built' "$TMP/faulty.out" && fail "a ray tracing kernel did not build"
order=$(grep -n -E '^GPU guard: ' "$TMP/faulty.out" | sed -e 's/.*MetalFX //' -e 's/ .*//' | tr '\n' ' ')
[ "$order" = "held on off " ] || fail "the guard's steps were '$order' (expected: held, on again, off for good)"
grep -q '^GPU error: IW2_GPU_FAKE_ERROR' "$TMP/faulty.out" || fail "the error was not reported"

bench clean
grep -q -E '^GPU (error|errors|guard)' "$TMP/clean.out" && fail "a GPU error or a hold in a run without one"

# shellcheck disable=SC2086
env IW2_HOME="$TMP/home" IW2_OFFSCREEN=640x360 IW2_GPU_TRACE=1 IW2_EXIT_AFTER=45 IW2_MENU_KEYS=enter "$VIEW" "$MAIN" "$MAP" --no-audio --no-intro \
  --size 640x360 --cvar ui_autoContinue=1 $ENH >"$TMP/live.out" 2>&1 || fail "live: iw2view exited with $?"
grep -q '^offscreen: 640x360' "$TMP/live.out" || fail "live: not drawn offscreen"
grep -q '^gputrace ' "$TMP/live.out" || fail "live: no trace line"
grep '^gputrace ' "$TMP/live.out" | grep -v -q ' errors 0 ' && fail "live: GPU errors"
grep '^gputrace ' "$TMP/live.out" | grep -q ' paused 0' || fail "live: the game never ran unpaused"
grep -q -E '^GPU (error|errors|guard)' "$TMP/live.out" && fail "live: a GPU error or a hold"
SRC=$(dirname "$0")/../apps/iw2view
for f in "$SRC/shaders_enhanced.metal.inc" "$SRC/shaders_enhanced_post.metal.inc" "$SRC/shaders.metal.inc" "$SRC/metalnative.mm"; do
  [ -f "$f" ] || fail "source not found: $f"
  grep -q -E 'commit_triangle_intersection|get_candidate_|InstanceOptionNonOpaque|opaque = NO' "$f" && fail "$f takes triangle candidates from a ray query"
done
if grep -q 'ray tracing yes' "$TMP/faulty.out" && [ "$MAP" = elalamein ]; then
  env IW2_HOME="$TMP/home" IW2_SHOT_FREE_CAM=1 IW2_BENCH=300 "$VIEW" "$MAIN" "$MAP" --no-audio --no-intro --size 3024x1708 --sim-seconds 3 \
    --pos -6817.88,-10910.1,236.125 --yaw 239 --pitch 30 --cvar r_enhanced=1 --cvar r_enhPreset=2 --cvar r_enhMetalFX=0 \
    --shot "$TMP/hatch.png" >"$TMP/hatch.out" 2>&1 || fail "hatch: iw2view exited with $?"
  grep -q '^ray tracing scene: [0-9]' "$TMP/hatch.out" || fail "hatch: no ray tracing scene"
  grep -q -E '^GPU (error|errors|guard)' "$TMP/hatch.out" && fail "hatch: the GPU did not run a frame to the end"
fi
echo "iw2view gpu guard: ok"
