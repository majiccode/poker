#!/bin/sh
set -eu
binary="$1"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/poker-cli.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

"$binary" --help > "$scratch/help"
"$binary" profile record tight --db "$scratch/profiles.tsv" --pfr --folded --raises 1 > "$scratch/profile"
"$binary" profile show tight --db "$scratch/profiles.tsv" > "$scratch/profile"

study() {
    "$binary" analyze --hero AsAh --board '2c 3d 7h 9s Jc' --pot 100 --call 20 \
        --stack 400 --villain 'tight,UTG,400,10,0.4' --range KcKd \
        --trials 100 --seed 42 --cache-dir "$scratch/cache" --history-dir "$scratch/hands" "$@"
}
study --save hand1 --profit 80 > "$scratch/first"
study > "$scratch/second"
"$binary" history show hand1 --history-dir "$scratch/hands" > "$scratch/hand"
"$binary" history stats --history-dir "$scratch/hands" > "$scratch/stats"
"$binary" analyze --hero AsKh --pot 10 --call 3 --stack 100 \
    --villain 'tight,UTG,100,20,0.5' --profile tight --action raise \
    --db "$scratch/profiles.tsv" --trials 100 --no-cache > "$scratch/raised"

contains() {
    case "$(< "$1" tr '\n' '|')" in *"$2"*) ;; *) printf 'Missing output: %s\n' "$2" >&2; exit 1;; esac
}
contains "$scratch/first" 'Equity: 100.00%'
contains "$scratch/first" 'miss (stored simulation)'
contains "$scratch/second" 'hit (reused simulation)'
contains "$scratch/stats" 'AA'
contains "$scratch/raised" 'preflop raise'

if study --save hand1 > "$scratch/error" 2>&1; then exit 1; fi
if study --hero AsAs > "$scratch/error" 2>&1; then exit 1; fi
if study --call NaN > "$scratch/error" 2>&1; then exit 1; fi
if study --pot 1e308 --call 1e308 --stack 1e308 > "$scratch/error" 2>&1; then exit 1; fi
if study --villain 'short,BB,1,10,0.5' > "$scratch/error" 2>&1; then exit 1; fi
if study --profit 20 > "$scratch/error" 2>&1; then exit 1; fi
if study --save ../escape > "$scratch/error" 2>&1; then exit 1; fi
if "$binary" analyze --hero AsKh --board 'QsJh2c' --pot 10 --villain 'x,UTG,100,10,0.5' --action raise --no-cache > "$scratch/error" 2>&1; then exit 1; fi
printf 'CLI integration tests passed.\n'
