#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "poker.h"

#include <dirent.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks;
static unsigned failures;
static char scratch[4096];

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static bool near(double actual, double expected)
{
    return isfinite(actual) && fabs(actual - expected) < 1e-10;
}

static size_t cards(const char *text, int *out, size_t capacity)
{
    size_t count = 0;
    char error[160];
    CHECK(poker_cards_parse(text, out, capacity, &count, error, sizeof error));
    return count;
}

static uint32_t hand(const char *text)
{
    int out[7];
    size_t count = cards(text, out, 7);
    return poker_evaluate(out, count);
}

static uint64_t mask(const char *text)
{
    int out[52];
    size_t count = cards(text, out, 52);
    uint64_t result = 0;
    for (size_t i = 0; i < count; ++i) {
        result |= UINT64_C(1) << (unsigned)out[i];
    }
    return result;
}

static void test_cards(void)
{
    for (int card = 0; card < 52; ++card) {
        char text[3];
        poker_card_format(card, text);
        CHECK(strlen(text) == 2);
        CHECK(poker_card_parse(text) == card);
    }
    const char *const bad[] = {"", "A", "Asx", "10s", "as", "AS", "1s", "Ax", " As", "As "};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(poker_card_parse(bad[i]) == -1);
    }
    CHECK(poker_card_parse(NULL) == -1);
    char text[3];
    poker_card_format(-1, text);
    CHECK(strcmp(text, "??") == 0);
    poker_card_format(52, text);
    CHECK(strcmp(text, "??") == 0);
    poker_card_format(0, NULL);

    int out[7];
    size_t count = 99;
    char error[160] = "old error";
    CHECK(poker_cards_parse(" As,Kd\tQh\nJcTs ", out, 7, &count, error, sizeof error));
    CHECK(count == 5 && error[0] == '\0');
    CHECK(out[0] == poker_card_parse("As") && out[4] == poker_card_parse("Ts"));
    CHECK(poker_cards_parse(" ,\t\n", NULL, 0, &count, NULL, 0));
    CHECK(count == 0);
    const char *const bad_lists[] = {"As As", "As,Kd,As", "AsK", "AsXx", "10s", "A s"};
    for (size_t i = 0; i < sizeof bad_lists / sizeof bad_lists[0]; ++i) {
        count = 99;
        error[0] = '\0';
        CHECK(!poker_cards_parse(bad_lists[i], out, 7, &count, error, sizeof error));
        CHECK(count == 0 && error[0] != '\0');
    }
    CHECK(!poker_cards_parse("AsKd", out, 1, &count, error, sizeof error));
    CHECK(!poker_cards_parse(NULL, out, 7, &count, NULL, 0));
    CHECK(!poker_cards_parse("As", NULL, 1, &count, NULL, 0));
    CHECK(!poker_cards_parse("As", out, 7, NULL, NULL, 0));
    char tiny[2] = {'x', 'x'};
    CHECK(!poker_cards_parse("Xx", out, 7, &count, tiny, sizeof tiny));
    CHECK(tiny[0] != '\0' && tiny[1] == '\0');
}

static void test_hands(void)
{
    const struct { const char *cards; const char *category; } categories[] = {
        {"As Kd Qh Jc 9s", "High card"},
        {"As Ad Kh Qc Js", "One pair"},
        {"As Ad Kh Kc Qs", "Two pair"},
        {"As Ad Ah Kc Qs", "Three of a kind"},
        {"As Kd Qh Jc Ts", "Straight"},
        {"As Ks Qs Js 9s", "Flush"},
        {"As Ad Ah Kc Ks", "Full house"},
        {"As Ad Ah Ac Ks", "Four of a kind"},
        {"As Ks Qs Js Ts", "Straight flush"}
    };
    uint32_t previous = 0;
    for (size_t i = 0; i < sizeof categories / sizeof categories[0]; ++i) {
        uint32_t value = hand(categories[i].cards);
        CHECK((value >> 20) == i);
        CHECK(strcmp(poker_hand_category(value), categories[i].category) == 0);
        CHECK(value > previous);
        previous = value;
    }
    /* Each tie-break digit, not just the category or leading rank. */
    const struct { const char *better; const char *worse; } kickers[] = {
        {"As Qd Jh 9c 7s", "Ks Qd Jh 9c 7s"},
        {"As Kd Jh 9c 7s", "As Qd Jh 9c 7s"},
        {"As Kd Qh 9c 7s", "As Kd Jh 9c 7s"},
        {"As Kd Qh Tc 7s", "As Kd Qh 9c 7s"},
        {"As Kd Qh Tc 8s", "As Kd Qh Tc 7s"},
        {"As Ad Qh Jc 9s", "Ks Kd Qh Jc 9s"},
        {"As Ad Kh Jc 9s", "As Ad Qh Jc 9s"},
        {"As Ad Kh Qc 9s", "As Ad Kh Jc 9s"},
        {"As Ad Kh Qc Ts", "As Ad Kh Qc 9s"},
        {"As Ad Qh Qc 2s", "Ks Kd Qh Qc As"},
        {"As Ad Kh Kc 2s", "As Ad Qh Qc Ks"},
        {"As Ad Kh Kc Qs", "As Ad Kh Kc Js"},
        {"As Ad Ah 3c 2s", "Ks Kd Kh Ac Qs"},
        {"As Ad Ah Kc Js", "As Ad Ah Qc Js"},
        {"As Ad Ah Kc Qs", "As Ad Ah Kc Js"},
        {"6s 5d 4h 3c 2s", "As 5d 4h 3c 2s"},
        {"As Ks Js 9s 7s", "Qs Js 9s 7s 3s"},
        {"As Ks Js 9s 7s", "As Qs Js 9s 7s"},
        {"As Ks Qs 9s 7s", "As Ks Js 9s 7s"},
        {"As Ks Qs Ts 7s", "As Ks Qs 9s 7s"},
        {"As Ks Qs Ts 8s", "As Ks Qs Ts 7s"},
        {"As Ad Ah 2c 2s", "Ks Kd Kh Ac As"},
        {"As Ad Ah Kc Ks", "As Ad Ah Qc Qs"},
        {"As Ad Ah Ac 2s", "Ks Kd Kh Kc As"},
        {"As Ad Ah Ac Ks", "As Ad Ah Ac Qs"},
        {"6s 5s 4s 3s 2s", "As 5s 4s 3s 2s"}
    };
    for (size_t i = 0; i < sizeof kickers / sizeof kickers[0]; ++i) {
        CHECK(hand(kickers[i].better) > hand(kickers[i].worse));
    }
    CHECK((hand("As 2d 3h 4c 5s") >> 20) == 4);
    CHECK(hand("As 2d 3h 4c 5s") == hand("5c 4d 3s 2h Ac"));
    CHECK((hand("As 2s 3s 4s 5s") >> 20) == 8);
    CHECK((hand("Qs Kd Ah 2c 3s") >> 20) == 0);
    CHECK(hand("As Kd Qh Jc Ts") == hand("Tc Jh Qd Ks Ac"));

    const struct { const char *seven; const char *best; } best_hands[] = {
        {"2c As 3d Ks Qh Js 9c", "As Ks Qh Js 9c"},
        {"As Ad Kh Qc Js 2c 3d", "As Ad Kh Qc Js"},
        {"As Ad Kh Kc Qs Qd 2c", "As Ad Kh Kc Qs"},
        {"As Ad Ah Kc Qs 2c 3d", "As Ad Ah Kc Qs"},
        {"As 2d 3h 4c 5s 6d 7h", "3h 4c 5s 6d 7h"},
        {"As Ks Qs Js 8s 2s 3s", "As Ks Qs Js 8s"},
        {"As Ad Ah Kc Ks Kd 2c", "As Ad Ah Kc Ks"},
        {"As Ad Ah Kc Ks Qd Qh", "As Ad Ah Kc Ks"},
        {"As Ad Ah Ac Ks Kd 2c", "As Ad Ah Ac Ks"},
        {"As 2s 3s 4s 5s 6s 7d", "2s 3s 4s 5s 6s"},
        {"As Ks Qs Js Ts Ad Ah", "As Ks Qs Js Ts"}
    };
    for (size_t i = 0; i < sizeof best_hands / sizeof best_hands[0]; ++i) {
        CHECK(hand(best_hands[i].seven) == hand(best_hands[i].best));
        int out[7];
        CHECK(cards(best_hands[i].seven, out, 7) == 7);
        for (size_t j = 0; j < 3; ++j) {
            int swap = out[j]; out[j] = out[6 - j]; out[6 - j] = swap;
        }
        CHECK(poker_evaluate(out, 7) == hand(best_hands[i].best));
    }
    CHECK(hand("As Ad Ah Kc Ks 2d") == hand("As Ad Ah Kc Ks"));
    int out[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    CHECK(poker_evaluate(NULL, 5) == 0);
    CHECK(poker_evaluate(out, 4) == 0);
    CHECK(poker_evaluate(out, 8) == 0);
    out[4] = 0;
    CHECK(poker_evaluate(out, 5) == 0);
    out[4] = -1;
    CHECK(poker_evaluate(out, 5) == 0);
    out[4] = 52;
    CHECK(poker_evaluate(out, 5) == 0);
    CHECK(strcmp(poker_hand_category(UINT32_C(9) << 20), "Invalid") == 0);
}

static void test_exhaustive(void)
{
    const uint64_t expected[9] = {1302540, 1098240, 123552, 54912, 10200,
                                  5108, 3744, 624, 40};
    uint64_t counts[9] = {0};
    uint64_t total = 0;
    uint64_t invalid = 0;
    for (int a = 0; a < 48; ++a)
        for (int b = a + 1; b < 49; ++b)
            for (int c = b + 1; c < 50; ++c)
                for (int d = c + 1; d < 51; ++d)
                    for (int e = d + 1; e < 52; ++e) {
                        const int out[5] = {a, b, c, d, e};
                        uint32_t value = poker_evaluate(out, 5);
                        unsigned category = value >> 20;
                        ++total;
                        if (value == 0 || category >= 9) ++invalid;
                        else ++counts[category];
                    }
    CHECK(total == UINT64_C(2598960));
    CHECK(invalid == 0);
    for (size_t i = 0; i < 9; ++i) CHECK(counts[i] == expected[i]);
    (void)printf("Exhaustive five-card census: %llu hands\n", (unsigned long long)total);
}

static void check_range(const PokerRange *range, uint64_t dead)
{
    uint64_t seen[52] = {0};
    CHECK(range->count > 0 && range->count <= POKER_MAX_COMBOS);
    for (size_t i = 0; i < range->count; ++i) {
        const PokerCombo *combo = &range->combos[i];
        int a = combo->cards[0], b = combo->cards[1];
        CHECK(a >= 0 && a < b && b < 52);
        if (a < 0 || a >= b || b >= 52) continue;
        uint64_t bit = UINT64_C(1) << (unsigned)b;
        CHECK((seen[a] & bit) == 0);
        CHECK((dead & (bit | (UINT64_C(1) << (unsigned)a))) == 0);
        CHECK(near(combo->weight, 1));
        seen[a] |= bit;
    }
}

static void test_ranges(void)
{
    PokerRange range, repeat;
    char error[160];
    const struct { const char *text; size_t count; } classes[] = {
        {"AA", 6}, {"22", 6}, {"AKs", 4}, {"AKo", 12}, {"AK", 16},
        {"KAs", 4}, {"AsKd", 1}, {"KdAs,AsKd", 1},
        {" AKs , AKo , AK, AsKs, KAs ", 16}, {"AA,KK,AKs", 16}
    };
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; ++i) {
        CHECK(poker_range_parse(&range, classes[i].text, 0, error, sizeof error));
        CHECK(range.count == classes[i].count && error[0] == '\0');
        check_range(&range, 0);
    }
    uint64_t dead = mask("As");
    const struct { const char *text; size_t count; } blocked[] = {
        {"AA", 3}, {"AKs", 3}, {"AKo", 9}, {"AK", 12}
    };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; ++i) {
        CHECK(poker_range_parse(&range, blocked[i].text, dead, error, sizeof error));
        CHECK(range.count == blocked[i].count);
        check_range(&range, dead);
    }
    const char *const bad[] = {"", " ", ",AA", "AA,", "AA,,KK", "AAs", "AAo",
                               "AKx", "ZZ", "aa", "AA+", "22-99", "AsAs", "AsXx", "AA KK"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(!poker_range_parse(&range, bad[i], 0, error, sizeof error));
        CHECK(range.count == 0 && error[0] != '\0');
    }
    CHECK(!poker_range_parse(&range, "AsKd", dead, NULL, 0));
    CHECK(!poker_range_parse(&range, "AA", mask("As Ah Ad"), NULL, 0));
    CHECK(!poker_range_parse(&range, NULL, 0, NULL, 0));
    CHECK(!poker_range_parse(NULL, "AA", 0, NULL, 0));
    CHECK(!poker_range_parse(&range, "AA", UINT64_C(1) << 52, NULL, 0));

    CHECK(poker_range_build(&range, 100, 0, error, sizeof error));
    CHECK(range.count == 1326);
    check_range(&range, 0);
    dead = mask("As Kd");
    CHECK(poker_range_build(&range, 100, dead, error, sizeof error));
    CHECK(range.count == 1225);
    check_range(&range, dead);
    CHECK(poker_range_build(&range, 10, 0, error, sizeof error));
    CHECK(range.count == 133);
    CHECK(poker_range_build(&repeat, 10, 0, error, sizeof error));
    CHECK(repeat.count == range.count);
    for (size_t i = 0; i < range.count; ++i) {
        CHECK(range.combos[i].cards[0] == repeat.combos[i].cards[0]);
        CHECK(range.combos[i].cards[1] == repeat.combos[i].cards[1]);
    }
    CHECK(poker_range_build(&range, 0.001, 0, error, sizeof error));
    CHECK(range.count == 1 && range.combos[0].cards[0] / 4 == 12 &&
          range.combos[0].cards[1] / 4 == 12);
    dead = UINT64_C(1) << (unsigned)range.combos[0].cards[0];
    CHECK(!poker_range_build(&range, 0.001, dead, error, sizeof error));
    CHECK(range.count == 0); /* Blockers remove selected combos, never refill. */
    CHECK(poker_range_build(&range, DBL_MIN, 0, error, sizeof error));
    CHECK(range.count == 1);
    const double bad_percent[] = {0, -1, 100.1, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof bad_percent / sizeof bad_percent[0]; ++i) {
        CHECK(!poker_range_build(&range, bad_percent[i], 0, error, sizeof error));
        CHECK(range.count == 0);
    }
    CHECK(!poker_range_build(NULL, 100, 0, NULL, 0));
    CHECK(!poker_range_build(&range, 100, UINT64_C(1) << 52, NULL, 0));
    CHECK(!poker_range_build(&range, 100, (UINT64_C(1) << 52) - 1, NULL, 0));
}

static void test_rng(void)
{
    PokerRng a, b, zero = {0}, fallback;
    poker_rng_seed(&a, 42);
    CHECK(poker_rng_next(&a) == UINT64_C(6255019084209693600));
    poker_rng_seed(&a, 42);
    poker_rng_seed(&b, 42);
    for (unsigned i = 0; i < 1000; ++i) CHECK(poker_rng_next(&a) == poker_rng_next(&b));
    poker_rng_seed(&a, 0);
    poker_rng_seed(&fallback, UINT64_C(0x9e3779b97f4a7c15));
    CHECK(poker_rng_next(&zero) == poker_rng_next(&a));
    CHECK(poker_rng_next(&fallback) == UINT64_C(973819730272012410));
    uint64_t saved = b.state;
    CHECK(poker_rng_bounded(&b, 0) == 0 && b.state == saved);
    const unsigned bounds[] = {1, 2, 3, 52, 1000, 4294967295U};
    poker_rng_seed(&a, 7);
    poker_rng_seed(&b, 7);
    for (size_t j = 0; j < sizeof bounds / sizeof bounds[0]; ++j) {
        for (unsigned i = 0; i < 100; ++i) {
            unsigned value = poker_rng_bounded(&a, bounds[j]);
            CHECK(value < bounds[j]);
            CHECK(value == poker_rng_bounded(&b, bounds[j]));
            double value_double = poker_rng_double(&a);
            CHECK(value_double >= 0 && value_double < 1);
            CHECK(value_double == poker_rng_double(&b));
        }
    }
    poker_rng_seed(&a, 42);
    poker_rng_seed(&b, 43);
    CHECK(poker_rng_next(&a) != poker_rng_next(&b));
}

static PokerScenario scenario(const char *hero, const char *board, size_t opponents)
{
    PokerScenario s = {0};
    CHECK(cards(hero, s.hero, 2) == 2);
    s.board_count = cards(board, s.board, 5);
    s.opponent_count = opponents;
    s.pot = 100; s.to_call = 20; s.hero_stack = 200; s.bet_size = 60;
    s.trials = 100; s.seed = 42;
    for (size_t i = 0; i < opponents; ++i) {
        s.opponents[i].stack = 200;
        s.opponents[i].range_percent = 100;
        s.opponents[i].fold_to_bet = 0.3;
    }
    return s;
}

static void test_validation(void)
{
    PokerScenario valid = scenario("As Ah", "", 1), s;
    char error[160];
    CHECK(poker_scenario_validate(&valid, error, sizeof error));
    CHECK(!poker_scenario_validate(NULL, error, sizeof error));
#define INVALID(change) do { \
    s = valid; change; error[0] = '\0'; \
    CHECK(!poker_scenario_validate(&s, error, sizeof error)); \
    CHECK(error[0] != '\0'); \
} while (0)
    INVALID(s.opponent_count = 0);
    INVALID(s.opponent_count = POKER_MAX_OPPONENTS + 1);
    INVALID(s.board_count = 1);
    INVALID(s.board_count = 2);
    INVALID(s.board_count = 6);
    INVALID(s.hero[0] = -1);
    INVALID(s.hero[1] = 52);
    INVALID(s.hero[1] = s.hero[0]);
    INVALID(s.board_count = 3; s.board[0] = s.hero[0]; s.board[1] = 0; s.board[2] = 1);
    INVALID(s.board_count = 3; s.board[0] = 0; s.board[1] = 0; s.board[2] = 1);
    INVALID(s.board_count = 3; s.board[0] = 0; s.board[1] = 1; s.board[2] = 52);
    INVALID(s.pot = 0);
    INVALID(s.pot = -1);
    INVALID(s.pot = NAN);
    INVALID(s.pot = INFINITY);
    INVALID(s.to_call = -1);
    INVALID(s.to_call = NAN);
    INVALID(s.to_call = INFINITY);
    INVALID(s.to_call = s.hero_stack + 1);
    INVALID(s.hero_stack = 0);
    INVALID(s.hero_stack = -1);
    INVALID(s.hero_stack = NAN);
    INVALID(s.hero_stack = INFINITY);
    INVALID(s.bet_size = -1);
    INVALID(s.bet_size = NAN);
    INVALID(s.bet_size = INFINITY);
    INVALID(s.trials = 99);
    INVALID(s.trials = 1000001);
    INVALID(s.opponents[0].stack = 0);
    INVALID(s.opponents[0].stack = -1);
    INVALID(s.opponents[0].stack = NAN);
    INVALID(s.opponents[0].stack = INFINITY);
    INVALID(s.opponents[0].range_percent = 0);
    INVALID(s.opponents[0].range_percent = 101);
    INVALID(s.opponents[0].range_percent = NAN);
    INVALID(s.opponents[0].range_percent = INFINITY);
    INVALID(s.opponents[0].fold_to_bet = -0.1);
    INVALID(s.opponents[0].fold_to_bet = 1.1);
    INVALID(s.opponents[0].fold_to_bet = NAN);
    INVALID(s.opponents[0].fold_to_bet = INFINITY);
#undef INVALID
    for (size_t count = 3; count <= 5; ++count) {
        s = scenario("As Ah", "2c 3d 4h 5s 6c", POKER_MAX_OPPONENTS);
        s.board_count = count;
        s.to_call = 0; s.bet_size = 0; s.trials = 1000000;
        s.opponents[0].fold_to_bet = 0;
        s.opponents[1].fold_to_bet = 1;
        CHECK(poker_scenario_validate(&s, NULL, 0));
    }
    s = valid; s.to_call = s.hero_stack;
    CHECK(poker_scenario_validate(&s, NULL, 0));
}

static void parse_range(PokerRange *range, const char *text)
{
    char error[160];
    CHECK(poker_range_parse(range, text, 0, error, sizeof error));
}

static void exact_equity(const PokerScenario *s, const PokerRange *ranges,
                         double share, double wins, double ties)
{
    PokerEquity e;
    char error[160];
    bool ok = poker_equity(s, ranges, &e, error, sizeof error);
    CHECK(ok);
    if (!ok) { (void)fprintf(stderr, "equity: %s\n", error); return; }
    CHECK(e.trials == s->trials);
    CHECK(near(e.equity, share));
    CHECK(near(e.win_rate, wins));
    CHECK(near(e.tie_rate, ties));
    CHECK(e.standard_error < 1e-8);
}

static void test_equity(void)
{
    PokerRange ranges[POKER_MAX_OPPONENTS];
    PokerEquity a, b;
    char error[160];
    PokerScenario s = scenario("As Ah", "", 1);
    s.trials = 3000;
    parse_range(&ranges[0], "KK");
    CHECK(poker_equity(&s, ranges, &a, error, sizeof error));
    CHECK(poker_equity(&s, ranges, &b, error, sizeof error));
    CHECK(a.trials == 3000 && a.available_combos[0] == 6);
    CHECK(fabs(a.equity - 0.82) < 0.035);
    CHECK(a.equity == b.equity && a.win_rate == b.win_rate && a.tie_rate == b.tie_rate);
    CHECK(a.standard_error == b.standard_error && a.trials == b.trials);
    CHECK(a.standard_error > 0 && a.standard_error < 0.01);
    CHECK(a.win_rate + a.tie_rate <= 1 && a.equity >= a.win_rate);
    (void)printf("AA vs KK, seed 42, 3000 trials: %.6f equity\n", a.equity);

    s = scenario("As Ah", "Ac 7d 5h 3c 2s", 1);
    parse_range(&ranges[0], "KsKh,AsKd,AcQd");
    exact_equity(&s, ranges, 1, 1, 0);
    CHECK(poker_equity(&s, ranges, &a, error, sizeof error));
    CHECK(a.available_combos[0] == 1);
    s = scenario("Ks Kh", "Ac 7d 5h 3c 2s", 1);
    parse_range(&ranges[0], "AsAh");
    exact_equity(&s, ranges, 0, 0, 0);
    s = scenario("2c 3d", "As Ks Qs Js Ts", POKER_MAX_OPPONENTS);
    s.opponent_count = 1;
    parse_range(&ranges[0], "4c5d");
    exact_equity(&s, ranges, 0.5, 0, 1);
    const char *const opponents[] = {"4c5d", "6c7d", "8c9d", "TcJd",
                                    "QcKd", "Ac2d", "3c4d", "5c6d"};
    for (size_t i = 0; i < POKER_MAX_OPPONENTS; ++i) parse_range(&ranges[i], opponents[i]);
    s.opponent_count = 2;
    exact_equity(&s, ranges, 1.0 / 3, 0, 1);
    s.opponent_count = 3;
    exact_equity(&s, ranges, 0.25, 0, 1);
    s.opponent_count = POKER_MAX_OPPONENTS;
    exact_equity(&s, ranges, 1.0 / 9, 0, 1);
    s = scenario("As Kd", "Ac Kh 7s 5d 2c", 2);
    parse_range(&ranges[0], "AhKc");
    parse_range(&ranges[1], "QsQh");
    exact_equity(&s, ranges, 0.5, 0, 1);
    parse_range(&ranges[1], "7c7d");
    exact_equity(&s, ranges, 0, 0, 0); /* Tying one opponent is not a tie if another wins. */

    s = scenario("2c 3d", "As Ks Qs Js Ts", 2);
    parse_range(&ranges[0], "4c5d,6c7d");
    parse_range(&ranges[1], "4c8d,9cTd");
    exact_equity(&s, ranges, 1.0 / 3, 0, 1); /* Some draws collide, compatible ones survive. */
    parse_range(&ranges[0], "4c5d");
    parse_range(&ranges[1], "4c8d");
    CHECK(!poker_equity(&s, ranges, &a, error, sizeof error));
    CHECK(strstr(error, "compatible") != NULL && a.trials == 0);

    s = scenario("As Ah", "Ac 7d 5h 3c 2s", 1);
    parse_range(&ranges[0], "AsKd");
    CHECK(!poker_equity(&s, ranges, &a, error, sizeof error));
    CHECK(strstr(error, "blockers") != NULL);
    parse_range(&ranges[0], "KsKh");
    CHECK(!poker_equity(NULL, ranges, &a, error, sizeof error));
    CHECK(!poker_equity(&s, NULL, &a, NULL, 0));
    CHECK(!poker_equity(&s, ranges, NULL, NULL, 0));
    PokerRange good = ranges[0];
#define BAD_RANGE(change) do { \
    ranges[0] = good; change; error[0] = '\0'; \
    CHECK(!poker_equity(&s, ranges, &a, error, sizeof error)); \
    CHECK(error[0] != '\0'); \
} while (0)
    BAD_RANGE(ranges[0].count = 0);
    BAD_RANGE(ranges[0].count = POKER_MAX_COMBOS + 1);
    BAD_RANGE(ranges[0].combos[0].cards[0] = -1);
    BAD_RANGE(ranges[0].combos[0].cards[1] = 52);
    BAD_RANGE(ranges[0].combos[0].cards[1] = ranges[0].combos[0].cards[0]);
    BAD_RANGE(ranges[0].combos[0].weight = 0);
    BAD_RANGE(ranges[0].combos[0].weight = -1);
    BAD_RANGE(ranges[0].combos[0].weight = NAN);
    BAD_RANGE(ranges[0].combos[0].weight = INFINITY);
    BAD_RANGE(ranges[0].count = 2; ranges[0].combos[1] = ranges[0].combos[0];
              ranges[0].combos[0].weight = DBL_MAX; ranges[0].combos[1].weight = DBL_MAX);
#undef BAD_RANGE
    /* Weighted river outcomes expose accidental uniform combo sampling. */
    s = scenario("Ks Kh", "Ac 7d 5h 3c 2s", 1);
    s.trials = 3000;
    parse_range(&ranges[0], "AsAh,QcQd");
    for (size_t i = 0; i < ranges[0].count; ++i)
        ranges[0].combos[i].weight = ranges[0].combos[i].cards[0] / 4 == 12 ? 3 : 1;
    CHECK(poker_equity(&s, ranges, &a, error, sizeof error));
    CHECK(fabs(a.equity - 0.25) < 0.035);

    /* Joint rejection preserves product weights: compatible mass is 3:1:1.
     * Resampling only the colliding player would bias the tie frequency. */
    s = scenario("As Kd", "Ac Kh 7s 5d 2c", 2);
    s.trials = 3000;
    parse_range(&ranges[0], "AhKc,QsQh");
    ranges[0].combos[0].weight = 3;
    parse_range(&ranges[1], "AhJd,JcTd");
    CHECK(poker_equity(&s, ranges, &a, error, sizeof error));
    CHECK(fabs(a.equity - 0.7) < 0.025);
    CHECK(fabs(a.win_rate - 0.4) < 0.04);
    CHECK(fabs(a.tie_rate - 0.6) < 0.04);
    CHECK(a.available_combos[0] == 2 && a.available_combos[1] == 2);
}

static void check_advice(const PokerScenario *s, const PokerEquity *e)
{
    PokerAdvice a, b;
    poker_advice(s, e, &a);
    poker_advice(s, e, &b);
    CHECK(near(a.fold_frequency + a.call_frequency + a.aggressive_frequency, 1));
    CHECK(a.fold_frequency >= 0 && a.fold_frequency <= 1);
    CHECK(a.call_frequency >= 0 && a.call_frequency <= 1);
    CHECK(a.aggressive_frequency >= 0 && a.aggressive_frequency <= 1);
    CHECK(near(a.fold_ev, 0));
    CHECK(near(a.call_ev, e->equity * (s->pot + s->to_call) - s->to_call));
    CHECK(isfinite(a.aggressive_ev) && a.risk <= s->hero_stack);
    CHECK(a.fold_frequency == b.fold_frequency && a.call_frequency == b.call_frequency &&
          a.aggressive_frequency == b.aggressive_frequency);
    CHECK(strcmp(a.sampled_action, b.sampled_action) == 0);
    if (s->to_call == 0) CHECK(a.fold_frequency == 0);
    if (!a.aggressive_available) CHECK(a.aggressive_frequency == 0);
    if (strcmp(a.sampled_action, "fold") == 0) CHECK(s->to_call > 0 && a.fold_frequency > 0);
    else if (strcmp(a.sampled_action, "call") == 0) CHECK(s->to_call > 0 && a.call_frequency > 0);
    else if (strcmp(a.sampled_action, "check") == 0) CHECK(s->to_call == 0 && a.call_frequency > 0);
    else if (strcmp(a.sampled_action, "raise") == 0) CHECK(s->to_call > 0 && a.aggressive_available && a.aggressive_frequency > 0);
    else if (strcmp(a.sampled_action, "bet") == 0) CHECK(s->to_call == 0 && a.aggressive_available && a.aggressive_frequency > 0);
    else CHECK(false);
}

static void test_advice(void)
{
    PokerScenario s = scenario("As Ah", "", 1);
    PokerEquity e = {0};
    PokerAdvice a;
    for (size_t opponents = 1; opponents <= POKER_MAX_OPPONENTS; ++opponents) {
        s = scenario("As Ah", "", opponents);
        for (unsigned i = 0; i <= 10; ++i) {
            e.equity = i / 10.0;
            e.standard_error = i % 2 ? 0.02 : 0;
            check_advice(&s, &e);
            s.to_call = 0;
            check_advice(&s, &e);
            s.to_call = 20;
        }
        poker_advice(&s, &e, &a);
        CHECK(a.aggressive_available == (opponents == 1));
    }
    s = scenario("As Ah", "", 1);
    e.equity = 0.5; e.standard_error = 0;
    poker_advice(&s, &e, &a);
    CHECK(near(a.risk, 60) && a.aggressive_available);
    CHECK(near(a.aggressive_ev, 0.3 * 100 + 0.7 * (0.5 * 200 - 60)));
    CHECK(a.aggressive_frequency == 0);
    CHECK(strcmp(a.sampled_action, "raise") != 0 && strcmp(a.sampled_action, "bet") != 0);
    s.bet_size = 0;
    poker_advice(&s, &e, &a);
    CHECK(near(a.risk, 110));
    s.bet_size = 1000;
    poker_advice(&s, &e, &a);
    CHECK(near(a.risk, s.hero_stack));
    s.bet_size = s.to_call;
    poker_advice(&s, &e, &a);
    CHECK(!a.aggressive_available);
    check_advice(&s, &e);
    s.bet_size = 60; s.opponents[0].stack = 39;
    poker_advice(&s, &e, &a);
    CHECK(!a.aggressive_available);
    s.opponents[0].stack = 40;
    poker_advice(&s, &e, &a);
    CHECK(a.aggressive_available);
    s.hero_stack = s.to_call;
    poker_advice(&s, &e, &a);
    CHECK(!a.aggressive_available);
    check_advice(&s, &e);
    /* At exact pot odds, fold and call both belong in the mix. */
    s = scenario("As Ah", "", 2);
    e.equity = s.to_call / (s.pot + s.to_call);
    poker_advice(&s, &e, &a);
    CHECK(near(a.fold_frequency, 0.5) && near(a.call_frequency, 0.5));
    CHECK(!a.aggressive_available && a.aggressive_frequency == 0);
}

static void cleanup(void)
{
    if (scratch[0] == '\0') return;
    DIR *dir = opendir(scratch);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[8192];
            int length = snprintf(path, sizeof path, "%s/%s", scratch, entry->d_name);
            if (length < 0 || (size_t)length >= sizeof path) { CHECK(false); continue; }
            struct stat st;
            if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) CHECK(rmdir(path) == 0);
            else CHECK(unlink(path) == 0);
        }
        CHECK(closedir(dir) == 0);
    } else CHECK(false);
    CHECK(rmdir(scratch) == 0);
    scratch[0] = '\0';
}

static size_t snapshot(const char *path, unsigned char *out, size_t capacity)
{
    FILE *stream = fopen(path, "rb");
    CHECK(stream != NULL);
    if (stream == NULL) return 0;
    size_t count = fread(out, 1, capacity, stream);
    CHECK(!ferror(stream) && count < capacity);
    CHECK(fclose(stream) == 0);
    return count;
}

static void same_profile(const PokerProfile *a, const PokerProfile *b)
{
    CHECK(strcmp(a->name, b->name) == 0);
    CHECK(a->hands == b->hands && a->vpip == b->vpip && a->pfr == b->pfr);
    CHECK(a->bets == b->bets && a->raises == b->raises && a->calls == b->calls);
    CHECK(a->faced_bet == b->faced_bet && a->folded_to_bet == b->folded_to_bet);
}

static void test_profiles(void)
{
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL) tmp = "/tmp";
    int length = snprintf(scratch, sizeof scratch, "%s/poker-tests-XXXXXX", tmp);
    CHECK(length >= 0 && (size_t)length < sizeof scratch);
    if (length < 0 || (size_t)length >= sizeof scratch) { scratch[0] = '\0'; return; }
    if (mkdtemp(scratch) == NULL) { CHECK(false); scratch[0] = '\0'; return; }
    CHECK(atexit(cleanup) == 0);
    char path[8192], blocked[8192];
    length = snprintf(path, sizeof path, "%s/profiles.tsv", scratch);
    CHECK(length > 0 && (size_t)length < sizeof path);
    length = snprintf(blocked, sizeof blocked, "%s/directory", scratch);
    CHECK(length > 0 && (size_t)length < sizeof blocked);
    char error[160];
    PokerProfile alice = {"Alice_1", 100, 30, 20, 11, 12, 13, 40, 10};
    PokerProfile bob = {"Bob-2", 50, 25, 10, 4, 5, 6, 20, 15}, loaded;
    CHECK(poker_profile_load(path, "Alice_1", &loaded, error, sizeof error));
    CHECK(strcmp(loaded.name, "Alice_1") == 0 && loaded.hands == 0);
    CHECK(access(path, F_OK) != 0);
    CHECK(near(poker_profile_vpip(&loaded), 0.2));
    CHECK(near(poker_profile_pfr(&loaded), 0.1));
    CHECK(near(poker_profile_fold(&loaded), 0.5));
    CHECK(near(poker_profile_vpip(NULL), 0.2));
    CHECK(near(poker_profile_pfr(NULL), 0.1));
    CHECK(near(poker_profile_fold(NULL), 0.5));
    CHECK(near(poker_profile_vpip(&alice), 32.0 / 110));
    CHECK(near(poker_profile_pfr(&alice), 21.0 / 110));
    CHECK(near(poker_profile_fold(&alice), 12.0 / 44));
    CHECK(poker_profile_save(path, &alice, error, sizeof error) && error[0] == '\0');
    CHECK(poker_profile_save(path, &bob, error, sizeof error));
    CHECK(poker_profile_load(path, alice.name, &loaded, error, sizeof error));
    same_profile(&loaded, &alice);
    CHECK(poker_profile_load(path, loaded.name, &loaded, error, sizeof error));
    same_profile(&loaded, &alice); /* Name may alias the destination. */
    alice.hands = 101; alice.calls = 14;
    CHECK(poker_profile_save(path, &alice, error, sizeof error));
    CHECK(poker_profile_load(path, alice.name, &loaded, error, sizeof error));
    same_profile(&loaded, &alice);
    CHECK(poker_profile_load(path, bob.name, &loaded, error, sizeof error));
    same_profile(&loaded, &bob);
    CHECK(poker_profile_load(path, "Unknown", &loaded, error, sizeof error));
    CHECK(strcmp(loaded.name, "Unknown") == 0 && loaded.hands == 0 && loaded.calls == 0);

    unsigned char before[4096], after[4096];
    size_t before_count = snapshot(path, before, sizeof before);
    PokerProfile invalid = alice;
    invalid.pfr = invalid.vpip + 1;
    CHECK(!poker_profile_save(path, &invalid, error, sizeof error));
    size_t after_count = snapshot(path, after, sizeof after);
    CHECK(before_count == after_count && memcmp(before, after, before_count) == 0);
    invalid = alice; invalid.vpip = invalid.hands + 1;
    CHECK(!poker_profile_save(path, &invalid, NULL, 0));
    invalid = alice; invalid.folded_to_bet = invalid.faced_bet + 1;
    CHECK(!poker_profile_save(path, &invalid, NULL, 0));
    const char *const names[] = {"", "../Alice", "A B", "A\tB", "A/B", "A.B"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        CHECK(!poker_profile_load(path, names[i], &loaded, NULL, 0));
        invalid = alice;
        (void)snprintf(invalid.name, sizeof invalid.name, "%s", names[i]);
        CHECK(!poker_profile_save(path, &invalid, NULL, 0));
    }
    invalid = alice; memset(invalid.name, 'A', sizeof invalid.name);
    CHECK(!poker_profile_save(path, &invalid, NULL, 0));
    char long_name[POKER_NAME_SIZE + 1];
    memset(long_name, 'A', sizeof long_name - 1); long_name[sizeof long_name - 1] = '\0';
    CHECK(!poker_profile_load(path, long_name, &loaded, NULL, 0));
    CHECK(!poker_profile_load(NULL, "Alice", &loaded, NULL, 0));
    CHECK(!poker_profile_load("", "Alice", &loaded, NULL, 0));
    CHECK(!poker_profile_load(path, NULL, &loaded, NULL, 0));
    CHECK(!poker_profile_load(path, "Alice", NULL, NULL, 0));
    CHECK(!poker_profile_save(NULL, &alice, NULL, 0));
    CHECK(!poker_profile_save("", &alice, NULL, 0));
    CHECK(!poker_profile_save(path, NULL, NULL, 0));
    after_count = snapshot(path, after, sizeof after);
    CHECK(before_count == after_count && memcmp(before, after, before_count) == 0);
    CHECK(mkdir(blocked, 0700) == 0);
    CHECK(!poker_profile_save(blocked, &alice, error, sizeof error));
    CHECK(error[0] != '\0');

    const char *const corrupt[] = {
        "bad row\n", "Alice\t1\t0\n", "Alice\t1\t0\t0\t0\t0\t0\t0\t0\textra\n",
        "Alice\t-1\t0\t0\t0\t0\t0\t0\t0\n",
        "Alice\t+1\t0\t0\t0\t0\t0\t0\t0\n",
        "Alice\t 1\t0\t0\t0\t0\t0\t0\t0\n",
        "Alice\t18446744073709551616\t0\t0\t0\t0\t0\t0\t0\n",
        "Alice\t1\t2\t0\t0\t0\t0\t0\t0\n",
        "Alice\t1\t1\t2\t0\t0\t0\t0\t0\n",
        "Alice\t1\t0\t0\t0\t0\t0\t1\t2\n",
        "bad name\t1\t0\t0\t0\t0\t0\t0\t0\n", "\n",
        "Alice\t1\t0\t0\t0\t0\t0\t0\t0\nAlice\t1\t0\t0\t0\t0\t0\t0\t0\n"
    };
    for (size_t i = 0; i < sizeof corrupt / sizeof corrupt[0] + 2; ++i) {
        FILE *stream = fopen(path, "wb");
        CHECK(stream != NULL);
        if (stream == NULL) continue;
        CHECK(fprintf(stream, "Good\t1\t0\t0\t0\t0\t0\t0\t0\n") > 0);
        if (i < sizeof corrupt / sizeof corrupt[0]) CHECK(fprintf(stream, "%s", corrupt[i]) > 0);
        else if (i == sizeof corrupt / sizeof corrupt[0])
            CHECK(fprintf(stream, "Alice\t1\t0\t0\t0\t0\t0\t0\t0%cjunk\n", 0) > 0);
        else CHECK(fprintf(stream, "%0300d\n", 0) > 0);
        CHECK(fclose(stream) == 0);
        before_count = snapshot(path, before, sizeof before);
        CHECK(!poker_profile_load(path, "Good", &loaded, error, sizeof error));
        CHECK(error[0] != '\0'); /* Even a requested valid row cannot hide corrupt later rows. */
        CHECK(!poker_profile_save(path, &alice, error, sizeof error));
        after_count = snapshot(path, after, sizeof after);
        CHECK(before_count == after_count && memcmp(before, after, before_count) == 0);
    }
    FILE *stream = fopen(path, "wb");
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(fprintf(stream, "Alice\t1\t1\t1\t2\t3\t4\t5\t5\r\n"
                              "Bob\t0\t0\t0\t0\t0\t0\t0\t0") > 0);
        CHECK(fclose(stream) == 0);
        CHECK(poker_profile_load(path, "Alice", &loaded, error, sizeof error));
        CHECK(loaded.hands == 1 && loaded.calls == 4 && loaded.folded_to_bet == 5);
        CHECK(poker_profile_load(path, "Bob", &loaded, error, sizeof error));
        CHECK(loaded.hands == 0); /* CRLF and final row without newline are valid. */
    }
    CHECK(unlink(path) == 0);
    PokerProfile maximum = {0};
    memset(maximum.name, 'A', sizeof maximum.name - 1);
    maximum.hands = maximum.vpip = maximum.pfr = UINT64_MAX;
    maximum.bets = maximum.raises = maximum.calls = UINT64_MAX;
    maximum.faced_bet = maximum.folded_to_bet = UINT64_MAX;
    CHECK(poker_profile_save(path, &maximum, error, sizeof error));
    CHECK(poker_profile_load(path, maximum.name, &loaded, error, sizeof error));
    same_profile(&maximum, &loaded);
    CHECK(near(poker_profile_vpip(&maximum), 1));
    CHECK(near(poker_profile_pfr(&maximum), 1));
    CHECK(near(poker_profile_fold(&maximum), 1));
    /* A successful save must replace the inode, not truncate the original file. */
    FILE *old = fopen(path, "rb");
    CHECK(old != NULL);
    before_count = snapshot(path, before, sizeof before);
    CHECK(poker_profile_save(path, &alice, error, sizeof error));
    if (old != NULL) {
        after_count = fread(after, 1, sizeof after, old);
        CHECK(!ferror(old));
        CHECK(before_count == after_count && memcmp(before, after, before_count) == 0);
        CHECK(fclose(old) == 0);
    }
    CHECK(poker_profile_load(path, maximum.name, &loaded, error, sizeof error));
    same_profile(&maximum, &loaded);
    CHECK(poker_profile_load(path, alice.name, &loaded, error, sizeof error));
    same_profile(&alice, &loaded);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(blocked) == 0);
    DIR *dir = opendir(scratch);
    CHECK(dir != NULL);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
            CHECK(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);
        CHECK(closedir(dir) == 0); /* No temporary save files left behind. */
    }
    cleanup();
}

int main(void)
{
    test_cards();
    test_hands();
    test_exhaustive();
    test_ranges();
    test_rng();
    test_validation();
    test_equity();
    test_advice();
    test_profiles();
    (void)printf("%u checks, %u failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
