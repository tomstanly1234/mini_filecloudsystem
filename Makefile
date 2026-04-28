# ─────────────────────────────────────────────────────────────────────────────
# Makefile — Mini Cloud Drive (C)
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g \
           -Ishared \
           -Iserver \
           -Iclient
LDFLAGS = -lpthread -lssl -lcrypto -lrt   # -lrt for POSIX shm + mq on Linux

SHARED_SRC = shared/protocol.c

SERVER_SRC = $(SHARED_SRC) \
             server/auth.c \
             server/lock_manager.c \
             server/metadata.c \
             server/ipc_shm.c \
             server/ipc_mq.c \
             server/server.c

CLIENT_SRC = $(SHARED_SRC) \
             client/client.c

SERVER_BIN = server_bin
CLIENT_BIN = client_bin

.PHONY: all clean run_server run_client dirs

all: dirs $(SERVER_BIN) $(CLIENT_BIN)
	@echo ""
	@echo "  Build successful!"
	@echo "  Run server:  ./$(SERVER_BIN)"
	@echo "  Run client:  ./$(CLIENT_BIN) [host] [port]"
	@echo ""

dirs:
	@mkdir -p storage/files storage/metadata storage/logs downloads

$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "  [OK] Server compiled -> $(SERVER_BIN)"

$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "  [OK] Client compiled -> $(CLIENT_BIN)"

run_server: $(SERVER_BIN)
	./$(SERVER_BIN)

run_client: $(CLIENT_BIN)
	./$(CLIENT_BIN)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
	rm -f storage/metadata/users.db* storage/metadata/files.db*
	rm -f storage/logs/activity.log
	@echo "  Cleaned."
