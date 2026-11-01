#include "poker.h"

#include <math.h>
#include <stdio.h>

bool poker_odds(const PokerScenario *s, const PokerEquity *e,
                const PokerOddsAssumptions *a, PokerOdds *o,
                char *error, size_t size) {
    if (!s || !e || !a || !o || !poker_scenario_validate(s, error, size)) return false;
    if (!isfinite(e->equity) || e->equity < 0 || e->equity > 1 ||
        !isfinite(a->realization) || a->realization < 0 || a->realization > 1 ||
        !isfinite(a->future_win) || a->future_win < 0 || a->future_win > 1e12 ||
        !isfinite(a->future_loss) || a->future_loss < 0 || a->future_loss > 1e12 ||
        !isfinite(a->rake_percent) || a->rake_percent < 0 || a->rake_percent > 100 ||
        !isfinite(a->rake_cap) || a->rake_cap < 0 || a->rake_cap > 1e12) {
        if (error && size) snprintf(error, size, "Invalid equity or odds sensitivity assumptions.");
        return false;
    }
    double opponent_chips = 0;
    for (size_t i = 0; i < s->opponent_count; ++i) opponent_chips += s->opponents[i].stack;
    if (a->future_loss > s->hero_stack - s->to_call || a->future_win > opponent_chips ||
        (s->board_count == 5 && (a->future_win > 0 || a->future_loss > 0))) {
        if (error && size) snprintf(error, size, "Future amounts exceed available stacks or are specified on the river.");
        return false;
    }
    /* Future amounts are conditional net gains/losses, not unconditional pot
       additions. Equity realization is a user-supplied stress-test proxy. */
    double terminal = s->pot + s->to_call;
    o->rake = fmin(terminal * a->rake_percent / 100, a->rake_cap);
    o->pot_odds = s->to_call / terminal;
    o->effective_equity = e->equity * a->realization;
    o->immediate_ev = e->equity * (terminal - o->rake) - s->to_call;
    o->adjusted_ev = o->effective_equity * (terminal - o->rake + a->future_win) -
        s->to_call - (1 - o->effective_equity) * a->future_loss;
    double denominator = terminal - o->rake + a->future_win + a->future_loss;
    o->break_even_equity = denominator > 0 ? (s->to_call + a->future_loss) / denominator : INFINITY;
    o->required_future_win = o->effective_equity > 0 ?
        fmax(0, (s->to_call + (1 - o->effective_equity) * a->future_loss) /
             o->effective_equity - (terminal - o->rake)) :
        (s->to_call > 0 || a->future_loss > 0 ? INFINITY : 0);
    return true;
}
