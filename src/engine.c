#include "poker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

bool poker_scenario_validate(const PokerScenario *s, char *error, size_t size) {
    if (!s || s->opponent_count < 1 || s->opponent_count > POKER_MAX_OPPONENTS)
        return fail(error, size, "Specify between 1 and 8 opponents.");
    if (s->board_count != 0 && s->board_count != 3 && s->board_count != 4 && s->board_count != 5)
        return fail(error, size, "Board must contain 0, 3, 4, or 5 cards.");
    uint64_t dead = 0;
    for (size_t i = 0; i < 2 + s->board_count; ++i) {
        int card = i < 2 ? s->hero[i] : s->board[i - 2];
        if (card < 0 || card >= 52 || (dead & (UINT64_C(1) << card)))
            return fail(error, size, "Invalid or repeated known card.");
        dead |= UINT64_C(1) << card;
    }
    if (!isfinite(s->pot) || s->pot <= 0 || s->pot > 1e12 || !isfinite(s->to_call) || s->to_call < 0 ||
        !isfinite(s->hero_stack) || s->hero_stack <= 0 || s->to_call > s->hero_stack ||
        s->hero_stack > 1e12 || !isfinite(s->bet_size) || s->bet_size < 0 || s->bet_size > 1e12 ||
        s->trials < 100 || s->trials > 1000000)
        return fail(error, size, "Invalid pot, call, stack, bet size, or trials (100..1000000).");
    for (size_t i = 0; i < s->opponent_count; ++i) {
        const PokerOpponent *o = &s->opponents[i];
        if (!isfinite(o->stack) || o->stack <= 0 || o->stack > 1e12 || !isfinite(o->range_percent) ||
            o->range_percent <= 0 || o->range_percent > 100 ||
            !isfinite(o->fold_to_bet) || o->fold_to_bet < 0 || o->fold_to_bet > 1)
            return fail(error, size, "Invalid opponent stack, range percentage, or fold probability.");
    }
    return true;
}

bool poker_equity(const PokerScenario *s, const PokerRange *ranges,
                  PokerEquity *result, char *error, size_t size) {
    if (!ranges || !result || !poker_scenario_validate(s, error, size)) return false;
    memset(result, 0, sizeof(*result));
    uint64_t known = (UINT64_C(1) << s->hero[0]) | (UINT64_C(1) << s->hero[1]);
    for (size_t i = 0; i < s->board_count; ++i) known |= UINT64_C(1) << s->board[i];
    double totals[POKER_MAX_OPPONENTS] = {0};
    for (size_t p = 0; p < s->opponent_count; ++p) {
        if (ranges[p].count == 0 || ranges[p].count > POKER_MAX_COMBOS)
            return fail(error, size, "Invalid range size.");
        for (size_t j = 0; j < ranges[p].count; ++j) {
            const PokerCombo *c = &ranges[p].combos[j];
            if (c->cards[0] < 0 || c->cards[0] >= 52 || c->cards[1] < 0 || c->cards[1] >= 52 ||
                c->cards[0] == c->cards[1] || !isfinite(c->weight) || c->weight <= 0)
                return fail(error, size, "Invalid range combination.");
            uint64_t mask = (UINT64_C(1) << c->cards[0]) | (UINT64_C(1) << c->cards[1]);
            if (!(known & mask)) {
                totals[p] += c->weight;
                result->available_combos[p]++;
            }
        }
        if (!isfinite(totals[p]) || totals[p] <= 0)
            return fail(error, size, "An opponent has no combinations after blockers.");
    }
    PokerRng rng;
    poker_rng_seed(&rng, s->seed);
    double sum = 0, sum_squared = 0;
    uint64_t attempts = 0;
    while (result->trials < s->trials) {
        if (++attempts > (uint64_t)s->trials * 1000)
            return fail(error, size, "Ranges overlap too heavily or have no compatible joint assignment.");
        uint64_t used = known;
        int hole[POKER_MAX_OPPONENTS][2];
        bool collision = false;
        /* Reject the entire independent draw, not individual players, to preserve
           the product of range weights conditioned on no shared cards. */
        for (size_t p = 0; p < s->opponent_count; ++p) {
            double pick = poker_rng_double(&rng) * totals[p];
            const PokerCombo *chosen = NULL;
            for (size_t j = 0; j < ranges[p].count; ++j) {
                const PokerCombo *c = &ranges[p].combos[j];
                uint64_t mask = (UINT64_C(1) << c->cards[0]) | (UINT64_C(1) << c->cards[1]);
                if (known & mask) continue;
                chosen = c;
                pick -= c->weight;
                if (pick < 0) break;
            }
            uint64_t mask = (UINT64_C(1) << chosen->cards[0]) | (UINT64_C(1) << chosen->cards[1]);
            if (used & mask) { collision = true; break; }
            used |= mask;
            hole[p][0] = chosen->cards[0]; hole[p][1] = chosen->cards[1];
        }
        if (collision) continue;
        int deck[52], n = 0, cards[7];
        for (int c = 0; c < 52; ++c) if (!(used & (UINT64_C(1) << c))) deck[n++] = c;
        memcpy(cards + 2, s->board, s->board_count * sizeof(int));
        for (size_t j = s->board_count; j < 5; ++j) {
            unsigned index = poker_rng_bounded(&rng, (unsigned)n);
            cards[j + 2] = deck[index]; deck[index] = deck[--n];
        }
        cards[0] = s->hero[0]; cards[1] = s->hero[1];
        uint32_t hero = poker_evaluate(cards, 7);
        unsigned tied = 1;
        bool beaten = false;
        for (size_t p = 0; p < s->opponent_count; ++p) {
            cards[0] = hole[p][0]; cards[1] = hole[p][1];
            uint32_t villain = poker_evaluate(cards, 7);
            if (villain > hero) beaten = true;
            if (villain == hero) ++tied;
        }
        double share = beaten ? 0 : 1.0 / tied;
        sum += share; sum_squared += share * share;
        if (!beaten && tied == 1) result->win_rate += 1;
        if (!beaten && tied > 1) result->tie_rate += 1;
        ++result->trials;
    }
    result->equity = sum / result->trials;
    result->win_rate /= result->trials;
    result->tie_rate /= result->trials;
    double variance = (sum_squared - sum * sum / result->trials) / (result->trials - 1);
    result->standard_error = sqrt(fmax(0, variance) / result->trials);
    return true;
}

void poker_advice(const PokerScenario *s, const PokerEquity *e, PokerAdvice *a) {
    memset(a, 0, sizeof(*a));
    /* One-street toy model: no future betting, side pots, rake, or range response. */
    a->call_ev = e->equity * (s->pot + s->to_call) - s->to_call;
    a->risk = s->bet_size > 0 ? s->bet_size : s->to_call + 0.75 * (s->pot + s->to_call);
    a->risk = fmin(a->risk, s->hero_stack);
    a->aggressive_available = s->opponent_count == 1 && a->risk > s->to_call &&
        a->risk - s->to_call <= s->opponents[0].stack;
    if (a->aggressive_available) {
        double fold = s->opponents[0].fold_to_bet;
        double called_ev = e->equity * (s->pot + 2 * a->risk - s->to_call) - a->risk;
        a->aggressive_ev = fold * s->pot + (1 - fold) * called_ev;
    }
    double ev[3] = {0, a->call_ev, a->aggressive_ev};
    /* Aggressive EV assumes an unchanged calling range, so it is illustrative
       only and must not drive an action recommendation. */
    bool legal[3] = {s->to_call > 0, true, false};
    double best = -INFINITY;
    for (int i = 0; i < 3; ++i) if (legal[i] && ev[i] > best) best = ev[i];
    double tolerance = fmax(s->pot * 0.01, 2 * e->standard_error * (s->pot + s->to_call));
    double weights[3] = {0}, total = 0;
    for (int i = 0; i < 3; ++i) if (legal[i] && best - ev[i] <= tolerance) {
        weights[i] = exp((ev[i] - best) / fmax(tolerance, 0.000001)); total += weights[i];
    }
    a->fold_frequency = weights[0] / total;
    a->call_frequency = weights[1] / total;
    a->aggressive_frequency = weights[2] / total;
    PokerRng rng; poker_rng_seed(&rng, s->seed ^ UINT64_C(0xa0761d6478bd642f));
    double pick = poker_rng_double(&rng);
    if (pick < a->fold_frequency) a->sampled_action = "fold";
    else if (pick < a->fold_frequency + a->call_frequency) a->sampled_action = s->to_call > 0 ? "call" : "check";
    else a->sampled_action = s->to_call > 0 ? "raise" : "bet";
}
