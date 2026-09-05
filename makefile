CC   = gcc
CXX  = g++
IDLC = idlc

IDL_SRC = messages.idl
IDL_GEN = messages.c messages.h

CXXFLAGS = -std=c++17 -Wall

GLFW_CFLAGS    = $(shell pkg-config --cflags glfw3)
GLFW_LIBS      = $(shell pkg-config --libs glfw3)
# GLEW loads the OpenGL 3.3 core entry points (shaders, VAOs, VBOs) that libGL
# does not export directly. Only the C++ client needs it; client.c still uses
# the fixed-function pipeline.
GLEW_CFLAGS    = $(shell pkg-config --cflags glew)
GLEW_LIBS      = $(shell pkg-config --libs glew)
DDS_CFLAGS     = $(shell pkg-config --cflags CycloneDDS)
DDS_LIBS       = $(shell pkg-config --libs CycloneDDS)
OPENSSL_CFLAGS = $(shell pkg-config --cflags libcrypto)
OPENSSL_LIBS   = $(shell pkg-config --libs libcrypto)

# C++ port of client/server, built alongside the existing C client/server
# while the port is in progress (see docs/NETWORKING.md). 'make all' does
# NOT build these yet -- run 'make cpp' explicitly.
CLIENT_CPP_SRC = client_folder/src/main.cpp client_folder/src/client.cpp \
                 client_folder/src/renderer.cpp
SERVER_CPP_SRC = server_folder/src/main.cpp server_folder/src/server.cpp

N ?= 1

.PHONY: all clean run certs cpp

all: server client certs

cpp: client_cpp server_cpp

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
	$(CC) -o server server.c auth.c messages.c $(DDS_CFLAGS) $(DDS_LIBS) $(OPENSSL_LIBS)

client: client.c messages.c
	$(CC) client.c messages.c -o client $(GLFW_CFLAGS) $(GLFW_LIBS) -lGL $(DDS_CFLAGS) $(DDS_LIBS) -pthread -lm

# messages.c/auth.c are plain C translation units, shared as-is with the C++
# port. Compiled separately with $(CC) (not $(CXX)) and linked into the C++
# binaries -- g++ compiles .c input as C++ by default, which would silently
# change how these files are parsed if built directly by the CXX rules below.
messages.o: messages.c messages.h
	$(CC) -c messages.c -o messages.o $(DDS_CFLAGS)

auth.o: auth.c auth.h
	$(CC) -c auth.c -o auth.o $(OPENSSL_CFLAGS)

client_cpp: $(CLIENT_CPP_SRC) client_folder/include/client.h client_folder/include/renderer.h messages.o
	$(CXX) $(CXXFLAGS) -o client_cpp $(CLIENT_CPP_SRC) messages.o \
		$(GLEW_CFLAGS) $(GLFW_CFLAGS) $(GLEW_LIBS) $(GLFW_LIBS) -lGL \
		$(DDS_CFLAGS) $(DDS_LIBS) -pthread -lm

server_cpp: $(SERVER_CPP_SRC) server_folder/include/server.h messages.o auth.o
	$(CXX) $(CXXFLAGS) -o server_cpp $(SERVER_CPP_SRC) messages.o auth.o \
		$(DDS_CFLAGS) $(DDS_LIBS) $(OPENSSL_CFLAGS) $(OPENSSL_LIBS)

clean:
	rm -f server client client_cpp server_cpp messages.c messages.h messages.o auth.o
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