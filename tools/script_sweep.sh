#!/bin/bash
# Headless mission-script sweep over the SP campaign maps.
#
#   IW2_MAIN=<main dir> tools/script_sweep.sh --full  [--seeds "1 2 3"] [--seconds 600] [--jobs 4] [--out DIR] [--maps "a b"] [--force]
#   IW2_MAIN=<main dir> tools/script_sweep.sh --smoke [--bin build/iw2game]          (the ctest subset; exit 1 on any finding)
#   IW2_MAIN=<main dir> tools/script_sweep.sh --determinism [--maps "a b"] [--seconds 120]
#
# Every run is `iw2game <main> <map> <seconds> --god --answer-menus ok,... --report <file>`; the full sweep runs each map plain and
# with --autoplay (a god-mode bot that enters the triggers the scripts wait for and kills the actors they wait the death of).
# Reports (one per run, `<out>/<map>-s<seed>-<plain|auto>.txt`, stdout in `.log`) and `<out>/summary.md` go to --out
# (default: a temporary directory under $TMPDIR); they quote shipped script sites by file:line and do not belong in the repo.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
bin="$here/build/iw2game"
mode=""
seeds="1"
seconds=600
jobs=4
out=""
maps=""
force=""
while [ $# -gt 0 ]; do
    case "$1" in
    --full | --smoke | --determinism) mode="${1#--}" ;;
    --bin) bin="$2"; shift ;;
    --seeds) seeds="$2"; shift ;;
    --seconds) seconds="$2"; shift ;;
    --jobs) jobs="$2"; shift ;;
    --out) out="$2"; shift ;;
    --maps) maps="$2"; shift ;;
    --force) force="--autoplay-force" ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$mode" ] || { echo "usage: $0 --full | --smoke | --determinism [options]" >&2; exit 2; }
: "${IW2_MAIN:?IW2_MAIN must point at the game main directory}"
[ -x "$bin" ] || { echo "$bin not built" >&2; exit 2; }
[ -n "$out" ] || out=$(mktemp -d "${TMPDIR:-/tmp}/iw2sweep.XXXXXX")
mkdir -p "$out"

# The SP maps: every maps/<name>.d3dbsp in the iwd archives. movie_* / slideshow_* get a short plain run only.
all_maps() {
    for f in "$IW2_MAIN"/iw_*.iwd; do unzip -Z1 "$f" 'maps/*.d3dbsp' 2>/dev/null; done | sed 's#^maps/##; s#\.d3dbsp$##' | sort -u
}
[ -n "$maps" ] || maps=$(all_maps | tr '\n' ' ')

answers="ok,ok,ok,ok,ok,ok,ok,ok"
# run <map> <seed> <seconds> <tag> [extra iw2game options]: one headless run, report + log in $out.
run() {
    local map=$1 seed=$2 secs=$3 tag=$4
    shift 4
    local base="$out/$map-s$seed-$tag"
    IW2_SCRIPT_SEED=$seed "$bin" "$IW2_MAIN" "$map" "$secs" --god --answer-menus "$answers" --report "$base.txt" "$@" > "$base.log" 2>&1
    local rc=$?
    [ $rc -eq 0 ] || echo "exit $rc" >> "$base.txt"
    return $rc
}
field() { awk -v k="$2" '$1 == k { $1 = ""; sub(/^ /, ""); print; exit }' "$1"; }
count_done_objectives() { awk '/^\[objectives\]/ { s = 1; next } /^\[/ { s = 0 } s && $2 == 3 { n++ } END { print n + 0 }' "$1"; }

do_smoke() {
    # Cheap ctest subset: maps known to run clean, 60 s, plain; one autoplay run; one determinism check. Fails on any
    # runtime error, loop-guard kill, unimplemented built-in, crash, or a non-deterministic report.
    rc=0
    for m in demolition eldaba 88ridge; do
        if ! run "$m" 1 60 plain; then echo "SMOKE FAIL: $m crashed, see $out/$m-s1-plain.log"; rc=1; continue; fi
        for k in runtime_errors loop_guard_kills unimplemented_builtins; do
            v=$(field "$out/$m-s1-plain.txt" $k)
            [ "${v:-1}" = 0 ] || { echo "SMOKE FAIL: $m $k = $v"; rc=1; }
        done
    done
    run breakout 1 90 auto --autoplay 20 || { echo "SMOKE FAIL: breakout autoplay crashed"; rc=1; }
    v=$(field "$out/breakout-s1-auto.txt" runtime_errors)
    [ "${v:-1}" = 0 ] || { echo "SMOKE FAIL: breakout autoplay runtime_errors = $v"; rc=1; }
    run demolition 1 60 plain2
    if ! diff -q "$out/demolition-s1-plain.txt" "$out/demolition-s1-plain2.txt" > /dev/null; then
        echo "SMOKE FAIL: demolition is not deterministic for seed 1"; diff "$out/demolition-s1-plain.txt" "$out/demolition-s1-plain2.txt" | head -20; rc=1
    fi
    [ $rc -eq 0 ] && echo "script smoke OK, reports in $out"
    exit $rc
}
do_determinism() {
    rc=0
    for m in $maps; do
        for seed in $seeds; do
            run "$m" "$seed" "$seconds" det1 --autoplay
            run "$m" "$seed" "$seconds" det2 --autoplay
            if diff -q "$out/$m-s$seed-det1.txt" "$out/$m-s$seed-det2.txt" > /dev/null; then echo "$m seed $seed: deterministic"
            else echo "$m seed $seed: DIFFERS"; diff "$out/$m-s$seed-det1.txt" "$out/$m-s$seed-det2.txt" | head -10; rc=1; fi
        done
    done
    exit $rc
}
do_full() {
    started=$(date +%s)
    for m in $maps; do
        case "$m" in
        movie_* | slideshow_*)
            run "$m" 1 30 plain &
            ;;
        *)
            for seed in $seeds; do
                run "$m" "$seed" "$seconds" plain &
                while [ "$(jobs -r | wc -l)" -ge "$jobs" ]; do sleep 1; done
                run "$m" "$seed" "$seconds" auto --autoplay $force &
                while [ "$(jobs -r | wc -l)" -ge "$jobs" ]; do sleep 1; done
            done
            ;;
        esac
        while [ "$(jobs -r | wc -l)" -ge "$jobs" ]; do sleep 1; done
    done
    wait
    wall=$(( $(date +%s) - started ))
    {
        echo "# Script sweep $(date '+%Y-%m-%d %H:%M'), $seconds s, seeds: $seeds, $wall s wall"
        echo
        echo "| map | seed | mode | errors | loop kills | missing built-ins | objectives done | notifies | stalls | forced | level change | exit |"
        echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
        for r in "$out"/*.txt; do
            b=$(basename "$r" .txt)
            m=${b%-s*}; rest=${b#*-s}; seed=${rest%%-*}; tag=${rest#*-}
            echo "| $m | $seed | $tag | $(field "$r" runtime_errors) | $(field "$r" loop_guard_kills) | $(field "$r" unimplemented_builtins) | $(count_done_objectives "$r") | $(field "$r" notifies) | $(field "$r" stalls) | $(field "$r" forced) | $(field "$r" level_change) | $(field "$r" exit) |"
        done
        echo
        echo "## Errors: count, run, site"
        for r in "$out"/*.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[errors\]/ { s = 1; next } /^\[/ { s = 0 } s { print "- " b ": " $0 }' "$r"
        done
        echo
        echo "## Unimplemented built-ins: count, run, name"
        for r in "$out"/*.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[builtins\]/ { s = 1; next } /^\[/ { s = 0 } s { print "- " b ": " $0 }' "$r"
        done
        echo
        echo "## Stalls: autoplay runs that ran out of triggers and kill gates"
        for r in "$out"/*-auto.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[autoplay\]/ { s = 1; next } /^\[/ { s = 0 } s && /stall:/ { print "- " b ": " $0 } s && /^    / { print "  " $0 }' "$r"
        done
    } > "$out/summary.md"
    echo "summary: $out/summary.md"
}
do_$mode
