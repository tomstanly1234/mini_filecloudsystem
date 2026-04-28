/*
 * server/metadata.c
 * -----------------
 * Flat-file metadata DB for stored files.
 *
 * DB record format (one per line):
 *   filename:owner:size:version:uploaded:modified\n
 *
 * OS Concepts demonstrated
 * ------------------------
 *   Data Consistency – pthread_mutex_t guards every read/write/update,
 *                      preventing race conditions, dirty reads, lost updates
 */

#include "metadata.h"
#include "../shared/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define TMP_DB "storage/metadata/files.db.tmp"
#define FIELD_SZ 64

static pthread_mutex_t meta_mutex = PTHREAD_MUTEX_INITIALIZER;

static void now_str(char *buf, int sz)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", tm);
}

/* ── Parse a single DB line ───────────────────────────────────────────────── */
typedef struct {
    char filename[MAX_NAME];
    char owner[MAX_NAME];
    long size;
    int  version;
    char uploaded[FIELD_SZ];
    char modified[FIELD_SZ];
} MetaRecord;

static int parse_record(const char *line, MetaRecord *r)
{
    return sscanf(line,
        "%127[^:]:%127[^:]:%ld:%d:%63[^:]:%63[^\n]",
        r->filename, r->owner, &r->size, &r->version,
        r->uploaded, r->modified) == 6;
}

/* ── Upsert ───────────────────────────────────────────────────────────────── */
int meta_upsert(const char *filename, const char *owner, long size)
{
    pthread_mutex_lock(&meta_mutex);
    system("mkdir -p storage/metadata");

    char ts[FIELD_SZ]; now_str(ts, sizeof ts);
    FILE *f   = fopen(FILES_DB, "r");
    FILE *tmp = fopen(TMP_DB,   "w");
    if (!tmp) { if (f) fclose(f); pthread_mutex_unlock(&meta_mutex); return -1; }

    int  version = 1;
    int  found   = 0;
    char line[512];

    if (f) {
        while (fgets(line, sizeof line, f)) {
            MetaRecord r;
            if (!parse_record(line, &r)) { fputs(line, tmp); continue; }
            if (strcmp(r.filename, filename) == 0) {
                /* Update existing record */
                version = r.version + 1;
                fprintf(tmp, "%s:%s:%ld:%d:%s:%s\n",
                        filename, owner, size, version, r.uploaded, ts);
                found = 1;
            } else {
                fputs(line, tmp);
            }
        }
        fclose(f);
    }
    if (!found)
        fprintf(tmp, "%s:%s:%ld:%d:%s:%s\n",
                filename, owner, size, 1, ts, ts);

    fclose(tmp);
    rename(TMP_DB, FILES_DB);
    pthread_mutex_unlock(&meta_mutex);
    return version;
}

/* ── Remove ───────────────────────────────────────────────────────────────── */
int meta_remove(const char *filename)
{
    pthread_mutex_lock(&meta_mutex);
    FILE *f   = fopen(FILES_DB, "r");
    if (!f) { pthread_mutex_unlock(&meta_mutex); return 0; }
    FILE *tmp = fopen(TMP_DB, "w");
    if (!tmp) { fclose(f); pthread_mutex_unlock(&meta_mutex); return 0; }

    char line[512]; int found = 0;
    while (fgets(line, sizeof line, f)) {
        MetaRecord r;
        if (parse_record(line, &r) && strcmp(r.filename, filename) == 0) {
            found = 1; continue;
        }
        fputs(line, tmp);
    }
    fclose(f); fclose(tmp);
    rename(TMP_DB, FILES_DB);
    pthread_mutex_unlock(&meta_mutex);
    return found;
}

/* ── Owner lookup ─────────────────────────────────────────────────────────── */
int meta_owner(const char *filename, char *owner_out)
{
    pthread_mutex_lock(&meta_mutex);
    FILE *f = fopen(FILES_DB, "r");
    if (!f) { pthread_mutex_unlock(&meta_mutex); return 0; }
    char line[512]; int found = 0;
    while (fgets(line, sizeof line, f)) {
        MetaRecord r;
        if (parse_record(line, &r) && strcmp(r.filename, filename) == 0) {
            strncpy(owner_out, r.owner, MAX_NAME - 1);
            found = 1; break;
        }
    }
    fclose(f);
    pthread_mutex_unlock(&meta_mutex);
    return found;
}

/* ── List all (JSON) ──────────────────────────────────────────────────────── */
void meta_list_json(char *buf, int bufsz)
{
    pthread_mutex_lock(&meta_mutex);
    FILE *f = fopen(FILES_DB, "r");
    int off = snprintf(buf, bufsz, "[");
    int first = 1;
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            MetaRecord r;
            if (!parse_record(line, &r)) continue;
            if (!first) off += snprintf(buf+off, bufsz-off, ",");
            off += snprintf(buf+off, bufsz-off,
                "{\"filename\":\"%s\",\"owner\":\"%s\","
                "\"size\":%ld,\"version\":%d,"
                "\"uploaded\":\"%s\",\"modified\":\"%s\"}",
                r.filename, r.owner, r.size, r.version,
                r.uploaded, r.modified);
            first = 0;
        }
        fclose(f);
    }
    snprintf(buf+off, bufsz-off, "]");
    pthread_mutex_unlock(&meta_mutex);
}

/* ── Search (JSON) ────────────────────────────────────────────────────────── */
void meta_search_json(const char *query, char *buf, int bufsz)
{
    pthread_mutex_lock(&meta_mutex);
    FILE *f = fopen(FILES_DB, "r");
    int off = snprintf(buf, bufsz, "[");
    int first = 1;
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            MetaRecord r;
            if (!parse_record(line, &r)) continue;
            if (!strstr(r.filename, query)) continue;
            if (!first) off += snprintf(buf+off, bufsz-off, ",");
            off += snprintf(buf+off, bufsz-off,
                "{\"filename\":\"%s\",\"owner\":\"%s\","
                "\"size\":%ld,\"version\":%d}",
                r.filename, r.owner, r.size, r.version);
            first = 0;
        }
        fclose(f);
    }
    snprintf(buf+off, bufsz-off, "]");
    pthread_mutex_unlock(&meta_mutex);
}
