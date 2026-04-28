#ifndef IPC_MQ_H
#define IPC_MQ_H

/*
 * server/ipc_mq.h
 * ---------------
 * Event notification via POSIX message queue (mq_send / mq_receive).
 *
 * OS Concept:
 *   IPC (Message Queue) – mq_open / mq_send / mq_receive
 */

#define MQ_NAME     "/minicloud_events"
#define MQ_MSGSIZE  256
#define MQ_MAXMSG   10

/* Initialise the message queue */
int  mq_event_init(void);

/* Cleanup */
void mq_event_cleanup(void);

/* Send an event string (non-blocking) */
void mq_event_send(const char *event_json);

/* Background consumer thread: reads from queue, writes to log */
void *mq_event_consumer(void *arg);

/* Convenience senders */
void mq_notify_login(const char *user, const char *role);
void mq_notify_logout(const char *user);
void mq_notify_upload(const char *user, const char *file, int version);
void mq_notify_download(const char *user, const char *file);
void mq_notify_delete(const char *user, const char *file);

#endif /* IPC_MQ_H */
