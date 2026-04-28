/*
 * server/ipc_mq.c
 * ---------------
 * POSIX message queue event bus.
 * A background consumer thread drains the queue and writes to the activity log.
 *
 * OS Concepts demonstrated
 * ------------------------
 *   IPC (Message Queue) – mq_open / mq_send / mq_receive
 *   Concurrency Control – consumer runs in a dedicated pthread
 */

#include "ipc_mq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE "storage/logs/activity.log"

static mqd_t g_mqd = (mqd_t)-1;
static volatile int g_running = 1;

/* ── Init ─────────────────────────────────────────────────────────────────── */
int mq_event_init(void)
{
    mq_unlink(MQ_NAME); /* remove stale queue */

    struct mq_attr attr;
    attr.mq_flags   = 0;
    attr.mq_maxmsg  = MQ_MAXMSG;
    attr.mq_msgsize = MQ_MSGSIZE;
    attr.mq_curmsgs = 0;

    g_mqd = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0660, &attr);
    if (g_mqd == (mqd_t)-1) { perror("mq_open"); return -1; }

    printf("[IPC-MQ]  Message queue '%s' opened\n", MQ_NAME);
    return 0;
}

void mq_event_cleanup(void)
{
    g_running = 0;
    if (g_mqd != (mqd_t)-1) { mq_close(g_mqd); mq_unlink(MQ_NAME); }
}

/* ── Send (non-blocking) ──────────────────────────────────────────────────── */
void mq_event_send(const char *event_json)
{
    if (g_mqd == (mqd_t)-1) return;
    /* priority 0, non-blocking (drop if full) */
    mq_send(g_mqd, event_json, strlen(event_json) + 1, 0);
}

/* ── Consumer thread ──────────────────────────────────────────────────────── */
void *mq_event_consumer(void *arg)
{
    (void)arg;
    char msg[MQ_MSGSIZE];
    system("mkdir -p storage/logs");

    while (g_running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        ssize_t n = mq_timedreceive(g_mqd, msg, MQ_MSGSIZE, NULL, &ts);
        if (n <= 0) continue;

        /* Write to activity log */
        FILE *lf = fopen(LOG_FILE, "a");
        if (lf) {
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            char ts_str[32];
            strftime(ts_str, sizeof ts_str, "%Y-%m-%d %H:%M:%S", tm);
            fprintf(lf, "[%s] %s\n", ts_str, msg);
            fclose(lf);
        }
    }
    return NULL;
}

/* ── Convenience senders ──────────────────────────────────────────────────── */
void mq_notify_login(const char *user, const char *role)
{
    char buf[MQ_MSGSIZE];
    snprintf(buf, sizeof buf, "LOGIN  user=%-12s role=%s", user, role);
    mq_event_send(buf);
}
void mq_notify_logout(const char *user)
{
    char buf[MQ_MSGSIZE];
    snprintf(buf, sizeof buf, "LOGOUT user=%s", user);
    mq_event_send(buf);
}
void mq_notify_upload(const char *user, const char *file, int version)
{
    char buf[MQ_MSGSIZE];
    snprintf(buf, sizeof buf, "UPLOAD user=%-12s file=%-20s v%d", user, file, version);
    mq_event_send(buf);
}
void mq_notify_download(const char *user, const char *file)
{
    char buf[MQ_MSGSIZE];
    snprintf(buf, sizeof buf, "DWNLD  user=%-12s file=%s", user, file);
    mq_event_send(buf);
}
void mq_notify_delete(const char *user, const char *file)
{
    char buf[MQ_MSGSIZE];
    snprintf(buf, sizeof buf, "DELETE user=%-12s file=%s", user, file);
    mq_event_send(buf);
}
