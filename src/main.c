#include "poker.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("Poker: offline Texas Hold'em study tool (not a GTO solver)\n"
         "\nAnalyze a completed or hypothetical hand:\n"
         "  poker analyze --hero AsKh --board 'Qs Jh 2c' --pot 100 --call 25\n"
         "    --stack 500 --villain 'alice,BTN,500,25,0.4' [options]\n"
         "  Repeat --villain for up to eight opponents. Fields: name,position,\n"
         "  remaining stack,range percent,fold probability (0..1).\n"
         "  --range 'QQ,AKs,AsKh' overrides the preceding villain's range.\n"
         "  --profile NAME uses that villain's saved VPIP/fold statistics.\n"
         "  --action raise|call|unknown adjusts the preceding villain's\n"
         "  preflop heuristic range using position and profile PFR/VPIP.\n"
         "  --db PATH (default profiles.tsv) --trials N (default 20000)\n"
         "  --seed N (default 42) --bet N (total additional chips).\n"
         "  Sensitivity: --realization 0..1 --implied-win CHIPS\n"
         "    --reverse-implied CHIPS --rake-percent 0..100 --rake-cap CHIPS\n"
         "  --cache-dir PATH (default .poker-cache) --no-cache\n"
         "  --save ID [--profit SIGNED_NET_CHIPS] [--history-dir PATH]\n"
         "  poker history show ID [--history-dir PATH]\n"
         "  poker history stats [--history-dir PATH]\n"
         "  poker catalog build|show [--hero AsKh (show only)]\n"
         "    [--trials 2000 --seed 42 --jobs 4 --opponents 1]\n"
         "    [--range 'QQ,AKs' | --range-percent 100] [--cache-dir PATH]\n"
         "  poker table --hands 3 --players 6 --session demo\n"
         "    [--trials 1000 --seed 42 --delay-ms 0 --history-dir PATH]\n"
         "  poker train run --questions 10 --seed 42 --session baseline\n"
         "    [--progress-dir PATH] (answer call|fold and estimated equity %)\n"
         "  poker train stats [--progress-dir PATH]\n"
         "  poker model import --input completed.tsv --dataset actions.tsv\n"
         "  poker model predict|evaluate (see model --help for context options)\n"
         "  --board may be omitted for preflop. Pot includes outstanding bets;\n"
         "  stacks are chips remaining now, not starting stacks.\n"
         "\nRecord one opponent observation from a reviewed hand:\n"
         "  poker profile record NAME [--db PATH] [--vpip] [--pfr]\n"
         "    [--bets N] [--raises N] [--calls N] [--faced-bet] [--folded]\n"
         "  poker profile show NAME [--db PATH]\n"
         "  PFR implies VPIP; folded implies faced-bet. Record once per hand.\n"
         "\nNo live-table capture, automation, hidden-card access, or profit guarantee.");
}

static bool number(const char *text, double *value) {
    char *end; errno = 0;
    *value = strtod(text, &end);
    return text[0] && end != text && !*end && !errno && isfinite(*value) && *value >= 0;
}

static bool signed_number(const char *text, double *value) {
    char *end; errno = 0;
    *value = strtod(text, &end);
    return text[0] && end != text && !*end && !errno && isfinite(*value);
}

static bool integer(const char *text, uint64_t *value) {
    if (!text[0]) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    char *end; errno = 0;
    uintmax_t n = strtoumax(text, &end, 10);
    if (errno || *end || n > UINT64_MAX) return false;
    *value = (uint64_t)n; return true;
}

static bool opponent(const char *text, PokerOpponent *o) {
    if (strlen(text) >= 256) return false;
    char copy[256]; strcpy(copy, text);
    char *parts[5]; parts[0] = copy;
    for (int i = 1; i < 5; ++i) {
        char *comma = strchr(parts[i - 1], ',');
        if (!comma) return false;
        *comma = '\0'; parts[i] = comma + 1;
    }
    if (strchr(parts[4], ',') || !parts[0][0] || strlen(parts[0]) >= sizeof(o->name) ||
        strlen(parts[1]) >= sizeof(o->position)) return false;
    const char *positions[] = {"UTG", "UTG1", "UTG2", "LJ", "HJ", "CO", "BTN", "SB", "BB"};
    bool valid = false;
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i)
        if (!strcmp(parts[1], positions[i])) valid = true;
    if (!valid) return false;
    for (const char *p = parts[0]; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    strcpy(o->name, parts[0]); strcpy(o->position, parts[1]);
    return number(parts[2], &o->stack) && number(parts[3], &o->range_percent) &&
           number(parts[4], &o->fold_to_bet);
}

static int analyze(int argc, char **argv) {
    PokerScenario s = {0};
    s.hero[0] = s.hero[1] = -1; s.hero_stack = 1000; s.trials = 20000;
    s.seed = 42;
    const char *db = "profiles.tsv";
    const char *cache_dir = ".poker-cache", *history_dir = ".poker-hands", *save_id = NULL;
    bool use_cache = true, has_profit = false;
    double profit = 0;
    PokerOddsAssumptions assumptions = {1, 0, 0, 0, 1e12};
    bool sensitivity = false;
    const char *explicit_ranges[POKER_MAX_OPPONENTS] = {0};
    const char *profiles[POKER_MAX_OPPONENTS] = {0};
    const char *actions[POKER_MAX_OPPONENTS] = {0};
    char error[256] = "Invalid argument.";
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-cache")) { use_cache = false; continue; }
        if (i + 1 >= argc) { snprintf(error, sizeof(error), "Missing value for %s", argv[i]); goto invalid; }
        const char *key = argv[i], *value = argv[++i];
        size_t count;
        if (!strcmp(key, "--hero")) {
            if (!poker_cards_parse(value, s.hero, 2, &count, error, sizeof(error)) || count != 2) {
                snprintf(error, sizeof(error), "Hero requires two distinct valid cards."); goto invalid;
            }
        } else if (!strcmp(key, "--board")) {
            if (!poker_cards_parse(value, s.board, 5, &s.board_count, error, sizeof(error))) goto invalid;
        } else if (!strcmp(key, "--pot")) {
            if (!number(value, &s.pot)) goto invalid;
        } else if (!strcmp(key, "--call")) {
            if (!number(value, &s.to_call)) goto invalid;
        } else if (!strcmp(key, "--stack")) {
            if (!number(value, &s.hero_stack)) goto invalid;
        } else if (!strcmp(key, "--bet")) {
            if (!number(value, &s.bet_size)) goto invalid;
        } else if (!strcmp(key, "--realization") || !strcmp(key, "--implied-win") ||
                   !strcmp(key, "--reverse-implied") || !strcmp(key, "--rake-percent") ||
                   !strcmp(key, "--rake-cap")) {
            double *target = !strcmp(key, "--realization") ? &assumptions.realization :
                !strcmp(key, "--implied-win") ? &assumptions.future_win :
                !strcmp(key, "--reverse-implied") ? &assumptions.future_loss :
                !strcmp(key, "--rake-percent") ? &assumptions.rake_percent : &assumptions.rake_cap;
            if (!number(value, target)) goto invalid;
            sensitivity = true;
        } else if (!strcmp(key, "--db")) db = value;
        else if (!strcmp(key, "--cache-dir")) cache_dir = value;
        else if (!strcmp(key, "--history-dir")) history_dir = value;
        else if (!strcmp(key, "--save")) save_id = value;
        else if (!strcmp(key, "--profit")) {
            if (!signed_number(value, &profit)) goto invalid;
            has_profit = true;
        }
        else if (!strcmp(key, "--seed")) {
            if (!integer(value, &s.seed)) goto invalid;
        } else if (!strcmp(key, "--trials")) {
            uint64_t n;
            if (!integer(value, &n) || n < 100 || n > 1000000) goto invalid;
            s.trials = (unsigned)n;
        } else if (!strcmp(key, "--villain")) {
            if (s.opponent_count == POKER_MAX_OPPONENTS || !opponent(value, &s.opponents[s.opponent_count])) {
                snprintf(error, sizeof(error), "Invalid villain; expected name,position,stack,range%%,fold_probability."); goto invalid;
            }
            for (size_t j = 0; j < s.opponent_count; ++j)
                if (!strcmp(s.opponents[j].name, s.opponents[s.opponent_count].name)) {
                    snprintf(error, sizeof(error), "Opponent names must be unique."); goto invalid;
                }
            ++s.opponent_count;
        } else if (!strcmp(key, "--range") || !strcmp(key, "--profile") || !strcmp(key, "--action")) {
            if (!s.opponent_count) { snprintf(error, sizeof(error), "%s must follow a villain.", key); goto invalid; }
            if (!strcmp(key, "--range")) explicit_ranges[s.opponent_count - 1] = value;
            else if (!strcmp(key, "--profile")) profiles[s.opponent_count - 1] = value;
            else {
                if (strcmp(value, "raise") && strcmp(value, "call") && strcmp(value, "unknown")) {
                    snprintf(error, sizeof(error), "Action must be raise, call, or unknown."); goto invalid;
                }
                actions[s.opponent_count - 1] = value;
            }
        } else { snprintf(error, sizeof(error), "Unknown option: %s", key); goto invalid; }
    }
    if (has_profit && !save_id) { snprintf(error, sizeof(error), "--profit requires --save ID."); goto invalid; }
    for (size_t i = 0; i < s.opponent_count; ++i) {
        double raise_percent = s.opponents[i].range_percent * 0.5;
        if (profiles[i]) {
            PokerProfile p;
            if (!poker_profile_load(db, profiles[i], &p, error, sizeof(error))) goto invalid;
            s.opponents[i].range_percent = 100 * poker_profile_vpip(&p);
            raise_percent = 100 * poker_profile_pfr(&p);
            s.opponents[i].fold_to_bet = poker_profile_fold(&p);
            printf("Profile %s: %" PRIu64 " hands, %" PRIu64 " faced bets (smoothed estimates).\n",
                   p.name, p.hands, p.faced_bet);
        }
        if (actions[i] && strcmp(actions[i], "unknown")) {
            if (s.board_count != 0) {
                snprintf(error, sizeof(error), "--action adjustment is preflop-only; use --range for postflop history."); goto invalid;
            }
            if (!explicit_ranges[i] && !strcmp(actions[i], "raise")) {
                const char *pos = s.opponents[i].position;
                double factor = !strncmp(pos, "UTG", 3) ? 0.65 :
                    !strcmp(pos, "LJ") ? 0.8 : !strcmp(pos, "HJ") ? 0.9 :
                    !strcmp(pos, "CO") ? 1.2 : !strcmp(pos, "BTN") ? 1.5 : 1.0;
                s.opponents[i].range_percent = fmax(0.1, fmin(100, raise_percent * factor));
            }
            printf("%s preflop %s: %s%.2f%% range assumption; not a Bayesian action model.\n",
                   s.opponents[i].name, actions[i], explicit_ranges[i] ? "explicit range overrides " : "top ",
                   s.opponents[i].range_percent);
        }
    }
    if (!poker_scenario_validate(&s, error, sizeof(error))) goto invalid;
    PokerOdds odds;
    PokerEquity validation_equity = {0};
    if (!poker_odds(&s, &validation_equity, &assumptions, &odds, error, sizeof(error))) goto invalid;
    if (sensitivity && save_id) {
        snprintf(error, sizeof(error), "Sensitivity reports are not persisted yet; omit --save or sensitivity options to avoid losing assumptions.");
        goto invalid;
    }
    for (size_t i = 0; i < s.opponent_count; ++i) if (s.opponents[i].stack < s.to_call) {
        snprintf(error, sizeof(error), "Short-stack/all-in geometry is unsupported; review separate pot eligibility first.");
        goto invalid;
    }
    PokerRange *ranges = calloc(s.opponent_count, sizeof(*ranges));
    if (!ranges) { snprintf(error, sizeof(error), "Out of memory."); goto invalid; }
    uint64_t dead = (UINT64_C(1) << s.hero[0]) | (UINT64_C(1) << s.hero[1]);
    for (size_t i = 0; i < s.board_count; ++i) dead |= UINT64_C(1) << s.board[i];
    for (size_t i = 0; i < s.opponent_count; ++i) {
        bool ok = explicit_ranges[i] ? poker_range_parse(&ranges[i], explicit_ranges[i], dead, error, sizeof(error)) :
            poker_range_build(&ranges[i], s.opponents[i].range_percent, dead, error, sizeof(error));
        if (!ok) { free(ranges); goto invalid; }
    }
    PokerEquity e; bool hit = false;
    if (use_cache && !poker_cache_read(cache_dir, &s, ranges, &e, &hit, error, sizeof(error))) {
        free(ranges); goto invalid;
    }
    if (!hit && !poker_equity(&s, ranges, &e, error, sizeof(error))) { free(ranges); goto invalid; }
    if (use_cache && !hit && !poker_cache_write(cache_dir, &s, ranges, &e, error, sizeof(error))) {
        free(ranges); goto invalid;
    }
    PokerAdvice a; poker_advice(&s, &e, &a);
    if (!poker_odds(&s, &e, &assumptions, &odds, error, sizeof(error))) { free(ranges); goto invalid; }
    if (save_id && !poker_history_save(history_dir, save_id, &s, ranges, &e, &a,
                                     has_profit, profit, error, sizeof(error))) { free(ranges); goto invalid; }
    free(ranges);
    printf("\nOffline study | seed %" PRIu64 " | %u trials\n", s.seed, e.trials);
    printf("Equity cache: %s\n", !use_cache ? "disabled" : hit ? "hit (reused simulation)" : "miss (stored simulation)");
    if (save_id) printf("Saved study hand: %s%s\n", save_id, has_profit ? " with observed net profit" : " (outcome not recorded)");
    if (s.board_count >= 3) {
        int cards[7] = {s.hero[0], s.hero[1]};
        memcpy(cards + 2, s.board, s.board_count * sizeof(int));
        printf("Current hand: %s\n", poker_hand_category(poker_evaluate(cards, 2 + s.board_count)));
    }
    printf("Equity: %.2f%% (approx. 95%% Monte Carlo interval %.2f%%..%.2f%%)\n",
           100 * e.equity, 100 * fmax(0, e.equity - 1.96 * e.standard_error),
           100 * fmin(1, e.equity + 1.96 * e.standard_error));
    printf("Outright wins: %.2f%% | shared wins: %.2f%%\n", 100 * e.win_rate, 100 * e.tie_rate);
    for (size_t i = 0; i < s.opponent_count; ++i)
        printf("%s (%s): %zu combos after known-card blockers; %s\n", s.opponents[i].name,
               s.opponents[i].position, e.available_combos[i], explicit_ranges[i] ? explicit_ranges[i] : "heuristic top-percent range");
    printf("Call break-even equity: %.2f%%\n", 100 * s.to_call / (s.pot + s.to_call));
    double call_low = fmax(0, e.equity - 1.96 * e.standard_error) * (s.pot + s.to_call) - s.to_call;
    double call_high = fmin(1, e.equity + 1.96 * e.standard_error) * (s.pot + s.to_call) - s.to_call;
    printf("Baseline call EV simulation interval: %.2f..%.2f chips\n", call_low, call_high);
    if (s.to_call > 0) {
        printf("Call/fold comparison under full-realization assumptions: %s\n",
               call_low > 0 ? "positive call EV throughout the simulation interval" :
               call_high < 0 ? "negative call EV throughout the simulation interval" :
               "unresolved by simulation precision; do not infer a preferred action");
        if (e.equity > 0)
            printf("Minimum realization proxy for break-even call (no rake/future gains): %.2f%%\n",
                   100 * s.to_call / ((s.pot + s.to_call) * e.equity));
    }
    if (s.board_count < 5) puts("Future betting is unmodeled: this comparison is NOT a solved street decision.");
    if (sensitivity) {
        printf("\nCall-only sensitivity (user assumptions, not a solved strategy):\n"
               "Realization %.1f%% | effective equity %.2f%% | modeled current-pot rake %.2f\n"
               "Future net gain conditional on winning %.2f | extra loss conditional on losing %.2f\n"
               "Immediate call EV after rake %.2f | adjusted call EV %.2f\n"
               "Break-even effective equity %.2f%% | required conditional future gain %.2f\n",
               100 * assumptions.realization, 100 * odds.effective_equity, odds.rake,
               assumptions.future_win, assumptions.future_loss, odds.immediate_ev, odds.adjusted_ev,
               100 * odds.break_even_equity, odds.required_future_win);
        printf("Call-versus-fold under these assumptions: %s\n",
               odds.adjusted_ev > 0 ? "call has positive estimated EV" :
               odds.adjusted_ev < 0 ? "fold has higher estimated EV" : "equal estimated EV");
        puts("This does not compare raises. Future amounts must fit remaining effective stacks;\n"
             "they are net amounts after future costs/rake, not guaranteed payments.\n"
             "Baseline EV/mix below ignores these sensitivity assumptions.");
    }
    puts("\nToy one-street chip EV, NOT GTO or a prediction of profit:");
    if (s.to_call > 0) printf("Fold EV: 0.00\n");
    printf("%s EV: %.2f\n", s.to_call > 0 ? "Call" : "Check (showdown assumption)", a.call_ev);
    if (a.aggressive_available)
        printf("Hypothetical %s EV: %.2f at %.2f additional chips (fold assumption %.1f%%)\n",
               s.to_call > 0 ? "Raise" : "Bet", a.aggressive_ev, a.risk, 100 * s.opponents[0].fold_to_bet);
    else puts("Bet/raise estimate unavailable: multiway or unsupported stack geometry.");
    puts("Bet/raise estimates are excluded from the mix: no modeled calling-range response.");
    printf("Illustrative call/fold-only mix: fold %.1f%% | %s %.1f%% | %s %.1f%%\n", 100 * a.fold_frequency,
           s.to_call > 0 ? "call" : "check", 100 * a.call_frequency,
           s.to_call > 0 ? "raise" : "bet", 100 * a.aggressive_frequency);
    printf("Sampled restricted-model action (not a full strategy): %s\n", a.sampled_action);
    puts("Assumes full equity realization, unchanged calling ranges and no future bets.\n"
         "No side pots, rake, ICM, minimum-raise validation, or betting-history model.\n"
         "Position/action adjustments are preflop heuristics, not solved ranges.\n"
         "Intervals measure simulation noise, NOT range/model uncertainty.");
    return EXIT_SUCCESS;
invalid:
    fprintf(stderr, "Error: %s\n", error); return EXIT_FAILURE;
}

static int profile_command(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[0], "show") && strcmp(argv[0], "record"))) {
        fputs("Expected profile show NAME or profile record NAME.\n", stderr); return EXIT_FAILURE;
    }
    bool record = !strcmp(argv[0], "record");
    const char *db = "profiles.tsv";
    uint64_t bets = 0, raises = 0, calls = 0;
    bool vpip = false, pfr = false, faced = false, folded = false;
    for (int i = 2; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--db") && i + 1 < argc) db = argv[++i];
        else if (record && !strcmp(key, "--vpip")) vpip = true;
        else if (record && !strcmp(key, "--pfr")) pfr = vpip = true;
        else if (record && !strcmp(key, "--faced-bet")) faced = true;
        else if (record && !strcmp(key, "--folded")) folded = faced = true;
        else if (record && i + 1 < argc && (!strcmp(key, "--bets") || !strcmp(key, "--raises") || !strcmp(key, "--calls"))) {
            uint64_t *n = !strcmp(key, "--bets") ? &bets : !strcmp(key, "--raises") ? &raises : &calls;
            if (!integer(argv[++i], n)) { fputs("Invalid counter.\n", stderr); return EXIT_FAILURE; }
        } else { fprintf(stderr, "Invalid profile option: %s\n", key); return EXIT_FAILURE; }
    }
    char error[256]; PokerProfile p;
    if (!poker_profile_load(db, argv[1], &p, error, sizeof(error))) goto invalid;
    if (record) {
        if (p.hands == UINT64_MAX || (vpip && p.vpip == UINT64_MAX) || (pfr && p.pfr == UINT64_MAX) ||
            (faced && p.faced_bet == UINT64_MAX) || (folded && p.folded_to_bet == UINT64_MAX) ||
            bets > UINT64_MAX - p.bets || raises > UINT64_MAX - p.raises || calls > UINT64_MAX - p.calls) {
            snprintf(error, sizeof(error), "Counter overflow."); goto invalid;
        }
        ++p.hands; p.vpip += vpip; p.pfr += pfr; p.faced_bet += faced; p.folded_to_bet += folded;
        p.bets += bets; p.raises += raises; p.calls += calls;
        if (!poker_profile_save(db, &p, error, sizeof(error))) goto invalid;
    }
    printf("%s: %" PRIu64 " hands | VPIP %.1f%% | PFR %.1f%% | fold-to-bet %.1f%%\n",
           p.name, p.hands, 100 * poker_profile_vpip(&p), 100 * poker_profile_pfr(&p), 100 * poker_profile_fold(&p));
    printf("Bets %" PRIu64 ", raises %" PRIu64 ", calls %" PRIu64 ", faced bets %" PRIu64 "\n",
           p.bets, p.raises, p.calls, p.faced_bet);
    puts("Rates use Bayesian smoothing; few observations are not a reliable read.");
    return EXIT_SUCCESS;
invalid:
    fprintf(stderr, "Error: %s\n", error); return EXIT_FAILURE;
}

int main(int argc, char **argv) {
    if (argc == 1 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { usage(); return EXIT_SUCCESS; }
    if (!strcmp(argv[1], "analyze")) return analyze(argc - 2, argv + 2);
    if (!strcmp(argv[1], "profile")) return profile_command(argc - 2, argv + 2);
    if (!strcmp(argv[1], "catalog")) return poker_catalog_command(argc - 2, argv + 2);
    if (!strcmp(argv[1], "table")) return poker_table_command(argc - 2, argv + 2);
    if (!strcmp(argv[1], "train")) return poker_train_command(argc - 2, argv + 2);
    if (!strcmp(argv[1], "model")) return poker_model_command(argc - 2, argv + 2);
    if (!strcmp(argv[1], "history")) {
        const char *directory = ".poker-hands", *id = NULL;
        bool stats = argc >= 3 && !strcmp(argv[2], "stats");
        int next = 3;
        if (!stats) {
            if (argc < 4 || strcmp(argv[2], "show")) { usage(); return EXIT_FAILURE; }
            id = argv[3]; next = 4;
        }
        if (argc != next && (argc != next + 2 || strcmp(argv[next], "--history-dir"))) {
            fputs("Invalid history options.\n", stderr); return EXIT_FAILURE;
        }
        if (argc == next + 2) directory = argv[next + 1];
        char error[256];
        bool ok = stats ? poker_history_stats(directory, error, sizeof(error)) :
                         poker_history_show(directory, id, error, sizeof(error));
        if (!ok) fprintf(stderr, "Error: %s\n", error);
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    fprintf(stderr, "Unknown command: %s. Use --help.\n", argv[1]); return EXIT_FAILURE;
}
