#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "poker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { FILE *file; bool reading, ok; } Codec;
typedef struct { uint64_t count, labeled; double profit, equity; } Totals;

static bool fail(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

static bool safe_id(const char *id)
{
    if (!id || !*id || strlen(id) > 64) return false;
    for (; *id; ++id)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
              (*id >= '0' && *id <= '9') || *id == '_' || *id == '-')) return false;
    return true;
}

static bool text_valid(const char *s, size_t capacity)
{
    for (size_t i = 0; i < capacity; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!c) return true;
        if (c < 32 || c == 127) return false;
    }
    return false;
}

static char *path_for(const char *directory, const char *name, const char *suffix)
{
    size_t a = strlen(directory), b = strlen(name), c = strlen(suffix);
    if (a > SIZE_MAX - b - c - 2) return NULL;
    char *path = malloc(a + b + c + 2);
    if (path) snprintf(path, a + b + c + 2, "%s/%s%s", directory, name, suffix);
    return path;
}

static const char *prepare(const char *directory, bool history)
{
    if (!directory || !*directory) directory = history ? ".poker-hands" : ".poker-cache";
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return NULL;
    struct stat st;
    return stat(directory, &st) == 0 && S_ISDIR(st.st_mode) ? directory : NULL;
}

static FILE *open_record(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd); errno = EINVAL; return NULL;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) { int saved = errno; close(fd); errno = saved; }
    return file;
}

/* Each field is a newline-terminated, bounded TSV row. Embedded NULs fail. */
static void row(Codec *c, const char *tag, char *value, size_t capacity)
{
    if (!c->ok) return;
    if (!c->reading) {
        c->ok = fprintf(c->file, "%s\t%s\n", tag, value) >= 0;
        return;
    }
    char line[256];
    size_t n = 0;
    int ch;
    while ((ch = fgetc(c->file)) != EOF && ch != '\n') {
        if (!ch || n == sizeof(line) - 1) { c->ok = false; return; }
        line[n++] = (char)ch;
    }
    line[n] = 0;
    size_t t = strlen(tag);
    if (ch != '\n' || n <= t || memcmp(line, tag, t) || line[t] != '\t' ||
        n - t >= capacity) { c->ok = false; return; }
    memcpy(value, line + t + 1, n - t);
}

static uint64_t integer(Codec *c, const char *tag, uint64_t value, uint64_t limit)
{
    char text[32], canonical[32], *end;
    snprintf(text, sizeof(text), "%" PRIu64, value);
    row(c, tag, text, sizeof(text));
    if (c->ok && c->reading) {
        errno = 0;
        value = strtoull(text, &end, 10);
        snprintf(canonical, sizeof(canonical), "%" PRIu64, value);
        c->ok = !errno && !*end && !strcmp(text, canonical);
    }
    if (value > limit) { c->ok = false; return 0; }
    return value;
}

static void real(Codec *c, const char *tag, double *value)
{
    char text[64], canonical[64], *end;
    snprintf(text, sizeof(text), "%a", *value);
    row(c, tag, text, sizeof(text));
    if (c->ok && c->reading) {
        *value = strtod(text, &end);
        snprintf(canonical, sizeof(canonical), "%a", *value);
        c->ok = !*end && !strcmp(text, canonical);
    }
    if (!isfinite(*value)) c->ok = false;
}

static void literal(Codec *c, const char *tag, const char *expected)
{
    char text[80];
    snprintf(text, sizeof(text), "%s", expected);
    row(c, tag, text, sizeof(text));
    if (strcmp(text, expected)) c->ok = false;
}

static void cards(Codec *c, const char *tag, int *values, size_t count)
{
    char text[32] = {0};
    for (size_t i = 0; i < count; ++i) {
        if (values[i] < 0 || values[i] >= 52) c->ok = false;
        poker_card_format(values[i], text + i * 3);
        if (i + 1 < count) text[i * 3 + 2] = ' ';
    }
    row(c, tag, text, sizeof(text));
    if (!c->ok || !c->reading) return;
    if (strlen(text) != (count ? count * 3 - 1 : 0)) { c->ok = false; return; }
    for (size_t i = 0; i < count; ++i) {
        char card[3] = {text[i * 3], text[i * 3 + 1], 0};
        values[i] = poker_card_parse(card);
        if (values[i] < 0 || (i + 1 < count && text[i * 3 + 2] != ' ')) c->ok = false;
    }
}

#define U(tag, value, limit) ((value) = integer(c, tag, (value), limit))
#define D(tag, value) real(c, tag, &(value))

/* Algorithm version changes whenever sampling or evaluation semantics change.
 * Sequence order is significant: the sampler walks the supplied combos in order. */
static void key_fields(Codec *c, PokerScenario *s, PokerRange *ranges)
{
    literal(c, "algorithm", "poker-equity-1");
    cards(c, "hero", s->hero, 2);
    U("board_count", s->board_count, 5);
    if (s->board_count != 0 && s->board_count != 3 && s->board_count != 4 && s->board_count != 5) c->ok = false;
    cards(c, "board", s->board, s->board_count);
    U("opponents", s->opponent_count, POKER_MAX_OPPONENTS);
    U("seed", s->seed, UINT64_MAX);
    U("trials", s->trials, 1000000);
    if (!s->opponent_count || s->trials < 100) c->ok = false;
    uint64_t known = 0;
    for (size_t i = 0; c->ok && i < 2 + s->board_count; ++i) {
        int card = i < 2 ? s->hero[i] : s->board[i - 2];
        uint64_t bit = UINT64_C(1) << card;
        if (known & bit) c->ok = false;
        known |= bit;
    }
    for (size_t i = 0; c->ok && i < s->opponent_count; ++i) {
        U("range_count", ranges[i].count, POKER_MAX_COMBOS);
        if (!ranges[i].count) c->ok = false;
        double total = 0;
        for (size_t j = 0; c->ok && j < ranges[i].count; ++j) {
            PokerCombo *combo = &ranges[i].combos[j];
            cards(c, "combo", combo->cards, 2);
            D("weight", combo->weight);
            if (combo->cards[0] == combo->cards[1] || combo->weight <= 0) c->ok = false;
            if (c->ok && !(known & ((UINT64_C(1) << combo->cards[0]) |
                                   (UINT64_C(1) << combo->cards[1])))) total += combo->weight;
        }
        if (!isfinite(total) || total <= 0) c->ok = false;
    }
}

static void result_fields(Codec *c, const PokerScenario *s, const PokerRange *ranges, PokerEquity *e)
{
    D("equity", e->equity); D("win_rate", e->win_rate);
    D("tie_rate", e->tie_rate); D("standard_error", e->standard_error);
    U("result_trials", e->trials, 1000000);
    if (e->trials != s->trials || e->equity < 0 || e->equity > 1 ||
        e->win_rate < 0 || e->win_rate > 1 || e->tie_rate < 0 || e->tie_rate > 1 ||
        e->standard_error < 0 || e->standard_error > 0.5 / sqrt((double)s->trials - 1) + 1e-12 ||
        e->win_rate + e->tie_rate > 1 + 1e-12 ||
        e->equity < e->win_rate + e->tie_rate / (s->opponent_count + 1) - 1e-12 ||
        e->equity > e->win_rate + e->tie_rate / 2 + 1e-12) c->ok = false;
    uint64_t known = 0;
    for (size_t i = 0; c->ok && i < 2 + s->board_count; ++i)
        known |= UINT64_C(1) << (i < 2 ? s->hero[i] : s->board[i - 2]);
    for (size_t i = 0; c->ok && i < s->opponent_count; ++i) {
        U("available_combos", e->available_combos[i], POKER_MAX_COMBOS);
        size_t available = 0;
        for (size_t j = 0; j < ranges[i].count; ++j) {
            const PokerCombo *combo = &ranges[i].combos[j];
            if (!(known & ((UINT64_C(1) << combo->cards[0]) | (UINT64_C(1) << combo->cards[1])))) ++available;
        }
        if (!available || e->available_combos[i] != available) c->ok = false;
    }
}

static void history_fields(Codec *c, char *id, PokerScenario *s,
                           PokerAdvice *a, bool *has_profit, double *profit)
{
    row(c, "id", id, 65);
    if (!safe_id(id)) c->ok = false;
    D("pot", s->pot); D("to_call", s->to_call);
    D("hero_stack", s->hero_stack); D("bet_size", s->bet_size);
    for (size_t i = 0; c->ok && i < s->opponent_count; ++i) {
        PokerOpponent *o = &s->opponents[i];
        row(c, "name", o->name, sizeof(o->name));
        row(c, "position", o->position, sizeof(o->position));
        if (!text_valid(o->name, sizeof(o->name)) || !text_valid(o->position, sizeof(o->position))) c->ok = false;
        D("stack", o->stack); D("range_percent", o->range_percent); D("fold_to_bet", o->fold_to_bet);
    }
    if (c->ok && !poker_scenario_validate(s, NULL, 0)) c->ok = false;
    D("fold_ev", a->fold_ev); D("call_ev", a->call_ev); D("aggressive_ev", a->aggressive_ev);
    D("fold_frequency", a->fold_frequency); D("call_frequency", a->call_frequency);
    D("aggressive_frequency", a->aggressive_frequency); D("risk", a->risk);
    U("aggressive_available", a->aggressive_available, 1);
    char action[16] = "";
    if (!c->reading && a->sampled_action) snprintf(action, sizeof(action), "%s", a->sampled_action);
    row(c, "action", action, sizeof(action));
    if (strcmp(action, "fold") && strcmp(action, "call") && strcmp(action, "check") &&
        strcmp(action, "raise") && strcmp(action, "bet")) c->ok = false;
    if (a->fold_frequency < 0 || a->fold_frequency > 1 || a->call_frequency < 0 ||
        a->call_frequency > 1 || a->aggressive_frequency < 0 || a->aggressive_frequency > 1 ||
        fabs(a->fold_frequency + a->call_frequency + a->aggressive_frequency - 1) > 1e-12 || a->risk < 0) c->ok = false;
    U("has_profit", *has_profit, 1);
    if (*has_profit) D("net_profit", *profit);
}

static bool codec(FILE *file, bool reading, bool history, char *id,
                  PokerScenario *s, PokerRange *ranges, PokerEquity *e,
                  PokerAdvice *a, bool *labeled, double *profit)
{
    locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!locale) return false;
    locale_t previous = uselocale(locale);
    if (!previous) { freelocale(locale); return false; }
    Codec state = {file, reading, true}, *c = &state;
    literal(c, "format", history ? "poker-hand-1" : "poker-cache-1");
    key_fields(c, s, ranges);
    if (c->ok) result_fields(c, s, ranges, e);
    if (c->ok && history) history_fields(c, id, s, a, labeled, profit);
    literal(c, "end", "1");
    if (reading && (fgetc(file) != EOF || ferror(file))) c->ok = false;
    uselocale(previous);
    freelocale(locale);
    return c->ok;
}

static char *canonical(const PokerScenario *scenario, const PokerRange *ranges, size_t *length)
{
    char *text = NULL;
    FILE *file = open_memstream(&text, length);
    if (!file) return NULL;
    PokerScenario s = *scenario;
    PokerRange *copy = malloc(POKER_MAX_OPPONENTS * sizeof(*copy));
    locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    bool ok = copy && locale;
    locale_t previous = (locale_t)0;
    if (ok) {
        previous = uselocale(locale);
        ok = previous != (locale_t)0;
    }
    if (ok) {
        if (s.opponent_count > POKER_MAX_OPPONENTS) ok = false;
        else {
            memcpy(copy, ranges, s.opponent_count * sizeof(*copy));
            Codec c = {file, false, true};
            key_fields(&c, &s, copy);
            ok = c.ok;
        }
    }
    if (previous) uselocale(previous);
    if (locale) freelocale(locale);
    free(copy);
    if (fclose(file) != 0) ok = false;
    if (!ok) { free(text); return NULL; }
    return text;
}

static bool publish(const char *directory, const char *path, bool history, char *id,
                    PokerScenario *s, PokerRange *ranges, PokerEquity *e, PokerAdvice *a,
                    bool *labeled, double *profit)
{
    char *temp = path_for(directory, ".tmp-", "XXXXXX");
    if (!temp) return false;
    int fd = mkstemp(temp);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "w");
    bool ok = file && codec(file, false, history, id, s, ranges, e, a, labeled, profit);
    if (file) {
        if (fflush(file) != 0 || fsync(fd) != 0) ok = false;
        if (fclose(file) != 0) ok = false;
    } else if (fd >= 0) close(fd);
    if (ok) ok = (history ? link(temp, path) : rename(temp, path)) == 0;
    if (fd >= 0) unlink(temp);
    free(temp);
    return ok;
}

static bool cache_access(const char *directory, const PokerScenario *s, const PokerRange *ranges,
                         PokerEquity *e, bool *hit, bool writing, char *error, size_t size)
{
    if (hit) *hit = false;
    if (!s || !ranges || !e || (!writing && !hit)) return fail(error, size, "Invalid cache arguments.");
    size_t length = 0;
    char *key = canonical(s, ranges, &length);
    directory = prepare(directory, false);
    if (!key || !directory) { free(key); return fail(error, size, "Invalid cache key or directory."); }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ (unsigned char)key[i]) * UINT64_C(1099511628211);
    char name[17];
    snprintf(name, sizeof(name), "%016" PRIx64, hash);
    char *path = path_for(directory, name, ".cache");
    PokerRange *copy = calloc(POKER_MAX_OPPONENTS, sizeof(*copy));
    PokerScenario decoded = {0};
    PokerEquity result = {0};
    bool ok = path && copy;
    if (ok && writing) {
        decoded = *s; result = *e;
        memcpy(copy, ranges, s->opponent_count * sizeof(*copy));
        ok = publish(directory, path, false, NULL, &decoded, copy, &result, NULL, NULL, NULL);
    } else if (ok) {
        FILE *file = open_record(path);
        if (!file) ok = errno == ENOENT;
        else {
            ok = codec(file, true, false, NULL, &decoded, copy, &result, NULL, NULL, NULL);
            if (fclose(file) != 0) ok = false;
            if (ok) {
                size_t other_length = 0;
                char *other = canonical(&decoded, copy, &other_length);
                ok = other != NULL;
                if (ok && length == other_length && !memcmp(key, other, length)) { *hit = true; *e = result; }
                free(other);
            }
        }
    }
    free(copy); free(path); free(key);
    return ok || fail(error, size, "Invalid cache entry or cache I/O failure.");
}

bool poker_cache_read(const char *d, const PokerScenario *s, const PokerRange *r,
                      PokerEquity *e, bool *hit, char *error, size_t size)
{ return cache_access(d, s, r, e, hit, false, error, size); }

bool poker_cache_write(const char *d, const PokerScenario *s, const PokerRange *r,
                       const PokerEquity *e, char *error, size_t size)
{
    if (!e) return fail(error, size, "Invalid cache result.");
    PokerEquity copy = *e;
    return cache_access(d, s, r, &copy, NULL, true, error, size);
}

bool poker_history_save(const char *d, const char *id, const PokerScenario *s,
                        const PokerRange *r, const PokerEquity *e, const PokerAdvice *a,
                        bool labeled, double profit, char *error, size_t size)
{
    if (!safe_id(id) || !s || !r || !e || !a || s->opponent_count > POKER_MAX_OPPONENTS ||
        (labeled && !isfinite(profit))) return fail(error, size, "Invalid history arguments.");
    for (size_t i = 0; i < s->opponent_count; ++i)
        if (!text_valid(s->opponents[i].name, POKER_NAME_SIZE) || !text_valid(s->opponents[i].position, 8))
            return fail(error, size, "Invalid opponent text.");
    d = prepare(d, true);
    if (!d) return fail(error, size, "Cannot create history directory.");
    char *path = path_for(d, id, ".hand"), saved_id[65];
    snprintf(saved_id, sizeof(saved_id), "%s", id);
    PokerScenario scenario = *s;
    PokerEquity equity = *e;
    PokerAdvice advice = *a;
    PokerRange *ranges = calloc(POKER_MAX_OPPONENTS, sizeof(*ranges));
    bool ok = path && ranges;
    if (ok) {
        memcpy(ranges, r, s->opponent_count * sizeof(*ranges));
        ok = publish(d, path, true, saved_id, &scenario, ranges, &equity, &advice, &labeled, &profit);
    }
    free(ranges); free(path);
    return ok || fail(error, size, "History save failed (invalid record, existing ID, or I/O failure).");
}

static bool load_hand(const char *path, const char *expected, FILE **stream,
                      PokerScenario *s, PokerEquity *e, bool *labeled, double *profit)
{
    FILE *file = open_record(path);
    if (!file) return false;
    PokerRange *ranges = calloc(POKER_MAX_OPPONENTS, sizeof(*ranges));
    PokerAdvice advice = {0};
    char id[65] = "";
    bool ok = ranges && codec(file, true, true, id, s, ranges, e, &advice, labeled, profit) && !strcmp(id, expected);
    free(ranges);
    if (ok && stream) { rewind(file); *stream = file; }
    else if (fclose(file) != 0) ok = false;
    return ok;
}

bool poker_history_show(const char *d, const char *id, char *error, size_t size)
{
    if (!safe_id(id)) return fail(error, size, "Invalid history ID.");
    d = prepare(d, true);
    char *path = d ? path_for(d, id, ".hand") : NULL;
    PokerScenario s = {0}; PokerEquity e = {0};
    bool labeled = false; double profit = 0;
    FILE *file = NULL;
    bool ok = path && load_hand(path, id, &file, &s, &e, &labeled, &profit);
    if (ok) {
        int ch;
        while ((ch = fgetc(file)) != EOF) if (fputc(ch, stdout) == EOF) { ok = false; break; }
        if (ferror(file) || fflush(stdout) != 0) ok = false;
        if (fclose(file) != 0) ok = false;
    }
    free(path);
    return ok || fail(error, size, "Cannot show missing or invalid history record.");
}

static bool add(Totals *t, const PokerEquity *e, bool labeled, double profit)
{
    if (t->count == UINT64_MAX || (labeled && t->labeled == UINT64_MAX)) return false;
    ++t->count; t->equity += e->equity;
    if (labeled) { ++t->labeled; t->profit += profit; }
    return isfinite(t->profit) && isfinite(t->equity);
}

static bool print_totals(const char *label, const Totals *t)
{
    return printf("%s\t%" PRIu64 "\t%" PRIu64 "\t%.17g\t%.17g\t%.17g\n", label,
                  t->count, t->labeled, t->profit, t->labeled ? t->profit / (double)t->labeled : 0,
                  t->count ? t->equity / (double)t->count : 0) >= 0;
}

bool poker_history_stats(const char *d, char *error, size_t size)
{
    d = prepare(d, true);
    DIR *directory = d ? opendir(d) : NULL;
    if (!directory) return fail(error, size, "Cannot open history directory.");
    Totals total = {0}, groups[13][13][2] = {{{{0}}}};
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) ok = false; break; }
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcmp(entry->d_name + n - 5, ".hand")) continue;
        char id[65];
        if (n - 5 > 64) { ok = false; break; }
        memcpy(id, entry->d_name, n - 5); id[n - 5] = 0;
        char *path = path_for(d, entry->d_name, "");
        PokerScenario s = {0}; PokerEquity e = {0};
        bool labeled = false; double profit = 0;
        ok = safe_id(id) && path && load_hand(path, id, NULL, &s, &e, &labeled, &profit);
        free(path);
        if (!ok) break;
        int high = s.hero[0] / 4, low = s.hero[1] / 4;
        if (high < low) { int swap = high; high = low; low = swap; }
        int suited = high != low && s.hero[0] % 4 == s.hero[1] % 4;
        if (!add(&total, &e, labeled, profit) || !add(&groups[high][low][suited], &e, labeled, profit)) { ok = false; break; }
    }
    if (closedir(directory) != 0) ok = false;
    if (!ok) return fail(error, size, "Invalid history file, duplicate ID, overflow, or directory I/O failure.");
    ok = printf("class\tcount\tlabeled_count\ttotal_net_profit\taverage_labeled_profit\taverage_equity\n") >= 0 && print_totals("total", &total);
    const char *ranks = "23456789TJQKA";
    for (int high = 12; high >= 0; --high) for (int low = high; low >= 0; --low)
        for (int suited = 0; suited < 2; ++suited) if (groups[high][low][suited].count) {
            char label[4] = {ranks[high], ranks[low], high == low ? 0 : suited ? 's' : 'o', 0};
            if (!print_totals(label, &groups[high][low][suited])) ok = false;
        }
    if (fflush(stdout) != 0) ok = false;
    return ok || fail(error, size, "Cannot print history statistics.");
}
