#include "poker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Odds check failed at line %d: %s\n", __LINE__, #x); return EXIT_FAILURE; } } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-9)

int main(void) {
    PokerScenario s = {.hero = {51, 45}, .opponent_count = 1, .pot = 100,
        .to_call = 25, .hero_stack = 500, .trials = 100};
    s.opponents[0] = (PokerOpponent){.stack = 500, .range_percent = 100};
    PokerEquity e = {.equity = 0.2};
    PokerOddsAssumptions a = {1, 0, 0, 0, 100};
    PokerOdds o; char error[256];
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    NEAR(o.pot_odds, 0.2); NEAR(o.immediate_ev, 0); NEAR(o.adjusted_ev, 0);
    NEAR(o.required_future_win, 0); NEAR(o.break_even_equity, 0.2);
    e.equity = 0.1;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    NEAR(o.adjusted_ev, -12.5); NEAR(o.required_future_win, 125);
    a.future_win = 125;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    NEAR(o.adjusted_ev, 0); NEAR(o.break_even_equity, 0.1);
    a.future_loss = 10;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    NEAR(o.adjusted_ev, -9); NEAR(o.required_future_win, 215);
    a = (PokerOddsAssumptions){0.5, 0, 0, 5, 3}; e.equity = 0.4;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    NEAR(o.rake, 3); NEAR(o.effective_equity, 0.2);
    NEAR(o.immediate_ev, 23.8); NEAR(o.adjusted_ev, -0.6);
    a.realization = 1.1; CHECK(!poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    a.realization = NAN; CHECK(!poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    a.realization = 1; a.future_loss = 476;
    CHECK(!poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    a.future_loss = 0; a.future_win = 501;
    CHECK(!poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    a.future_win = 0; e.equity = 0;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    CHECK(isinf(o.required_future_win));
    a.rake_percent = 100; a.rake_cap = 1000;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    CHECK(isinf(o.break_even_equity));
    s.to_call = 0;
    CHECK(poker_odds(&s, &e, &a, &o, error, sizeof(error)));
    NEAR(o.required_future_win, 0);
    puts("Odds sensitivity tests passed.");
    return EXIT_SUCCESS;
}
