#include "poker.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int first;
    int second;
    int score;
} ScoredCombo;

static bool range_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
    return false;
}

/* Chen-style score, stored in half-point units (not solver equity).
 * High-card points: 2..9 = 1..4.5, T=5, J=6, Q=7, K=8, A=10.
 * Pairs double that score, minimum 5. Nonpairs get +2 if suited;
 * missing ranks between cards cost 0,1,2,4,5 points for gaps 0,1,2,3,4+.
 * Add 1 for gap <= 1 with both ranks below Q. No ace-low adjustment. */
static int preflop_score(int first, int second)
{
    static const int points[13] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 20};
    const int low = first / 4;
    const int high = second / 4;
    if (low == high) {
        const int score = 2 * points[high];
        return score < 10 ? 10 : score;
    }
    static const int penalties[5] = {0, 2, 4, 8, 10};
    const int gap = high - low - 1;
    int score = points[high] - penalties[gap < 4 ? gap : 4];
    if (first % 4 == second % 4) {
        score += 4;
    }
    if (gap <= 1 && high < 10) {
        score += 2;
    }
    return score;
}

/* Ties: higher high rank, higher low rank, suited first, then ascending
 * canonical card IDs. Every physical combo therefore has a total order. */
static int compare_combos(const void *left, const void *right)
{
    const ScoredCombo *a = left;
    const ScoredCombo *b = right;
    if (a->score != b->score) {
        return a->score > b->score ? -1 : 1;
    }
    if (a->second / 4 != b->second / 4) {
        return a->second / 4 > b->second / 4 ? -1 : 1;
    }
    if (a->first / 4 != b->first / 4) {
        return a->first / 4 > b->first / 4 ? -1 : 1;
    }
    const bool a_suited = a->first % 4 == a->second % 4;
    const bool b_suited = b->first % 4 == b->second % 4;
    if (a_suited != b_suited) {
        return a_suited ? -1 : 1;
    }
    if (a->first != b->first) {
        return a->first < b->first ? -1 : 1;
    }
    return (a->second > b->second) - (a->second < b->second);
}

bool poker_range_build(PokerRange *range, double percent, uint64_t dead,
                       char *error, size_t error_size)
{
    if (range == NULL) {
        return range_error(error, error_size, "Invalid range pointer");
    }
    range->count = 0;
    if (!isfinite(percent) || percent <= 0.0 || percent > 100.0) {
        return range_error(error, error_size, "Range percent must be finite and in (0, 100]");
    }
    if ((dead >> 52) != 0) {
        return range_error(error, error_size, "Invalid dead-card mask");
    }
    ScoredCombo *combos = malloc(POKER_MAX_COMBOS * sizeof *combos);
    if (combos == NULL) {
        return range_error(error, error_size, "Cannot allocate range scores");
    }
    size_t count = 0;
    for (int first = 0; first < 51; ++first) {
        for (int second = first + 1; second < 52; ++second) {
            combos[count++] = (ScoredCombo){first, second, preflop_score(first, second)};
        }
    }
    qsort(combos, count, sizeof *combos, compare_combos);
    const double requested = percent / 100.0 * (double)POKER_MAX_COMBOS;
    size_t selected = (size_t)requested;
    if ((double)selected < requested) {
        ++selected;
    }
    if (selected == 0) {
        selected = 1; /* Positive percentages can underflow during scaling. */
    }
    for (size_t i = 0; i < selected; ++i) {
        const uint64_t mask = (UINT64_C(1) << (unsigned)combos[i].first) |
                              (UINT64_C(1) << (unsigned)combos[i].second);
        if ((dead & mask) == 0) {
            range->combos[range->count++] =
                (PokerCombo){{combos[i].first, combos[i].second}, 1.0};
        }
    }
    free(combos);
    if (range->count == 0) {
        return range_error(error, error_size, "Range has no combos after removing dead cards");
    }
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    return true;
}

bool poker_range_parse(PokerRange *range, const char *text, uint64_t dead,
                       char *error, size_t error_size)
{
    if (range == NULL) {
        return range_error(error, error_size, "Invalid range pointer");
    }
    range->count = 0;
    if (text == NULL || (dead >> 52) != 0) {
        return range_error(error, error_size, "Invalid range text or dead-card mask");
    }
    uint64_t seen[52] = {0};
    size_t count = 0;
    for (;;) {
        while (isspace((unsigned char)*text)) {
            ++text;
        }
        const char *end = text;
        while (*end != '\0' && *end != ',') {
            ++end;
        }
        const char *trimmed = end;
        while (trimmed > text && isspace((unsigned char)trimmed[-1])) {
            --trimmed;
        }
        const size_t length = (size_t)(trimmed - text);
        int explicit_first = -1;
        int explicit_second = -1;
        int rank_first = -1;
        int rank_second = -1;
        char mode = '\0';
        if (length == 4) {
            const char first[3] = {text[0], text[1], '\0'};
            const char second[3] = {text[2], text[3], '\0'};
            explicit_first = poker_card_parse(first);
            explicit_second = poker_card_parse(second);
            if (explicit_first < 0 || explicit_second < 0 ||
                explicit_first == explicit_second) {
                return range_error(error, error_size, "Invalid explicit combo");
            }
            if (explicit_first > explicit_second) {
                const int swap = explicit_first;
                explicit_first = explicit_second;
                explicit_second = swap;
            }
        } else if (length == 2 || length == 3) {
            const char first[3] = {text[0], 'c', '\0'};
            const char second[3] = {text[1], 'c', '\0'};
            rank_first = poker_card_parse(first);
            rank_second = poker_card_parse(second);
            if (length == 3) {
                mode = text[2];
            }
            if (rank_first < 0 || rank_second < 0 ||
                (length == 3 && (mode != 's' && mode != 'o')) ||
                (length == 3 && rank_first == rank_second)) {
                return range_error(error, error_size, "Invalid hand class; expected QQ, AKs, AKo or AK");
            }
            rank_first /= 4;
            rank_second /= 4;
        } else {
            return range_error(error, error_size, "Invalid or empty range token");
        }
        for (int first = 0; first < 51; ++first) {
            for (int second = first + 1; second < 52; ++second) {
                if (explicit_first >= 0) {
                    if (first != explicit_first || second != explicit_second) {
                        continue;
                    }
                } else {
                    if (!((first / 4 == rank_first && second / 4 == rank_second) ||
                          (first / 4 == rank_second && second / 4 == rank_first))) {
                        continue;
                    }
                    const bool suited = first % 4 == second % 4;
                    if ((mode == 's' && !suited) || (mode == 'o' && suited)) {
                        continue;
                    }
                }
                const uint64_t first_bit = UINT64_C(1) << (unsigned)first;
                const uint64_t second_bit = UINT64_C(1) << (unsigned)second;
                if ((dead & (first_bit | second_bit)) != 0 ||
                    (seen[first] & second_bit) != 0) {
                    continue;
                }
                if (count >= POKER_MAX_COMBOS) {
                    return range_error(error, error_size, "Range capacity exceeded");
                }
                seen[first] |= second_bit;
                range->combos[count++] = (PokerCombo){{first, second}, 1.0};
            }
        }
        if (*end == '\0') {
            break;
        }
        text = end + 1;
    }
    if (count == 0) {
        return range_error(error, error_size, "Range has no combos after removing dead cards");
    }
    range->count = count;
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    return true;
}
