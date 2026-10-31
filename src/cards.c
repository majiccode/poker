#include "poker.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char ranks[] = "23456789TJQKA";
static const char suits[] = "cdhs";

void poker_rng_seed(PokerRng *rng, uint64_t seed)
{
    rng->state = seed != 0 ? seed : UINT64_C(0x9e3779b97f4a7c15);
}

uint64_t poker_rng_next(PokerRng *rng)
{
    uint64_t state = rng->state;
    if (state == 0) {
        state = UINT64_C(0x9e3779b97f4a7c15);
    }
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    rng->state = state;
    return state * UINT64_C(2685821657736338717);
}

unsigned poker_rng_bounded(PokerRng *rng, unsigned bound)
{
    if (bound == 0) {
        return 0;
    }
    const uint64_t limit = (uint64_t)bound;
    const uint64_t threshold = (UINT64_C(0) - limit) % limit;
    uint64_t value;
    do {
        value = poker_rng_next(rng);
    } while (value < threshold);
    return (unsigned)(value % limit);
}

double poker_rng_double(PokerRng *rng)
{
    return (double)(poker_rng_next(rng) >> 11) * 0x1.0p-53;
}

int poker_card_parse(const char *text)
{
    if (text == NULL || text[0] == '\0' || text[1] == '\0' ||
        text[2] != '\0') {
        return -1;
    }
    const char *rank = strchr(ranks, text[0]);
    const char *suit = strchr(suits, text[1]);
    if (rank == NULL || suit == NULL) {
        return -1;
    }
    return (int)(rank - ranks) * 4 + (int)(suit - suits);
}

void poker_card_format(int card, char out[3])
{
    if (out == NULL) {
        return;
    }
    out[0] = card >= 0 && card < 52 ? ranks[card / 4] : '?';
    out[1] = card >= 0 && card < 52 ? suits[card % 4] : '?';
    out[2] = '\0';
}

static bool cards_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return false;
}

bool poker_cards_parse(const char *text, int *cards, size_t capacity,
                       size_t *count, char *error, size_t error_size)
{
    if (count != NULL) {
        *count = 0;
    }
    if (text == NULL || count == NULL || (cards == NULL && capacity != 0)) {
        return cards_error(error, error_size, "Invalid card parser arguments");
    }
    uint64_t seen = 0;
    size_t used = 0;
    while (*text != '\0') {
        if (isspace((unsigned char)*text) || *text == ',') {
            ++text;
            continue;
        }
        if (text[1] == '\0') {
            return cards_error(error, error_size, "Incomplete card");
        }
        const char token[3] = {text[0], text[1], '\0'};
        const int card = poker_card_parse(token);
        if (card < 0) {
            return cards_error(error, error_size, "Invalid card; expected rank 23456789TJQKA and suit cdhs");
        }
        const uint64_t bit = UINT64_C(1) << (unsigned)card;
        if ((seen & bit) != 0) {
            return cards_error(error, error_size, "Duplicate card");
        }
        if (used >= capacity) {
            return cards_error(error, error_size, "Card capacity exceeded");
        }
        cards[used++] = card;
        seen |= bit;
        text += 2;
    }
    *count = used;
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    return true;
}

/* Category occupies bits 20..23; five tie-break ranks occupy bits 0..19.
 * Ranks are 2..14, unused digits are zero, and larger values win. */
static uint32_t evaluate_five(const int cards[5])
{
    unsigned counts[15] = {0};
    unsigned ordered[5] = {0};
    unsigned pairs[2] = {0};
    unsigned pair_count = 0;
    unsigned triple = 0;
    unsigned four = 0;
    bool flush = true;
    for (size_t i = 0; i < 5; ++i) {
        ++counts[(unsigned)(cards[i] / 4) + 2U];
        if (cards[i] % 4 != cards[0] % 4) {
            flush = false;
        }
    }
    size_t used = 0;
    for (unsigned rank = 14; rank >= 2; --rank) {
        for (unsigned i = 0; i < counts[rank]; ++i) {
            ordered[used++] = rank;
        }
        if (counts[rank] == 4) {
            four = rank;
        } else if (counts[rank] == 3) {
            triple = rank;
        } else if (counts[rank] == 2) {
            pairs[pair_count++] = rank;
        }
    }
    unsigned straight = 0;
    for (unsigned high = 14; high >= 6; --high) {
        if (counts[high] && counts[high - 1] && counts[high - 2] &&
            counts[high - 3] && counts[high - 4]) {
            straight = high;
            break;
        }
    }
    if (straight == 0 && counts[14] && counts[5] && counts[4] &&
        counts[3] && counts[2]) {
        straight = 5;
    }
    unsigned category = 0;
    unsigned digits[5] = {0};
    size_t digit_count = 0;
    if (flush && straight != 0) {
        category = 8;
        digits[digit_count++] = straight;
    } else if (four != 0) {
        category = 7;
        digits[digit_count++] = four;
    } else if (triple != 0 && pair_count != 0) {
        category = 6;
        digits[digit_count++] = triple;
        digits[digit_count++] = pairs[0];
    } else if (flush) {
        category = 5;
        memcpy(digits, ordered, sizeof digits);
    } else if (straight != 0) {
        category = 4;
        digits[digit_count++] = straight;
    } else if (triple != 0) {
        category = 3;
        digits[digit_count++] = triple;
    } else if (pair_count == 2) {
        category = 2;
        digits[digit_count++] = pairs[0];
        digits[digit_count++] = pairs[1];
    } else if (pair_count == 1) {
        category = 1;
        digits[digit_count++] = pairs[0];
    } else {
        memcpy(digits, ordered, sizeof digits);
    }
    if (category == 1 || category == 2 || category == 3 || category == 7) {
        for (unsigned rank = 14; rank >= 2; --rank) {
            if (counts[rank] == 1) {
                digits[digit_count++] = rank;
            }
        }
    }
    uint32_t value = (uint32_t)category;
    for (size_t i = 0; i < 5; ++i) {
        value = (value << 4) | (uint32_t)digits[i];
    }
    return value;
}

uint32_t poker_evaluate(const int *cards, size_t count)
{
    if (cards == NULL || count < 5 || count > 7) {
        return 0;
    }
    uint64_t seen = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cards[i] < 0 || cards[i] >= 52) {
            return 0;
        }
        const uint64_t bit = UINT64_C(1) << (unsigned)cards[i];
        if ((seen & bit) != 0) {
            return 0;
        }
        seen |= bit;
    }
    uint32_t best = 0;
    for (size_t a = 0; a < count - 4; ++a) {
        for (size_t b = a + 1; b < count - 3; ++b) {
            for (size_t c = b + 1; c < count - 2; ++c) {
                for (size_t d = c + 1; d < count - 1; ++d) {
                    for (size_t e = d + 1; e < count; ++e) {
                        const int hand[5] = {cards[a], cards[b], cards[c], cards[d], cards[e]};
                        const uint32_t value = evaluate_five(hand);
                        if (value > best) {
                            best = value;
                        }
                    }
                }
            }
        }
    }
    return best;
}

const char *poker_hand_category(uint32_t value)
{
    static const char *const names[] = {
        "High card", "One pair", "Two pair", "Three of a kind", "Straight",
        "Flush", "Full house", "Four of a kind", "Straight flush"
    };
    const uint32_t category = value >> 20;
    return category < 9 ? names[category] : "Invalid";
}
