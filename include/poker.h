#ifndef POKER_H
#define POKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POKER_MAX_OPPONENTS 8
#define POKER_MAX_COMBOS 1326
#define POKER_NAME_SIZE 48

typedef struct { uint64_t state; } PokerRng;
typedef struct { int cards[2]; double weight; } PokerCombo;
typedef struct { PokerCombo combos[POKER_MAX_COMBOS]; size_t count; } PokerRange;
typedef struct {
    char name[POKER_NAME_SIZE];
    char position[8];
    double stack;
    double range_percent;
    double fold_to_bet;
} PokerOpponent;
typedef struct {
    int hero[2];
    int board[5];
    size_t board_count;
    PokerOpponent opponents[POKER_MAX_OPPONENTS];
    size_t opponent_count;
    double pot;
    double to_call;
    double hero_stack;
    double bet_size;
    unsigned trials;
    uint64_t seed;
} PokerScenario;
typedef struct {
    double equity;
    double win_rate;
    double tie_rate;
    double standard_error;
    unsigned trials;
    size_t available_combos[POKER_MAX_OPPONENTS];
} PokerEquity;
typedef struct {
    double fold_ev;
    double call_ev;
    double aggressive_ev;
    double fold_frequency;
    double call_frequency;
    double aggressive_frequency;
    double risk;
    bool aggressive_available;
    const char *sampled_action;
} PokerAdvice;
typedef struct {
    char name[POKER_NAME_SIZE];
    uint64_t hands;
    uint64_t vpip;
    uint64_t pfr;
    uint64_t bets;
    uint64_t raises;
    uint64_t calls;
    uint64_t faced_bet;
    uint64_t folded_to_bet;
} PokerProfile;

typedef struct {
    double realization;
    double future_win;
    double future_loss;
    double rake_percent;
    double rake_cap;
} PokerOddsAssumptions;
typedef struct {
    double pot_odds;
    double effective_equity;
    double rake;
    double immediate_ev;
    double adjusted_ev;
    double required_future_win;
    double break_even_equity;
} PokerOdds;

void poker_rng_seed(PokerRng *rng, uint64_t seed);
uint64_t poker_rng_next(PokerRng *rng);
unsigned poker_rng_bounded(PokerRng *rng, unsigned bound);
double poker_rng_double(PokerRng *rng);
int poker_card_parse(const char *text);
void poker_card_format(int card, char out[3]);
bool poker_cards_parse(const char *text, int *cards, size_t capacity,
                       size_t *count, char *error, size_t error_size);
uint32_t poker_evaluate(const int *cards, size_t count);
const char *poker_hand_category(uint32_t value);
bool poker_range_build(PokerRange *range, double percent, uint64_t dead,
                       char *error, size_t error_size);
bool poker_range_parse(PokerRange *range, const char *text, uint64_t dead,
                       char *error, size_t error_size);
bool poker_scenario_validate(const PokerScenario *scenario, char *error,
                             size_t error_size);
bool poker_equity(const PokerScenario *scenario, const PokerRange *ranges,
                  PokerEquity *result, char *error, size_t error_size);
/* Exact weighted enumeration, heads-up river only. trials counts legal combos,
 * standard_error=0; results must not be stored in the Monte Carlo cache. */
bool poker_equity_river_exact(const PokerScenario *scenario, const PokerRange *range,
                              PokerEquity *result, char *error, size_t error_size);
void poker_advice(const PokerScenario *scenario, const PokerEquity *equity,
                  PokerAdvice *advice);
bool poker_odds(const PokerScenario *scenario, const PokerEquity *equity,
                const PokerOddsAssumptions *assumptions, PokerOdds *odds,
                char *error, size_t error_size);
/* Standalone offline CLI commands. */
int poker_catalog_command(int argc, char **argv);
int poker_table_command(int argc, char **argv);
int poker_train_command(int argc, char **argv);
int poker_model_command(int argc, char **argv);
bool poker_profile_load(const char *path, const char *name, PokerProfile *profile,
                        char *error, size_t error_size);
bool poker_profile_save(const char *path, const PokerProfile *profile,
                        char *error, size_t error_size);
double poker_profile_vpip(const PokerProfile *profile);
double poker_profile_pfr(const PokerProfile *profile);
double poker_profile_fold(const PokerProfile *profile);

/* Cache entries include the exact effective ranges, known cards and RNG settings.
 * read returns true on a miss and sets hit=false; malformed entries fail. */
bool poker_cache_read(const char *directory, const PokerScenario *scenario,
                      const PokerRange *ranges, PokerEquity *equity, bool *hit,
                      char *error, size_t error_size);
bool poker_cache_write(const char *directory, const PokerScenario *scenario,
                       const PokerRange *ranges, const PokerEquity *equity,
                       char *error, size_t error_size);
bool poker_history_save(const char *directory, const char *id,
                        const PokerScenario *scenario, const PokerRange *ranges,
                        const PokerEquity *equity, const PokerAdvice *advice,
                        bool has_profit, double profit, char *error, size_t error_size);
bool poker_history_show(const char *directory, const char *id,
                        char *error, size_t error_size);
bool poker_history_stats(const char *directory, char *error, size_t error_size);

#endif
