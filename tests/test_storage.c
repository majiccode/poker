#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "poker.h"
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cache_path(const char *directory, char *out, size_t size)
{
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(dir))) {
        if (strstr(entry->d_name, ".cache")) {
            assert(!found);
            snprintf(out, size, "%s/%s", directory, entry->d_name);
            found = true;
        }
    }
    assert(found && closedir(dir) == 0);
}

int main(void)
{
    char root[] = "/tmp/poker-storage-XXXXXX";
    assert(mkdtemp(root));
    char cache[128], hands[128], path[512], second[512], error[256];
    snprintf(cache, sizeof(cache), "%s/cache", root);
    snprintf(hands, sizeof(hands), "%s/hands", root);
    PokerScenario s = { .hero = {48, 49}, .opponent_count = 1,
        .pot = 100, .hero_stack = 200, .trials = 100, .seed = 17 };
    strcpy(s.opponents[0].name, "Player One");
    strcpy(s.opponents[0].position, "BTN");
    s.opponents[0].stack = 200;
    s.opponents[0].range_percent = 20;
    s.opponents[0].fold_to_bet = 0.4;
    PokerRange *r = calloc(POKER_MAX_OPPONENTS, sizeof(*r));
    assert(r);
    r[0].count = 2;
    r[0].combos[0] = (PokerCombo){{44, 45}, 0.5};
    r[0].combos[1] = (PokerCombo){{48, 40}, 0.25};
    PokerEquity e = {.equity = 0.75, .win_rate = 0.7, .tie_rate = 0.1,
        .standard_error = 0.02, .trials = 100, .available_combos = {1}};
    PokerEquity decoded = {0};
    bool hit = true;
    assert(poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && !hit);
    assert(poker_cache_write(cache, &s, r, &e, error, sizeof(error)));
    assert(poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && hit);
    assert(decoded.equity == e.equity && decoded.standard_error == e.standard_error);
    s.pot = NAN; s.opponents[0].stack = NAN;
    assert(poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && hit);
    s.pot = 100; s.opponents[0].stack = 200;
    r[0].combos[0].weight = nextafter(0.5, 1.0);
    assert(poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && !hit);
    r[0].combos[0].weight = 0.5;
    cache_path(cache, path, sizeof(path));
    ++s.seed;
    assert(poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && !hit);
    assert(poker_cache_write(cache, &s, r, &e, error, sizeof(error)));
    assert(unlink(path) == 0);
    cache_path(cache, second, sizeof(second));
    assert(rename(second, path) == 0);
    --s.seed;
    /* Force a valid different full key into this hash slot. */
    assert(poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && !hit);
    assert(poker_cache_write(cache, &s, r, &e, error, sizeof(error)));
    e.equity = NAN;
    assert(!poker_cache_write(cache, &s, r, &e, error, sizeof(error)));
    e.equity = 0.75; e.trials = 101;
    assert(!poker_cache_write(cache, &s, r, &e, error, sizeof(error)));
    e.trials = 100; e.available_combos[0] = 2;
    assert(!poker_cache_write(cache, &s, r, &e, error, sizeof(error)));
    e.available_combos[0] = 1;
    FILE *file = fopen(path, "a");
    assert(file && fputs("junk\n", file) >= 0 && fclose(file) == 0);
    assert(!poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && !hit);
    assert(unlink(path) == 0);
    file = fopen(path, "w");
    assert(file && fputs("format\tpoker-cache-1\nalgorithm\tpoker-equity-1\nhero\tAc Ad\nboard_count\t18446744073709551615\n", file) >= 0 && fclose(file) == 0);
    assert(!poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)) && !hit);
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);
    assert(!poker_cache_read(cache, &s, r, &decoded, &hit, error, sizeof(error)));
    assert(unlink(path) == 0);
    PokerAdvice a;
    poker_advice(&s, &e, &a);
    assert(!poker_history_save(hands, "../bad", &s, r, &e, &a, false, 0, error, sizeof(error)));
    assert(!poker_history_save(hands, "bad", &s, r, &e, &a, true, INFINITY, error, sizeof(error)));
    assert(poker_history_save(hands, "one", &s, r, &e, &a, true, -12.5, error, sizeof(error)));
    assert(!poker_history_save(hands, "one", &s, r, &e, &a, true, 500, error, sizeof(error)));
    assert(poker_history_save(hands, "two", &s, r, &e, &a, false, NAN, error, sizeof(error)));
    assert(poker_history_show(hands, "one", error, sizeof(error)));
    assert(poker_history_stats(hands, error, sizeof(error)));
    snprintf(path, sizeof(path), "%s/one.hand", hands);
    snprintf(second, sizeof(second), "%s/copy.hand", hands);
    assert(link(path, second) == 0);
    assert(!poker_history_stats(hands, error, sizeof(error)));
    assert(unlink(second) == 0);
    file = fopen(path, "a");
    assert(file && fputs("junk\n", file) >= 0 && fclose(file) == 0);
    assert(!poker_history_show(hands, "one", error, sizeof(error)));
    assert(!poker_history_stats(hands, error, sizeof(error)));
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/two.hand", hands);
    assert(unlink(path) == 0);
    assert(rmdir(cache) == 0 && rmdir(hands) == 0 && rmdir(root) == 0);
    free(r);
    puts("storage tests passed");
    return 0;
}
