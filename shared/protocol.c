/*
 * shared/protocol.c
 * -----------------
 * Wire-protocol helpers and a minimal JSON key-value extractor.
 * No external libraries required.
 */

#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ── Internal: read exactly n bytes ──────────────────────────────────────── */
static int read_exact(int fd, void *buf, int n)
{
    int total = 0;
    char *p   = (char *)buf;
    while (total < n) {
        int r = read(fd, p + total, n - total);
        if (r <= 0) return -1;
        total += r;
    }
    return total;
}

/* ── Send a length-prefixed JSON string ──────────────────────────────────── */
int proto_send_msg(int fd, const char *json)
{
    uint32_t len   = (uint32_t)strlen(json);
    uint32_t nlen  = htonl(len);
    if (write(fd, &nlen, 4) != 4)         return -1;
    if ((int)write(fd, json, len) != (int)len) return -1;
    return 0;
}

/* ── Receive a length-prefixed JSON string ───────────────────────────────── */
int proto_recv_msg(int fd, char *buf, int bufsz)
{
    uint32_t nlen;
    if (read_exact(fd, &nlen, 4) < 0) return -1;
    uint32_t len = ntohl(nlen);
    if ((int)len >= bufsz) return -1;
    if (read_exact(fd, buf, (int)len) < 0) return -1;
    buf[len] = '\0';
    return (int)len;
}

/* ── Send raw binary with 4-byte length prefix ───────────────────────────── */
int proto_send_bin(int fd, const unsigned char *data, uint32_t len)
{
    uint32_t nlen = htonl(len);
    if (write(fd, &nlen, 4) != 4) return -1;
    uint32_t sent = 0;
    while (sent < len) {
        int w = write(fd, data + sent, len - sent);
        if (w <= 0) return -1;
        sent += w;
    }
    return 0;
}

/* ── Receive raw binary ──────────────────────────────────────────────────── */
int proto_recv_bin(int fd, unsigned char **out, uint32_t *out_len)
{
    uint32_t nlen;
    if (read_exact(fd, &nlen, 4) < 0) return -1;
    uint32_t len = ntohl(nlen);
    if (len == 0 || len > MAX_FILE_SIZE) return -1;
    *out = malloc(len);
    if (!*out) return -1;
    if (read_exact(fd, *out, (int)len) < 0) { free(*out); return -1; }
    *out_len = len;
    return 0;
}

/* ── Minimal JSON string field extractor ─────────────────────────────────── */
/*
 * Finds  "key":"value"  inside a flat JSON string.
 * Returns 1 on success, 0 if not found.
 * Works for simple string values (no nested objects/arrays).
 */
int json_get_str(const char *json, const char *key, char *out, int outsz)
{
    char needle[MAX_NAME + 4];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++; /* skip opening quote */
    int i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\') { p++; if (!*p) break; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

/* ── Minimal JSON integer field extractor ────────────────────────────────── */
int json_get_int(const char *json, const char *key, long *out)
{
    char needle[MAX_NAME + 4];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p < '0' || *p > '9') return 0;
    *out = strtol(p, NULL, 10);
    return 1;
}

/* ── Escape special chars for JSON strings ───────────────────────────────── */
void json_escape(const char *in, char *out, int outsz)
{
    int j = 0;
    for (int i = 0; in[i] && j < outsz - 2; i++) {
        if (in[i] == '"' || in[i] == '\\') out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = '\0';
}
