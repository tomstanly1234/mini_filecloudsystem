/*
 * server/server.c
 * ---------------
 * Mini Cloud Drive — TCP server
 *
 * OS Concepts demonstrated
 * ------------------------
 *   Socket Programming     – TCP server, AF_INET, bind/listen/accept
 *   Concurrency Control    – one pthread per client (one-thread-per-connection)
 *   Mutex / Semaphore      – sem_t limits max concurrent clients
 *   File Locking           – pthread_rwlock via lock_manager
 *   Role-Based Auth        – checked before every operation
 *   IPC Shared Memory      – POSIX shm tracks live sessions (ipc_shm)
 *   IPC Message Queue      – POSIX mq logs events (ipc_mq)
 *   Data Consistency       – locks + metadata mutex prevent race conditions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../shared/protocol.h"
#include "auth.h"
#include "lock_manager.h"
#include "metadata.h"
#include "ipc_shm.h"
#include "ipc_mq.h"

#define STORAGE_DIR   "storage/files"
#define TRASH_DIR     "storage/trash"
#define LOGS_FILE     "storage/logs/activity.log"
#define SHARE_DB_FILE "storage/metadata/shares.db"

/* ── Share Permission Table ──────────────────────────────────────────────── */
#define MAX_SHARES      512
#define SHARE_PERM_READ  1
#define SHARE_PERM_WRITE 2

typedef struct {
    char filename[MAX_NAME];
    char owner[MAX_NAME];       /* who shared it */
    char grantee[MAX_NAME];     /* who receives access */
    int  perm;                  /* SHARE_PERM_READ | SHARE_PERM_WRITE */
} ShareEntry;

static ShareEntry  g_shares[MAX_SHARES];
static int         g_share_count = 0;
static pthread_mutex_t g_share_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Share persistence helpers ───────────────────────────────────────────── */
static void shares_load(void)
{
    FILE *f = fopen(SHARE_DB_FILE, "r");
    if (!f) return;
    g_share_count = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && g_share_count < MAX_SHARES) {
        ShareEntry *e = &g_shares[g_share_count];
        char perm_str[16] = {0};
        /* format: filename|owner|grantee|perm */
        if (sscanf(line, "%127[^|]|%127[^|]|%127[^|]|%15s",
                   e->filename, e->owner, e->grantee, perm_str) == 4) {
            e->perm = atoi(perm_str);
            g_share_count++;
        }
    }
    fclose(f);
}

static void shares_save(void)
{
    FILE *f = fopen(SHARE_DB_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g_share_count; i++) {
        fprintf(f, "%s|%s|%s|%d\n",
                g_shares[i].filename,
                g_shares[i].owner,
                g_shares[i].grantee,
                g_shares[i].perm);
    }
    fclose(f);
}

/*
 * Returns combined permission bits the user has on filename:
 *   0 if none, SHARE_PERM_READ, SHARE_PERM_WRITE, or both.
 * Owner always gets full access (caller must add that logic).
 */
static int share_get_perm(const char *filename, const char *username)
{
    int result = 0;
    pthread_mutex_lock(&g_share_mutex);
    for (int i = 0; i < g_share_count; i++) {
        if (strcmp(g_shares[i].filename, filename) == 0 &&
            strcmp(g_shares[i].grantee,  username)  == 0) {
            result |= g_shares[i].perm;
        }
    }
    pthread_mutex_unlock(&g_share_mutex);
    return result;
}

/* Binary semaphore: limits total concurrent clients */
static sem_t connection_sem;

/* Mutex protecting console output */
static pthread_mutex_t console_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Per-client context ───────────────────────────────────────────────────── */
typedef struct {
    int  fd;
    char addr[32];
} ClientCtx;

/* ── Helper macros ───────────────────────────────────────────────────────────*/
#define SEND_OK(fd, fmt, ...)  do { \
    char _b[MAX_MSG_SIZE]; \
    snprintf(_b, sizeof _b, "{\"status\":\"OK\"," fmt "}", ##__VA_ARGS__); \
    proto_send_msg(fd, _b); } while(0)

#define SEND_ERR(fd, msg) do { \
    char _b[512]; \
    snprintf(_b, sizeof _b, \
        "{\"status\":\"ERROR\",\"message\":\"%s\"}", msg); \
    proto_send_msg(fd, _b); } while(0)

#define SEND_DENIED(fd, msg) do { \
    char _b[512]; \
    snprintf(_b, sizeof _b, \
        "{\"status\":\"DENIED\",\"message\":\"%s\"}", msg); \
    proto_send_msg(fd, _b); } while(0)

#define SEND_LOCKED(fd, file) do { \
    char _b[512]; \
    snprintf(_b, sizeof _b, \
        "{\"status\":\"LOCKED\",\"message\":\"'%s' is locked\"}", file); \
    proto_send_msg(fd, _b); } while(0)

#define SEND_NOTFOUND(fd, file) do { \
    char _b[512]; \
    snprintf(_b, sizeof _b, \
        "{\"status\":\"NOT_FOUND\",\"message\":\"'%s' not found\"}", file); \
    proto_send_msg(fd, _b); } while(0)

/* ── Command handlers ────────────────────────────────────────────────────── */

static void handle_login(int fd, const char *req,
                          char *username, char *role)
{
    if (username[0] != '\0') { SEND_ERR(fd,"Already logged in"); return; }

    char user[MAX_NAME] = {0}, pass[MAX_PASS] = {0};
    if (!json_get_str(req, "username", user, MAX_NAME) ||
        !json_get_str(req, "password", pass, MAX_PASS)) {
        SEND_ERR(fd, "Missing username or password"); return;
    }

    char errmsg[128] = {0};
    if (!auth_authenticate(user, pass, role, errmsg)) {
        SEND_DENIED(fd, errmsg);
        return;
    }
    strncpy(username, user, MAX_NAME - 1);
    SEND_OK(fd, "\"role\":\"%s\",\"message\":\"Welcome %s!\"", role, user);
    mq_notify_login(user, role);
}

static void handle_logout(int fd, char *username, char *role)
{
    if (!username[0]) { SEND_ERR(fd, "Not logged in"); return; }
    shm_session_remove(username);
    mq_notify_logout(username);
    SEND_OK(fd, "\"message\":\"Goodbye!\"");
    username[0] = '\0';
    role[0]     = '\0';
}

static void handle_upload(int fd, const char *req,
                           const char *username, const char *role)
{
    if (!auth_has_permission(role, "upload")) {
        SEND_DENIED(fd, "No upload permission"); return;
    }
    char filename[MAX_NAME] = {0};
    long size = 0;
    if (!json_get_str(req, "filename", filename, MAX_NAME) ||
        !json_get_int(req, "size", &size) || size <= 0) {
        SEND_ERR(fd, "Bad request: need filename + size"); return;
    }
    /* Sanitise filename: no slashes */
    if (strchr(filename, '/') || strchr(filename, '\\')) {
        SEND_ERR(fd, "Invalid filename"); return;
    }

    /* ── Overwrite policy (cloud-style) ──────────────────────────────────
     * A file can only be overwritten by:
     *   1. The original owner, OR
     *   2. An admin, OR
     *   3. A user who has been granted "write" share permission.
     * If the file doesn't exist yet, anyone with upload permission may create it.
     * ──────────────────────────────────────────────────────────────────── */
    char existing_owner[MAX_NAME] = {0};
    meta_owner(filename, existing_owner);   /* empty if file doesn't exist */

    if (existing_owner[0] != '\0') {
        /* File already exists — check overwrite rights */
        int is_owner = (strcmp(existing_owner, username) == 0);
        int is_admin = (strcmp(role, ROLE_ADMIN) == 0);
        int share_perm = share_get_perm(filename, username);
        int has_write_share = (share_perm & SHARE_PERM_WRITE) != 0;

        if (!is_owner && !is_admin && !has_write_share) {
            SEND_DENIED(fd, "Overwrite denied: you are not the owner and "
                            "have no write permission on this file"); return;
        }
    }

    /* Acquire write lock ── File Locking */
    if (!lm_write_lock(filename, username)) {
        SEND_LOCKED(fd, filename); return;
    }

    /* Signal client to proceed */
    SEND_OK(fd, "\"message\":\"READY\"");

    /* Receive file binary */
    unsigned char *data = NULL; uint32_t recv_len = 0;
    if (proto_recv_bin(fd, &data, &recv_len) < 0 || (long)recv_len != size) {
        lm_write_unlock(filename);
        if (data) free(data);
        SEND_ERR(fd, "Incomplete upload"); return;
    }

    /* Atomic write: write to a temp file first, then rename */
    char path[MAX_PATH], tmp_path[MAX_PATH];
    snprintf(path,     sizeof path,     "%s/%s",      STORAGE_DIR, filename);
    snprintf(tmp_path, sizeof tmp_path, "%s/.%s.tmp", STORAGE_DIR, filename);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        lm_write_unlock(filename);
        free(data);
        SEND_ERR(fd, "Server storage error"); return;
    }
    size_t written = fwrite(data, 1, recv_len, f);
    fclose(f); free(data);

    if (written != recv_len) {
        remove(tmp_path);
        lm_write_unlock(filename);
        SEND_ERR(fd, "Disk write error: partial write"); return;
    }

    /* Atomic rename — guarantees readers never see a half-written file */
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        lm_write_unlock(filename);
        SEND_ERR(fd, "Server storage error (rename failed)"); return;
    }

    lm_write_unlock(filename);   /* release write lock */

    /* Preserve original owner on overwrite — a write-share grantee must NOT
     * become the new owner just because they uploaded a new version.
     * existing_owner is non-empty when the file already existed; in that case
     * we keep the original owner in metadata. Only a brand-new file records
     * the uploader as owner. */
    const char *meta_owner_to_set = (existing_owner[0] != '\0')
                                    ? existing_owner
                                    : username;

    int version = meta_upsert(filename, meta_owner_to_set, (long)recv_len);
    mq_notify_upload(username, filename, version);
    shm_session_touch(username);

    SEND_OK(fd, "\"message\":\"'%s' uploaded (v%d)\",\"version\":%d",
            filename, version, version);
}

static void handle_download(int fd, const char *req,
                             const char *username, const char *role)
{
    if (!auth_has_permission(role, "download")) {
        SEND_DENIED(fd, "No download permission"); return;
    }
    char filename[MAX_NAME] = {0};
    if (!json_get_str(req, "filename", filename, MAX_NAME)) {
        SEND_ERR(fd, "Missing filename"); return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, filename);

    struct stat st;
    if (stat(path, &st) != 0) { SEND_NOTFOUND(fd, filename); return; }

    /* Acquire read lock ── File Locking */
    if (!lm_read_lock(filename, username)) {
        SEND_LOCKED(fd, filename); return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { lm_read_unlock(filename); SEND_NOTFOUND(fd, filename); return; }
    long fsz = st.st_size;
    unsigned char *data = malloc(fsz);
    fread(data, 1, fsz, f); fclose(f);
    lm_read_unlock(filename);   /* release read lock */

    /* Send metadata first */
    char resp[512];
    snprintf(resp, sizeof resp,
             "{\"status\":\"OK\",\"filename\":\"%s\",\"size\":%ld}",
             filename, fsz);
    proto_send_msg(fd, resp);

    /* Then send file binary */
    proto_send_bin(fd, data, (uint32_t)fsz);
    free(data);

    mq_notify_download(username, filename);
    shm_session_touch(username);
}

static void handle_delete(int fd, const char *req,
                           const char *username, const char *role)
{
    char filename[MAX_NAME] = {0};
    if (!json_get_str(req, "filename", filename, MAX_NAME)) {
        SEND_ERR(fd, "Missing filename"); return;
    }
    char owner[MAX_NAME] = {0};
    meta_owner(filename, owner);

    /* Role-Based Authorization */
    if (strcmp(role, ROLE_ADMIN) == 0) {
        /* admin: can delete anything */
    } else if (strcmp(role, ROLE_USER) == 0) {
        if (strcmp(owner, username) != 0) {
            SEND_DENIED(fd, "You can only delete your own files"); return;
        }
    } else {
        SEND_DENIED(fd, "Guests cannot delete files"); return;
    }

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, filename);
    struct stat st;
    if (stat(path, &st) != 0) { SEND_NOTFOUND(fd, filename); return; }

    if (!lm_write_lock(filename, username)) {
        SEND_LOCKED(fd, filename); return;
    }
    remove(path);
    lm_write_unlock(filename);

    meta_remove(filename);
    mq_notify_delete(username, filename);
    shm_session_touch(username);
    SEND_OK(fd, "\"message\":\"'%s' deleted\"", filename);
}

static void handle_list(int fd, const char *username, const char *role)
{
    if (!auth_has_permission(role, "list")) {
        SEND_DENIED(fd, "No permission"); return;
    }

    /* Build enriched file list with lock status ── Real-Time Lock Indicator */
    char files_json[MAX_MSG_SIZE - 64];
    meta_list_json(files_json, sizeof files_json);

    /* We need to inject "lock_holder" into each file entry.
     * Walk the JSON array and insert lock info per file. */
    char enriched[MAX_MSG_SIZE - 32];
    int  out = 0;
    char *p  = files_json;

    /* Copy leading '[' */
    if (*p == '[') { enriched[out++] = *p++; }

    int first_entry = 1;
    while ((p = strstr(p, "{\"filename\"")) != NULL) {
        char fn[MAX_NAME] = {0};
        json_get_str(p, "filename", fn, MAX_NAME);

        /* Find end of this JSON object */
        char *obj_end = strchr(p, '}');
        if (!obj_end) break;

        /* Copy everything up to the closing '}' */
        if (!first_entry) {
            if (out < (int)sizeof enriched - 2)
                enriched[out++] = ',';
        }
        first_entry = 0;
        int obj_len = (int)(obj_end - p);
        if (out + obj_len < (int)sizeof enriched - 64) {
            memcpy(enriched + out, p, obj_len);
            out += obj_len;
        }

        /* Ask lock_manager who holds this file (if anyone) */
        char holder[MAX_NAME] = {0};
        int locked = lm_get_lock_holder(fn, holder, sizeof holder);
        if (locked && holder[0]) {
            out += snprintf(enriched + out, sizeof enriched - out,
                            ",\"lock_holder\":\"%s\"", holder);
        } else {
            out += snprintf(enriched + out, sizeof enriched - out,
                            ",\"lock_holder\":\"\"");
        }
        enriched[out++] = '}';
        p = obj_end + 1;
    }
    enriched[out++] = ']';
    enriched[out]   = '\0';

    char resp[MAX_MSG_SIZE];
    snprintf(resp, sizeof resp,
             "{\"status\":\"OK\",\"files\":%s}", enriched);
    proto_send_msg(fd, resp);
    shm_session_touch(username);
}

static void handle_search(int fd, const char *req,
                           const char *username, const char *role)
{
    if (!auth_has_permission(role, "search")) {
        SEND_DENIED(fd, "No permission"); return;
    }
    char query[MAX_NAME] = {0};
    json_get_str(req, "query", query, MAX_NAME);
    char results_json[MAX_MSG_SIZE - 64];
    meta_search_json(query, results_json, sizeof results_json);
    char resp[MAX_MSG_SIZE];
    snprintf(resp, sizeof resp,
             "{\"status\":\"OK\",\"results\":%s}", results_json);
    proto_send_msg(fd, resp);
    shm_session_touch(username);
}

/* ── SHARE command handler ───────────────────────────────────────────────── */
/*
 * Protocol: {"cmd":"SHARE","filename":"x","grantee":"alice","perm":"read|write|revoke"}
 * Rules:
 *   - Only the file owner or an admin may share/revoke.
 *   - "read"   → grants read-only access.
 *   - "write"  → grants read+write access (can overwrite, not delete).
 *   - "revoke" → removes all share entries for that grantee on that file.
 */
static void handle_share(int fd, const char *req,
                          const char *username, const char *role)
{
    char filename[MAX_NAME] = {0};
    char grantee[MAX_NAME]  = {0};
    char perm_str[16]       = {0};

    if (!json_get_str(req, "filename", filename, MAX_NAME) ||
        !json_get_str(req, "grantee",  grantee,  MAX_NAME) ||
        !json_get_str(req, "perm",     perm_str, sizeof perm_str)) {
        SEND_ERR(fd, "Usage: share <file> <user> <read|write|revoke>"); return;
    }

    /* Verify file exists */
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, filename);
    struct stat st;
    if (stat(path, &st) != 0) { SEND_NOTFOUND(fd, filename); return; }

    /* Only owner or admin may share */
    char owner[MAX_NAME] = {0};
    meta_owner(filename, owner);
    if (strcmp(role, ROLE_ADMIN) != 0 && strcmp(owner, username) != 0) {
        SEND_DENIED(fd, "Only the file owner or an admin can share this file");
        return;
    }

    /* Cannot share with yourself */
    if (strcmp(grantee, username) == 0) {
        SEND_ERR(fd, "Cannot share a file with yourself"); return;
    }

    int new_perm = 0;
    int revoke   = 0;
    if      (strcmp(perm_str, "read")   == 0) new_perm = SHARE_PERM_READ;
    else if (strcmp(perm_str, "write")  == 0) new_perm = SHARE_PERM_READ | SHARE_PERM_WRITE;
    else if (strcmp(perm_str, "revoke") == 0) revoke   = 1;
    else { SEND_ERR(fd, "perm must be 'read', 'write', or 'revoke'"); return; }

    pthread_mutex_lock(&g_share_mutex);

    if (revoke) {
        /* Remove all entries for this file+grantee pair */
        int w = 0;
        for (int i = 0; i < g_share_count; i++) {
            if (strcmp(g_shares[i].filename, filename) == 0 &&
                strcmp(g_shares[i].grantee,  grantee)  == 0) {
                continue;   /* drop this entry */
            }
            g_shares[w++] = g_shares[i];
        }
        g_share_count = w;
        shares_save();
        pthread_mutex_unlock(&g_share_mutex);
        SEND_OK(fd, "\"message\":\"Access to '%s' revoked from '%s'\"",
                filename, grantee);
        return;
    }

    /* Upsert: update existing entry or create new one */
    int found = 0;
    for (int i = 0; i < g_share_count; i++) {
        if (strcmp(g_shares[i].filename, filename) == 0 &&
            strcmp(g_shares[i].grantee,  grantee)  == 0) {
            g_shares[i].perm = new_perm;
            strncpy(g_shares[i].owner, username, MAX_NAME - 1);
            found = 1;
            break;
        }
    }
    if (!found) {
        if (g_share_count >= MAX_SHARES) {
            pthread_mutex_unlock(&g_share_mutex);
            SEND_ERR(fd, "Share table full"); return;
        }
        ShareEntry *e = &g_shares[g_share_count++];
        strncpy(e->filename, filename, MAX_NAME - 1);
        strncpy(e->owner,    username, MAX_NAME - 1);
        strncpy(e->grantee,  grantee,  MAX_NAME - 1);
        e->perm = new_perm;
    }
    shares_save();
    pthread_mutex_unlock(&g_share_mutex);

    const char *perm_label = (new_perm & SHARE_PERM_WRITE) ? "read+write" : "read-only";
    SEND_OK(fd, "\"message\":\"'%s' shared with '%s' (%s)\"",
            filename, grantee, perm_label);
}

/* ── SHARES command: list shares for a file ──────────────────────────────── */
static void handle_listshares(int fd, const char *req, const char *username,
                               const char *role)
{
    char filename[MAX_NAME] = {0};
    json_get_str(req, "filename", filename, MAX_NAME);

    /* Admins see all; owners see their file's shares; others see shares on them */
    pthread_mutex_lock(&g_share_mutex);
    char buf[MAX_MSG_SIZE - 128];
    int  off = snprintf(buf, sizeof buf, "[");
    int  first = 1;

    for (int i = 0; i < g_share_count; i++) {
        ShareEntry *e = &g_shares[i];
        int show = 0;
        if (strcmp(role, ROLE_ADMIN) == 0)                         show = 1;
        else if (filename[0] && strcmp(e->filename, filename) == 0
                 && strcmp(e->owner, username) == 0)               show = 1;
        else if (!filename[0] && strcmp(e->owner, username) == 0)  show = 1;
        else if (!filename[0] && strcmp(e->grantee, username) == 0) show = 1;

        if (!show) continue;
        if (!first) off += snprintf(buf+off, sizeof buf-off, ",");
        first = 0;
        const char *pl = (e->perm & SHARE_PERM_WRITE) ? "write" : "read";
        off += snprintf(buf+off, sizeof buf-off,
                        "{\"filename\":\"%s\",\"owner\":\"%s\","
                        "\"grantee\":\"%s\",\"perm\":\"%s\"}",
                        e->filename, e->owner, e->grantee, pl);
    }
    snprintf(buf+off, sizeof buf-off, "]");
    pthread_mutex_unlock(&g_share_mutex);

    char resp[MAX_MSG_SIZE];
    snprintf(resp, sizeof resp, "{\"status\":\"OK\",\"shares\":%s}", buf);
    proto_send_msg(fd, resp);
    (void)username;
}

static void handle_adduser(int fd, const char *req,
                            const char *username, const char *role)
{
    if (!auth_has_permission(role, "adduser")) {
        SEND_DENIED(fd, "Only admins can add users"); return;
    }
    char user[MAX_NAME]={0}, pass[MAX_PASS]={0}, r[MAX_ROLE]={0};
    json_get_str(req, "username", user, MAX_NAME);
    json_get_str(req, "password", pass, MAX_PASS);
    json_get_str(req, "role",     r,    MAX_ROLE);
    char errmsg[128] = {0};
    if (auth_add_user(user, pass, r, errmsg))
        SEND_OK(fd, "\"message\":\"User '%s' (%s) created\"", user, r);
    else
        SEND_ERR(fd, errmsg);
    (void)username;
}

static void handle_deluser(int fd, const char *req,
                            const char *username, const char *role)
{
    if (!auth_has_permission(role, "deluser")) {
        SEND_DENIED(fd, "Only admins can delete users"); return;
    }
    char user[MAX_NAME] = {0};
    json_get_str(req, "username", user, MAX_NAME);
    char errmsg[128] = {0};
    if (auth_delete_user(user, errmsg))
        SEND_OK(fd, "\"message\":\"User '%s' deleted\"", user);
    else
        SEND_ERR(fd, errmsg);
    (void)username;
}

static void handle_listusers(int fd, const char *role)
{
    if (!auth_has_permission(role, "listusers")) {
        SEND_DENIED(fd, "Only admins can list users"); return;
    }
    char users_json[MAX_MSG_SIZE - 64];
    auth_list_users(users_json, sizeof users_json);
    char resp[MAX_MSG_SIZE];
    snprintf(resp, sizeof resp,
             "{\"status\":\"OK\",\"users\":%s}", users_json);
    proto_send_msg(fd, resp);
}

static void handle_logs(int fd, const char *req, const char *role)
{
    if (!auth_has_permission(role, "logs")) {
        SEND_DENIED(fd, "Only admins can view logs"); return;
    }
    long n = 50;
    json_get_int(req, "n", &n);

    FILE *lf = fopen(LOGS_FILE, "r");
    char lines_json[MAX_MSG_SIZE - 128];
    int off = snprintf(lines_json, sizeof lines_json, "[");
    if (lf) {
        /* Buffer all lines, keep last n */
        char **lbuf = calloc((int)n, sizeof(char*));
        char line[256]; int cnt = 0, idx = 0;
        while (fgets(line, sizeof line, lf)) {
            if (lbuf[idx]) free(lbuf[idx]);
            lbuf[idx] = strdup(line);
            idx = (idx + 1) % (int)n;
            cnt++;
        }
        fclose(lf);
        int start = (cnt >= (int)n) ? idx : 0;
        int total = (cnt >= (int)n) ? (int)n : cnt;
        int first = 1;
        for (int i = 0; i < total; i++) {
            int k = (start + i) % (int)n;
            if (!lbuf[k]) continue;
            char esc[512]; json_escape(lbuf[k], esc, sizeof esc);
            /* strip trailing newline from escaped string */
            int el = strlen(esc);
            while (el > 0 && (esc[el-1]=='n' && el>1 && esc[el-2]=='\\')) {
                esc[el-2] = '\0'; el -= 2;
            }
            if (!first) off += snprintf(lines_json+off, sizeof lines_json-off, ",");
            off += snprintf(lines_json+off, sizeof lines_json-off,
                            "\"%s\"", esc);
            first = 0;
            free(lbuf[k]);
        }
        free(lbuf);
    }
    snprintf(lines_json+off, sizeof lines_json-off, "]");
    char resp[MAX_MSG_SIZE];
    snprintf(resp, sizeof resp,
             "{\"status\":\"OK\",\"lines\":%s}", lines_json);
    proto_send_msg(fd, resp);
}

static void handle_sessions(int fd, const char *role)
{
    if (!auth_has_permission(role, "logs")) {
        SEND_DENIED(fd, "Only admins can view sessions"); return;
    }
    char sess_json[MAX_MSG_SIZE - 64];
    shm_session_list_json(sess_json, sizeof sess_json);
    char resp[MAX_MSG_SIZE];
    snprintf(resp, sizeof resp,
             "{\"status\":\"OK\",\"sessions\":%s}", sess_json);
    proto_send_msg(fd, resp);
}

/* ── Per-client thread ────────────────────────────────────────────────────── */
static void *client_thread(void *arg)
{
    ClientCtx *ctx = (ClientCtx *)arg;
    int fd = ctx->fd;
    char addr[32]; strncpy(addr, ctx->addr, 31); free(ctx);

    char username[MAX_NAME] = {0};
    char role[MAX_ROLE]     = {0};
    char req[MAX_MSG_SIZE];

    pthread_mutex_lock(&console_mutex);
    printf("[SERVER] (+) %s connected  [online=%d]\n",
           addr, shm_session_count() + 1);
    pthread_mutex_unlock(&console_mutex);

    while (1) {
        int r = proto_recv_msg(fd, req, sizeof req);
        if (r <= 0) break;

        char cmd[32] = {0};
        json_get_str(req, "cmd", cmd, sizeof cmd);

        if      (strcmp(cmd, CMD_LOGIN)     == 0) handle_login(fd, req, username, role);
        else if (strcmp(cmd, CMD_LOGOUT)    == 0) handle_logout(fd, username, role);
        else if (strcmp(cmd, CMD_UPLOAD)    == 0) handle_upload(fd, req, username, role);
        else if (strcmp(cmd, CMD_DOWNLOAD)  == 0) handle_download(fd, req, username, role);
        else if (strcmp(cmd, CMD_DELETE)    == 0) handle_delete(fd, req, username, role);
        else if (strcmp(cmd, CMD_LIST)      == 0) handle_list(fd, username, role);
        else if (strcmp(cmd, CMD_SEARCH)    == 0) handle_search(fd, req, username, role);
        else if (strcmp(cmd, "SHARE")       == 0) handle_share(fd, req, username, role);
        else if (strcmp(cmd, "LISTSHARES")  == 0) handle_listshares(fd, req, username, role);
        else if (strcmp(cmd, CMD_ADDUSER)   == 0) handle_adduser(fd, req, username, role);
        else if (strcmp(cmd, CMD_DELUSER)   == 0) handle_deluser(fd, req, username, role);
        else if (strcmp(cmd, CMD_LISTUSERS) == 0) handle_listusers(fd, role);
        else if (strcmp(cmd, CMD_LOGS)      == 0) handle_logs(fd, req, role);
        else if (strcmp(cmd, "SESSIONS")    == 0) handle_sessions(fd, role);
        else SEND_ERR(fd, "Unknown command");

        /* Add session to shared memory after successful login */
        if (strcmp(cmd, CMD_LOGIN) == 0 && username[0])
            shm_session_add(username, role, addr);
    }

    if (username[0]) {
        shm_session_remove(username);
        mq_notify_logout(username);
    }
    close(fd);
    sem_post(&connection_sem);   /* release semaphore slot */

    pthread_mutex_lock(&console_mutex);
    printf("[SERVER] (-) %s disconnected\n", addr);
    pthread_mutex_unlock(&console_mutex);
    return NULL;
}

/* ── Signal handler for clean shutdown ───────────────────────────────────── */
static volatile int g_running = 1;
static void on_sigint(int sig) { (void)sig; g_running = 0; }

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGINT,  on_sigint);
    signal(SIGPIPE, SIG_IGN);   /* ignore broken pipe */

    /* Create storage directories */
    system("mkdir -p " STORAGE_DIR);
    system("mkdir -p " TRASH_DIR);
    system("mkdir -p storage/metadata");
    system("mkdir -p storage/logs");

    /* Load persistent share table */
    shares_load();

    /* Initialise subsystems */
    auth_bootstrap();
    lm_init();
    if (shm_session_init() < 0)
        fprintf(stderr, "[WARN] Shared memory init failed (continuing)\n");
    if (mq_event_init() < 0)
        fprintf(stderr, "[WARN] Message queue init failed (continuing)\n");

    /* Start IPC-MQ consumer thread */
    pthread_t mq_thread;
    pthread_create(&mq_thread, NULL, mq_event_consumer, NULL);
    pthread_detach(mq_thread);

    /* Semaphore limiting concurrent clients */
    sem_init(&connection_sem, 0, MAX_CLIENTS);

    /* TCP socket setup */
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    if (listen(srv_fd, MAX_CLIENTS) < 0) {
        perror("listen"); return 1;
    }

    printf("[SERVER] Mini Cloud Drive listening on port %d\n", SERVER_PORT);
    printf("[SERVER] Max concurrent clients: %d\n\n", MAX_CLIENTS);

    while (g_running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof cli_addr;
        int cli_fd = accept(srv_fd,
                            (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) continue;

        sem_wait(&connection_sem);   /* block if at max capacity */

        ClientCtx *ctx = malloc(sizeof *ctx);
        ctx->fd = cli_fd;
        snprintf(ctx->addr, sizeof ctx->addr, "%s:%d",
                 inet_ntoa(cli_addr.sin_addr),
                 ntohs(cli_addr.sin_port));

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, client_thread, ctx);
        pthread_attr_destroy(&attr);
    }

    printf("\n[SERVER] Shutting down...\n");
    close(srv_fd);
    sem_destroy(&connection_sem);
    shm_session_cleanup();
    mq_event_cleanup();
    return 0;
}