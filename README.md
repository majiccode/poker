# Poker

A runnable C11 Texas Hold'em **offline study application**. Estimate your hand's showdown equity against plausible opponent ranges, compare simplified chip EVs, and persist simulations, reviewed hands, and opponent observations.

This is an initial training engine, **not a GTO solver, autonomous poker bot, or guaranteed winning strategy**. It does not capture live tables, read hidden cards, click buttons, or provide a live decision overlay. Use completed hands, hypothetical scenarios, or permitted training environments.

## Learn Opponent Responses

The contextual response model learns **fold/call/raise frequencies when facing a bet** from completed-hand decision snapshots. It is separate from the equity/action engine: it does not infer hole cards or automatically supply fold probabilities to an EV recommendation.

```sh
./build/poker model import --input examples/actions-train.tsv --dataset actions.tsv
./build/poker model predict --dataset actions.tsv --player alice \
  --street river --position BTN --players 2 --pot 100 --call 25 --stack 500
./build/poker model evaluate --dataset actions.tsv --test examples/actions-test.tsv
```

The examples are **synthetic format demonstrations**, not evidence of model quality. Native GGPoker hand-history parsing and screenshot ingestion are not implemented. Export reviewed decision snapshots into the exact tab-separated format below; include ordinary calls and folds, not only interesting showdowns:

```text
hand_id decision_id player street position players pot to_call stack action
```

Those ten fields must be separated by actual tabs, with the exact header shown in `examples/actions-train.tsv`. One row is one decision made while facing a bet. `hand_id` must be globally unique across source games/sites; `decision_id` is unique within a hand across all players. Names/IDs accept 1..48 ASCII letters, digits, underscores and hyphens. Street is preflop/flop/turn/river; position uses the supported table positions; players is the number of active players at that decision, 2..9. Pot includes the outstanding bet; to-call and remaining stack are positive and call cannot exceed stack. Action is fold/call/raise. Check/bet opportunities are not included in this model.

Files require newline-terminated LF lines, no embedded NULs, finite positive decimal chip amounts at most one trillion, and at most 100,000 decisions. Import validates the entire source and existing target before atomic replacement. Matching semantic rows are skipped on reimport; conflicting IDs fail without overwriting the dataset. Files must be regular non-symlink files. New merged data is owner-only; single-writer use is required and containing-directory fsync is not implemented. Custom datasets are not automatically ignored by Git; do not commit private opponent data.

Context combines street, position, active-player bucket (2, 3..5, 6..9), call price `to_call/(pot+to_call)` (at most .2, at most .333333, otherwise), and stack-to-terminal-pot ratio (at most 2, at most 6, otherwise). This loses information about board texture, previous actions, exact sizing, effective opponent stacks, and position availability. Input validation does not reconstruct a betting history or verify legal raises.

For each action, `g` is the other players' global count with a uniform Dirichlet prior; `c` shrinks their matched context toward `g`; the queried player's matched context shrinks toward `c`:

```text
g = (population_global_action_count + 1) / (population_global_count + 3)
c = (population_context_action_count + 20*g) / (population_context_count + 20)
p = (player_context_action_count + 10*c) / (player_context_count + 10)
```

All queried-player rows are excluded from population counts. With little data the model stays near its baseline; it prints matched counts and fallback levels. The fixed strengths 20 and 10 are initial tuning choices, not learned optimal values. This is empirical hierarchical smoothing: global counts include contextual rows, and prior uncertainty is not propagated. Reported marginal posterior SD is conditional on the estimated prior, not a full uncertainty interval. Decisions within a hand are correlated, so row counts are not independent effective sample sizes.

`evaluate` requires **disjoint hand IDs** in training and test files, rejecting overlap even if decisions differ. It never fits to test labels. It reports multiclass Brier score (0..2, lower better), natural-log loss (lower better), accuracy (0..1, higher better), and fold-probability reliability bins against a global-frequency baseline. Predictions use only training data. Evaluation is capped at 100 million train/test comparisons to bound the current scan-based implementation.

Use time-separated completed sessions and keep all decisions from each hand in one split. Avoid tuning repeatedly on the same test set. Hand disjointness does not ensure time separation or rule out leakage introduced during data preparation. Tiny reliability bins are noisy; the report has no cluster-aware confidence intervals. An improved held-out prediction score is evidence about responses, **not** about profit or accurate hidden-card ranges. Revealed cards are selection-biased, so a future range model must account for missing folded cards rather than treating showdowns as representative.

## Measure Your Learning

Practice estimating equity and choosing call/fold **before** seeing the answer:

```sh
./build/poker train run --questions 10 --seed 42 --session baseline
# Enter answers such as: call 35 or fold 20
./build/poker train stats

# After studying, use a fresh seed for a separate assessment:
./build/poker train run --questions 10 --seed 314 --session followup
./build/poker train stats
```

Each drill is a final heads-up river decision against an explicit assumed range, with call/fold as the only available actions, no rake, and no future betting. The benchmark enumerates **every legal opponent combination**, weighted by the range, so there is no Monte Carlo sampling noise in the answer. It accounts for blockers and split pots. It is exact for this exercise, not proof that the assumed range describes a real opponent. Raises are excluded by the exercise, not assumed inferior in real poker.

Feedback explains exact equity, the price's break-even threshold, call EV, the correct restricted action, and the EV cost of your choice. Persistent metrics report:

- Decision accuracy against the restricted benchmark.
- Mean normalized EV regret: missed expected chips divided by the current pot, averaged over questions. Lower is better; this measures decision quality, not lucky outcomes or actual money won.
- Equity mean absolute error in percentage points. Lower is better.

Sessions are immutable `.poker-training/SESSION.train` records. Use `--progress-dir PATH` for another directory. The versioned records retain seed, question count, and answers; questions and exact benchmarks are deterministically regenerated when checking stats. Changes to generation/evaluation semantics must version this format rather than silently regrade old answers. Invalid records are rejected. Interrupted sessions do not save partial answers. The parent directory must exist for a custom path; files are private to the current user. Atomic publication does not include directory fsync, so back up important records.

Question sets exclude near-indifferent decisions with call EV within 0.5 chips of zero and sample synthetic ranges/boards, not a representative distribution of real river spots. Session summaries are sorted by filename, not time. Fresh seeds reduce memorization, but repeated seeds or reading feedback can contaminate an assessment. These metrics are useful evidence of the practiced skill, not a causal test of an online win-rate improvement. For stronger evidence, use larger unseen assessments and a consistent difficulty distribution, then separately evaluate representative completed hands after rake.

## Build

Requires a C compiler, Make, and a POSIX environment. No third-party runtime dependencies. macOS: install Apple's command-line tools with `xcode-select --install`. Linux: install your distribution's C compiler and Make.

```sh
make
./build/poker --help
make test
make sanitize
```

The executable is `build/poker`. GitHub Actions builds and tests on macOS and Linux. Tests include all 2,598,960 five-card hands, tie-breaking, blockers, multiway equity, persistent data validation, and CLI integration.

## Starting-Hand Catalog

Build reusable equity estimates for all **169 starting-hand classes** with parallel workers. The shell can run this in the background while you use other commands:

```sh
./build/poker catalog build --trials 20000 --jobs 4 \
  > /tmp/poker-catalog.tsv 2> /tmp/poker-catalog.log &

./build/poker catalog show --hero AsKh --trials 20000
./build/poker catalog show --trials 20000
```

`build` fills missing cache entries and reuses completed work. `show` only reads cache results; it never starts a simulation. Missing entries are reported as pending. Use matching seed, trials, ranges, and opponent count when looking up a catalog. Jobs affect concurrency, not results. Stop a background job with your shell's job control; completed entries remain reusable.

By default, the catalog compares each class to one random opponent. Study a tight range or premium class instead:

```sh
./build/poker catalog build --range 'AA,KK,QQ,JJ,AK' --trials 10000 --jobs 4
./build/poker catalog show --hero AsQh --range 'AA,KK,QQ,JJ,AK' --trials 10000

./build/poker catalog build --range QQ --trials 10000 --jobs 4
./build/poker catalog show --hero AsKh --range QQ --trials 10000
```

Use `--opponents 1..8` for equal opponent ranges and `--range-percent` for a heuristic percentage range. Full results are sorted by simulated equity. **AA, KK, QQ, JJ, AKs, and AKo** receive educational premium labels, not unconditional instructions to play them. Position, price, opponents, and effective stacks still matter. A high random-opponent ranking is not a GTO opening chart.

Catalog heroes use canonical suits: pairs/offsuit `c,d`, suited `c,c`. Actual suits map to the same class only when opponent ranges are suit-symmetric. Explicit physical combos are rejected in catalog mode; percentage ranges can cut suit classes and are labeled canonical estimates. Use `analyze` for exact physical-card matchups, blockers, boards, and pot odds. Preflop canonical cache entries are deliberately separate from physical-hand entries. Estimates built with low trial counts are noisy; increase trials for meaningful comparisons.

## Watch A Local Table

```sh
./build/poker table --players 6 --hands 3 --trials 1000 \
  --session practice1 --delay-ms 750

./build/poker history show practice1_hand1_showdown
./build/poker history stats
```

This automatically deals and renders a **local terminal demo**, showing hero's cards, rotating positions, pot, and visible board. Opponent holes stay hidden until showdown. Each street's equity uses only hero cards and the currently visible board, never the hidden deal. Each hand saves four unlabeled street snapshots and a labeled showdown with exact revealed holes and actual net chip result.

Everyone antes one chip and checks down. Stacks reset to 100 each hand. There are no blinds, wagering decisions, opponent strategy, or interactive actions yet; this demonstrates dealing, visibility, chip conservation, and persistent tracking, not winning play. Use a new session ID for each run. Interrupted sessions cannot resume. `history stats` counts snapshots, not distinct hands; only the showdown snapshot contributes an observed outcome.

This is not a GGPoker window attachment or real-time advice overlay. For external games, use completed hand records or post-session screenshots for review. This release does not implement an external capture/import pipeline.

## Review A Hand

```sh
./build/poker analyze \
  --hero AsKh --board 'Qs Jh 2c' \
  --pot 100 --call 25 --stack 500 \
  --villain 'alice,BTN,500,25,0.4' \
  --range 'QQ,JJ,AK,AQs,KQs' \
  --trials 20000 --seed 42 \
  --save session1_hand12 --profit -25
```

- Cards use ranks `23456789TJQKA` and suits `cdhs`. Hero needs two cards; board needs zero, three, four, or five. Cards cannot repeat.
- Repeat `--villain` for up to eight opponents. Fields are name, position, remaining stack, range percentage, and fold probability from 0 to 1.
- Positions: `UTG`, `UTG1`, `UTG2`, `LJ`, `HJ`, `CO`, `BTN`, `SB`, `BB`. Position by itself does not modify a supplied range.
- `--range` applies to the preceding villain. Supported classes: `QQ`, `AKs`, `AKo`, `AK`, or specific combinations `AsKh`. No `+`, interval, or weight syntax yet. Multiple classes are comma-separated and deduplicated.
- Without an explicit range, the engine uses a documented heuristic top percentage, not solved opening charts.
- `--pot` includes all chips in the pot now, including outstanding bets. `--call` is the additional amount hero must contribute. Stacks are remaining chips before the decision.
- `--bet` is hero's total additional contribution, not the raise increment over the call. Default is call plus 75% of the pot after calling, capped at hero's stack.
- Chip amounts are bounded at one trillion to keep EV arithmetic finite. The CLI rejects opponents with remaining stacks below the call amount; this conservative check is not a complete side-pot detector. Only use the EV layer for a single shared pot with no all-in opponents.
- `--profit` is the observed **net chip result for the entire hand**, including blinds and other earlier contributions. It is optional and requires `--save`.

Known cards are removed from every range before simulation. Opponent holes are sampled without sharing cards, and unknown community cards are dealt from the remaining deck. The output reports remaining combination counts, split-pot equity, simulation uncertainty, and toy-model action EVs.

## Tight Early-Position Raise

Record observations once per opponent per reviewed hand. PFR means the player raised preflop; VPIP means they voluntarily put money in preflop (not merely posting a blind). `--pfr` implies `--vpip`; `--folded` implies `--faced-bet`. A hand with multiple faced bets counts once for the fold-to-bet opportunity statistic. Use a consistent definition when recording it.

```sh
./build/poker profile record tight_player --pfr --raises 1
./build/poker profile record tight_player
./build/poker profile record tight_player --vpip --calls 1 --folded
./build/poker profile show tight_player

./build/poker analyze \
  --hero AsQh --pot 12 --call 4 --stack 200 \
  --villain 'tight_player,UTG,200,15,0.5' \
  --profile tight_player --action raise --save utg_review1
```

Saved VPIP supplies the initial percentage range and saved fold-to-bet supplies the fold assumption. A preflop `--action raise` uses smoothed PFR instead, scaled by position: UTG variants 0.65, LJ 0.8, HJ 0.9, CO 1.2, BTN 1.5, blinds 1.0. Without a profile, baseline PFR is guessed as half the supplied percentage. `--action call` retains the VPIP/supplied range; it does **not** subtract raising combinations or infer a true calling range. `unknown` makes no adjustment. Explicit `--range` always overrides these percentage estimates.

These multipliers are transparent initial heuristics, not a learned conditional range model. They do not distinguish open raises, three-bets, limpers, bet sizes, or table sizes. Action adjustments reject postflop inputs; specify a reviewed postflop range explicitly. PFR, VPIP, and aggregate betting counts alone cannot reliably infer cards. Do not treat a few observations as a strong read.

## Persistence

```sh
./build/poker history show session1_hand12
./build/poker history stats
```

- `profiles.tsv`: opponent counts, atomically replaced while retaining other opponents. Use `--db PATH` to select another database.
- `.poker-cache/`: reusable equity results. The exact key includes algorithm version, hero cards, board, all ordered opponent combinations and weights, seed, and trial count. Full keys are verified despite hashed filenames. Changes to ranges or board cause a miss. Pot, price, stacks, and fold assumptions recompute the inexpensive EV layer without rerunning matching equity.
- `.poker-hands/`: immutable reviewed records keyed by your `--save ID`. Existing IDs cannot be overwritten. Records include exact ranges, cards, stacks, names, positions, simulation settings/results, estimated action mix, and optional observed net profit. They are saved snapshots, not full betting histories or automatically captured games.
- `history stats` groups reviews by starting-hand class and reports labeled outcome counts, total/average observed net profit, and average simulated equity separately. Unlabeled hands do not contribute to average profit. Statistics describe the saved sample; selective recording creates selection bias.
- Set `--cache-dir PATH`, `--history-dir PATH`, or `--no-cache`. Parent directories must already exist. The default deterministic seed is 42 to enable reuse. Change `--seed` for another simulation and randomized study action.

Persistent files are versioned text, not raw C structs. New files use owner-only permissions. Invalid files fail rather than silently returning misleading results; cache entries can be removed and recalculated. Local data is ignored by Git. Back up profiles and history yourself. Concurrent profile writers are not supported; atomic writes prevent partial files, not lost updates. Directory entries are not fsynced, so atomic publication does not guarantee survival of sudden power loss.

## What The Numbers Mean

Equity is your expected share of the pot **if everyone reaches showdown against the modeled ranges**. It is not the probability of making a profit or a complete EV strategy. An 80% equity hand may deserve a raise, call, or a different response depending on price and assumptions.

For pot `P`, call cost `C`, and equity `E`, the check/call toy model is:

```text
call_EV = E * (P + C) - C
break_even_equity = C / (P + C)
fold_EV = 0
```

Fold EV is relative to the current decision; previously invested chips are sunk costs. When heads-up with supported stacks, additional aggressive contribution `B`, and assumed opponent fold probability `F`:

```text
called_aggressive_EV = E * (P + 2*B - C) - B
aggressive_EV = F*P + (1-F)*called_aggressive_EV
```

This assumes the same equity when called, which can badly overvalue bluffs because actual calling ranges usually strengthen. Check EV assumes free showdown. Multiway bet/raise EV is intentionally unavailable rather than pretending all opponents respond independently with unchanged equity. Calling equity is reported multiway, but unequal stacks/side pots invalidate the simple EV model.

The illustrative mix is now restricted to **call/fold**, or check when there is no call cost. Bet/raise EV remains a labeled hypothetical estimate and is **excluded from recommendations**, because the model does not know which hands continue against a raise. Tolerance is the larger of 1% of the current pot and twice Monte Carlo standard error scaled by the call terminal pot. Call/fold actions within that band are softmax-weighted. This is **not a GTO mixed strategy and does not establish unexploitable play**. The same seed gives the same sample. Existing saved reviews retain their historical mixes rather than being silently rewritten.

Analysis also reports a call EV interval derived from Monte Carlo uncertainty, identifies when it crosses zero, and displays the minimum realization proxy needed to justify a call without rake or future winnings. An interval entirely above zero supports that modeled call, not a robust real-world recommendation: range uncertainty is still unmeasured. Before the river, the output explicitly warns that future betting is unmodeled.

Confidence intervals measure simulation noise only. A wrong range can give a precise but wrong result. The baseline action model has no rake, future street betting, equity realization adjustment, ICM, side-pot accounting, or bankroll/risk management. The optional sensitivity report below is separate from that action model.

## Implied-Odds Sensitivity

The baseline action mix retains the one-street assumptions above. A separate **call-versus-fold sensitivity report** can stress-test rake, realization, implied winnings, and reverse-implied losses:

```sh
./build/poker analyze --hero AsKh --board 'Qs Jh 2c' \
  --pot 100 --call 25 --stack 500 \
  --villain 'alice,BTN,500,25,0.4' --range 'QQ,JJ,AK,AQs,KQs' \
  --realization 0.75 --implied-win 60 --reverse-implied 20 \
  --rake-percent 5 --rake-cap 3
```

Let `q = equity * realization`, current modeled rake `R = min((P+C)*rake_percent/100, rake_cap)`, conditional future net gain `W`, and conditional extra future loss `L`:

```text
adjusted_call_EV = q * (P + C - R + W) - C - (1-q)*L
break_even_effective_equity = (C+L) / (P+C-R+W+L)
required_conditional_future_gain = max(0, (C+(1-q)*L)/q - (P+C-R))
```

At zero effective equity, required future gain is infinite when there is a cost. Realization is a user-supplied stress-test proxy from 0 to 1, not a learned probability or exact implementation of strategic equity realization. `W` and `L` are conditional **net** amounts after future costs and rake; do not count your own returned bets as implied winnings. Current-pot rake is an approximation, not a site's complete fee schedule. The default rake rate is zero.

The engine bounds future losses by hero's remaining stack after calling and future gains by aggregate opponent remaining chips; those are loose caps, not proof that the amounts are collectable. Future amounts are rejected on the river. Choose conservative assumptions and vary them: positive EV under an optimistic implied-payment guess is not evidence of a profitable strategy. The report does not estimate raise EV or change the baseline action mix. Sensitivity options cannot currently be combined with `--save`, preventing a saved recommendation from silently dropping its assumptions; the underlying equity is still cached.

## Implementation

The evaluator enumerates five-card subsets of five to seven known cards and compares packed category/kicker scores. Percentage ranges use Chen-style high-card/pair/suitedness/gap scores (see `src/range.c`). Physical combinations are sorted deterministically, selected before blocker removal, and weighted equally. Cutting through a tied class can create a suit-biased boundary; use explicit classes for suit-symmetric ranges.

Opponent profiles use Beta posterior means: VPIP `(vpip+2)/(hands+10)`, PFR `(pfr+1)/(hands+10)`, fold-to-bet `(folds+2)/(opportunities+4)`. The storage row order is `name, hands, vpip, pfr, bets, raises, calls, faced_bet, folded_to_bet`, tab-separated with no header.

The simulator draws independent weighted opponent combinations, rejecting the **entire joint draw** when cards collide. This preserves the product-of-range-weights distribution conditioned on legal assignments. Incompatible or heavily overlapping ranges fail after a bounded number of attempts. Xorshift64star drives deterministic simulation; it is not a cryptographic RNG.

## Next Milestones

1. Import completed hand histories with full action sequences, sessions, observed hero actions, and independently recorded outcomes.
2. Add action/position/bet-size-conditioned probabilistic range updates with uncertainty and calibration tests.
3. Add rake, side pots, legal raise sizing, future-street models, and equity-realization sensitivity.
4. Add a separate heads-up training solver with explicitly bounded abstractions and exploitability evaluation.
5. Evaluate screenshots or recordings for **post-session** ingestion after identifying the exact client and table layout. PokerGO is a video service; a playable client may be a different product. No screenshot-recognition implementation ships in this MVP.

MIT licensed. Respect platform rules and other players' privacy.
