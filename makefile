CC   = gcc
IDLC = idlc

IDL_SRC = messages.idl
IDL_GEN = messages.c messages.h

N ?= 1

.PHONY: all clean run

all: server client

$(IDL_GEN): $(IDL_SRC)
	$(IDLC) -l c $(IDL_SRC)

server: server.c auth.c auth.h messages.c
	$(CC) -o server server.c auth.c messages.c $(shell pkg-config --cflags --libs CycloneDDS) -lcrypto

client: client.c messages.c
	$(CC) client.c messages.c -o client $(shell pkg-config --cflags --libs glfw3) -lGL $(shell pkg-config --cflags --libs CycloneDDS) -pthread -lm

clean:
	rm -f server client messages.c messages.h
	rm -f certs/*.key certs/*.pem certs/*.p7s certs/*.csr certs/*.srl

# Starts the server, waits a few seconds for it to come up, then launches
# N clients (1-4). Usage: make run N=3
run: all
	@if [ $(N) -lt 1 ] || [ $(N) -gt 4 ]; then \
		echo "N must be between 1 and 4 (got $(N))"; exit 1; \
	fi
	@echo "Starting server..."; \
	./server & \
	server_pid=$$!; \
	trap 'kill $$server_pid $$client_pids 2>/dev/null' EXIT INT TERM; \
	sleep 3; \
	client_pids=""; \
	for i in $$(seq 1 $(N)); do \
		echo "Starting client $$i..."; \
		./client $$i & \
		client_pids="$$client_pids $$!"; \
	done; \
	wait $$client_pids; \
	kill $$server_pid 2>/dev/null