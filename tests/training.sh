#!/bin/sh
set -eu
binary="$1"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/poker-training.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
contains() {
    case "$(< "$1" tr '\n' '|')" in *"$2"*) ;; *) printf 'Missing output: %s\n' "$2" >&2; exit 1;; esac
}
printf 'invalid\ncall nan\nfold 30\ncall 70\nfold 20\n' |
    "$binary" train run --questions 3 --seed 42 --session baseline --progress-dir "$scratch/progress" > "$scratch/run"
contains "$scratch/run" 'Invalid answer.'
contains "$scratch/run" 'Exact equity'
contains "$scratch/run" 'mean normalized EV regret'
"$binary" train stats --progress-dir "$scratch/progress" > "$scratch/stats"
contains "$scratch/stats" 'baseline.train: 3 questions'
contains "$scratch/stats" 'Overall: 3 questions'
if "$binary" train run --questions 1 --session baseline --progress-dir "$scratch/progress" < /dev/null > "$scratch/error" 2>&1; then exit 1; fi
if "$binary" train run --questions 2 --session partial --progress-dir "$scratch/progress" < /dev/null > "$scratch/cancel" 2>&1; then exit 1; fi
contains "$scratch/cancel" 'no partial session saved'
test ! -e "$scratch/progress/partial.train"
printf 'unexpected\n' >> "$scratch/progress/baseline.train"
if "$binary" train stats --progress-dir "$scratch/progress" > "$scratch/error" 2>&1; then exit 1; fi
contains "$scratch/error" 'no aggregate reported'
printf 'Training persistence and feedback tests passed.\n'
