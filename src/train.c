#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "poker.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TRAIN_LIMIT 1000
#define TRAIN_TOLERANCE 1e-9

typedef struct {
    PokerScenario s;
    PokerEquity e;
    unsigned range;
    uint64_t hero_mask, board_mask;
    bool call;
    double estimate;
} TrainQuestion;
typedef struct {
    char name[47];
    unsigned count, correct;
    double regret, error;
} TrainStats;
static const char *const train_ranges[] = {
    "AA,KK,QQ,JJ,AK", "QQ,JJ,TT,99,AQ,AK", "100%"
};

static bool train_integer(const char *text, uint64_t *value)
{
    if (!*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    char *end;
    errno = 0;
    uintmax_t n = strtoumax(text, &end, 10);
    if (errno || *end || n > UINT64_MAX) return false;
    *value = (uint64_t)n;
    return true;
}

static bool train_session(const char *text)
{
    if (!*text || strlen(text) > 40) return false;
    for (const char *p = text; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

/* Consume the entire line even on overflow; reject embedded NULs as well. */
static int train_line(FILE *file, char *line, size_t size)
{
    size_t n = 0;
    bool bad = false;
    int c;
    while ((c = fgetc(file)) != EOF && c != '\n') {
        if (c == 0 || n + 1 >= size) bad = true;
        else line[n++] = (char)c;
    }
    line[n] = '\0';
    if (ferror(file)) return -1;
    if (c == EOF) return n == 0 && !bad ? 0 : -1;
    return bad ? -1 : 1;
}

static bool train_answer(const char *text, bool *call, double *estimate)
{
    while (isspace((unsigned char)*text)) ++text;
    if (!strncmp(text, "call", 4)) *call = true;
    else if (!strncmp(text, "fold", 4)) *call = false;
    else return false;
    text += 4;
    if (!isspace((unsigned char)*text)) return false;
    while (isspace((unsigned char)*text)) ++text;
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || errno || !isfinite(value) || value < 0 || value > 100) return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end) return false;
    *estimate = value == 0 ? 0 : value;
    return true;
}

static double train_ev(const TrainQuestion *q)
{
    return q->e.equity * (q->s.pot + q->s.to_call) - q->s.to_call;
}

static bool train_generate(TrainQuestion *questions, unsigned count, uint64_t seed)
{
    PokerRng rng;
    poker_rng_seed(&rng, seed);
    for (unsigned i = 0; i < count; ++i) {
        bool accepted = false;
        for (unsigned attempt = 0; attempt < 100 && !accepted; ++attempt) {
            TrainQuestion q = {0};
            int deck[52];
            for (int j = 0; j < 52; ++j) deck[j] = j;
            for (unsigned j = 0; j < 7; ++j) {
                unsigned k = j + poker_rng_bounded(&rng, 52 - j);
                int swap = deck[j]; deck[j] = deck[k]; deck[k] = swap;
                if (j < 2) {
                    q.s.hero[j] = deck[j]; q.hero_mask |= UINT64_C(1) << deck[j];
                } else {
                    q.s.board[j - 2] = deck[j]; q.board_mask |= UINT64_C(1) << deck[j];
                }
            }
            q.range = poker_rng_bounded(&rng, 3);
            static const double prices[] = {10, 25, 50, 100};
            q.s.to_call = prices[poker_rng_bounded(&rng, 4)];
            q.s.board_count = 5; q.s.opponent_count = 1;
            q.s.pot = 100; q.s.hero_stack = 10000; q.s.trials = 100; q.s.seed = seed;
            q.s.opponents[0].stack = 10000; q.s.opponents[0].range_percent = 100;
            bool duplicate = false;
            for (unsigned j = 0; j < i; ++j)
                if (q.hero_mask == questions[j].hero_mask && q.board_mask == questions[j].board_mask)
                    duplicate = true;
            if (duplicate) continue;
            PokerRange range;
            char error[256];
            uint64_t dead = q.hero_mask | q.board_mask;
            bool built = q.range == 2 ? poker_range_build(&range, 100, dead, error, sizeof(error)) :
                poker_range_parse(&range, train_ranges[q.range], dead, error, sizeof(error));
            if (!built) {
                if (range.count == 0 && strstr(error, "no combos")) continue;
                fprintf(stderr, "train: %s\n", error); return false;
            }
            if (!poker_equity_river_exact(&q.s, &range, &q.e, error, sizeof(error))) {
                fprintf(stderr, "train: %s\n", error); return false;
            }
            if (fabs(train_ev(&q)) < 0.5) continue;
            questions[i] = q; accepted = true;
        }
        if (!accepted) {
            fprintf(stderr, "train: generation exhausted 100 attempts for question %u.\n", i + 1);
            return false;
        }
    }
    return true;
}

static void train_score(const TrainQuestion *q, TrainStats *stats)
{
    double ev = train_ev(q), chosen = q->call ? ev : 0;
    double regret = fmax(0, ev) - chosen;
    ++stats->count;
    if (regret <= TRAIN_TOLERANCE) ++stats->correct;
    stats->regret += regret / q->s.pot;
    stats->error += fabs(q->estimate - q->e.equity * 100);
}

static void train_report(const char *label, const TrainStats *stats)
{
    printf("%s: %u questions | accuracy %.2f%% | mean normalized EV regret %.6f | equity MAE %.2f percentage points\n",
           label, stats->count, stats->count ? 100.0 * stats->correct / stats->count : 0,
           stats->count ? stats->regret / stats->count : 0,
           stats->count ? stats->error / stats->count : 0);
}

static bool train_save(int directory, const char *path, const char *session,
                       uint64_t seed, const TrainQuestion *q, unsigned count)
{
    char temporary[4096], target[47];
    int n = snprintf(temporary, sizeof(temporary), "%s/.train-XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) { errno = ENAMETOOLONG; return false; }
    snprintf(target, sizeof(target), "%s.train", session);
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    const char *name = strrchr(temporary, '/') + 1;
    struct stat actual, relative;
    /* Pin publication to the opened directory, even if its pathname is replaced. */
    bool ok = fstat(fd, &actual) == 0 &&
        fstatat(directory, name, &relative, AT_SYMLINK_NOFOLLOW) == 0 &&
        actual.st_dev == relative.st_dev && actual.st_ino == relative.st_ino;
    if (!ok) { close(fd); unlink(temporary); errno = ESTALE; return false; }
    FILE *file = NULL;
    if (fchmod(fd, 0600) != 0 || !(file = fdopen(fd, "w"))) ok = false;
    if (ok) {
        ok = fprintf(file, "poker-training-1\nsession\t%s\nseed\t%" PRIu64 "\ncount\t%u\n",
                     session, seed, count) >= 0;
        for (unsigned i = 0; i < count && ok; ++i)
            ok = fprintf(file, "%u\t%s\t%.17g\n", i + 1, q[i].call ? "call" : "fold", q[i].estimate) >= 0;
        if (fflush(file) != 0 || fsync(fd) != 0) ok = false;
        if (fclose(file) != 0) ok = false;
    } else close(fd);
    if (ok && linkat(directory, name, directory, target, 0) != 0) ok = false;
    int saved = errno;
    if (unlinkat(directory, name, 0) != 0) ok = false;
    errno = saved;
    return ok;
}

static bool train_load(int directory, TrainStats *stats)
{
    int fd = openat(directory, stats->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size > 100000) { close(fd); return false; }
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return false; }
    char line[256], canonical[256], session[41];
    size_t length = strlen(stats->name) - 6;
    memcpy(session, stats->name, length); session[length] = '\0';
    uint64_t seed = 0, count = 0;
    bool ok = train_line(file, line, sizeof(line)) == 1 && !strcmp(line, "poker-training-1");
    snprintf(canonical, sizeof(canonical), "session\t%s", session);
    ok = ok && train_line(file, line, sizeof(line)) == 1 && !strcmp(line, canonical);
    ok = ok && train_line(file, line, sizeof(line)) == 1 && !strncmp(line, "seed\t", 5) &&
        train_integer(line + 5, &seed);
    snprintf(canonical, sizeof(canonical), "seed\t%" PRIu64, seed);
    ok = ok && !strcmp(line, canonical);
    ok = ok && train_line(file, line, sizeof(line)) == 1 && !strncmp(line, "count\t", 6) &&
        train_integer(line + 6, &count) && count >= 1 && count <= TRAIN_LIMIT;
    snprintf(canonical, sizeof(canonical), "count\t%" PRIu64, count);
    ok = ok && !strcmp(line, canonical);
    TrainQuestion *questions = ok ? calloc((size_t)count, sizeof(*questions)) : NULL;
    ok = ok && questions && train_generate(questions, (unsigned)count, seed);
    for (unsigned i = 0; ok && i < count; ++i) {
        ok = train_line(file, line, sizeof(line)) == 1;
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%u\t", i + 1);
        ok = ok && !strncmp(line, prefix, strlen(prefix)) &&
            train_answer(line + strlen(prefix), &questions[i].call, &questions[i].estimate);
        if (!ok) break;
        snprintf(canonical, sizeof(canonical), "%u\t%s\t%.17g", i + 1,
                 questions[i].call ? "call" : "fold", questions[i].estimate);
        ok = !strcmp(line, canonical);
        if (ok) train_score(&questions[i], stats);
    }
    if (ok) ok = train_line(file, line, sizeof(line)) == 0;
    free(questions);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static int train_compare(const void *a, const void *b)
{
    return strcmp(((const TrainStats *)a)->name, ((const TrainStats *)b)->name);
}

static int train_stats(int directory)
{
    int fd = dup(directory);
    if (fd < 0) return 1;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return 1; }
    TrainStats *sessions = NULL, total = {0};
    size_t count = 0, capacity = 0;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) ok = false; break; }
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 6, ".train")) continue;
        char session[41];
        if (n - 6 > 40) { ok = false; break; }
        memcpy(session, entry->d_name, n - 6); session[n - 6] = '\0';
        if (!train_session(session)) { ok = false; break; }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next < capacity || next > SIZE_MAX / sizeof(*sessions)) { ok = false; break; }
            TrainStats *grown = realloc(sessions, next * sizeof(*sessions));
            if (!grown) { ok = false; break; }
            sessions = grown; capacity = next;
        }
        sessions[count] = (TrainStats){0};
        strcpy(sessions[count++].name, entry->d_name);
    }
    if (closedir(dir) != 0) ok = false;
    if (count > 1) qsort(sessions, count, sizeof(*sessions), train_compare);
    for (size_t i = 0; ok && i < count; ++i) {
        if (!train_load(directory, &sessions[i])) {
            fprintf(stderr, "train: invalid or unreadable session %s.\n", sessions[i].name);
            ok = false; break;
        }
        if (total.count > UINT32_MAX - sessions[i].count) { ok = false; break; }
        total.count += sessions[i].count; total.correct += sessions[i].correct;
        total.regret += sessions[i].regret; total.error += sessions[i].error;
    }
    if (ok) {
        puts("Sessions sorted by filename, not chronology. No causal improvement claim.");
        for (size_t i = 0; i < count; ++i) train_report(sessions[i].name, &sessions[i]);
        train_report("Overall", &total);
        puts("Compare fresh seeds to avoid memorization. Educational drills offer no win guarantee.");
    } else fprintf(stderr, "train: statistics rejected; no aggregate reported.\n");
    free(sessions);
    return ok ? 0 : 1;
}

int poker_train_command(int argc, char **argv)
{
    const char *path = ".poker-training", *session = NULL;
    uint64_t count = 10, seed = 42;
    if (argc < 1 || (strcmp(argv[0], "run") && strcmp(argv[0], "stats"))) goto usage;
    bool run = !strcmp(argv[0], "run");
    unsigned seen = 0;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (++i >= argc) goto usage;
        const char *value = argv[i];
        unsigned bit;
        if (!strcmp(key, "--progress-dir")) { bit = 1; path = value; if (!*path) goto usage; }
        else if (run && !strcmp(key, "--session")) { bit = 2; session = value; if (!train_session(session)) goto usage; }
        else if (run && !strcmp(key, "--seed")) { bit = 4; if (!train_integer(value, &seed)) goto usage; }
        else if (run && !strcmp(key, "--questions")) {
            bit = 8; if (!train_integer(value, &count) || count < 1 || count > TRAIN_LIMIT) goto usage;
        } else goto usage;
        if (seen & bit) goto usage;
        seen |= bit;
    }
    if (run && !session) goto usage;
    if (run && mkdir(path, 0700) != 0 && errno != EEXIST) goto io_error;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) goto io_error;
    if (!run) { int result = train_stats(directory); close(directory); return result; }
    char target[47];
    snprintf(target, sizeof(target), "%s.train", session);
    struct stat st;
    if (fstatat(directory, target, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        fprintf(stderr, "train: session must be unused: %s.\n", session); close(directory); return 1;
    }
    TrainQuestion *questions = calloc((size_t)count, sizeof(*questions));
    if (!questions || !train_generate(questions, (unsigned)count, seed)) {
        free(questions); close(directory); return 1;
    }
    puts("Offline educational river drill: final heads-up cash decision, no rake, call/fold only.\n"
         "Pot includes the outstanding bet; both remaining stacks are 10000.\n"
         "Ranges are uniform over legal combinations (AK/AQ include suited and offsuit).\n"
         "Answer 'call 35' or 'fold 20'; the number is estimated equity percent (0..100).");
    TrainStats stats = {0};
    int result = 1;
    for (unsigned i = 0; i < count; ++i) {
        TrainQuestion *q = &questions[i];
        char cards[7][3];
        for (unsigned j = 0; j < 7; ++j) poker_card_format(j < 2 ? q->s.hero[j] : q->s.board[j - 2], cards[j]);
        printf("\nQuestion %u/%u | Hero %s %s | Board %s %s %s %s %s\n"
               "Assumed range: %s | Pot 100 | To call %.0f\n", i + 1, (unsigned)count,
               cards[0], cards[1], cards[2], cards[3], cards[4], cards[5], cards[6],
               train_ranges[q->range], q->s.to_call);
        for (;;) {
            char line[256];
            printf("Answer: "); fflush(stdout);
            int status = train_line(stdin, line, sizeof(line));
            if (status == 0 || (status < 0 && (feof(stdin) || ferror(stdin)))) {
                puts("\nSession cancelled; no partial session saved."); goto done;
            }
            if (status == 1 && train_answer(line, &q->call, &q->estimate)) break;
            puts("Invalid answer. Use call|fold followed by a finite equity percent from 0 to 100.");
        }
        train_score(q, &stats);
        double ev = train_ev(q);
        printf("Exact equity %.6f%% | %u legal combos | call threshold %.6f%%\n"
               "Call EV %.6f chips | best action %s | normalized EV regret %.6f\n",
               q->e.equity * 100, q->e.trials, 100 * q->s.to_call / (q->s.pot + q->s.to_call),
               ev, fabs(ev) <= TRAIN_TOLERANCE ? "call or fold (tie)" : ev > 0 ? "call" : "fold",
               (fmax(0, ev) - (q->call ? ev : 0)) / q->s.pot);
    }
    if (!train_save(directory, path, session, seed, questions, (unsigned)count)) {
        fprintf(stderr, "train: cannot publish session without overwriting: %s\n", strerror(errno)); goto done;
    }
    train_report(session, &stats);
    printf("Saved %s/%s.train\n", path, session);
    puts("Compare fresh seeds to avoid memorization. Educational drills offer no win guarantee.");
    result = 0;
done:
    free(questions); close(directory); return result;
io_error:
    fprintf(stderr, "train: cannot open progress directory: %s\n", strerror(errno)); return 1;
usage:
    fprintf(stderr, "Usage: train run --questions 1..1000 --seed UINT64 --session ID [--progress-dir PATH]\n"
                    "       train stats [--progress-dir PATH]\n"
                    "Defaults: 10 questions, seed 42, progress directory .poker-training.\n"
                    "Session ID: 1..40 ASCII letters, digits, '_' or '-'; must be unused.\n");
    return 1;
}
