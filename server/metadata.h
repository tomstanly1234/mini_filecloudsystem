#ifndef METADATA_H
#define METADATA_H

/*
 * server/metadata.h
 * -----------------
 * File metadata: owner, version, size, timestamps.
 * DB: storage/metadata/files.db  (one record per line)
 *
 * OS Concept:
 *   Data Consistency – pthread_mutex_t prevents concurrent metadata corruption
 */

#define FILES_DB "storage/metadata/files.db"

/* Add or update a file record. Returns new version number. */
int  meta_upsert(const char *filename, const char *owner, long size);

/* Remove a file record. Returns 1 on success. */
int  meta_remove(const char *filename);

/* Get owner of a file (fills owner_out[MAX_NAME]). Returns 1 if found. */
int  meta_owner(const char *filename, char *owner_out);

/* Fill buf with JSON array of all file records. */
void meta_list_json(char *buf, int bufsz);

/* Fill buf with JSON array of files matching query substring. */
void meta_search_json(const char *query, char *buf, int bufsz);

#endif /* METADATA_H */
