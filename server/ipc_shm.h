#ifndef IPC_SHM_H
#define IPC_SHM_H

/*
 * server/ipc_shm.h
 * ----------------
 * Active session table stored in POSIX shared memory.
 *
 * OS Concept:
 *   Inter-Process Communication (IPC) – POSIX shm_open / mmap
 *   Concurrency Control               – Named semaphore guards every access
 */

#define SHM_NAME   "/minicloud_sessions"
#define SEM_NAME   "/minicloud_sem"
#define MAX_SESSIONS 64

typedef struct {
    char username[64];
    char role[16];
    char client_addr[32];
    char login_time[32];
    char last_active[32];
    int  active;
} Session;

typedef struct {
    Session sessions[MAX_SESSIONS];
    int     count;
} SessionTable;

/* Initialise shared memory and semaphore (call once in server) */
int  shm_session_init(void);

/* Cleanup (call on server shutdown) */
void shm_session_cleanup(void);

/* Add / remove / touch session */
void shm_session_add(const char *username, const char *role, const char *addr);
void shm_session_remove(const char *username);
void shm_session_touch(const char *username);

/* Copy current sessions snapshot into buf as JSON array */
void shm_session_list_json(char *buf, int bufsz);

int  shm_session_count(void);

#endif /* IPC_SHM_H */
