#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

/*
 * server/lock_manager.h
 * ---------------------
 * Per-file Read/Write locking using pthreads primitives.
 *
 * OS Concepts:
 *   • File Locking       – advisory read/write locks per filename
 *   • Concurrency Control – pthread_rwlock_t (multiple readers / one writer)
 *   • Data Consistency   – prevents simultaneous conflicting file access
 */

#define MAX_LOCKED_FILES 256
#define LOCK_TIMEOUT_SEC 10

/* Initialise the lock table */
void lm_init(void);

/*
 * Acquire a READ lock on filename for user `who`.
 * Blocks up to LOCK_TIMEOUT_SEC.
 * Returns 1 on success, 0 on timeout/error.
 */
int lm_read_lock(const char *filename, const char *who);

/* Release a read lock */
void lm_read_unlock(const char *filename);

/*
 * Acquire a WRITE lock on filename for user `who`.
 * Returns 1 on success, 0 on timeout/error.
 */
int lm_write_lock(const char *filename, const char *who);

/* Release a write lock */
void lm_write_unlock(const char *filename);

/* Fill buf with JSON status of all locks */
void lm_status_json(char *buf, int bufsz);
int lm_get_lock_holder(const char *filename, char *holder_out, int holder_sz);
#endif /* LOCK_MANAGER_H */
