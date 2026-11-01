#define _POSIX_C_SOURCE 200809L
#include "poker.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG_COUNT 169

typedef struct {
    char label[4];
    int hero[2];
    PokerEquity equity;
    bool ready, hit;
    char error[256];
} CatalogEntry;

typedef struct {
    pthread_mutex_t mutex;
    size_t next;
    PokerScenario scenario;
    PokerRange ranges[POKER_MAX_OPPONENTS];
    const char *directory;
    CatalogEntry entries[CATALOG_COUNT];
} Catalog;

static bool catalog_integer(const char *text, uint64_t *value)
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

static bool catalog_percent(const char *text, double *value)
{
    bool digit = false, dot = false;
    for (const char *p = text; *p; ++p) {
        if (*p >= '0' && *p <= '9') digit = true;
        else if (*p == '.' && !dot) dot = true;
        else return false;
    }
    char *end;
    errno = 0;
    *value = strtod(text, &end);
    return digit && !errno && !*end && isfinite(*value) && *value > 0 && *value <= 100;
}

static bool catalog_class_range(const char *text)
{
    /* The range parser also accepts physical combos, which are not safe to map. */
    for (;;) {
        while (isspace((unsigned char)*text)) ++text;
        const char *end = text;
        while (*end && *end != ',') ++end;
        const char *trimmed = end;
        while (trimmed > text && isspace((unsigned char)trimmed[-1])) --trimmed;
        size_t length = (size_t)(trimmed - text);
        if (length != 2 && length != 3) return false;
        if (!*end) return true;
        text = end + 1;
    }
}

static void catalog_classes(CatalogEntry *entries)
{
    const char *ranks = "23456789TJQKA";
    size_t index = 0;
    for (int high = 12; high >= 0; --high) {
        for (int low = high; low >= 0; --low) {
            for (int mode = 0; mode < (high == low ? 1 : 2); ++mode) {
                CatalogEntry *entry = &entries[index++];
                bool suited = high != low && mode == 0;
                entry->label[0] = ranks[high];
                entry->label[1] = ranks[low];
                entry->label[2] = high == low ? '\0' : suited ? 's' : 'o';
                /* Fixed high-rank-first representatives: c,d or c,c. */
                entry->hero[0] = high * 4;
                entry->hero[1] = low * 4 + (suited ? 0 : 1);
            }
        }
    }
}

static void catalog_load(Catalog *catalog, size_t index, bool build)
{
    CatalogEntry *entry = &catalog->entries[index];
    PokerScenario scenario = catalog->scenario;
    memcpy(scenario.hero, entry->hero, sizeof(scenario.hero));
    if (!poker_cache_read(catalog->directory, &scenario, catalog->ranges,
                          &entry->equity, &entry->hit, entry->error,
                          sizeof(entry->error))) return;
    if (entry->hit) {
        entry->ready = true;
        return;
    }
    if (!build) return;
    if (!poker_equity(&scenario, catalog->ranges, &entry->equity,
                      entry->error, sizeof(entry->error))) return;
    if (!poker_cache_write(catalog->directory, &scenario, catalog->ranges,
                           &entry->equity, entry->error, sizeof(entry->error))) return;
    entry->ready = true;
}

static void *catalog_worker(void *argument)
{
    Catalog *catalog = argument;
    for (;;) {
        pthread_mutex_lock(&catalog->mutex);
        size_t index = catalog->next;
        if (index < CATALOG_COUNT) ++catalog->next;
        pthread_mutex_unlock(&catalog->mutex);
        if (index == CATALOG_COUNT) return NULL;
        catalog_load(catalog, index, true);
    }
}

static int catalog_rank(const void *left, const void *right)
{
    const CatalogEntry *a = *(const CatalogEntry *const *)left;
    const CatalogEntry *b = *(const CatalogEntry *const *)right;
    if (a->equity.equity != b->equity.equity)
        return a->equity.equity > b->equity.equity ? -1 : 1;
    return strcmp(a->label, b->label);
}

static bool catalog_print(const CatalogEntry *entry)
{
    const char *label = entry->label;
    bool premium = !strcmp(label, "AA") || !strcmp(label, "KK") ||
        !strcmp(label, "QQ") || !strcmp(label, "JJ") ||
        !strcmp(label, "AKs") || !strcmp(label, "AKo");
    const char *education = premium ? "premium (educational)" : "-";
    if (!entry->ready)
        return printf("%s\t%s\t-\t-\t%s\n", label,
                      *entry->error ? "error" : "pending", education) >= 0;
    const PokerEquity *e = &entry->equity;
    fprintf(stderr, "%s: trials=%u cache=%s\n", label, e->trials,
            entry->hit ? "hit" : "miss (stored)");
    return printf("%s\t%.4f\t%.4f\t%.4f\t%s\n", label, 100 * e->equity,
                  100 * fmax(0, e->equity - 1.96 * e->standard_error),
                  100 * fmin(1, e->equity + 1.96 * e->standard_error), education) >= 0;
}

int poker_catalog_command(int argc, char **argv)
{
    if (argc < 1 || (strcmp(argv[0], "build") && strcmp(argv[0], "show"))) {
        fputs("Usage: catalog build|show [--trials 2000] [--seed 42] [--jobs 4]\n"
              "  [--cache-dir PATH] [--range 'QQ,AKs,AKo' | --range-percent 100]\n"
              "  [--opponents 1] [--hero AsKh (show only)]\n"
              "Run build in the background with shell &: no child is detached.\n", stderr);
        return EXIT_FAILURE;
    }
    bool build = !strcmp(argv[0], "build");
    unsigned trials = 2000, jobs = 4;
    uint64_t seed = 42, opponents = 1;
    double percent = 100;
    const char *directory = ".poker-cache", *range_text = NULL, *hero_text = NULL;
    const char *options[] = {"--trials", "--seed", "--jobs", "--cache-dir",
                            "--range", "--range-percent", "--opponents", "--hero"};
    unsigned seen = 0;
    char error[256] = "Invalid argument.";
    for (int i = 1; i < argc; ++i) {
        size_t option = 0;
        while (option < sizeof(options) / sizeof(options[0]) &&
               strcmp(argv[i], options[option])) ++option;
        if (option == sizeof(options) / sizeof(options[0])) {
            snprintf(error, sizeof(error), "Unknown option: %s", argv[i]);
            goto invalid;
        }
        if ((seen & (1u << option)) || i + 1 >= argc) {
            snprintf(error, sizeof(error), "Repeated option or missing value: %s", argv[i]);
            goto invalid;
        }
        seen |= 1u << option;
        const char *value = argv[++i];
        uint64_t n = 0;
        snprintf(error, sizeof(error), "Invalid value for %s: %s", options[option], value);
        switch (option) {
        case 0:
            if (!catalog_integer(value, &n) || n < 100 || n > 1000000) goto invalid;
            trials = (unsigned)n;
            break;
        case 1:
            if (!catalog_integer(value, &seed)) goto invalid;
            break;
        case 2:
            if (!catalog_integer(value, &n) || n < 1 || n > 32) goto invalid;
            jobs = (unsigned)n;
            break;
        case 3:
            if (!*value) goto invalid;
            directory = value;
            break;
        case 4:
            if (!catalog_class_range(value)) {
                snprintf(error, sizeof(error),
                         "Catalog ranges must use classes (QQ, AKs, AKo, AK), not physical 4-character combos.");
                goto invalid;
            }
            range_text = value;
            break;
        case 5:
            if (!catalog_percent(value, &percent)) goto invalid;
            break;
        case 6:
            if (!catalog_integer(value, &opponents) || opponents < 1 || opponents > 8) goto invalid;
            break;
        case 7:
            if (build) {
                snprintf(error, sizeof(error), "--hero is only supported by catalog show.");
                goto invalid;
            }
            hero_text = value;
            break;
        }
    }
    Catalog *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) {
        snprintf(error, sizeof(error), "Cannot allocate catalog.");
        goto invalid;
    }
    catalog_classes(catalog->entries);
    size_t requested = CATALOG_COUNT;
    if (hero_text) {
        int cards[2];
        size_t count;
        if (!poker_cards_parse(hero_text, cards, 2, &count, error, sizeof(error)) || count != 2) {
            snprintf(error, sizeof(error), "Hero requires two distinct valid cards.");
            free(catalog);
            goto invalid;
        }
        int high = cards[0] / 4, low = cards[1] / 4;
        if (high < low) { int swap = high; high = low; low = swap; }
        bool suited = high != low && cards[0] % 4 == cards[1] % 4;
        for (size_t i = 0; i < CATALOG_COUNT; ++i) {
            const CatalogEntry *e = &catalog->entries[i];
            if (e->hero[0] / 4 == high && e->hero[1] / 4 == low &&
                (e->label[2] == 's') == suited) { requested = i; break; }
        }
    }
    catalog->directory = directory;
    catalog->scenario = (PokerScenario){.pot = 1, .hero_stack = 1000,
        .trials = trials, .seed = seed, .opponent_count = (size_t)opponents};
    for (size_t i = 0; i < opponents; ++i) {
        PokerOpponent *opponent = &catalog->scenario.opponents[i];
        snprintf(opponent->name, sizeof(opponent->name), "villain%zu", i + 1);
        strcpy(opponent->position, "BTN");
        opponent->stack = 1000;
        opponent->range_percent = percent;
        opponent->fold_to_bet = 0;
    }
    bool ok = range_text ? poker_range_parse(&catalog->ranges[0], range_text, 0, error, sizeof(error)) :
                          poker_range_build(&catalog->ranges[0], percent, 0, error, sizeof(error));
    if (!ok) { free(catalog); goto invalid; }
    for (size_t i = 1; i < opponents; ++i) catalog->ranges[i] = catalog->ranges[0];
    fprintf(stderr, "Catalog: trials=%u seed=%" PRIu64 " opponents=%" PRIu64 " cache=%s\n",
            trials, seed, opponents, directory);
    fputs("Canonical-suit equity: pairs c,d; suited c,c; offsuit c,d (high rank first).\n"
          "Arbitrary hero suits map to these representatives, not physical-hand cache entries.\n"
          "Suit mapping is valid only for suit-symmetric ranges, including explicit class ranges.\n"
          "Heuristic percent cutoffs may split suit classes; those results are canonical estimates only.\n"
          "Approximate 95% Monte Carlo intervals measure sampling noise, not range/model uncertainty.\n"
          "Premium is an educational label, not a strategy or profit guarantee.\n", stderr);
    if (requested != CATALOG_COUNT) {
        char first[3], second[3];
        poker_card_format(catalog->entries[requested].hero[0], first);
        poker_card_format(catalog->entries[requested].hero[1], second);
        fprintf(stderr, "Hero %s -> %s -> %s%s\n", hero_text,
                catalog->entries[requested].label, first, second);
    }
    int status = EXIT_SUCCESS;
    if (build) {
        int rc = pthread_mutex_init(&catalog->mutex, NULL);
        if (rc) {
            snprintf(error, sizeof(error), "Cannot initialize worker mutex: %s", strerror(rc));
            free(catalog);
            goto invalid;
        }
        pthread_t threads[32];
        unsigned started = 0;
        for (; started < jobs; ++started) {
            rc = pthread_create(&threads[started], NULL, catalog_worker, catalog);
            if (rc) {
                fprintf(stderr, "Cannot create worker: %s; completing with available workers.\n", strerror(rc));
                status = EXIT_FAILURE;
                break;
            }
        }
        if (started < jobs) catalog_worker(catalog);
        for (unsigned i = 0; i < started; ++i) {
            rc = pthread_join(threads[i], NULL);
            if (rc) {
                /* Do not free shared state when a worker's termination is unconfirmed. */
                fprintf(stderr, "Cannot join worker: %s\n", strerror(rc));
                return EXIT_FAILURE;
            }
        }
        pthread_mutex_destroy(&catalog->mutex);
    } else if (requested != CATALOG_COUNT) {
        catalog_load(catalog, requested, false);
    } else {
        for (size_t i = 0; i < CATALOG_COUNT; ++i) catalog_load(catalog, i, false);
    }
    CatalogEntry *ready[CATALOG_COUNT];
    size_t ready_count = 0, pending_count = 0, error_count = 0, hits = 0;
    for (size_t i = 0; i < CATALOG_COUNT; ++i) {
        if (requested != CATALOG_COUNT && i != requested) continue;
        CatalogEntry *entry = &catalog->entries[i];
        if (entry->ready) { ready[ready_count++] = entry; hits += entry->hit; }
        else if (*entry->error) {
            ++error_count;
            fprintf(stderr, "%s: %s\n", entry->label, entry->error);
            status = EXIT_FAILURE;
        } else ++pending_count;
    }
    qsort(ready, ready_count, sizeof(*ready), catalog_rank);
    if (printf("class\tequity_percent\tmc95_low_percent\tmc95_high_percent\tlabel\n") < 0)
        status = EXIT_FAILURE;
    for (size_t i = 0; i < ready_count; ++i)
        if (!catalog_print(ready[i])) status = EXIT_FAILURE;
    for (size_t i = 0; i < CATALOG_COUNT; ++i)
        if ((requested == CATALOG_COUNT || i == requested) && !catalog->entries[i].ready)
            if (!catalog_print(&catalog->entries[i])) status = EXIT_FAILURE;
    fprintf(stderr, "Catalog: ready=%zu pending=%zu errors=%zu cache_hits=%zu stored=%zu\n",
            ready_count, pending_count, error_count, hits, build ? ready_count - hits : 0);
    if (requested != CATALOG_COUNT && pending_count) {
        fputs("Requested hero is pending; run catalog build with matching settings.\n", stderr);
        status = EXIT_FAILURE;
    }
    if (fflush(stdout) != 0 || ferror(stderr)) status = EXIT_FAILURE;
    free(catalog);
    return status;
invalid:
    fprintf(stderr, "Error: %s\n", error);
    return EXIT_FAILURE;
}
