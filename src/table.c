#define _POSIX_C_SOURCE 200809L
#include "poker.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static bool table_integer(const char *text, uint64_t *value)
{
    if (!*text) return false;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    char *end;
    errno = 0;
    uintmax_t n = strtoumax(text, &end, 10);
    if (errno || *end || n > UINT64_MAX) return false;
    *value = (uint64_t)n;
    return true;
}

static bool table_session_valid(const char *session)
{
    if (!*session || strlen(session) > 40) return false;
    for (const char *p = session; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

static bool table_new_session(const char *directory, const char *session,
                              char *error, size_t size)
{
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        snprintf(error, size, "Cannot create history directory: %s", strerror(errno));
        return false;
    }
    DIR *dir = opendir(directory);
    if (!dir) {
        snprintf(error, size, "Cannot open history directory: %s", strerror(errno));
        return false;
    }
    char prefix[46];
    snprintf(prefix, sizeof(prefix), "%s_hand", session);
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) {
                snprintf(error, size, "Cannot scan history directory: %s", strerror(errno));
                ok = false;
            }
            break;
        }
        size_t n = strlen(entry->d_name);
        if (!strncmp(entry->d_name, prefix, strlen(prefix)) &&
            n >= 5 && !strcmp(entry->d_name + n - 5, ".hand")) {
            snprintf(error, size, "Session '%s' already has snapshots. Resume is not supported; use a new session ID.", session);
            ok = false;
            break;
        }
    }
    if (closedir(dir) != 0 && ok) {
        snprintf(error, size, "Cannot close history directory.");
        ok = false;
    }
    return ok;
}

static const char *table_position(unsigned seat, unsigned button, unsigned players)
{
    static const char *const positions[10][9] = {
        [2] = {"BTN/SB", "BB"},
        [3] = {"BTN", "SB", "BB"},
        [4] = {"BTN", "SB", "BB", "CO"},
        [5] = {"BTN", "SB", "BB", "UTG", "CO"},
        [6] = {"BTN", "SB", "BB", "UTG", "HJ", "CO"},
        [7] = {"BTN", "SB", "BB", "UTG", "LJ", "HJ", "CO"},
        [8] = {"BTN", "SB", "BB", "UTG", "UTG1", "LJ", "HJ", "CO"},
        [9] = {"BTN", "SB", "BB", "UTG", "UTG1", "UTG2", "LJ", "HJ", "CO"}
    };
    return positions[players][(seat + players - button) % players];
}

static void table_render(unsigned hand, unsigned players, unsigned button,
                         const char *street, const int *holes, const int *board,
                         size_t visible, bool showdown, const double *profits)
{
    printf("\n+---------- DEMO hand %u | %s ----------+\n", hand, street);
    printf("  Button: seat %u | %s: %u chips\n", button,
           showdown ? "Distributed pot" : "Pot", players);
    printf("  Board:");
    for (size_t i = 0; i < 5; ++i) {
        char card[3];
        if (i < visible) { poker_card_format(board[i], card); printf(" [%s]", card); }
        else printf(" [??]");
    }
    putchar('\n');
    for (unsigned seat = 0; seat < players; ++seat) {
        char name[POKER_NAME_SIZE];
        if (!seat) snprintf(name, sizeof(name), "hero");
        else snprintf(name, sizeof(name), "bot%u", seat);
        printf("  seat %u %-6s %-6s stack %7.3f | ", seat, name,
               table_position(seat, button, players), showdown ? 100 + profits[seat] : 99.0);
        if (!seat || showdown) {
            char first[3], second[3];
            poker_card_format(holes[seat * 2], first);
            poker_card_format(holes[seat * 2 + 1], second);
            printf("[%s] [%s]", first, second);
        } else printf("[??] [??]");
        if (showdown) {
            int cards[7] = {holes[seat * 2], holes[seat * 2 + 1]};
            memcpy(cards + 2, board, 5 * sizeof(*board));
            printf(" | %-15s net %+.6f%s", poker_hand_category(poker_evaluate(cards, 7)),
                   profits[seat], profits[seat] > -1 ? " WINNER" : "");
        }
        putchar('\n');
    }
    puts("+------------------------------------------------+");
}

static bool table_pause(unsigned delay, char *error, size_t size)
{
    if (fflush(stdout) != 0) {
        snprintf(error, size, "Cannot flush terminal output.");
        return false;
    }
    struct timespec remaining = {(time_t)(delay / 1000), (long)(delay % 1000) * 1000000L};
    while (delay && nanosleep(&remaining, &remaining) != 0) {
        if (errno == EINTR) continue;
        snprintf(error, size, "Cannot delay display: %s", strerror(errno));
        return false;
    }
    return true;
}

int poker_table_command(int argc, char **argv)
{
    unsigned hands = 3, players = 6, trials = 1000, delay = 0;
    uint64_t seed = 42;
    const char *history_dir = ".poker-hands", *cache_dir = ".poker-cache", *session = "demo";
    bool use_cache = true;
    char error[256] = "Invalid table argument.";
    PokerRange *ranges = NULL;
    for (int i = 0; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) {
            puts("Offline local automatic dealt training DEMO only, not a full poker game.\n"
                 "table --hands 3 --players 6 --trials 1000 --seed 42\n"
                 "      --history-dir PATH --session demo --delay-ms 0\n"
                 "      [--cache-dir PATH | --no-cache]\n"
                 "Hands: 1..1000000; players: 2..9; trials: 100..1000000; delay: 0..10000 ms.\n"
                 "Session: 1..40 letters, digits, underscores or hyphens; must be unused.\n"
                 "Defaults: .poker-hands history, .poker-cache cache. Resume is not supported.\n"
                 "Stacks reset to 100 each hand; everyone antes 1 chip and checks down.\n"
                 "Position labels only: no blinds, bets, decisions, or external screen capture.");
            return EXIT_SUCCESS;
        }
        if (!strcmp(key, "--no-cache")) { use_cache = false; continue; }
        if (i + 1 >= argc) {
            snprintf(error, sizeof(error), "Missing value for %s", key);
            goto invalid;
        }
        const char *value = argv[++i];
        if (!strcmp(key, "--history-dir")) history_dir = value;
        else if (!strcmp(key, "--cache-dir")) cache_dir = value;
        else if (!strcmp(key, "--session")) session = value;
        else {
            uint64_t n;
            if (strcmp(key, "--hands") && strcmp(key, "--players") &&
                strcmp(key, "--trials") && strcmp(key, "--seed") && strcmp(key, "--delay-ms")) {
                snprintf(error, sizeof(error), "Unknown table option: %s (resume is not supported).", key);
                goto invalid;
            }
            if (!table_integer(value, &n)) {
                snprintf(error, sizeof(error), "Invalid integer for %s", key);
                goto invalid;
            }
            if (!strcmp(key, "--seed")) seed = n;
            else if (!strcmp(key, "--hands") && n >= 1 && n <= 1000000) hands = (unsigned)n;
            else if (!strcmp(key, "--players") && n >= 2 && n <= POKER_MAX_OPPONENTS + 1) players = (unsigned)n;
            else if (!strcmp(key, "--trials") && n >= 100 && n <= 1000000) trials = (unsigned)n;
            else if (!strcmp(key, "--delay-ms") && n <= 10000) delay = (unsigned)n;
            else {
                snprintf(error, sizeof(error), "Out-of-range value for %s", key);
                goto invalid;
            }
        }
    }
    if (!table_session_valid(session) || !*history_dir || !*cache_dir) {
        snprintf(error, sizeof(error), "Use nonempty directories and a safe session ID of 1..40 characters.");
        goto invalid;
    }
    if (!table_new_session(history_dir, session, error, sizeof(error))) goto invalid;
    ranges = calloc(players - 1, sizeof(*ranges));
    if (!ranges) { snprintf(error, sizeof(error), "Out of memory."); goto invalid; }
    puts("Offline local automatic dealt training DEMO ONLY, not a full poker game.\n"
         "No external screen capture. No blinds, betting decisions, rake or side pots.\n"
         "Every hand resets stacks to 100; each seat antes 1 chip, then checks down.\n"
         "Position labels are informational only; the button rotates each hand.\n"
         "Street equity is study-only versus symmetric 100% random ranges, using only\n"
         "hero cards and the visible board, never hidden holes or future board cards.\n"
         "Saved action 'check' is the forced demo check, not sampled advice.\n"
         "Four unlabeled reviews and one labeled completed river/showdown per hand.\n"
         "Resume is not supported; interrupted sessions require a new session ID.");
    printf("Session %s | history %s | seed %" PRIu64 " | %u trials | cache %s\n",
           session, history_dir, seed, trials, use_cache ? cache_dir : "disabled");
    if (!table_pause(0, error, sizeof(error))) goto invalid;
    PokerRng deal_rng;
    poker_rng_seed(&deal_rng, seed);
    double totals[POKER_MAX_OPPONENTS + 1] = {0};
    static const char *const streets[] = {"preflop", "flop", "turn", "river"};
    static const size_t visible[] = {0, 3, 4, 5};
    for (unsigned hand = 1; hand <= hands; ++hand) {
        unsigned button = (hand - 1) % players;
        int deck[52], holes[(POKER_MAX_OPPONENTS + 1) * 2], board[5];
        for (int i = 0; i < 52; ++i) deck[i] = i;
        for (unsigned i = 51; i > 0; --i) {
            unsigned j = poker_rng_bounded(&deal_rng, i + 1);
            int swap = deck[i]; deck[i] = deck[j]; deck[j] = swap;
        }
        unsigned next = 0;
        for (unsigned round = 0; round < 2; ++round)
            for (unsigned offset = 1; offset <= players; ++offset)
                holes[((button + offset) % players) * 2 + round] = deck[next++];
        for (size_t i = 0; i < 5; ++i) board[i] = deck[next++];
        PokerScenario s = {0};
        s.hero[0] = holes[0]; s.hero[1] = holes[1];
        for (size_t i = 0; i < 5; ++i) s.board[i] = -1;
        s.opponent_count = players - 1; s.pot = players; s.hero_stack = 99; s.trials = trials;
        for (unsigned seat = 1; seat < players; ++seat) {
            PokerOpponent *o = &s.opponents[seat - 1];
            snprintf(o->name, sizeof(o->name), "bot%u", seat);
            snprintf(o->position, sizeof(o->position), "%s", table_position(seat, button, players));
            o->stack = 99; o->range_percent = 100;
        }
        /* The history schema requires an action/mix; this is the actual forced
         * check-down rule, not poker_advice's sampled betting recommendation. */
        PokerAdvice advice = {0};
        advice.call_frequency = 1; advice.sampled_action = "check";
        char id[65];
        for (size_t street = 0; street < 4; ++street) {
            s.board_count = visible[street];
            memcpy(s.board, board, s.board_count * sizeof(*board));
            /* Simulation randomness is independent of the shuffle stream. */
            s.seed = seed ^ ((uint64_t)hand * UINT64_C(0x9e3779b97f4a7c15)) ^
                     ((uint64_t)street * UINT64_C(0xd1b54a32d192ed03));
            uint64_t dead = (UINT64_C(1) << s.hero[0]) | (UINT64_C(1) << s.hero[1]);
            for (size_t i = 0; i < s.board_count; ++i) dead |= UINT64_C(1) << s.board[i];
            if (!poker_range_build(&ranges[0], 100, dead, error, sizeof(error))) goto invalid;
            for (size_t i = 1; i < s.opponent_count; ++i) ranges[i] = ranges[0];
            table_render(hand, players, button, streets[street], holes, board, s.board_count, false, NULL);
            if (!table_pause(0, error, sizeof(error))) goto invalid;
            PokerEquity equity = {0};
            bool hit = false;
            if (use_cache && !poker_cache_read(cache_dir, &s, ranges, &equity, &hit, error, sizeof(error))) goto invalid;
            if (!hit && !poker_equity(&s, ranges, &equity, error, sizeof(error))) goto invalid;
            if (use_cache && !hit && !poker_cache_write(cache_dir, &s, ranges, &equity, error, sizeof(error))) goto invalid;
            advice.call_ev = equity.equity * s.pot;
            snprintf(id, sizeof(id), "%s_hand%u_%s", session, hand, streets[street]);
            if (!poker_history_save(history_dir, id, &s, ranges, &equity, &advice, false, 0, error, sizeof(error))) goto invalid;
            printf("Study equity: %.2f%% | SE %.2f percentage points | %u trials | cache %s\n",
                   equity.equity * 100, equity.standard_error * 100, equity.trials,
                   !use_cache ? "disabled" : hit ? "hit" : "miss");
            printf("Saved reviewed snapshot: %s (unlabeled; no net result). Everyone checks.\n", id);
            if (!table_pause(delay, error, sizeof(error))) goto invalid;
        }
        uint32_t values[POKER_MAX_OPPONENTS + 1], best = 0;
        unsigned winners = 0;
        for (unsigned seat = 0; seat < players; ++seat) {
            int cards[7] = {holes[seat * 2], holes[seat * 2 + 1]};
            memcpy(cards + 2, board, sizeof(board));
            values[seat] = poker_evaluate(cards, 7);
            if (values[seat] > best) { best = values[seat]; winners = 1; }
            else if (values[seat] == best) ++winners;
        }
        double profits[POKER_MAX_OPPONENTS + 1], net = 0;
        for (unsigned seat = 0; seat < players; ++seat) {
            profits[seat] = values[seat] == best ? (double)players / winners - 1 : -1;
            totals[seat] += profits[seat]; net += profits[seat];
        }
        if (fabs(net) > 1e-10) {
            snprintf(error, sizeof(error), "Demo hand chip conservation failed."); goto invalid;
        }
        /* A complete board plus singleton revealed ranges has an exact result;
         * populate it directly, without Monte Carlo rounding of split pots. */
        PokerEquity exact = {0};
        exact.trials = trials;
        if (values[0] == best) {
            exact.equity = 1.0 / winners;
            exact.win_rate = winners == 1 ? 1 : 0;
            exact.tie_rate = winners > 1 ? 1 : 0;
        }
        for (unsigned seat = 1; seat < players; ++seat) {
            memset(&ranges[seat - 1], 0, sizeof(*ranges));
            ranges[seat - 1].count = 1;
            ranges[seat - 1].combos[0] = (PokerCombo){{holes[seat * 2], holes[seat * 2 + 1]}, 1};
            exact.available_combos[seat - 1] = 1;
        }
        advice.call_ev = exact.equity * s.pot;
        snprintf(id, sizeof(id), "%s_hand%u_showdown", session, hand);
        if (!poker_history_save(history_dir, id, &s, ranges, &exact, &advice, true, profits[0], error, sizeof(error))) goto invalid;
        table_render(hand, players, button, "completed river / showdown", holes, board, 5, true, profits);
        printf("Exact outcome: hero %s | %u winner(s) | payout %.6f | net %+.6f chips\n",
               values[0] != best ? "loses" : winners == 1 ? "wins" : "ties", winners,
               profits[0] + 1, profits[0]);
        printf("Saved completed river: %s (exact revealed holes; labeled hero net result).\n", id);
        printf("Hand net across all seats: %.12f (conserved).\n", net);
        if (!table_pause(delay, error, sizeof(error))) goto invalid;
    }
    double session_net = 0;
    printf("\nDemo session totals (%u hands; stacks reset each hand):\n", hands);
    for (unsigned seat = 0; seat < players; ++seat) {
        printf("seat %u net %+.6f chips%s\n", seat, totals[seat], seat ? "" : " (hero)");
        session_net += totals[seat];
    }
    if (fabs(session_net) > 1e-8 * hands) {
        snprintf(error, sizeof(error), "Demo session chip conservation failed."); goto invalid;
    }
    printf("Session net across all seats: %.12f (conserved).\n", session_net);
    printf("Available IDs: %s_handN_{preflop,flop,turn,river,showdown}, N=1..%u.\n", session, hands);
    if (!table_pause(0, error, sizeof(error))) goto invalid;
    free(ranges);
    return EXIT_SUCCESS;
invalid:
    free(ranges);
    fprintf(stderr, "Error: %s\n", error);
    return EXIT_FAILURE;
}
