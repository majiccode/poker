#include "poker.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool poker_equity_river_exact(const PokerScenario *s, const PokerRange *r,
                              PokerEquity *e, char *error, size_t size) {
    if (!s || !r || !e || !poker_scenario_validate(s, error, size)) return false;
    if (s->board_count != 5 || s->opponent_count != 1 || r->count == 0 || r->count > POKER_MAX_COMBOS) {
        if (error && size) snprintf(error, size, "Exact enumeration requires one opponent and a complete river board.");
        return false;
    }
    memset(e, 0, sizeof(*e));
    int cards[7] = {s->hero[0], s->hero[1]};
    memcpy(cards + 2, s->board, sizeof(s->board));
    uint64_t known = 0, seen[52] = {0};
    for (int i = 0; i < 7; ++i) known |= UINT64_C(1) << cards[i];
    uint32_t hero = poker_evaluate(cards, 7);
    double maximum = 0;
    for (size_t i = 0; i < r->count; ++i) {
        const PokerCombo *c = &r->combos[i];
        int a = c->cards[0], b = c->cards[1];
        if (a < 0 || a >= 52 || b < 0 || b >= 52 || a == b || !isfinite(c->weight) || c->weight <= 0) {
            if (error && size) snprintf(error, size, "Invalid weighted range combination.");
            return false;
        }
        if (a > b) { int swap = a; a = b; b = swap; }
        if (seen[a] & (UINT64_C(1) << b)) {
            if (error && size) snprintf(error, size, "Duplicate range combination.");
            return false;
        }
        seen[a] |= UINT64_C(1) << b;
        if (!(known & ((UINT64_C(1) << a) | (UINT64_C(1) << b)))) maximum = fmax(maximum, c->weight);
    }
    if (maximum == 0) {
        if (error && size) snprintf(error, size, "No legal combinations after blockers.");
        return false;
    }
    double total = 0;
    for (size_t i = 0; i < r->count; ++i) {
        const PokerCombo *c = &r->combos[i];
        if (known & ((UINT64_C(1) << c->cards[0]) | (UINT64_C(1) << c->cards[1]))) continue;
        cards[0] = c->cards[0]; cards[1] = c->cards[1];
        uint32_t villain = poker_evaluate(cards, 7);
        double weight = c->weight / maximum;
        total += weight;
        if (hero > villain) e->win_rate += weight;
        if (hero == villain) e->tie_rate += weight;
        ++e->trials;
    }
    e->win_rate /= total; e->tie_rate /= total;
    e->equity = e->win_rate + e->tie_rate / 2;
    e->available_combos[0] = e->trials;
    return true;
}
