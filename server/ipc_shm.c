/*
 * server/ipc_shm.c
 * ----------------
 * Session table in POSIX shared memory, guarded by a named semaphore.
 *
 * OS Concepts demonstrated
 * ------------------------
 *   IPC (Shared Memory) – shm_open / ftruncate / mmap
 *   IPC (Semaphore)     – sem_open / sem_wait / sem_post  (binary semaphore)
 *   Concurrency Control – semaphore ensures mutual exclusion on shared data
 */

#include "ipc_shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

static SessionTable *g_table = NULL;
static sem_t        *g_sem   = NULL;
static int           g_shm_fd = -1;

/* ── Helper: get current timestamp string ─────────────────────────────────── */
static void now_str(char *buf, int sz)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", tm);
}

/* ── Init ─────────────────────────────────────────────────────────────────── */
int shm_session_init(void)
{
    /* Remove stale objects from a previous run */
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_NAME);

    /* Create shared memory */
    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0660);
    if (g_shm_fd < 0) { perror("shm_open"); return -1; }

    if (ftruncate(g_shm_fd, sizeof(SessionTable)) < 0) {
        perror("ftruncate"); return -1;
    }

    g_table = mmap(NULL, sizeof(SessionTable),
                   PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_table == MAP_FAILED) { perror("mmap"); return -1; }

    memset(g_table, 0, sizeof(SessionTable));

    /* Create binary semaphore (initial value = 1 → unlocked) */
    g_sem = sem_open(SEM_NAME, O_CREAT | O_EXCL, 0660, 1);
    if (g_sem == SEM_FAILED) { perror("sem_open"); return -1; }

    printf("[IPC-SHM] Shared memory + semaphore initialised\n");
    return 0;
}

void shm_session_cleanup(void)
{
    if (g_table) munmap(g_table, sizeof(SessionTable));
    if (g_shm_fd >= 0) close(g_shm_fd);
    shm_unlink(SHM_NAME);
    if (g_sem) { sem_close(g_sem); sem_unlink(SEM_NAME); }
}

/* ── sem_wait / sem_post wrappers ─────────────────────────────────────────── */
static void lock_shm(void)  { sem_wait(g_sem); }
static void unlock_shm(void){ sem_post(g_sem); }

/* ── Add session ──────────────────────────────────────────────────────────── */
void shm_session_add(const char *username, const char *role, const char *addr)
{
    if (!g_table) return;
    lock_shm();
    char ts[32]; now_str(ts, sizeof ts);

    /* Update existing slot first */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_table->sessions[i].active &&
            strcmp(g_table->sessions[i].username, username) == 0) {
            strncpy(g_table->sessions[i].role,        role, 15);
            strncpy(g_table->sessions[i].client_addr, addr, 31);
            strncpy(g_table->sessions[i].login_time,  ts,   31);
            strncpy(g_table->sessions[i].last_active, ts,   31);
            unlock_shm();
            return;
        }
    }
    /* New slot */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_table->sessions[i].active) {
            strncpy(g_table->sessions[i].username,    username, 63);
            strncpy(g_table->sessions[i].role,        role,     15);
            strncpy(g_table->sessions[i].client_addr, addr,     31);
            strncpy(g_table->sessions[i].login_time,  ts,       31);
            strncpy(g_table->sessions[i].last_active, ts,       31);
            g_table->sessions[i].active = 1;
            g_table->count++;
            break;
        }
    }
    unlock_shm();
}

/* ── Remove session ───────────────────────────────────────────────────────── */
void shm_session_remove(const char *username)
{
    if (!g_table) return;
    lock_shm();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_table->sessions[i].active &&
            strcmp(g_table->sessions[i].username, username) == 0) {
            memset(&g_table->sessions[i], 0, sizeof(Session));
            g_table->count--;
            break;
        }
    }
    unlock_shm();
}

/* ── Touch last_active ────────────────────────────────────────────────────── */
void shm_session_touch(const char *username)
{
    if (!g_table) return;
    lock_shm();
    char ts[32]; now_str(ts, sizeof ts);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_table->sessions[i].active &&
            strcmp(g_table->sessions[i].username, username) == 0) {
            strncpy(g_table->sessions[i].last_active, ts, 31);
            break;
        }
    }
    unlock_shm();
}

/* ── List sessions as JSON ────────────────────────────────────────────────── */
void shm_session_list_json(char *buf, int bufsz)
{
    if (!g_table) { snprintf(buf, bufsz, "[]"); return; }
    lock_shm();
    int off = snprintf(buf, bufsz, "[");
    int first = 1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &g_table->sessions[i];
        if (!s->active) continue;
        if (!first) off += snprintf(buf+off, bufsz-off, ",");
        off += snprintf(buf+off, bufsz-off,
            "{\"username\":\"%s\",\"role\":\"%s\","
            "\"addr\":\"%s\",\"login\":\"%s\",\"last_active\":\"%s\"}",
            s->username, s->role, s->client_addr,
            s->login_time, s->last_active);
        first = 0;
    }
    snprintf(buf+off, bufsz-off, "]");
    unlock_shm();
}

int shm_session_count(void)
{
    if (!g_table) return 0;
    lock_shm();
    int c = g_table->count;
    unlock_shm();
    return c;
}
