/*
 * client/client.c
 * ---------------
 * Interactive command-line client for Mini Cloud Drive (FULL VERSION)
 *
 * Usage:
 *   ./client [host] [port]
 *
 * OS Concepts (client side)
 * -------------------------
 *   Socket Programming – TCP client, connect / send / recv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../shared/protocol.h"

#define DOWNLOAD_DIR "downloads"

static int g_fd = -1;
static char g_user[MAX_NAME] = {0};
static char g_role[MAX_ROLE] = {0};

/* ── Connect ─────────────────────────────────────────────────────────────── */
static int do_connect(const char *host, int port)
{
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &srv.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return -1;
    }
    if (connect(g_fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
        perror("connect"); return -1;
    }
    return 0;
}

/* ── Simple request/response ─────────────────────────────────────────────── */
static int send_recv(const char *req, char *resp, int rsz)
{
    if (proto_send_msg(g_fd, req) < 0) return -1;
    return proto_recv_msg(g_fd, resp, rsz);
}

/* ── Print status line ────────────────────────────────────────────────────── */
static void print_status(const char *resp)
{
    char status[32] = {0}, msg[512] = {0};
    json_get_str(resp, "status",  status, sizeof status);
    json_get_str(resp, "message", msg,    sizeof msg);
    if (strcmp(status, STATUS_OK) == 0)
        printf("  \033[32m✓\033[0m  %s\n", msg[0] ? msg : "OK");
    else
        printf("  \033[31m✗\033[0m  [%s] %s\n", status, msg);
}

/* ── Commands ────────────────────────────────────────────────────────────── */
static void cmd_login(char *args)
{
    char user[MAX_NAME]={0}, pass[MAX_PASS]={0};
    if (sscanf(args, "%127s %127s", user, pass) < 2) {
        printf("  Usage: login <username> <password>\n"); return;
    }
    char req[512], resp[MAX_MSG_SIZE];
    snprintf(req, sizeof req,
             "{\"cmd\":\"LOGIN\",\"username\":\"%s\",\"password\":\"%s\"}",
             user, pass);
    if (send_recv(req, resp, sizeof resp) < 0) { printf("  Connection error\n"); return; }

    char status[32]={0}, role[MAX_ROLE]={0};
    json_get_str(resp, "status", status, sizeof status);
    json_get_str(resp, "role",   role,   sizeof role);
    if (strcmp(status, STATUS_OK) == 0) {
        strncpy(g_user, user, MAX_NAME-1);
        g_user[MAX_NAME-1] = '\0';
        strncpy(g_role, role, MAX_ROLE-1);
        g_role[MAX_ROLE-1] = '\0';
    }
    print_status(resp);
}

static void cmd_logout(void)
{
    char resp[512];
    send_recv("{\"cmd\":\"LOGOUT\"}", resp, sizeof resp);
    print_status(resp);
    g_user[0] = g_role[0] = '\0';
}

static void cmd_upload(char *args)
{
    char local[MAX_PATH]={0}, remote[MAX_NAME]={0};
    int n = sscanf(args, "%511s %127s", local, remote);
    if (n < 1) { printf("  Usage: upload <local_path> [remote_name]\n"); return; }
    if (n < 2) {
        char *slash = strrchr(local, '/');
        strncpy(remote, slash ? slash+1 : local, MAX_NAME-1);
        remote[MAX_NAME-1] = '\0';
    }

    struct stat st;
    if (stat(local, &st) != 0) { printf("  File not found: %s\n", local); return; }
    long size = st.st_size;

    /* Phase 1: send metadata */
    char req[512], resp[MAX_MSG_SIZE];
    snprintf(req, sizeof req,
             "{\"cmd\":\"UPLOAD\",\"filename\":\"%s\",\"size\":%ld}",
             remote, size);
    if (proto_send_msg(g_fd, req) < 0) { printf("  Send error\n"); return; }
    if (proto_recv_msg(g_fd, resp, sizeof resp) < 0) { printf("  Recv error\n"); return; }

    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) {
        print_status(resp);
        return;
    }

    /* Phase 2: send file bytes */
    FILE *f = fopen(local, "rb");
    if (!f) { printf("  Cannot open %s\n", local); return; }
    unsigned char *buf = malloc(size);
    size_t bytes_read = fread(buf, 1, size, f);
    fclose(f);
    if (bytes_read != (size_t)size) {
        printf("  Error reading file\n");
        free(buf);
        return;
    }
    proto_send_bin(g_fd, buf, (uint32_t)size);
    free(buf);

    /* Phase 3: receive final response */
    if (proto_recv_msg(g_fd, resp, sizeof resp) < 0) { printf("  Recv error\n"); return; }
    print_status(resp);
}

static void cmd_download(char *args)
{
    char remote[MAX_NAME]={0}, local[MAX_PATH]={0};
    int n = sscanf(args, "%127s %511s", remote, local);
    if (n < 1) { printf("  Usage: download <remote_name> [local_path]\n"); return; }
    if (n < 2) snprintf(local, sizeof local, "%s/%s", DOWNLOAD_DIR, remote);

    char req[512], resp[MAX_MSG_SIZE];
    snprintf(req, sizeof req,
             "{\"cmd\":\"DOWNLOAD\",\"filename\":\"%s\"}", remote);
    if (proto_send_msg(g_fd, req) < 0) { printf("  Send error\n"); return; }
    if (proto_recv_msg(g_fd, resp, sizeof resp) < 0) { printf("  Recv error\n"); return; }

    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    long size = 0;
    json_get_int(resp, "size", &size);

    unsigned char *data = NULL; uint32_t recv_len = 0;
    if (proto_recv_bin(g_fd, &data, &recv_len) < 0) {
        printf("  Failed to receive file data\n"); return;
    }

    mkdir(DOWNLOAD_DIR, 0755);
    FILE *f = fopen(local, "wb");
    if (!f) { printf("  Cannot write %s\n", local); free(data); return; }
    fwrite(data, 1, recv_len, f);
    fclose(f);
    free(data);

    printf("  \033[32m✓\033[0m  Saved %ld bytes to '%s'\n", (long)recv_len, local);
}

static void cmd_delete(char *args)
{
    char filename[MAX_NAME] = {0};
    sscanf(args, "%127s", filename);
    char req[256], resp[512];
    snprintf(req, sizeof req,
             "{\"cmd\":\"DELETE\",\"filename\":\"%s\"}", filename);
    send_recv(req, resp, sizeof resp);
    print_status(resp);
}

static void cmd_trash(void)
{
    char resp[MAX_MSG_SIZE];
    if (send_recv("{\"cmd\":\"TRASH\"}", resp, sizeof resp) < 0) {
        printf("  Connection error\n");
        return;
    }
    char status[32] = {0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) {
        print_status(resp);
        return;
    }
    printf("\n  Files in Trash:\n");
    char *files = strstr(resp, "\"files\":");
    if (files) {
        files += 8;
        printf("  %s\n", files);
    }
}

static void cmd_restore(char *args)
{
    char filename[MAX_NAME] = {0};
    sscanf(args, "%127s", filename);
    if (strlen(filename) == 0) {
        printf("  Usage: restore <filename>\n");
        return;
    }
    char req[256], resp[MAX_MSG_SIZE];
    snprintf(req, sizeof req,
             "{\"cmd\":\"RESTORE\",\"filename\":\"%s\"}", filename);
    if (send_recv(req, resp, sizeof resp) < 0) {
        printf("  Connection error\n");
        return;
    }
    print_status(resp);
}

static void cmd_list(void)
{
    char resp[MAX_MSG_SIZE];
    send_recv("{\"cmd\":\"LIST\"}", resp, sizeof resp);
    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    char *files = strstr(resp, "\"files\":");
    if (!files) { printf("  (no files)\n"); return; }
    files += 8;

    printf("\n  %-30s %-12s %8s %5s  %-22s %s\n",
           "Filename", "Owner", "Size", "Ver", "Modified", "Status");
    printf("  %s\n",
           "────────────────────────────────────────────────────────────────────────────────");

    char *p = files;
    while ((p = strstr(p, "{\"filename\"")) != NULL) {
        char fn[MAX_NAME]={0}, owner[MAX_NAME]={0}, mod[64]={0}, holder[MAX_NAME]={0};
        long size=0, ver=0;
        json_get_str(p, "filename",    fn,     MAX_NAME);
        json_get_str(p, "owner",       owner,  MAX_NAME);
        json_get_str(p, "modified",    mod,    64);
        json_get_str(p, "lock_holder", holder, MAX_NAME);
        json_get_int(p, "size",    &size);
        json_get_int(p, "version", &ver);

        /* Format lock status indicator */
        char lock_indicator[48] = {0};
        if (holder[0] != '\0') {
            snprintf(lock_indicator, sizeof lock_indicator,
                     "\033[33m⚿ LOCKED by %s\033[0m", holder);
        } else {
            snprintf(lock_indicator, sizeof lock_indicator,
                     "\033[32m✓ AVAILABLE\033[0m");
        }

        printf("  %-30s %-12s %8ld %5ld  %-22s %s\n",
               fn, owner, size, ver, mod, lock_indicator);
        p++;
    }
    printf("\n");
}

static void cmd_search(char *args)
{
    char query[MAX_NAME] = {0};
    sscanf(args, "%127s", query);
    char req[256], resp[MAX_MSG_SIZE];
    snprintf(req, sizeof req,
             "{\"cmd\":\"SEARCH\",\"query\":\"%s\"}", query);
    send_recv(req, resp, sizeof resp);
    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    char *res = strstr(resp, "\"results\":");
    if (!res) { printf("  No results\n"); return; }
    res += 10;
    printf("\n  Search results:\n");
    char *p = res;
    int cnt = 0;
    while ((p = strstr(p, "{\"filename\"")) != NULL) {
        char fn[MAX_NAME]={0}, owner[MAX_NAME]={0};
        long size=0, ver=0;
        json_get_str(p, "filename", fn,    MAX_NAME);
        json_get_str(p, "owner",    owner, MAX_NAME);
        json_get_int(p, "size",    &size);
        json_get_int(p, "version", &ver);
        printf("  • %-28s  owner=%-10s  size=%-8ld  v%ld\n",
               fn, owner, size, ver);
        cnt++; p++;
    }
    if (!cnt) printf("  (no matches)\n");
    printf("\n");
}

static void cmd_adduser(char *args)
{
    char user[MAX_NAME]={0}, pass[MAX_PASS]={0}, role[MAX_ROLE]="user";
    if (sscanf(args, "%127s %127s %15s", user, pass, role) < 2) {
        printf("  Usage: adduser <username> <password> [role]\n"); return;
    }
    char req[512], resp[512];
    snprintf(req, sizeof req,
             "{\"cmd\":\"ADDUSER\",\"username\":\"%s\","
             "\"password\":\"%s\",\"role\":\"%s\"}", user, pass, role);
    send_recv(req, resp, sizeof resp);
    print_status(resp);
}

static void cmd_deluser(char *args)
{
    char user[MAX_NAME]={0};
    sscanf(args, "%127s", user);
    char req[256], resp[512];
    snprintf(req, sizeof req,
             "{\"cmd\":\"DELUSER\",\"username\":\"%s\"}", user);
    send_recv(req, resp, sizeof resp);
    print_status(resp);
}

static void cmd_users(void)
{
    char resp[MAX_MSG_SIZE];
    send_recv("{\"cmd\":\"LISTUSERS\"}", resp, sizeof resp);
    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    printf("\n  %-20s %s\n", "Username", "Role");
    printf("  ──────────────────────────────\n");
    char *p = strstr(resp, "\"users\":");
    if (!p) { printf("  (none)\n\n"); return; }
    p += 8;
    while ((p = strstr(p, "{\"username\"")) != NULL) {
        char u[MAX_NAME]={0}, r[MAX_ROLE]={0};
        json_get_str(p, "username", u, MAX_NAME);
        json_get_str(p, "role",     r, MAX_ROLE);
        printf("  %-20s %s\n", u, r);
        p++;
    }
    printf("\n");
}

static void cmd_logs(char *args)
{
    long n = 30;
    sscanf(args, "%ld", &n);
    if (n <= 0) n = 30;
    if (n > 200) n = 200;
    char req[128], resp[MAX_MSG_SIZE];
    snprintf(req, sizeof req, "{\"cmd\":\"LOGS\",\"n\":%ld}", n);
    send_recv(req, resp, sizeof resp);
    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    printf("\n  Activity Log (last %ld entries):\n", n);
    printf("  ──────────────────────────────────────────────────\n");
    char *p = strstr(resp, "\"lines\":");
    if (!p) { printf("  (empty)\n\n"); return; }
    p += 8;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        char *end = strchr(p, '"');
        if (!end) break;
        *end = '\0';
        printf("  %s\n", p);
        *end = '"';
        p = end + 1;
        if (*p == ']') break;
    }
    printf("\n");
}

static void cmd_share(char *args)
{
    char filename[MAX_NAME]={0}, grantee[MAX_NAME]={0}, perm[16]={0};
    if (sscanf(args, "%127s %127s %15s", filename, grantee, perm) < 3) {
        printf("  Usage: share <filename> <user> <read|write|revoke>\n");
        return;
    }
    char req[512], resp[512];
    snprintf(req, sizeof req,
             "{\"cmd\":\"SHARE\",\"filename\":\"%s\","
             "\"grantee\":\"%s\",\"perm\":\"%s\"}", filename, grantee, perm);
    send_recv(req, resp, sizeof resp);
    print_status(resp);
}

static void cmd_myshares(char *args)
{
    char filename[MAX_NAME] = {0};
    sscanf(args, "%127s", filename);          /* optional filter by filename */
    char req[256], resp[MAX_MSG_SIZE];
    if (filename[0])
        snprintf(req, sizeof req,
                 "{\"cmd\":\"LISTSHARES\",\"filename\":\"%s\"}", filename);
    else
        snprintf(req, sizeof req, "{\"cmd\":\"LISTSHARES\"}");

    send_recv(req, resp, sizeof resp);
    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    printf("\n  %-28s %-14s %-14s %s\n", "File", "Owner", "Shared With", "Permission");
    printf("  ──────────────────────────────────────────────────────────────────\n");
    char *p = strstr(resp, "\"shares\":");
    if (!p) { printf("  (none)\n\n"); return; }
    p += 9;
    int cnt = 0;
    while ((p = strstr(p, "{\"filename\"")) != NULL) {
        char fn[MAX_NAME]={0}, owner[MAX_NAME]={0}, grantee[MAX_NAME]={0}, pm[16]={0};
        json_get_str(p, "filename", fn,      MAX_NAME);
        json_get_str(p, "owner",    owner,   MAX_NAME);
        json_get_str(p, "grantee",  grantee, MAX_NAME);
        json_get_str(p, "perm",     pm,      16);
        const char *color = strcmp(pm, "write") == 0 ? "\033[33m" : "\033[36m";
        printf("  %-28s %-14s %-14s %s%s\033[0m\n",
               fn, owner, grantee, color, pm);
        cnt++; p++;
    }
    if (!cnt) printf("  (no shares found)\n");
    printf("\n");
}

static void cmd_sessions(void)
{
    char resp[MAX_MSG_SIZE];
    send_recv("{\"cmd\":\"SESSIONS\"}", resp, sizeof resp);
    char status[32]={0};
    json_get_str(resp, "status", status, sizeof status);
    if (strcmp(status, STATUS_OK) != 0) { print_status(resp); return; }

    printf("\n  Active Sessions:\n");
    printf("  %-16s %-8s %-22s %s\n", "User", "Role", "Address", "Last Active");
    printf("  ──────────────────────────────────────────────────────────\n");
    char *p = strstr(resp, "\"sessions\":");
    if (!p) { printf("  (none)\n\n"); return; }
    p += 11;
    while ((p = strstr(p, "{\"username\"")) != NULL) {
        char u[MAX_NAME]={0}, r[MAX_ROLE]={0}, addr[32]={0}, la[32]={0};
        json_get_str(p, "username",    u,    MAX_NAME);
        json_get_str(p, "role",        r,    MAX_ROLE);
        json_get_str(p, "addr",        addr, 32);
        json_get_str(p, "last_active", la,   32);
        printf("  %-16s %-8s %-22s %s\n", u, r, addr, la);
        p++;
    }
    printf("\n");
}

/* ── REPL ────────────────────────────────────────────────────────────────── */
static void print_help(void)
{
    printf("\n"
"  Commands:\n"
"  ──────────────────────────────────────────────────────────────────────\n"
"  login  <user> <pass>              Log in\n"
"  logout                            Log out\n"
"  upload <local> [remote_name]      Upload a file\n"
"  download <remote> [local_path]    Download a file\n"
"  delete <filename>                 Delete a file (move to trash)\n"
"  trash                             List files in trash\n"
"  restore <filename>                Restore from trash\n"
"  list                              List files with real-time lock status\n"
"  search <query>                    Search files by name\n"
"  share <file> <user> <perm>        Share: perm = read | write | revoke\n"
"  myshares [filename]               Show shares you own or can access\n"
"  adduser <user> <pass> [role]      (admin) Add user\n"
"  deluser <username>                (admin) Delete user\n"
"  users                             (admin) List users\n"
"  logs [n]                          (admin) Show last n log entries\n"
"  sessions                          (admin) Show active sessions\n"
"  help                              Show this help\n"
"  exit                              Disconnect and quit\n"
"  ──────────────────────────────────────────────────────────────────────\n\n");
}

int main(int argc, char *argv[])
{
    const char *host = SERVER_HOST;
    int         port = SERVER_PORT;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    printf("\n"
"  ╔══════════════════════════════════════╗\n"
"  ║   Mini Cloud Drive  —  C Client      ║\n"
"  ╚══════════════════════════════════════╝\n\n");

    if (do_connect(host, port) < 0) {
        fprintf(stderr, "  Cannot connect to %s:%d\n", host, port);
        return 1;
    }
    printf("  Connected to %s:%d\n", host, port);
    printf("  Type 'help' for commands.\n\n");

    char line[1024];
    while (1) {
        if (g_user[0])
            printf("(%s:%s) > ", g_user, g_role);
        else
            printf("(not logged in) > ");
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;

        char cmd[32] = {0};
        char *args   = line;
        sscanf(line, "%31s", cmd);
        args = line + strlen(cmd);
        while (*args == ' ') args++;

        if      (strcmp(cmd, "exit")      == 0 ||
                 strcmp(cmd, "quit")      == 0) break;
        else if (strcmp(cmd, "help")      == 0) print_help();
        else if (strcmp(cmd, "login")     == 0) cmd_login(args);
        else if (strcmp(cmd, "logout")    == 0) cmd_logout();
        else if (strcmp(cmd, "upload")    == 0) cmd_upload(args);
        else if (strcmp(cmd, "download")  == 0) cmd_download(args);
        else if (strcmp(cmd, "delete")    == 0) cmd_delete(args);
        else if (strcmp(cmd, "trash")     == 0) cmd_trash();
        else if (strcmp(cmd, "restore")   == 0) cmd_restore(args);
        else if (strcmp(cmd, "list")      == 0) cmd_list();
        else if (strcmp(cmd, "search")    == 0) cmd_search(args);
        else if (strcmp(cmd, "share")     == 0) cmd_share(args);
        else if (strcmp(cmd, "myshares")  == 0) cmd_myshares(args);
        else if (strcmp(cmd, "adduser")   == 0) cmd_adduser(args);
        else if (strcmp(cmd, "deluser")   == 0) cmd_deluser(args);
        else if (strcmp(cmd, "users")     == 0) cmd_users();
        else if (strcmp(cmd, "logs")      == 0) cmd_logs(args);
        else if (strcmp(cmd, "sessions")  == 0) cmd_sessions();
        else printf("  Unknown command '%s'. Type 'help'.\n", cmd);
    }

    close(g_fd);
    printf("\n  Disconnected. Bye!\n\n");
    return 0;
}

