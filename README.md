# Poker

A runnable C11 Texas Hold'em **offline study application**. Estimate your hand's showdown equity against plausible opponent ranges, compare simplified chip EVs, and persist simulations, reviewed hands, and opponent observations.

This is an initial training engine, **not a GTO solver, autonomous poker bot, or guaranteed winning strategy**. It does not capture live tables, read hidden cards, click buttons, or provide a live decision overlay. Use completed hands, hypothetical scenarios, or permitted training environments.

## Build

Requires a C compiler, Make, and a POSIX environment. No third-party runtime dependencies. macOS: install Apple's command-line tools with `xcode-select --install`. Linux: install your distribution's C compiler and Make.

```sh
make
./build/poker --help
make test
make sanitize
```

The executable is `build/poker`. GitHub Actions builds and tests on macOS and Linux. Tests include all 2,598,960 five-card hands, tie-breaking, blockers, multiway equity, persistent data validation, and CLI integration.

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

The study mix randomizes only among actions within an EV tolerance of the best toy-model action. Tolerance is the larger of 1% of the current pot and twice Monte Carlo standard error scaled by the aggressive terminal pot. Actions are softmax-weighted within that band. This is **not a GTO mixed strategy and does not establish unexploitable play**. The same seed gives the same sample. Only the call/fold/check legality is modeled; bet/raise suggestions need independent minimum-raise and betting-state validation.

Confidence intervals measure simulation noise only. A wrong range can give a precise but wrong result. There is no rake, future street betting, equity realization adjustment, ICM, side-pot accounting, or bankroll/risk management.

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
