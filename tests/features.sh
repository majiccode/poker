#!/bin/sh
set -eu
binary="$1"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/poker-features.XXXXXX")
job=''
trap 'if [ -n "$job" ]; then wait "$job" || :; fi; rm -rf "$scratch"' EXIT HUP INT TERM
contains() {
    case "$(< "$1" tr '\n' '|')" in *"$2"*) ;; *) printf 'Missing output: %s\n' "$2" >&2; exit 1;; esac
}
"$binary" catalog build --trials 100 --jobs 4 --cache-dir "$scratch/catalog" > "$scratch/catalog.tsv" 2> "$scratch/catalog.log" &
job=$!
wait "$job"
job=''
contains "$scratch/catalog.log" 'ready=169 pending=0 errors=0'
contains "$scratch/catalog.tsv" 'premium (educational)'
"$binary" catalog show --hero AsAh --trials 100 --cache-dir "$scratch/catalog" > "$scratch/lookup" 2> "$scratch/lookup.log"
contains "$scratch/lookup.log" 'cache=hit'
contains "$scratch/lookup.log" 'Hero AsAh -> AA -> AcAd'
"$binary" catalog build --trials 100 --jobs 1 --cache-dir "$scratch/catalog" > "$scratch/catalog2.tsv" 2> "$scratch/catalog2.log"
cmp "$scratch/catalog.tsv" "$scratch/catalog2.tsv"
contains "$scratch/catalog2.log" 'cache_hits=169 stored=0'
if "$binary" catalog show --hero AsKh --trials 101 --cache-dir "$scratch/catalog" > "$scratch/miss" 2> "$scratch/miss.log"; then exit 1; fi
contains "$scratch/miss" 'pending'
if "$binary" catalog build --range AsKh --trials 100 --cache-dir "$scratch/catalog" > "$scratch/error" 2>&1; then exit 1; fi

"$binary" table --players 3 --hands 2 --trials 100 --session smoke \
    --history-dir "$scratch/hands" --cache-dir "$scratch/table-cache" > "$scratch/table"
contains "$scratch/table" 'Session net across all seats: 0.000000000000 (conserved).'
contains "$scratch/table" 'smoke_hand2_showdown'
contains "$scratch/table" '[??] [??]'
"$binary" history stats --history-dir "$scratch/hands" > "$scratch/stats"
tab=$(printf '\t')
contains "$scratch/stats" "total${tab}10${tab}2${tab}"
if "$binary" table --players 3 --hands 2 --trials 100 --session smoke --history-dir "$scratch/hands" --no-cache > "$scratch/error" 2>&1; then exit 1; fi

"$binary" analyze --hero AsKh --board QsJh2c --pot 100 --call 25 --stack 500 \
    --villain 'tight,UTG,500,10,0.5' --range 'QQ,JJ,AK' --trials 100 --no-cache \
    --realization 0.7 --implied-win 40 --reverse-implied 15 --rake-percent 5 --rake-cap 3 > "$scratch/odds"
contains "$scratch/odds" 'Call-only sensitivity'
contains "$scratch/odds" 'modeled current-pot rake 3.00'
contains "$scratch/odds" 'Baseline EV/mix below ignores'
if "$binary" analyze --hero AsKh --pot 100 --call 25 --stack 500 \
    --villain 'tight,UTG,500,10,0.5' --realization 2 --no-cache > "$scratch/error" 2>&1; then exit 1; fi
printf 'Catalog, background worker, local table, and sensitivity CLI tests passed.\n'
