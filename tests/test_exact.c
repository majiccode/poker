#include "poker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Exact check failed at line %d: %s\n", __LINE__, #x); return EXIT_FAILURE; } } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-12)

int main(void) {
    PokerScenario s = {.opponent_count = 1, .pot = 100, .to_call = 20,
        .hero_stack = 500, .board_count = 5, .trials = 20000, .seed = 42};
    s.opponents[0] = (PokerOpponent){.stack = 500, .range_percent = 100};
    char error[256]; size_t n;
    CHECK(poker_cards_parse("AsAh", s.hero, 2, &n, error, sizeof(error)));
    CHECK(poker_cards_parse("2c3d7h9sJc", s.board, 5, &n, error, sizeof(error)));
    PokerRange r; PokerEquity e, mc;
    CHECK(poker_range_parse(&r, "KcKd,JhJd", 0, error, sizeof(error)));
    r.combos[0].weight = 3; r.combos[1].weight = 1;
    CHECK(poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    NEAR(e.equity, 0.75); NEAR(e.win_rate, 0.75); NEAR(e.tie_rate, 0);
    CHECK(e.trials == 2 && e.available_combos[0] == 2 && e.standard_error == 0);
    CHECK(poker_equity(&s, &r, &mc, error, sizeof(error)));
    CHECK(fabs(mc.equity - e.equity) < 0.02);
    r.combos[0].weight = 1e308; r.combos[1].weight = 1e308;
    CHECK(poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    NEAR(e.equity, 0.5);
    r.combos[1] = r.combos[0];
    CHECK(!poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    CHECK(poker_range_parse(&r, "AsKd,KcKd", 0, error, sizeof(error)));
    CHECK(poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    NEAR(e.equity, 1); CHECK(e.trials == 1);
    CHECK(poker_range_parse(&r, "AsKd", 0, error, sizeof(error)));
    CHECK(!poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    CHECK(poker_cards_parse("TsJsQsKsAs", s.board, 5, &n, error, sizeof(error)));
    CHECK(poker_cards_parse("2c3d", s.hero, 2, &n, error, sizeof(error)));
    CHECK(poker_range_build(&r, 100, 0, error, sizeof(error)));
    CHECK(poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    NEAR(e.equity, 0.5); NEAR(e.tie_rate, 1); CHECK(e.trials == 990);
    s.board_count = 4;
    CHECK(!poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    s.board_count = 5; s.opponent_count = 2;
    s.opponents[1] = s.opponents[0];
    CHECK(!poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    s.opponent_count = 1; r.combos[0].cards[0] = 52;
    CHECK(!poker_equity_river_exact(&s, &r, &e, error, sizeof(error)));
    puts("Exact weighted river benchmark tests passed.");
    return EXIT_SUCCESS;
}
