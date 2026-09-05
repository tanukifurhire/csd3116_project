CC   = gcc
IDLC = idlc

IDL_SRC = messages.idl
IDL_GEN = messages.c messages.h

N ?= 1

.PHONY: all clean run certs

all: server client certs

$(IDL_GEN): $(IDL_SRC)
	$(IDLC) -l c $(IDL_SRC)

# certs/ is gitignored (private keys shouldn't be committed) and gets wiped
# by 'make clean', so regenerate it from scratch whenever it's missing.
# certs/ca.pem stands in for the whole cert/key batch gen_certs.sh produces.
certs/ca.pem: gen_certs.sh
	chmod +x gen_certs.sh
	./gen_certs.sh

certs/governance.p7s: governance.xml certs/ca.pem
	openssl smime -sign -in governance.xml -text -out certs/governance.p7s -signer certs/ca.pem -inkey certs/ca.key

certs/permissions.p7s: permissions.xml certs/ca.pem
	openssl smime -sign -in permissions.xml -text -out certs/permissions.p7s -signer certs/ca.pem -inkey certs/ca.key

certs: certs/governance.p7s certs/permissions.p7s

server: server.c auth.c auth.h messages.c
	$(CC) -o server server.c auth.c messages.c $(shell pkg-config --cflags --libs CycloneDDS) -lcrypto

client: client.c messages.c
	$(CC) client.c messages.c -o client $(shell pkg-config --cflags --libs glfw3) -lGL $(shell pkg-config --cflags --libs CycloneDDS) -pthread -lm

clean:
	rm -f server client messages.c messages.h
	rm -f certs/*.key certs/*.pem certs/*.p7s certs/*.csr certs/*.srl

# Starts the server, waits a few seconds for it to come up, then launches
# N clients (1-4). Usage: make run N=3
run: all certs
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