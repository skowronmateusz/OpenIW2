#!/bin/bash
# Headless mission-script sweep over the SP campaign maps.
#
#   IW2_MAIN=<main dir> tools/script_sweep.sh --full  [--seeds "1 2 3"] [--seconds 600] [--jobs 4] [--out DIR] [--maps "a b"] [--force]
#   IW2_MAIN=<main dir> tools/script_sweep.sh --smoke [--bin build/iw2game]          (the ctest subset; exit 1 on any finding)
#   IW2_MAIN=<main dir> tools/script_sweep.sh --determinism [--maps "a b"] [--seconds 120]
#   IW2_MAIN=<main dir> tools/script_sweep.sh --reload --maps "a b" [--seconds 600]  (reload every autosave, compare with the unbroken run)
#   IW2_MAIN=<main dir> tools/script_sweep.sh --carry [--maps "a b"] [--seconds 200]    (autoplay each map, end it with the mission-complete call, then start the next map with the carried player and compare the inventory)
#   IW2_MAIN=<main dir> tools/script_sweep.sh --make-firerate-baseline --out DIR      (print tools/firerate-baseline.txt lines from the autoplay reports in DIR)
#   --full also writes the fire-rate flags (AI shots per combat minute, silent shooters) and the inventory checks of the maps whose autoplay run ended in a
#   persistent level change into summary.md; see docs/debug-flags.md "Fire rate" and "Inventory across level changes".
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
    --full | --smoke | --determinism | --summarize | --reload | --carry) mode="${1#--}" ;;
    --make-targets) mode="make_targets" ;;
    --make-firerate-baseline) mode="make_firerate_baseline" ;;
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
    # Long campaign maps: the autoplay bot needs more simulated time than the default to reach the last stages (hill400_assault ends
    # at about 1740 s of level time, moscow at about 2500 s); a shorter --seconds is raised for those maps' autoplay runs only.
    if [ "$tag" = auto ] || [ "${tag#reload-}" != "$tag" ]; then
        case "$map" in
        hill400_assault) [ "$secs" -ge 1800 ] || secs=1800 ;;
        moscow) [ "$secs" -ge 2600 ] || secs=2600 ;;
        esac
    fi
    local base="$out/$map-s$seed-$tag"
    # Autoplay runs keep the player state a persistent level change would hand over (checked by carry_checks).
    [ "$tag" = auto ] && set -- "$@" --carry-out "$base.carry"
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
    carry_checks
    write_summary
}
# --carry: autoplay each map and end it 20 s before --seconds (iw2game --end-level: what a finished mission calls, so the real next map and the
# persistent flag come out), then the carry checks. Use a short --seconds (120..300): the bot picks up what it finds meanwhile.
do_carry() {
    started=$(date +%s)
    for m in $maps; do
        run "$m" 1 "$seconds" auto --autoplay $force --end-level $(( seconds > 40 ? seconds - 20 : 20 )) &
        while [ "$(jobs -r | wc -l)" -ge "$jobs" ]; do sleep 1; done
    done
    wait
    wall=$(( $(date +%s) - started ))
    carry_checks
    write_summary
}
# For every autoplay run that ended in a persistent level change to a map of the campaign: start that map with the carried player
# state (iw2game --carry-from) and compare it with the inventory 8 s in, after the start scripts (report section [inventory]).
carry_checks() {
    local c m seed next
    for c in "$out"/*-s*-auto.carry; do
        [ -f "$c" ] || continue
        m=$(basename "$c" .carry); m=${m%-s*}
        seed=$(basename "$c" .carry); seed=${seed#*-s}; seed=${seed%%-*}
        next=$(field "${c%.carry}.txt" level_change); next=${next%% *}
        [ -n "$next" ] || continue
        all_maps | grep -qx "$next" || continue
        run "$next" "$seed" 30 "carry-from-$m" --carry-from "$c" --inventory-check 8 &
        while [ "$(jobs -r | wc -l)" -ge "$jobs" ]; do sleep 1; done
    done
    wait
}
# Fire-rate flags of one report (tools/firerate-baseline.txt: `map team shots-per-combat-minute` of a clean autoplay run):
#   NO-FIRE:team     a team with at least 120 combat seconds fired no shot
#   LOW-RATE:team    an autoplay run whose rate is under 65% of the baseline of that map and team (the animation-note bug cut it to 20..56%)
#   SILENT:n         10 or more silent-shooter stretches (an enemy in sight for 10 s or more, no shot)
fire_flags() {
    local r=$1 map=$2 tag=$3 f="" t secs shots rate base v
    for t in axis allies; do
        secs=$(field "$r" ai_combat_secs_$t); shots=$(field "$r" ai_shots_$t); rate=$(field "$r" ai_shots_per_min_$t)
        [ -n "$secs" ] || continue
        if awk -v s="$secs" -v n="$shots" 'BEGIN { exit !(s >= 120 && n == 0) }'; then f="$f NO-FIRE:$t"; fi
        if [ "$tag" = auto ] && [ -f "$here/tools/firerate-baseline.txt" ]; then
            base=$(awk -v m="$map" -v t="$t" '$1 == m && $2 == t { print $3; exit }' "$here/tools/firerate-baseline.txt")
            if [ -n "$base" ] && awk -v s="$secs" -v r="$rate" -v b="$base" 'BEGIN { exit !(s >= 120 && r < 0.65 * b) }'; then f="$f LOW-RATE:$t($rate<$base)"; fi
        fi
    done
    v=$(field "$r" silent_shooters)
    [ "${v:-0}" -ge 10 ] && f="$f SILENT:$v"
    echo "${f# }"
}
do_make_firerate_baseline() {
    echo "# Clean autoplay AI shots per combat minute (seed 1): '<map> <team> <rate>'. tools/script_sweep.sh flags a run under 65% of it."
    for r in "$out"/*-s1-auto.txt; do
        [ -f "$r" ] || continue
        m=$(basename "$r" .txt); m=${m%-s*}
        for t in axis allies; do
            awk -v m="$m" -v t="$t" '$1 == "ai_combat_secs_" t { s = $2 } $1 == "ai_shots_per_min_" t { r = $2 } END { if (s >= 120 && r > 0) printf "%s %s %.1f\n", m, t, r }' "$r"
        done
    done
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
        echo "| map | seed | mode | errors | loop kills | missing built-ins | objectives done | stages | notifies | stalls | softlock | short | dead waits | forced | level change | axis shots/min | allies shots/min | silent | fire flags | inventory | exit |"
        echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
        for r in "$out"/*-s*-*.txt; do
            b=$(basename "$r" .txt)
            m=${b%-s*}; rest=${b#*-s}; seed=${rest%%-*}; tag=${rest#*-}
            stages="$(field "$r" targets_reached)/$(field "$r" targets_total)"
            echo "| $m | $seed | $tag | $(field "$r" runtime_errors) | $(field "$r" loop_guard_kills) | $(field "$r" unimplemented_builtins) | $(count_done_objectives "$r") | $stages | $(field "$r" notifies) | $(field "$r" stalls) | $(field "$r" softlock_groups) | $(field "$r" short_spawners) | $(field "$r" dead_spawn_waits) | $(field "$r" forced) | $(field "$r" level_change) | $(field "$r" ai_shots_per_min_axis) | $(field "$r" ai_shots_per_min_allies) | $(field "$r" silent_shooters) | $(fire_flags "$r" "$m" "$tag") | $(field "$r" inventory_lost)/$(field "$r" inventory_unexplained) | $(field "$r" exit) |"
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
        echo "## Soft-locks: AI groups with spawners nothing can start, and flood spawners that stopped short"
        for r in "$out"/*-s*-*.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[softlock\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s && /^(SOFTLOCK|SHORT-SPAWNER|DEAD-SPAWN-WAIT)/ { print "- " b ": " $0; g = ($1 == "SOFTLOCK") } s && g && /^    / { print "  " $0 } s && /^(group|spawner|levelwait|untouched)/ { g = 0 }' "$r"
        done
        echo
        echo "## Fire rate: flagged runs (AI shots per combat minute; flags in docs/debug-flags.md) and their silent shooters"
        for r in "$out"/*-s*-*.txt; do
            b=$(basename "$r" .txt); m=${b%-s*}; rest=${b#*-s}; tag=${rest#*-}
            fl=$(fire_flags "$r" "$m" "$tag")
            [ -n "$fl" ] || continue
            echo "- $b: $fl"
            awk '/^\[firerate\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s && /^SILENT-SHOOTER/ && n++ < 5 { print "    " $0 }' "$r"
        done
        echo
        echo "## Inventory across level changes (carried player state vs the inventory 8 s into the next map)"
        for r in "$out"/*-s*-carry-from-*.txt; do
            [ -f "$r" ] || continue
            echo "- $(basename "$r" .txt): lost weapons $(field "$r" inventory_lost), unexplained $(field "$r" inventory_unexplained)"
            awk '/^\[inventory\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s { print "    " $0 }' "$r"
        done
        echo
        echo "## Stalls: autoplay runs that ran out of triggers and kill gates"
        for r in "$out"/*-auto.txt; do
            awk -v b="$(basename "$r" .txt)" '/^\[autoplay\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s && /stall:/ { print "- " b ": " $0 } s && /^    / { print "  " $0 }' "$r"
        done
    } > "$out/summary.md"
    echo "summary: $out/summary.md"
}
# One autoplay run with its autosaves kept, then one autoplay run from each autosave file. An autosave name the level reuses keeps its
# last save. A level notify the unbroken run reached after the save's time and the reloaded run did not is a finding (exit 1); so is
# a reload that fails. Spawner notifies are left out, they depend on the bot's timing. Bot timing still differs between the runs,
# so a finding needs a look; the run proves nothing about stages the unbroken run did not reach either.
late_notifies() { awk -v t="$2" '/^\[notifies\]/ { s = 1; next } /^\[[a-z_]+\]$/ { s = 0 } s && $1 + 0 > t && !/ spawner/ { $1 = ""; print }' "$1" | sort -u; }
do_reload() {
    local bad=0 m sv name t missing
    for m in $maps; do
        run "$m" 1 "$seconds" auto --autoplay 25 --save-dir "$out/$m-saves" || bad=1
        for sv in "$out/$m-saves"/autosave/*.svg; do
            [ -f "$sv" ] || continue
            name=$(basename "$sv" .svg)
            if ! run "$m" 1 "$seconds" "reload-$name" --autoplay 25 --load "$sv"; then echo "$m $name: reload run failed"; bad=1; continue; fi
            t=$(awk '/^loaded .*time [0-9]+ ms/ { for (i = 1; i < NF; ++i) if ($i == "time") print $(i + 1) / 1000; exit }' "$out/$m-s1-reload-$name.log")
            missing=$(comm -23 <(late_notifies "$out/$m-s1-auto.txt" "$t") <(late_notifies "$out/$m-s1-reload-$name.txt" "$t") | tr '\n' ';')
            if [ -n "$missing" ]; then echo "$m $name (time $t): not reached after the reload:$missing"; bad=1; else echo "$m $name (time $t): ok"; fi
        done
    done
    return $bad
}
do_$mode
