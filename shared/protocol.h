#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * shared/protocol.h
 * -----------------
 * Wire protocol, command codes, status codes, and helper macros
 * shared by server and client.
 *
 * Wire format (every message):
 *   [4-byte big-endian length][JSON payload string]
 * File data (upload/download):
 *   [4-byte big-endian length][raw bytes]
 */

#include <stdint.h>
#include <sys/types.h>

/* ── Port ────────────────────────────────────────────────────────────────── */
#define SERVER_PORT      7777
#define SERVER_HOST      "127.0.0.1"
#define MAX_CLIENTS      50
#define HEADER_SIZE      4          /* 4-byte length prefix */
#define MAX_MSG_SIZE     (8*1024)   /* 8 KB for control messages */
#define MAX_FILE_SIZE    (64*1024*1024) /* 64 MB max upload */
#define MAX_PATH         512
#define MAX_NAME         128
#define MAX_PASS         128
#define MAX_ROLE         16

/* ── Commands ────────────────────────────────────────────────────────────── */
#define CMD_LOGIN        "LOGIN"
#define CMD_LOGOUT       "LOGOUT"
#define CMD_UPLOAD       "UPLOAD"
#define CMD_DOWNLOAD     "DOWNLOAD"
#define CMD_DELETE       "DELETE"
#define CMD_LIST         "LIST"
#define CMD_SEARCH       "SEARCH"
#define CMD_ADDUSER      "ADDUSER"
#define CMD_DELUSER      "DELUSER"
#define CMD_LISTUSERS    "LISTUSERS"
#define CMD_LOGS         "LOGS"

/* ── Status codes ────────────────────────────────────────────────────────── */
#define STATUS_OK        "OK"
#define STATUS_ERROR     "ERROR"
#define STATUS_DENIED    "DENIED"
#define STATUS_LOCKED    "LOCKED"
#define STATUS_NOTFOUND  "NOT_FOUND"
#define STATUS_EXISTS    "EXISTS"

/* ── Roles ───────────────────────────────────────────────────────────────── */
#define ROLE_ADMIN       "admin"
#define ROLE_USER        "user"
#define ROLE_GUEST       "guest"

/* ── Wire helpers ────────────────────────────────────────────────────────── */
/* Write 4-byte big-endian length then body into buf.
   Returns total bytes written, or -1 on error. */
int  proto_send_msg(int fd, const char *json);
int  proto_recv_msg(int fd, char *buf, int bufsz);
int  proto_send_bin(int fd, const unsigned char *data, uint32_t len);
int  proto_recv_bin(int fd, unsigned char **out, uint32_t *out_len);

/* Simple JSON field helpers (no external lib needed) */
int  json_get_str(const char *json, const char *key, char *out, int outsz);
int  json_get_int(const char *json, const char *key, long *out);
void json_escape(const char *in, char *out, int outsz);

#endif /* PROTOCOL_H */
