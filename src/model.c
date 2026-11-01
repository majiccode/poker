#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "poker.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MODEL_LIMIT 100000u
#define MODEL_WORK 100000000u
static const char model_header[] =
    "hand_id\tdecision_id\tplayer\tstreet\tposition\tplayers\tpot\tto_call\tstack\taction";
static const char *const streets[] = {"preflop", "flop", "turn", "river"};
static const char *const positions[] = {"UTG", "UTG1", "UTG2", "LJ", "HJ", "CO", "BTN", "SB", "BB"};
static const char *const actions[] = {"fold", "call", "raise"};

typedef struct {
    char hand[49], decision[49], player[49];
    int street, position, players, action, context;
    double pot, call, stack;
    bool skip;
} ModelRow;
typedef struct {
    ModelRow *rows;
    ModelRow **index;
    size_t count, capacity;
} ModelData;
typedef struct {
    size_t global[3], context[3], own[3], player_total;
    size_t ng, nc, no;
    double p[3];
} ModelEstimate;
typedef struct {
    double brier, loss;
    size_t correct, bins[5], folded[5];
    double predicted[5];
} ModelMetrics;

static bool model_error(const char *message)
{
    fprintf(stderr, "model: %s\n", message);
    return false;
}

static bool model_id(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 48) return false;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return false;
    return true;
}

/* Decimal syntax only: no whitespace, hexadecimal floats, NaN or infinity. */
static bool model_number(const char *s, double *value)
{
    const char *p = s;
    if (*p == '+' || *p == '-') ++p;
    size_t digits = 0;
    while (*p >= '0' && *p <= '9') { ++p; ++digits; }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') { ++p; ++digits; }
    }
    if (!digits) return false;
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        const char *start = p;
        while (*p >= '0' && *p <= '9') ++p;
        if (p == start) return false;
    }
    if (*p) return false;
    char *end;
    *value = strtod(s, &end);
    return !*end && isfinite(*value) && *value > 0 && *value <= 1e12;
}

static int model_enum(const char *s, const char *const *names, int count)
{
    for (int i = 0; i < count; ++i) if (!strcmp(s, names[i])) return i;
    return -1;
}

static bool model_fields(ModelRow *r, char **f)
{
    if (!model_id(f[0]) || !model_id(f[1]) || !model_id(f[2])) return false;
    strcpy(r->hand, f[0]); strcpy(r->decision, f[1]); strcpy(r->player, f[2]);
    r->street = model_enum(f[3], streets, 4);
    r->position = model_enum(f[4], positions, 9);
    r->action = model_enum(f[9], actions, 3);
    r->players = 0;
    if (!*f[5]) return false;
    for (const char *p = f[5]; *p; ++p) {
        if (*p < '0' || *p > '9' || r->players > 9) return false;
        r->players = r->players * 10 + (*p - '0');
    }
    if (r->street < 0 || r->position < 0 || r->action < 0 ||
        r->players < 2 || r->players > 9 || !model_number(f[6], &r->pot) ||
        !model_number(f[7], &r->call) || !model_number(f[8], &r->stack) ||
        r->call > r->stack) return false;
    double denominator = r->pot + r->call;
    double price = r->call / denominator, spr = r->stack / denominator;
    int group = r->players == 2 ? 0 : r->players <= 5 ? 1 : 2;
    int price_bucket = price <= .2 ? 0 : price <= .333333 ? 1 : 2;
    int spr_bucket = spr <= 2 ? 0 : spr <= 6 ? 1 : 2;
    r->context = (((r->street * 9 + r->position) * 3 + group) * 3 + price_bucket) * 3 + spr_bucket;
    return true;
}

/* Require newline-terminated lines, at most 1024 bytes including newline. */
static int model_line(FILE *file, char line[1025])
{
    size_t n = 0;
    bool bad = false;
    int c;
    while ((c = fgetc(file)) != EOF && c != '\n') {
        if (!c || n >= 1023) bad = true;
        else line[n++] = (char)c;
    }
    line[n] = '\0';
    if (ferror(file) || bad) return -1;
    if (c == EOF) return n ? -1 : 0;
    return 1;
}

static int model_key(const void *a, const void *b)
{
    const ModelRow *x = *(ModelRow *const *)a, *y = *(ModelRow *const *)b;
    int order = strcmp(x->hand, y->hand);
    return order ? order : strcmp(x->decision, y->decision);
}

static void model_free(ModelData *d)
{
    free(d->index); free(d->rows);
}

static bool model_load(const char *path, bool missing_ok, ModelData *d)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (missing_ok && errno == ENOENT) return true;
        fprintf(stderr, "model: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd); return model_error("dataset/input must be a regular file, not a symlink");
    }
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return model_error("cannot open input stream"); }
    char line[1025];
    bool ok = model_line(file, line) == 1 && !strcmp(line, model_header);
    if (!ok) model_error("invalid exact TSV header");
    size_t line_number = 1;
    int status = 0;
    while (ok && (status = model_line(file, line)) == 1) {
        ++line_number;
        char *fields[10];
        fields[0] = line;
        int count = 1;
        for (char *p = line; *p; ++p) if (*p == '\t') {
            *p = '\0';
            if (count == 10) { count = 11; break; }
            fields[count++] = p + 1;
        }
        ModelRow row = {0};
        if (count != 10 || !model_fields(&row, fields)) {
            fprintf(stderr, "model: invalid row in %s at line %zu\n", path, line_number);
            ok = false; break;
        }
        if (d->count == MODEL_LIMIT) { ok = model_error("maximum 100000 decisions exceeded"); break; }
        if (d->count == d->capacity) {
            size_t cap = d->capacity ? d->capacity * 2 : 256;
            if (cap > MODEL_LIMIT) cap = MODEL_LIMIT;
            ModelRow *rows = realloc(d->rows, cap * sizeof(*rows));
            if (!rows) { ok = model_error("out of memory"); break; }
            d->rows = rows; d->capacity = cap;
        }
        d->rows[d->count++] = row;
    }
    if (ok && status < 0) ok = model_error("invalid, overlong, NUL-containing or unterminated row");
    if (fclose(file) && ok) ok = model_error("input close failed");
    if (!ok || !d->count) return ok;
    d->index = malloc(d->count * sizeof(*d->index));
    if (!d->index) return model_error("out of memory");
    for (size_t i = 0; i < d->count; ++i) d->index[i] = &d->rows[i];
    qsort(d->index, d->count, sizeof(*d->index), model_key);
    for (size_t i = 1; i < d->count; ++i)
        if (!model_key(&d->index[i - 1], &d->index[i]))
            return model_error("duplicate hand_id + decision_id in dataset/input");
    return true;
}

static bool model_equal(const ModelRow *a, const ModelRow *b)
{
    return !strcmp(a->player, b->player) && a->street == b->street &&
        a->position == b->position && a->players == b->players &&
        a->pot == b->pot && a->call == b->call && a->stack == b->stack && a->action == b->action;
}

static bool model_write(const char *path, const ModelData *old, const ModelData *input)
{
    size_t length = strlen(path);
    if (length > SIZE_MAX - 12) return model_error("dataset path too long");
    char *temporary = malloc(length + 12);
    if (!temporary) return model_error("out of memory");
    snprintf(temporary, length + 12, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return model_error("cannot create dataset temporary file"); }
    FILE *file = fdopen(fd, "w");
    bool ok = file != NULL;
    if (!file) close(fd);
    if (ok) ok = fprintf(file, "%s\n", model_header) >= 0;
    const ModelData *sets[] = {old, input};
    for (int set = 0; ok && set < 2; ++set) {
        for (size_t i = 0; ok && i < sets[set]->count; ++i) {
            const ModelRow *r = &sets[set]->rows[i];
            if (r->skip) continue;
            ok = fprintf(file, "%s\t%s\t%s\t%s\t%s\t%d\t%.17g\t%.17g\t%.17g\t%s\n",
                         r->hand, r->decision, r->player, streets[r->street], positions[r->position],
                         r->players, r->pot, r->call, r->stack, actions[r->action]) >= 0;
        }
    }
    if (file) {
        if (ok && (fflush(file) || fsync(fd))) ok = false;
        if (fclose(file)) ok = false;
    }
    /* Single writer only; reject an unsafe final component before publication. */
    struct stat st;
    if (ok) {
        if (!lstat(path, &st)) ok = S_ISREG(st.st_mode);
        else ok = errno == ENOENT;
    }
    if (ok && rename(temporary, path)) ok = false;
    if (!ok) { unlink(temporary); model_error("atomic dataset write failed; original unchanged"); }
    free(temporary);
    return ok;
}

static bool model_import(const char *path, ModelData *old, ModelData *input)
{
    size_t added = 0, skipped = 0;
    for (size_t i = 0; i < input->count; ++i) {
        ModelRow *r = &input->rows[i];
        ModelRow **found = old->count ? bsearch(&r, old->index, old->count,
                                               sizeof(*old->index), model_key) : NULL;
        if (found) {
            if (!model_equal(r, *found)) return model_error("conflicting hand_id + decision_id; original unchanged");
            r->skip = true; ++skipped;
        } else ++added;
    }
    if (added > MODEL_LIMIT - old->count) return model_error("merged dataset exceeds 100000 decisions");
    if (!model_write(path, old, input)) return false;
    printf("import: added=%zu skipped=%zu total=%zu (canonical snapshot; single writer, no locks)\n",
           added, skipped, old->count + added);
    return true;
}

static ModelEstimate model_estimate(const ModelData *d, const ModelRow *query)
{
    ModelEstimate e = {0};
    for (size_t i = 0; i < d->count; ++i) {
        const ModelRow *r = &d->rows[i];
        if (!strcmp(r->player, query->player)) {
            ++e.player_total;
            if (r->context == query->context) { ++e.own[r->action]; ++e.no; }
        } else {
            ++e.global[r->action]; ++e.ng;
            if (r->context == query->context) { ++e.context[r->action]; ++e.nc; }
        }
    }
    for (int a = 0; a < 3; ++a) {
        double global = (e.global[a] + 1.0) / (e.ng + 3.0);
        double context = (e.context[a] + 20.0 * global) / (e.nc + 20.0);
        e.p[a] = (e.own[a] + 10.0 * context) / (e.no + 10.0);
    }
    return e;
}

static void model_warning(void)
{
    puts("Offline completed-hand action frequencies only; no histories or hidden-card inference.\n"
         "Posterior SD is marginal uncertainty, NOT a confidence interval or causal effect;\n"
         "prior uncertainty is not propagated. No strategy, EV, or profit guarantee.");
}

static void model_predict(const ModelData *d, const ModelRow *query)
{
    ModelEstimate e = model_estimate(d, query);
    printf("player=%s street=%s position=%s players=%d\n", query->player,
           streets[query->street], positions[query->position], query->players);
    printf("context: players=%s price=%s SPR=%s\n",
           query->players == 2 ? "2" : query->players <= 5 ? "3-5" : "6-9",
           (query->context / 3) % 3 == 0 ? "(0,0.2]" : (query->context / 3) % 3 == 1 ? "(0.2,0.333333]" : "(0.333333,1)",
           query->context % 3 == 0 ? "(0,2]" : query->context % 3 == 1 ? "(2,6]" : "(6,infinity)");
    printf("counts: dataset=%zu player_total=%zu own_context=%zu population_total=%zu population_context=%zu\n",
           d->count, e.player_total, e.no, e.ng, e.nc);
    printf("priors: query player excluded from population; global +3 uniform, context strength=20, own strength=10\n"
           "fallback: global=%s context=%s player=%s; effective_count=%.0f\n",
           e.ng ? "population" : "uniform", e.nc ? "matched" : "global prior",
           e.no ? "matched" : "population context prior", e.no + 10.0);
    for (int a = 0; a < 3; ++a)
        printf("%s: probability=%.17g posterior_sd=%.17g own_context=%zu population_context=%zu population_global=%zu\n",
               actions[a], e.p[a], sqrt(e.p[a] * (1 - e.p[a]) / (e.no + 11.0)),
               e.own[a], e.context[a], e.global[a]);
    printf("probability_sum=%.17g\n", e.p[0] + e.p[1] + e.p[2]);
    model_warning();
}

static void model_score(ModelMetrics *m, const double p[3], int label)
{
    int best = 0;
    for (int a = 0; a < 3; ++a) {
        double difference = p[a] - (a == label ? 1.0 : 0.0);
        m->brier += difference * difference;
        if (p[a] > p[best]) best = a;
    }
    m->loss -= log(p[label]);
    if (best == label) ++m->correct;
    int bin = p[0] >= .8 ? 4 : p[0] >= .6 ? 3 : p[0] >= .4 ? 2 : p[0] >= .2 ? 1 : 0;
    ++m->bins[bin];
    m->predicted[bin] += p[0];
    if (label == 0) ++m->folded[bin];
}

static bool model_evaluate(const ModelData *train, const ModelData *test)
{
    size_t i = 0, j = 0;
    while (i < train->count && j < test->count) {
        int order = strcmp(train->index[i]->hand, test->index[j]->hand);
        if (!order) return model_error("train/test hand_id overlap; no metrics produced");
        if (order < 0) ++i; else ++j;
    }
    if (!test->count) return model_error("evaluation requires at least one test decision");
    if (train->count > MODEL_WORK / test->count)
        return model_error("evaluation exceeds 100 million train/test comparisons; use a smaller split");
    double baseline[3] = {1, 1, 1};
    for (i = 0; i < train->count; ++i) ++baseline[train->rows[i].action];
    for (int a = 0; a < 3; ++a) baseline[a] /= train->count + 3.0;
    ModelMetrics metrics[2] = {0};
    for (i = 0; i < test->count; ++i) {
        ModelEstimate e = model_estimate(train, &test->rows[i]);
        model_score(&metrics[0], baseline, test->rows[i].action);
        model_score(&metrics[1], e.p, test->rows[i].action);
    }
    double n = (double)test->count;
    printf("evaluate: train=%zu test=%zu disjoint hands; training unchanged, all test labels held out\n",
           train->count, test->count);
    puts("Brier=sum of three squared errors [0,2]; logloss=natural log; accuracy=argmax, ties fold/call/raise.");
    for (int k = 0; k < 2; ++k)
        printf("%s: brier=%.10g logloss=%.10g accuracy=%.10g\n", k ? "model" : "baseline",
               metrics[k].brier / n, metrics[k].loss / n, metrics[k].correct / n);
    printf("delta (model-baseline): brier=%.10g logloss=%.10g accuracy=%.10g; no win claim\n",
           (metrics[1].brier - metrics[0].brier) / n, (metrics[1].loss - metrics[0].loss) / n,
           ((double)metrics[1].correct - (double)metrics[0].correct) / n);
    puts("Baseline: all training global counts +3 uniform, including each queried player's training rows.\n"
         "Model: query player excluded from population; global +3 uniform, context strength=20, own strength=10.\n"
         "Fold reliability: [lower,upper), final bin includes 1; empty bins are NA.");
    for (int k = 0; k < 2; ++k) for (int b = 0; b < 5; ++b) {
        printf("%s fold_bin=[%.1f,%.1f%s n=%zu", k ? "model" : "baseline", b * .2,
               (b + 1) * .2, b == 4 ? "]" : ")", metrics[k].bins[b]);
        if (metrics[k].bins[b])
            printf(" predicted_avg=%.10g observed_freq=%.10g\n",
                   metrics[k].predicted[b] / metrics[k].bins[b],
                   (double)metrics[k].folded[b] / metrics[k].bins[b]);
        else puts(" predicted_avg=NA observed_freq=NA");
    }
    model_warning();
    return true;
}

static void model_help(void)
{
    puts("poker model import --input FILE --dataset FILE\n"
         "poker model predict --dataset FILE --player alice --street river --position BTN\n"
         "  --players 2 --pot 100 --call 25 --stack 500\n"
         "poker model evaluate --dataset TRAIN --test TEST\n"
         "Import merges completed-hand decision snapshots, NOT native GGPoker parsing.\n"
         "All files use this exact TSV header:");
    puts(model_header);
    puts("One facing-bet decision per row; unique (hand_id,decision_id), no histories.\n"
         "IDs/player: 1..48 ASCII letters, digits, underscore or hyphen.\n"
         "Street: preflop/flop/turn/river. Position: UTG/UTG1/UTG2/LJ/HJ/CO/BTN/SB/BB.\n"
         "Players: 2..9; 0<pot<=1e12; 0<to_call<=stack<=1e12; action: fold/call/raise.\n"
         "Decimal numbers must be finite; lines <=1024 bytes including required newline; no NULs.\n"
         "Limit: 100000 decisions/file and merged dataset. Identical semantic keys skip; conflicts fail.\n"
         "Regular non-symlink files only. Import uses flush/fsync/atomic rename; single writer, no locks.\n"
         "Evaluate accepts raw TSV fixtures; disjoint hand IDs required, max 100 million comparisons.\n"
         "Context: street, position, players 2/3-5/6-9, call/(pot+call) <=.2/<=.333333/else,\n"
         "stack/(pot+call) <=2/<=6/else. Query player's rows excluded from all population priors.\n"
         "g=(population_global+1)/(Nglobal+3); c=(population_context+20*g)/(Ncontext+20);\n"
         "p=(own_exact_context+10*c)/(Nown+10); SD=sqrt(p*(1-p)/(Nown+11)).");
    model_warning();
}

int poker_model_command(int argc, char **argv)
{
    if (argc == 0 || (argc == 1 && (!strcmp(argv[0], "--help") || !strcmp(argv[0], "help")))) {
        model_help(); return 0;
    }
    int mode = !strcmp(argv[0], "import") ? 0 : !strcmp(argv[0], "predict") ? 1 :
        !strcmp(argv[0], "evaluate") ? 2 : -1;
    if (mode < 0) { model_error("expected import, predict or evaluate"); return 1; }
    if (argc == 2 && !strcmp(argv[1], "--help")) { model_help(); return 0; }
    const char *keys[] = {"--dataset", "--input", "--test", "--player", "--street", "--position", "--players", "--pot", "--call", "--stack"};
    char *values[10] = {0};
    for (int i = 1; i < argc; i += 2) {
        int key = model_enum(argv[i], keys, 10);
        if (key < 0 || i + 1 >= argc || values[key] || !*argv[i + 1] ||
            (mode == 0 && key > 1) || (mode == 1 && (key == 1 || key == 2)) ||
            (mode == 2 && key != 0 && key != 2)) {
            model_error("invalid, duplicate or incomplete option; use model --help"); return 1;
        }
        values[key] = argv[i + 1];
    }
    if (!values[0] || (mode == 0 && !values[1]) || (mode == 2 && !values[2])) {
        model_error("required dataset/input/test option missing"); return 1;
    }
    ModelRow query = {0};
    if (mode == 1) {
        for (int k = 3; k < 10; ++k) if (!values[k]) {
            model_error("all prediction context options are required"); return 1;
        }
        char *fields[] = {"query", "query", values[3], values[4], values[5], values[6],
                          values[7], values[8], values[9], "fold"};
        if (!model_fields(&query, fields)) { model_error("invalid prediction context"); return 1; }
    }
    ModelData data = {0}, other = {0};
    bool ok = model_load(values[0], mode == 0, &data);
    if (ok && mode != 1) ok = model_load(values[mode == 0 ? 1 : 2], false, &other);
    if (ok) {
        if (mode == 0) ok = model_import(values[0], &data, &other);
        else if (mode == 1) model_predict(&data, &query);
        else ok = model_evaluate(&data, &other);
    }
    model_free(&other); model_free(&data);
    return ok ? 0 : 1;
}
