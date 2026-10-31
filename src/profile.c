#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "poker.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool profile_error(char *error, size_t error_size,
                          const char *message, const char *detail)
{
    if (error != NULL && error_size != 0) {
        (void)snprintf(error, error_size, "%s%s%s", message,
                       detail == NULL ? "" : ": ",
                       detail == NULL ? "" : detail);
    }
    return false;
}

static bool profile_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < POKER_NAME_SIZE; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == '\0') {
            return true;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return false;
}

static bool profile_valid(const PokerProfile *profile)
{
    return profile_name_valid(profile->name) &&
           profile->vpip <= profile->hands &&
           profile->pfr <= profile->vpip &&
           profile->folded_to_bet <= profile->faced_bet;
}

/* Headerless TSV: name, hands, vpip, pfr, bets, raises, calls,
 * faced_bet, folded_to_bet. Decimal counters have no signs or whitespace. */
static bool profile_parse(char *line, PokerProfile *profile)
{
    char *tab = strchr(line, '\t');
    if (tab == NULL) {
        return false;
    }
    *tab = '\0';
    if (!profile_name_valid(line)) {
        return false;
    }
    memset(profile, 0, sizeof(*profile));
    memcpy(profile->name, line, strlen(line) + 1);
    uint64_t *fields[] = {
        &profile->hands, &profile->vpip, &profile->pfr, &profile->bets,
        &profile->raises, &profile->calls, &profile->faced_bet,
        &profile->folded_to_bet
    };
    const char *cursor = tab + 1;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t value = 0;
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        do {
            uint64_t digit = (uint64_t)(*cursor - '0');
            if (value > (UINT64_MAX - digit) / UINT64_C(10)) {
                return false;
            }
            value = value * UINT64_C(10) + digit;
            ++cursor;
        } while (*cursor >= '0' && *cursor <= '9');
        *fields[i] = value;
        if (i + 1 == sizeof(fields) / sizeof(fields[0])) {
            if (*cursor != '\0') {
                return false;
            }
        } else if (*cursor++ != '\t') {
            return false;
        }
    }
    return profile_valid(profile);
}

static bool profile_read_db(const char *path, PokerProfile **rows,
                            size_t *count, char *error, size_t error_size)
{
    *rows = NULL;
    *count = 0;
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return true;
        }
        return profile_error(error, error_size, "Cannot open profile DB",
                             strerror(saved_errno));
    }

    size_t capacity = 0;
    size_t line_number = 0;
    bool ok = true;
    for (;;) {
        /* At most 47 name bytes, eight tabs and eight 20-digit counters,
         * plus an optional CR. Read bytes to detect embedded NULs too. */
        char line[POKER_NAME_SIZE + 8 * 21 + 1];
        size_t length = 0;
        bool invalid = false;
        int c;
        while ((c = fgetc(stream)) != EOF && c != '\n') {
            if (c == 0 || length == sizeof(line) - 1) {
                invalid = true;
                break;
            }
            line[length++] = (char)c;
        }
        if (ferror(stream)) {
            int saved_errno = errno;
            ok = profile_error(error, error_size, "Cannot read profile DB",
                               strerror(saved_errno));
            break;
        }
        if (c == EOF && length == 0 && !invalid) {
            break;
        }
        ++line_number;
        if (length != 0 && line[length - 1] == '\r' && c == '\n') {
            --length;
        }
        line[length] = '\0';
        PokerProfile row;
        if (invalid || !profile_parse(line, &row)) {
            char detail[3 * sizeof(size_t) + 1];
            (void)snprintf(detail, sizeof(detail), "%zu", line_number);
            ok = profile_error(error, error_size,
                               "Corrupt profile DB row", detail);
            break;
        }
        for (size_t i = 0; i < *count; ++i) {
            if (strcmp((*rows)[i].name, row.name) == 0) {
                char detail[3 * sizeof(size_t) + 1];
                (void)snprintf(detail, sizeof(detail), "%zu", line_number);
                ok = profile_error(error, error_size,
                                   "Duplicate profile name at row", detail);
                break;
            }
        }
        if (!ok) {
            break;
        }
        if (*count == capacity) {
            size_t maximum = SIZE_MAX / sizeof(**rows);
            size_t next = capacity == 0 ? 8 :
                          (capacity <= maximum / 2 ? capacity * 2 : maximum);
            if (next > maximum || next <= capacity) {
                ok = profile_error(error, error_size, "Profile DB is too large", NULL);
                break;
            }
            PokerProfile *grown = realloc(*rows, next * sizeof(**rows));
            if (grown == NULL) {
                ok = profile_error(error, error_size, "Out of memory reading profile DB", NULL);
                break;
            }
            *rows = grown;
            capacity = next;
        }
        (*rows)[(*count)++] = row;
    }
    if (fclose(stream) != 0 && ok) {
        int saved_errno = errno;
        ok = profile_error(error, error_size, "Cannot close profile DB",
                           strerror(saved_errno));
    }
    if (!ok) {
        free(*rows);
        *rows = NULL;
        *count = 0;
    }
    return ok;
}

bool poker_profile_load(const char *path, const char *name, PokerProfile *profile,
                        char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || profile == NULL ||
        !profile_name_valid(name)) {
        return profile_error(error, error_size, "Invalid profile load arguments", NULL);
    }
    /* The requested name may already reside in profile->name. */
    PokerProfile initial = {0};
    memcpy(initial.name, name, strlen(name) + 1);
    *profile = initial;
    PokerProfile *rows;
    size_t count;
    if (!profile_read_db(path, &rows, &count, error, error_size)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(rows[i].name, initial.name) == 0) {
            *profile = rows[i];
            break;
        }
    }
    free(rows);
    return true;
}

bool poker_profile_save(const char *path, const PokerProfile *profile,
                        char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || profile == NULL ||
        !profile_valid(profile)) {
        return profile_error(error, error_size, "Invalid profile save arguments", NULL);
    }
    PokerProfile *rows;
    size_t count;
    if (!profile_read_db(path, &rows, &count, error, error_size)) {
        return false;
    }
    const char suffix[] = ".tmp.XXXXXX";
    size_t path_length = strlen(path);
    if (path_length > SIZE_MAX - sizeof(suffix)) {
        free(rows);
        return profile_error(error, error_size, "Profile DB path is too long", NULL);
    }
    char *temporary = malloc(path_length + sizeof(suffix));
    if (temporary == NULL) {
        free(rows);
        return profile_error(error, error_size, "Out of memory saving profile DB", NULL);
    }
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, suffix, sizeof(suffix));
    int fd = mkstemp(temporary);
    if (fd == -1) {
        int saved_errno = errno;
        free(temporary);
        free(rows);
        return profile_error(error, error_size, "Cannot create temporary profile DB",
                             strerror(saved_errno));
    }
    FILE *stream = fdopen(fd, "wb");
    if (stream == NULL) {
        int saved_errno = errno;
        (void)close(fd);
        (void)unlink(temporary);
        free(temporary);
        free(rows);
        return profile_error(error, error_size, "Cannot open temporary profile DB",
                             strerror(saved_errno));
    }

    bool replaced = false;
    bool ok = true;
    /* Emit existing rows in order, replacing the target or appending it. */
    for (size_t i = 0;; ++i) {
        const PokerProfile *row;
        if (i == count) {
            if (replaced) {
                break;
            }
            row = profile;
        } else {
            row = &rows[i];
            if (strcmp(row->name, profile->name) == 0) {
                row = profile;
                replaced = true;
            }
        }
        if (fprintf(stream,
                    "%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                    "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                    "\t%" PRIu64 "\t%" PRIu64 "\n",
                    row->name, row->hands, row->vpip, row->pfr,
                    row->bets, row->raises, row->calls, row->faced_bet,
                    row->folded_to_bet) < 0) {
            int saved_errno = errno;
            ok = profile_error(error, error_size, "Cannot write profile DB",
                               strerror(saved_errno));
            break;
        }
        if (i == count) {
            break;
        }
    }
    if (ok && fflush(stream) != 0) {
        int saved_errno = errno;
        ok = profile_error(error, error_size, "Cannot flush profile DB",
                           strerror(saved_errno));
    }
    if (ok && fsync(fd) != 0) {
        int saved_errno = errno;
        ok = profile_error(error, error_size, "Cannot sync profile DB",
                           strerror(saved_errno));
    }
    if (fclose(stream) != 0 && ok) {
        int saved_errno = errno;
        ok = profile_error(error, error_size, "Cannot close temporary profile DB",
                           strerror(saved_errno));
    }
    if (ok && rename(temporary, path) != 0) {
        int saved_errno = errno;
        ok = profile_error(error, error_size, "Cannot replace profile DB",
                           strerror(saved_errno));
    }
    if (!ok) {
        (void)unlink(temporary);
    }
    free(temporary);
    free(rows);
    return ok;
}

/* Beta-binomial posterior means. Convert before adding to avoid uint64 overflow.
 * VPIP: Beta(2,8), PFR: Beta(1,9), fold-to-bet: Beta(2,2).
 * A NULL profile is treated as having no observations. */
double poker_profile_vpip(const PokerProfile *profile)
{
    return profile == NULL ? 0.2 :
           ((double)profile->vpip + 2.0) / ((double)profile->hands + 10.0);
}

double poker_profile_pfr(const PokerProfile *profile)
{
    return profile == NULL ? 0.1 :
           ((double)profile->pfr + 1.0) / ((double)profile->hands + 10.0);
}

double poker_profile_fold(const PokerProfile *profile)
{
    return profile == NULL ? 0.5 :
           ((double)profile->folded_to_bet + 2.0) /
           ((double)profile->faced_bet + 4.0);
}
