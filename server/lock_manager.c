/*
 * server/lock_manager.c
 * ---------------------
 * Per-file advisory read/write locks.
 *
 * OS Concepts demonstrated
 * ------------------------
 *   File Locking       – one pthread_rwlock_t per active file
 *   Concurrency Control – pthread_rwlock_rdlock / wrlock with timed wait
 *   Data Consistency   – readers share lock; writer gets exclusive access,
 *                        preventing dirty reads and lost updates
 */

#include "lock_manager.h"
#include "../shared/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* ── Lock table entry ────────────────────────────────────────────────────── */
typedef struct {
    char            filename[MAX_NAME];
    pthread_rwlock_t rwlock;
    char            write_owner[MAX_NAME]; /* who holds write lock */
    int             reader_count;
    int             in_use;
} FileLock;

static FileLock  lock_table[MAX_LOCKED_FILES];
static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Initialise ──────────────────────────────────────────────────────────── */
void lm_init(void)
{
    memset(lock_table, 0, sizeof lock_table);
    for (int i = 0; i < MAX_LOCKED_FILES; i++)
        pthread_rwlock_init(&lock_table[i].rwlock, NULL);
}

/* ── Find or create entry (call with table_mutex held) ───────────────────── */
static FileLock *get_entry(const char *filename)
{
    /* Look for existing */
    for (int i = 0; i < MAX_LOCKED_FILES; i++)
        if (lock_table[i].in_use &&
            strcmp(lock_table[i].filename, filename) == 0)
            return &lock_table[i];
    /* Allocate new slot */
    for (int i = 0; i < MAX_LOCKED_FILES; i++) {
        if (!lock_table[i].in_use) {
            lock_table[i].in_use = 1;
            strncpy(lock_table[i].filename, filename, MAX_NAME - 1);
            lock_table[i].write_owner[0] = '\0';
            lock_table[i].reader_count   = 0;
            return &lock_table[i];
        }
    }
    return NULL; /* table full */
}

/* ── Read lock ───────────────────────────────────────────────────────────── */
int lm_read_lock(const char *filename, const char *who)
{
    (void)who; /* recorded for status only */
    pthread_mutex_lock(&table_mutex);
    FileLock *e = get_entry(filename);
    pthread_mutex_unlock(&table_mutex);
    if (!e) return 0;

    /* pthread_rwlock_timedrdlock – blocks writers while readers hold */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += LOCK_TIMEOUT_SEC;

    if (pthread_rwlock_timedrdlock(&e->rwlock, &ts) != 0) return 0;

    pthread_mutex_lock(&table_mutex);
    e->reader_count++;
    pthread_mutex_unlock(&table_mutex);
    return 1;
}

void lm_read_unlock(const char *filename)
{
    pthread_mutex_lock(&table_mutex);
    for (int i = 0; i < MAX_LOCKED_FILES; i++) {
        if (lock_table[i].in_use &&
            strcmp(lock_table[i].filename, filename) == 0) {
            lock_table[i].reader_count--;
            pthread_mutex_unlock(&table_mutex);
            pthread_rwlock_unlock(&lock_table[i].rwlock);
            return;
        }
    }
    pthread_mutex_unlock(&table_mutex);
}

/* ── Write lock ──────────────────────────────────────────────────────────── */
int lm_write_lock(const char *filename, const char *who)
{
    pthread_mutex_lock(&table_mutex);
    FileLock *e = get_entry(filename);
    pthread_mutex_unlock(&table_mutex);
    if (!e) return 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += LOCK_TIMEOUT_SEC;

    if (pthread_rwlock_timedwrlock(&e->rwlock, &ts) != 0) return 0;

    pthread_mutex_lock(&table_mutex);
    strncpy(e->write_owner, who, MAX_NAME - 1);
    pthread_mutex_unlock(&table_mutex);
    return 1;
}

void lm_write_unlock(const char *filename)
{
    pthread_mutex_lock(&table_mutex);
    for (int i = 0; i < MAX_LOCKED_FILES; i++) {
        if (lock_table[i].in_use &&
            strcmp(lock_table[i].filename, filename) == 0) {
            lock_table[i].write_owner[0] = '\0';
            pthread_mutex_unlock(&table_mutex);
            pthread_rwlock_unlock(&lock_table[i].rwlock);
            return;
        }
    }
    pthread_mutex_unlock(&table_mutex);
}

/* ── Status JSON ─────────────────────────────────────────────────────────── */
void lm_status_json(char *buf, int bufsz)
{
    pthread_mutex_lock(&table_mutex);
    int off = snprintf(buf, bufsz, "[");
    int first = 1;
    for (int i = 0; i < MAX_LOCKED_FILES; i++) {
        if (!lock_table[i].in_use) continue;
        if (!first) off += snprintf(buf+off, bufsz-off, ",");
        off += snprintf(buf+off, bufsz-off,
            "{\"file\":\"%s\",\"readers\":%d,\"write_owner\":\"%s\"}",
            lock_table[i].filename,
            lock_table[i].reader_count,
            lock_table[i].write_owner);
        first = 0;
    }
    snprintf(buf+off, bufsz-off, "]");
    pthread_mutex_unlock(&table_mutex);
}
int lm_get_lock_holder(const char *filename, char *holder_out, int holder_sz)
{
    int result = 0;
    pthread_mutex_lock(&table_mutex);
    for (int i = 0; i < MAX_LOCKED_FILES; i++) {
        if (lock_table[i].in_use &&
            strcmp(lock_table[i].filename, filename) == 0) {
            if (lock_table[i].write_owner[0] != '\0') {
                strncpy(holder_out, lock_table[i].write_owner, holder_sz - 1);
                holder_out[holder_sz - 1] = '\0';
                result = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&table_mutex);
    return result;
}