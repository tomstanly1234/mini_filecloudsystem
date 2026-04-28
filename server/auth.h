#ifndef AUTH_H
#define AUTH_H

/*
 * server/auth.h
 * -------------
 * User account management.
 * Passwords stored as SHA-256(salt+password) hex strings.
 * File: storage/metadata/users.db  (simple text records)
 *
 * OS Concepts:
 *   • Role-Based Authorization  – roles admin / user / guest
 *   • Data Consistency          – pthread_mutex_t protects users.db
 */

#define USERS_DB  "storage/metadata/users.db"

/* Returns 1 if role can perform action, 0 otherwise */
int auth_has_permission(const char *role, const char *action);

/* Authenticate user. Returns 1 and fills role_out on success, 0 on failure. */
int auth_authenticate(const char *username, const char *password,
                      char *role_out, char *errmsg_out);

/* Add a new user. Returns 1 on success. */
int auth_add_user(const char *username, const char *password,
                  const char *role, char *errmsg_out);

/* Delete a user. Returns 1 on success. */
int auth_delete_user(const char *username, char *errmsg_out);

/*
 * List all users as a JSON array string written into buf.
 * Returns number of users listed.
 */
int auth_list_users(char *buf, int bufsz);

/* Creates default admin (admin/admin123) if DB is empty. */
void auth_bootstrap(void);

#endif /* AUTH_H */
