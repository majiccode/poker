#!/bin/sh
set -eu
binary="$1"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/poker-model.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
contains() {
    case "$(< "$1" tr '\n' '|')" in *"$2"*) ;; *) printf 'Missing output: %s\n' "$2" >&2; exit 1;; esac
}
"$binary" model import --input examples/actions-train.tsv --dataset "$scratch/data.tsv" > "$scratch/import"
contains "$scratch/import" 'added=6 skipped=0 total=6'
cp "$scratch/data.tsv" "$scratch/before.tsv"
"$binary" model import --input examples/actions-train.tsv --dataset "$scratch/data.tsv" > "$scratch/reimport"
contains "$scratch/reimport" 'added=0 skipped=6 total=6'
cmp "$scratch/data.tsv" "$scratch/before.tsv"
"$binary" model predict --dataset "$scratch/data.tsv" --player alice --street river --position BTN \
    --players 2 --pot 100 --call 25 --stack 500 > "$scratch/prediction"
contains "$scratch/prediction" 'own_context=3 population_total=3 population_context=2'
contains "$scratch/prediction" 'fold: probability=0.386946386946'
contains "$scratch/prediction" 'call: probability=0.344988344988'
contains "$scratch/prediction" 'raise: probability=0.268065268065'
"$binary" model evaluate --dataset "$scratch/data.tsv" --test examples/actions-test.tsv > "$scratch/evaluation"
contains "$scratch/evaluation" 'train=6 test=3 disjoint hands'
contains "$scratch/evaluation" 'baseline: brier='
contains "$scratch/evaluation" 'model: brier='
cmp "$scratch/data.tsv" "$scratch/before.tsv"
if "$binary" model evaluate --dataset "$scratch/data.tsv" --test "$scratch/data.tsv" > "$scratch/error" 2>&1; then exit 1; fi
contains "$scratch/error" 'hand_id overlap'

header='hand_id\tdecision_id\tplayer\tstreet\tposition\tplayers\tpot\tto_call\tstack\taction\n'
printf "$header" > "$scratch/conflict.tsv"
printf 'demo1\td1\talice\triver\tBTN\t2\t100\t25\t500\traise\n' >> "$scratch/conflict.tsv"
if "$binary" model import --dataset "$scratch/data.tsv" --input "$scratch/conflict.tsv" > "$scratch/error" 2>&1; then exit 1; fi
cmp "$scratch/data.tsv" "$scratch/before.tsv"
printf "$header" > "$scratch/invalid.tsv"
printf 'bad\td1\tx\triver\tBTN\t2\tNaN\t25\t500\tfold\n' >> "$scratch/invalid.tsv"
if "$binary" model import --dataset "$scratch/data.tsv" --input "$scratch/invalid.tsv" > "$scratch/error" 2>&1; then exit 1; fi
cmp "$scratch/data.tsv" "$scratch/before.tsv"
printf "$header" > "$scratch/empty.tsv"
"$binary" model predict --dataset "$scratch/empty.tsv" --player unknown --street river --position BTN \
    --players 2 --pot 100 --call 25 --stack 500 > "$scratch/fallback"
contains "$scratch/fallback" 'global=uniform context=global prior player=population context prior'
ln -s "$scratch/data.tsv" "$scratch/link.tsv"
if "$binary" model predict --dataset "$scratch/link.tsv" --player alice --street river --position BTN \
    --players 2 --pot 100 --call 25 --stack 500 > "$scratch/error" 2>&1; then exit 1; fi
printf 'Context model, idempotent import, held-out evaluation, and validation tests passed.\n'
