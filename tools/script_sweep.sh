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
seconds=900
jobs=4
out=""
maps=""
force=""
while [ $# -gt 0 ]; do
    case "$1" in
    --full | --smoke | --determinism | --summarize) mode="${1#--}" ;;
    --make-targets) mode="make_targets" ;;
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
# Per-map progress targets (tools/campaign-targets.txt, `map: notify ...`): every run reports targets_reached / targets_total.
targets_file="$here/tools/campaign-targets.txt"
targets_opt=""
[ -f "$targets_file" ] && targets_opt="--targets $targets_file"
# run <map> <seed> <seconds> <tag> [extra iw2game options]: one headless run, report + log in $out.
run() {
    local map=$1 seed=$2 secs=$3 tag=$4
    shift 4
    local base="$out/$map-s$seed-$tag"
    IW2_SCRIPT_SEED=$seed "$bin" "$IW2_MAIN" "$map" "$secs" --god --answer-menus "$answers" --report "$base.txt" $targets_opt "$@" > "$base.log" 2>&1
    local rc=$?
    [ $rc -eq 0 ] || echo "exit $rc" >> "$base.txt"
    return $rc
}
field() { awk -v k="$2" '$1 == k { $1 = ""; sub(/^ /, ""); print; exit }' "$1"; }
count_done_objectives() { awk '/^\[objectives\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s && $2 == 3 { n++ } END { print n + 0 }' "$1"; }

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
    # toujane has a script loop the loop guard kills; with the wall-clock guard the kill point, and so the run, differed.
    run toujane 1 60 plain
    run toujane 1 60 plain2
    if ! diff -q "$out/toujane-s1-plain.txt" "$out/toujane-s1-plain2.txt" > /dev/null; then
        echo "SMOKE FAIL: toujane is not deterministic for seed 1"; diff "$out/toujane-s1-plain.txt" "$out/toujane-s1-plain2.txt" | head -20; rc=1
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
    write_summary
}
do_summarize() { wall=0; seconds="?"; seeds="?"; write_summary; }
# --make-targets --out DIR: print a campaign-targets file from the reports in DIR: per map, every level notify any run reached
# (first-occurrence order of the first run that reached it), without the events --autoplay-force injected. These are
# "stages reached headless so far", a regression floor; a real play's IW2_NOTIFY_LOG=level is the intended source.
do_make_targets() {
    echo "# Per-map progress targets: '<map>: <level notify> ...'. iw2game --targets FILE reports targets_reached / targets_total."
    echo "# Generated by tools/script_sweep.sh --make-targets from headless runs ($(date '+%Y-%m-%d')); replace a line with the"
    echo "# IW2_NOTIFY_LOG=level notifies of a complete human play of that map when one is recorded."
    for m in $(ls "$out"/*-s*-*.txt 2>/dev/null | sed 's#.*/##; s/-s[0-9]*-.*//' | sort -u); do
        awk -v m="$m" '
            FNR == 1 { s = 0 }
            /^\[notifies\]/ { s = 1; next } /^\[autoplay\]/ { s = 2; next } /^\[[a-z_]+\]$/ { s = 0 }
            s == 1 { n = $0; sub(/^[0-9.]+ /, "", n); if (!(n in seen)) { seen[n] = 1; order[++k] = n } }
            s == 2 && /forced level notify / { n = $0; sub(/.*forced level notify /, "", n); forced[n] = 1 }
            END { printf "%s:", m; for (i = 1; i <= k; i++) if (!(order[i] in forced)) printf " %s;", order[i]; print "" }' "$out"/$m-s*-*.txt
    done
}
write_summary() {
    {
        echo "# Script sweep $(date '+%Y-%m-%d %H:%M'), $seconds s, seeds: $seeds, $wall s wall"
        echo
        echo "| map | seed | mode | errors | loop kills | missing built-ins | objectives done | stages | notifies | stalls | forced | level change | exit |"
        echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
        for r in "$out"/*-s*-*.txt; do
            b=$(basename "$r" .txt)
            m=${b%-s*}; rest=${b#*-s}; seed=${rest%%-*}; tag=${rest#*-}
            stages="$(field "$r" targets_reached)/$(field "$r" targets_total)"
            echo "| $m | $seed | $tag | $(field "$r" runtime_errors) | $(field "$r" loop_guard_kills) | $(field "$r" unimplemented_builtins) | $(count_done_objectives "$r") | $stages | $(field "$r" notifies) | $(field "$r" stalls) | $(field "$r" forced) | $(field "$r" level_change) | $(field "$r" exit) |"
        done
        echo
        echo "## Missed stages per autoplay run"
        for r in "$out"/*-auto.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[targets_missed\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s { printf "%s%s", (n++ ? " " : "- " b ": "), $0 } END { if (n) print "" }' "$r"
        done
        echo
        echo "## Errors: count, run, site"
        for r in "$out"/*-s*-*.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[errors\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s { print "- " b ": " $0 }' "$r"
        done
        echo
        echo "## Unimplemented built-ins: count, run, name"
        for r in "$out"/*-s*-*.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[builtins\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s { print "- " b ": " $0 }' "$r"
        done
        echo
        echo "## Stalls: autoplay runs that ran out of triggers and kill gates"
        for r in "$out"/*-auto.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[autoplay\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s && /stall:/ { print "- " b ": " $0 } s && /^    / { print "  " $0 }' "$r"
        done
    } > "$out/summary.md"
    echo "summary: $out/summary.md"
}
do_$mode
