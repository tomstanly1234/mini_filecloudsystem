/*
 * server/auth.c
 * -------------
 * User management: flat-file DB, SHA-256 password hashing, role checks.
 *
 * OS Concepts demonstrated
 * ------------------------
 *   Role-Based Authorization – permission table per role
 *   Data Consistency         – pthread_mutex_t guards every read/write
 *                              of users.db (prevents lost-updates / dirty reads)
 */

#include "auth.h"
#include "../shared/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <openssl/sha.h>

/* ── Mutex protecting users.db ───────────────────────────────────────────── */
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Permission table ────────────────────────────────────────────────────── */
typedef struct { const char *role; const char *actions[16]; } RolePerms;

static const RolePerms PERMS[] = {
    { ROLE_ADMIN, { "upload","download","delete","delete_own",
                    "list","search","adduser","deluser",
                    "listusers","logs", NULL } },
    { ROLE_USER,  { "upload","download","delete_own",
                    "list","search", NULL } },
    { ROLE_GUEST, { "download","list","search", NULL } },
    { NULL, { NULL } }
};

int auth_has_permission(const char *role, const char *action)
{
    for (int i = 0; PERMS[i].role; i++) {
        if (strcmp(PERMS[i].role, role) != 0) continue;
        for (int j = 0; PERMS[i].actions[j]; j++)
            if (strcmp(PERMS[i].actions[j], action) == 0) return 1;
        return 0;
    }
    return 0;
}

/* ── SHA-256 hex helper ───────────────────────────────────────────────────── */
static void sha256_hex(const char *data, char *hexout /* 65 bytes */)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data, strlen(data), hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(hexout + i*2, "%02x", hash[i]);
    hexout[64] = '\0';
}

/* ── Simple salt generator ────────────────────────────────────────────────── */
static void gen_salt(char *out /* 33 bytes */)
{
    srand((unsigned)time(NULL) ^ (unsigned)(size_t)out);
    const char *chars = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i] = chars[rand() % 16];
    out[32] = '\0';
}

/*
 * DB format (one user per line):
 *   username:salt:password_hash:role\n
 */

/* ── Internal helpers (call with db_mutex held) ────────────────────────────── */
static int find_user_locked(const char *username,
                             char *salt_out, char *hash_out, char *role_out)
{
    FILE *f = fopen(USERS_DB, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char u[MAX_NAME], s[64], h[128], r[MAX_ROLE];
        if (sscanf(line, "%127[^:]:%63[^:]:%127[^:]:%15[^\n]",
                   u, s, h, r) != 4) continue;
        if (strcmp(u, username) == 0) {
            if (salt_out) strcpy(salt_out, s);
            if (hash_out) strcpy(hash_out, h);
            if (role_out) strcpy(role_out, r);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int count_admins_locked(void)
{
    FILE *f = fopen(USERS_DB, "r");
    if (!f) return 0;
    char line[512];
    int  cnt = 0;
    while (fgets(line, sizeof line, f)) {
        char r[MAX_ROLE];
        if (sscanf(line + strlen(line) - strlen(r) - 2, "%15s", r) < 0) {}
        if (strstr(line, ":admin\n") || strstr(line, ":admin")) cnt++;
    }
    fclose(f);
    return cnt;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void auth_bootstrap(void)
{
    pthread_mutex_lock(&db_mutex);

    FILE *f = fopen(USERS_DB, "r");
    if (f) { fclose(f); pthread_mutex_unlock(&db_mutex); return; }

    /* Create DB directory */
    system("mkdir -p storage/metadata");

    char salt[33], combined[196], hexhash[65];
    gen_salt(salt);
    snprintf(combined, sizeof combined, "%s%s", salt, "admin123");
    sha256_hex(combined, hexhash);

    f = fopen(USERS_DB, "w");
    if (f) {
        fprintf(f, "admin:%s:%s:admin\n", salt, hexhash);
        fclose(f);
        printf("[AUTH] Default admin created (user=admin / pass=admin123)\n");
    }
    pthread_mutex_unlock(&db_mutex);
}

int auth_authenticate(const char *username, const char *password,
                      char *role_out, char *errmsg_out)
{
    pthread_mutex_lock(&db_mutex);
    char salt[64], stored_hash[128], role[MAX_ROLE];
    if (!find_user_locked(username, salt, stored_hash, role)) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(errmsg_out, 128, "Unknown user '%s'", username);
        return 0;
    }
    pthread_mutex_unlock(&db_mutex);

    char combined[320], hexhash[65];
    snprintf(combined, sizeof combined, "%s%s", salt, password);
    sha256_hex(combined, hexhash);

    if (strcmp(hexhash, stored_hash) != 0) {
        snprintf(errmsg_out, 128, "Wrong password");
        return 0;
    }
    strcpy(role_out, role);
    return 1;
}

int auth_add_user(const char *username, const char *password,
                  const char *role, char *errmsg_out)
{
    if (strcmp(role,ROLE_ADMIN)!=0 && strcmp(role,ROLE_USER)!=0
        && strcmp(role,ROLE_GUEST)!=0) {
        snprintf(errmsg_out, 128, "Invalid role '%s'", role);
        return 0;
    }

    pthread_mutex_lock(&db_mutex);
    if (find_user_locked(username, NULL, NULL, NULL)) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(errmsg_out, 128, "User '%s' already exists", username);
        return 0;
    }

    char salt[33], combined[320], hexhash[65];
    gen_salt(salt);
    snprintf(combined, sizeof combined, "%s%s", salt, password);
    sha256_hex(combined, hexhash);

    FILE *f = fopen(USERS_DB, "a");
    if (!f) {
        pthread_mutex_unlock(&db_mutex);
        snprintf(errmsg_out, 128, "Cannot open users DB");
        return 0;
    }
    fprintf(f, "%s:%s:%s:%s\n", username, salt, hexhash, role);
    fclose(f);
    pthread_mutex_unlock(&db_mutex);
    return 1;
}

int auth_delete_user(const char *username, char *errmsg_out)
{
    pthread_mutex_lock(&db_mutex);

    FILE *f = fopen(USERS_DB, "r");
    if (!f) { pthread_mutex_unlock(&db_mutex); snprintf(errmsg_out,128,"DB error"); return 0; }

    char tmp_path[] = "storage/metadata/users.db.tmp";
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); pthread_mutex_unlock(&db_mutex);
                snprintf(errmsg_out,128,"Tmp file error"); return 0; }

    char line[512];
    int  found = 0;
    char del_role[MAX_ROLE] = "";
    while (fgets(line, sizeof line, f)) {
        char u[MAX_NAME], r[MAX_ROLE];
        sscanf(line, "%127[^:]%*[^:]%*[^:]:%15[^\n]", u, r);
        char u2[MAX_NAME];
        sscanf(line, "%127[^:]", u2);
        if (strcmp(u2, username) == 0) {
            found = 1;
            /* extract role from end of line */
            char *colon = strrchr(line, ':');
            if (colon) { strncpy(del_role, colon+1, MAX_ROLE-1); }
            char *nl = strchr(del_role, '\n'); if (nl) *nl = '\0';
            continue; /* skip this line */
        }
        fputs(line, tmp);
    }
    fclose(f); fclose(tmp);

    if (!found) {
        remove(tmp_path);
        pthread_mutex_unlock(&db_mutex);
        snprintf(errmsg_out, 128, "User '%s' not found", username);
        return 0;
    }
    if (strcmp(del_role, ROLE_ADMIN) == 0 && count_admins_locked() <= 1) {
        remove(tmp_path);
        pthread_mutex_unlock(&db_mutex);
        snprintf(errmsg_out, 128, "Cannot delete last admin");
        return 0;
    }

    rename(tmp_path, USERS_DB);
    pthread_mutex_unlock(&db_mutex);
    return 1;
}

int auth_list_users(char *buf, int bufsz)
{
    pthread_mutex_lock(&db_mutex);
    FILE *f = fopen(USERS_DB, "r");
    if (!f) { pthread_mutex_unlock(&db_mutex); snprintf(buf,bufsz,"[]"); return 0; }

    char line[512];
    int  cnt = 0;
    int  off = 0;
    off += snprintf(buf + off, bufsz - off, "[");
    while (fgets(line, sizeof line, f)) {
        char u[MAX_NAME], r[MAX_ROLE];
        if (sscanf(line, "%127[^:]%*[^:]%*[^:]:%15[^\n]", u, r) < 1) {
            sscanf(line, "%127[^:]", u);
            char *cl = strrchr(line, ':');
            if (cl) { strncpy(r, cl+1, MAX_ROLE-1); char *nl=strchr(r,'\n'); if(nl)*nl='\0'; }
        }
        /* Simpler: grab username (before first :) and role (after last :) */
        char *first_colon = strchr(line, ':');
        char *last_colon  = strrchr(line, ':');
        if (!first_colon || !last_colon || first_colon == last_colon) continue;
        *first_colon = '\0';
        strcpy(u, line);
        strncpy(r, last_colon + 1, MAX_ROLE - 1);
        char *nl = strchr(r, '\n'); if (nl) *nl = '\0';

        if (cnt > 0) off += snprintf(buf+off, bufsz-off, ",");
        off += snprintf(buf+off, bufsz-off,
                        "{\"username\":\"%s\",\"role\":\"%s\"}", u, r);
        cnt++;
    }
    off += snprintf(buf + off, bufsz - off, "]");
    fclose(f);
    pthread_mutex_unlock(&db_mutex);
    return cnt;
}
