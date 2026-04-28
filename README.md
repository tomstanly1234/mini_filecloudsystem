# Mini Cloud Drive — C Project
### EGC 301P / Operating Systems Lab Mini Project

---

## Overview

A fully working **Distributed Multi-User File Storage System** (mini Google Drive / Dropbox)
built in C using core Operating Systems concepts. Multiple clients connect over TCP sockets
to a central server that handles concurrent file uploads, downloads, deletions, and user
management — all with proper locking, authorization, and IPC.

---

## OS Concepts Covered (All 6 Mandatory)

| # | Concept | Where Implemented | File |
|---|---------|-------------------|------|
| 1 | **Role-Based Authorization** | Permission table; every command checked against user role (admin/user/guest) | `server/auth.c` |
| 2 | **File Locking** | `pthread_rwlock_t` per file — multiple readers OR one exclusive writer | `server/lock_manager.c` |
| 3 | **Concurrency Control** | One `pthread` per client; `sem_t` caps max connections; `pthread_mutex_t` guards all shared data | `server/server.c`, all modules |
| 4 | **Data Consistency** | Mutexes on metadata DB and user DB; write locks on files prevent dirty reads and lost updates | `server/metadata.c`, `server/auth.c` |
| 5 | **Socket Programming** | TCP server (`bind/listen/accept`); length-prefixed binary protocol for both JSON control and raw file data | `server/server.c`, `client/client.c`, `shared/protocol.c` |
| 6 | **IPC** | POSIX **shared memory** (`shm_open/mmap`) for live session table; POSIX **message queue** (`mq_open`) as event bus | `server/ipc_shm.c`, `server/ipc_mq.c` |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        CLIENT                               │
│   client.c  ──  shared/protocol.c  (TCP socket)            │
└──────────────────────────┬──────────────────────────────────┘
                           │  TCP  (port 7777)
┌──────────────────────────▼──────────────────────────────────┐
│                        SERVER                               │
│                                                             │
│  server.c  (main loop + one pthread per client)             │
│     │                                                       │
│     ├── auth.c         Role-Based Authorization             │
│     │     └── users.db (salt:hash:role records)             │
│     │                                                       │
│     ├── lock_manager.c  pthread_rwlock_t per file           │
│     │                                                       │
│     ├── metadata.c      File registry (mutex protected)     │
│     │     └── files.db                                      │
│     │                                                       │
│     ├── ipc_shm.c       POSIX Shared Memory                 │
│     │     └── /minicloud_sessions  (live session table)     │
│     │         guarded by named semaphore /minicloud_sem      │
│     │                                                       │
│     └── ipc_mq.c        POSIX Message Queue                 │
│           └── /minicloud_events  (event bus → activity.log) │
│                                                             │
└─────────────────────────────────────────────────────────────┘
           │                    │
    storage/files/       storage/logs/
    (uploaded files)     activity.log
```

---

## Wire Protocol

Every message uses a **length-prefixed** binary framing:

```
Control message:
  [4-byte big-endian uint32 length][JSON string]

File data (upload / download):
  [4-byte big-endian uint32 length][raw bytes]
```

### Command flow — Upload example:

```
Client                          Server
  │                               │
  │── {"cmd":"UPLOAD",            │
  │    "filename":"x.txt",        │
  │    "size":1024} ─────────────▶│  acquires write lock
  │                               │
  │◀──── {"status":"OK",          │
  │       "message":"READY"} ─────│
  │                               │
  │── [4-byte len][1024 bytes] ──▶│  writes to disk
  │                               │  updates metadata
  │◀──── {"status":"OK",          │  releases write lock
  │       "version":1} ───────────│  notifies event bus
```

---

## Project Structure

```
mini_cloud_drive_c/
├── Makefile
├── README.md
├── shared/
│   ├── protocol.h          Wire protocol, all constants
│   └── protocol.c          send/recv helpers, mini JSON parser
├── server/
│   ├── server.c            Main server — TCP + thread-per-client
│   ├── auth.h / auth.c     User DB, SHA-256 passwords, role perms
│   ├── lock_manager.h/.c   Per-file pthread_rwlock_t table
│   ├── metadata.h/.c       File registry (mutex-protected flat DB)
│   ├── ipc_shm.h/.c        POSIX shared memory session table
│   └── ipc_mq.h/.c         POSIX message queue event bus
├── client/
│   └── client.c            Interactive CLI client
└── storage/
    ├── files/              Uploaded file storage
    ├── metadata/
    │   ├── users.db        User accounts (salt:hash:role)
    │   └── files.db        File metadata records
    └── logs/
        └── activity.log    Server activity log
```

---

## Build & Run

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install gcc libssl-dev

# Fedora / RHEL
sudo dnf install gcc openssl-devel
```

### Build

```bash
cd mini_cloud_drive_c
make
```

This produces:
- `./server_bin` — the file server
- `./client_bin` — the interactive CLI client

### Run

**Terminal 1 — Server:**
```bash
./server_bin
```
Output:
```
[AUTH]    Default admin created (user=admin / pass=admin123)
[IPC-SHM] Shared memory + semaphore initialised
[IPC-MQ]  Message queue '/minicloud_events' opened
[SERVER]  Mini Cloud Drive listening on port 7777
[SERVER]  Max concurrent clients: 50
```

**Terminal 2, 3, … — Clients:**
```bash
./client_bin              # connects to 127.0.0.1:7777
./client_bin 10.0.0.5    # connect to remote host
```

---

## Client Commands

```
login  <user> <pass>              Log in
logout                            Log out
upload <local_path> [name]        Upload a file to server
download <name> [local_path]      Download a file (saved to downloads/)
delete <name>                     Delete a file
list                              List all files (name, owner, size, version)
search <query>                    Search files by name substring
adduser <user> <pass> [role]      (admin) Add user  [role: admin|user|guest]
deluser <username>                (admin) Delete user
users                             (admin) List all users and roles
logs [n]                          (admin) Show last n activity log entries
sessions                          (admin) Show currently connected users
exit                              Disconnect and quit
```

---

## Full Demo Session

```
(not logged in) > login admin admin123
  ✓  Welcome admin!

(admin:admin) > adduser alice secret user
  ✓  User 'alice' (user) created

(admin:admin) > adduser bob readpass guest
  ✓  User 'bob' (guest) created

(admin:admin) > users
  Username             Role
  ──────────────────────────────
  admin                admin
  alice                user
  bob                  guest

(admin:admin) > upload /etc/hostname server_hostname.txt
  ✓  'server_hostname.txt' uploaded (v1)

(admin:admin) > list
  Filename                       Owner        Size   Ver  Modified
  ──────────────────────────────────────────────────────────────────
  server_hostname.txt            admin           12     1  2025-01-15 10:22:04

(admin:admin) > logout
  ✓  Goodbye!

(not logged in) > login alice secret
  ✓  Welcome alice!

(alice:user) > download server_hostname.txt
  ✓  Saved 12 bytes to 'downloads/server_hostname.txt'

(alice:user) > upload /tmp/mydata.csv mydata.csv
  ✓  'mydata.csv' uploaded (v1)

(alice:user) > delete server_hostname.txt
  ✗  [DENIED] You can only delete your own files

(alice:user) > logout

(not logged in) > login bob readpass
  ✓  Welcome bob!

(bob:guest) > list
  ✓  (shows all files)

(bob:guest) > upload /tmp/x.txt x.txt
  ✗  [DENIED] No upload permission

(bob:guest) > logout
```

---

## Concurrency Test

Open **3 terminals** simultaneously:

```bash
# Terminal 1
./client_bin
login admin admin123
upload bigfile.dat bigfile.dat      # acquires write lock

# Terminal 2 (while T1 is uploading)
./client_bin
login alice secret
download bigfile.dat                # blocks until write lock released

# Terminal 3
./client_bin
login bob readpass
list                                # list is always available (no lock needed)
```

---

## How Each OS Concept Appears in the Code

### 1. Role-Based Authorization — `server/auth.c`
```c
static const RolePerms PERMS[] = {
    { ROLE_ADMIN, { "upload","download","delete","adduser","logs", ... } },
    { ROLE_USER,  { "upload","download","delete_own","list", ... } },
    { ROLE_GUEST, { "download","list","search", NULL } },
};
```
Every handler calls `auth_has_permission(role, action)` before executing.

### 2. File Locking — `server/lock_manager.c`
```c
// Multiple readers allowed simultaneously:
pthread_rwlock_timedrdlock(&entry->rwlock, &timeout);

// Only one writer, blocks all readers:
pthread_rwlock_timedwrlock(&entry->rwlock, &timeout);
```

### 3. Concurrency Control — `server/server.c`
```c
// One thread per client
pthread_create(&tid, &attr, client_thread, ctx);

// Semaphore caps concurrent connections at MAX_CLIENTS=50
sem_wait(&connection_sem);   // on accept
sem_post(&connection_sem);   // on disconnect
```

### 4. Data Consistency — `server/metadata.c`, `server/auth.c`
```c
// Mutex protects every read/write/update to users.db and files.db
pthread_mutex_lock(&meta_mutex);
// ... read-modify-write ...
pthread_mutex_unlock(&meta_mutex);
```

### 5. Socket Programming — `server/server.c`, `client/client.c`
```c
// Server
bind(srv_fd, (struct sockaddr *)&addr, sizeof addr);
listen(srv_fd, MAX_CLIENTS);
int cli_fd = accept(srv_fd, &cli_addr, &cli_len);

// Client
connect(g_fd, (struct sockaddr *)&srv, sizeof srv);
```

### 6. IPC — `server/ipc_shm.c` + `server/ipc_mq.c`
```c
// Shared Memory — live session table
g_shm_fd = shm_open(SHM_NAME, O_CREAT|O_RDWR, 0660);
g_table  = mmap(NULL, sizeof(SessionTable),
                PROT_READ|PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
g_sem    = sem_open(SEM_NAME, O_CREAT|O_EXCL, 0660, 1);

// Message Queue — event bus → activity.log
g_mqd = mq_open(MQ_NAME, O_CREAT|O_RDWR, 0660, &attr);
mq_send(g_mqd, event_json, strlen(event_json)+1, 0);
mq_timedreceive(g_mqd, msg, MQ_MSGSIZE, NULL, &ts); // consumer thread
```

---

## Security Notes

- Passwords stored as `SHA-256(salt + password)` — never in plaintext
- Salts are 32-char random hex strings generated per user
- Filenames are sanitised (no `/` or `\` allowed)
- Admin is the only role that can delete other users' files or view logs
- The last admin account cannot be deleted

---

## Extending the Project

| Feature | How to add |
|---------|-----------|
| File versioning UI | Use `version` field already stored in `files.db` |
| Auto-timeout idle users | Add a watchdog thread checking `last_active` in shared memory |
| Pipes IPC | Add a `pipe()` between the main thread and logger instead of mq |
| TLS encryption | Wrap `send`/`recv` with OpenSSL `SSL_write`/`SSL_read` |
| File chunking | Split large files into 1 MB chunks, upload sequentially |
