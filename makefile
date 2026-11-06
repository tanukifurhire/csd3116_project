.DEFAULT_GOAL := all

CC   = gcc
CXX  = g++
IDLC = idlc

IDL_SRC = messages.idl
IDL_GEN = messages.c messages.h


GLFW_CFLAGS    = $(shell pkg-config --cflags glfw3)
GLFW_LIBS      = $(shell pkg-config --libs glfw3)
DDS_CFLAGS     = $(shell pkg-config --cflags CycloneDDS)
DDS_LIBS       = $(shell pkg-config --libs CycloneDDS)
OPENSSL_CFLAGS = $(shell pkg-config --cflags libcrypto)
OPENSSL_LIBS   = $(shell pkg-config --libs libcrypto)

# ---------------------------------------------------------------------------
# Drifto (C++): one shared object tree in build/, one makefile fragment per
# module so each module owns its own sources/targets:
#   game/game.mk  -> GAME_OBJS      (+ game_tests)
#   gfx/gfx.mk    -> GFX_OBJS       (+ gfx_demo, assets)
#   net/net.mk    -> NET_CLIENT_OBJS, api_server, game_server (+ net_tests)
# The client executable 'drifto' links all three with client_folder/src.
# ---------------------------------------------------------------------------
BUILD    = build
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -MMD -MP -I. -Ithird_party
CFLAGS_C = -O2 -g -MMD -MP

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MODULE_CXXFLAGS) -c $< -o $@

GAME_OBJS :=
GFX_OBJS :=
NET_CLIENT_OBJS :=
MODULE_CXXFLAGS :=
DRIFTO_LIBS :=

include game/game.mk
include gfx/gfx.mk
include net/net.mk
AUDIO_OBJS :=
AUDIO_LIBS :=
-include audio/audio.mk
-include perf/perf.mk

CLIENT_SRCS := $(wildcard client_folder/src/*.cpp)
CLIENT_OBJS := $(CLIENT_SRCS:%.cpp=$(BUILD)/%.o)

drifto: $(CLIENT_OBJS) $(GAME_OBJS) $(GFX_OBJS) $(NET_CLIENT_OBJS) $(AUDIO_OBJS)
	$(CXX) -o $@ $^ $(DRIFTO_LIBS) $(GLFW_LIBS) -lGL $(DDS_LIBS) $(OPENSSL_LIBS) $(AUDIO_LIBS) -pthread -lm

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

N ?= 1

.PHONY: all clean clean-certs run certs legacy

all: legacy drifto api_server game_server

legacy: server client certs

$(IDL_GEN): $(IDL_SRC)
	$(IDLC) -l c $(IDL_SRC)

# certs/ is gitignored (private keys shouldn't be committed). The submission
# ships the team's certs/ so clients can log in to our online servers, which
# only accept certificates signed by that CA. So certs are generated only when
# MISSING (order-only '|' prerequisites), never because a file looks newer:
# unzipping resets timestamps, and a regenerated CA would silently lock the
# player out of the online servers. To start over: make clean-certs certs.
# To re-sign after editing governance.xml/permissions.xml: delete that .p7s.
# certs/ca.pem stands in for the whole cert/key batch gen_certs.sh produces.
certs/ca.pem: | gen_certs.sh
	chmod +x gen_certs.sh
	./gen_certs.sh

certs/governance.p7s: | governance.xml certs/ca.pem
	openssl smime -sign -in governance.xml -text -out certs/governance.p7s -signer certs/ca.pem -inkey certs/ca.key

certs/permissions.p7s: | permissions.xml certs/ca.pem
	openssl smime -sign -in permissions.xml -text -out certs/permissions.p7s -signer certs/ca.pem -inkey certs/ca.key

certs: certs/governance.p7s certs/permissions.p7s

# Deletes all certificates/keys (not part of 'make clean'). Everyone who plays
# together, servers included, then needs the newly generated set.
clean-certs:
	rm -f certs/*.key certs/*.pem certs/*.p7s certs/*.csr certs/*.srl

server: server.c auth.c auth.h messages.c
	$(CC) -o server server.c auth.c messages.c $(DDS_CFLAGS) $(DDS_LIBS) $(OPENSSL_LIBS)

client: client.c messages.c
	$(CC) client.c messages.c -o client $(GLFW_CFLAGS) $(GLFW_LIBS) -lGL $(DDS_CFLAGS) $(DDS_LIBS) -pthread -lm

# messages.c/auth.c are plain C translation units, shared as-is with the C++
# drifto servers. Compiled separately with $(CC) (not $(CXX)) and linked into the C++
# binaries -- g++ compiles .c input as C++ by default, which would silently
# change how these files are parsed if built directly by the CXX rules below.
messages.o: messages.c messages.h
	$(CC) -c messages.c -o messages.o $(DDS_CFLAGS)

auth.o: auth.c auth.h
	$(CC) -c auth.c -o auth.o $(OPENSSL_CFLAGS)

clean:
	rm -f server client client_cpp server_cpp drifto api_server game_server gfx_demo audio_demo game_tests net_tests lan_probe messages.c messages.h messages.o auth.o
	rm -rf $(BUILD)

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