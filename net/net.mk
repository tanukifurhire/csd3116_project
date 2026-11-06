# net/ -- owned by the networking module. The only module that touches DDS.
#
#   NET_CLIENT_OBJS  client-side code linked into 'drifto' (NetClient + shared
#                    DDS helpers + idlc output)
#   api_server       API service: login (auth.c), rooms, game-server allocator
#   game_server      game service: per-room packet router (UDP + TCP-443)
#   net_tests        end-to-end tests (spawns both servers + headless clients)
#
# idlc output goes to build/net/gen/ (the repo root's messages.c/.h belong to
# the legacy client/server).

NET_GEN_DIR := $(BUILD)/net/gen
NET_IDL     := net/idl/drifto_net.idl
NET_GEN_H   := $(NET_GEN_DIR)/drifto_net.h
NET_GEN_C   := $(NET_GEN_DIR)/drifto_net.c
NET_GEN_OBJ := $(NET_GEN_DIR)/drifto_net.o

$(NET_GEN_H): $(NET_IDL)
	@mkdir -p $(NET_GEN_DIR)
	$(IDLC) -l c -x final -o $(NET_GEN_DIR) $(NET_IDL)
$(NET_GEN_C): $(NET_GEN_H) ;

# idlc emits C (uses _Alignof), so compile it with the C compiler.
$(NET_GEN_OBJ): $(NET_GEN_C)
	$(CC) $(CFLAGS_C) $(DDS_CFLAGS) -c $< -o $@

NET_CLIENT_SRCS := $(wildcard net/src/*.cpp)
NET_CLIENT_CXX_OBJS := $(NET_CLIENT_SRCS:%.cpp=$(BUILD)/%.o)
NET_CLIENT_OBJS += $(NET_CLIENT_CXX_OBJS) $(NET_GEN_OBJ)

# Objects shared with the servers (no NetClient in them).
NET_COMMON_OBJS := $(BUILD)/net/src/net_common.o $(NET_GEN_OBJ)

NET_SERVER_OBJS := $(BUILD)/server_folder/src/server.o
API_SERVER_OBJS := $(BUILD)/server_folder/src/api_server.o $(NET_SERVER_OBJS) $(NET_COMMON_OBJS) auth.o
GAME_SERVER_OBJS := $(BUILD)/server_folder/src/game_server.o $(NET_SERVER_OBJS) $(NET_COMMON_OBJS)
NET_TESTS_OBJS := $(BUILD)/net/tests/net_tests.o $(NET_CLIENT_OBJS)

NET_ALL_CXX_OBJS := $(BUILD)/net/tests/lan_probe.o $(NET_CLIENT_CXX_OBJS) $(NET_SERVER_OBJS) $(BUILD)/server_folder/src/api_server.o \
                    $(BUILD)/server_folder/src/game_server.o $(BUILD)/net/tests/net_tests.o

# DDS + generated headers for every object that includes net_common.h. The
# generated header must exist before those objects compile (order-only; the
# -MMD .d files take over after the first build).
$(NET_ALL_CXX_OBJS): MODULE_CXXFLAGS += $(DDS_CFLAGS) $(OPENSSL_CFLAGS) -I$(NET_GEN_DIR)
$(NET_ALL_CXX_OBJS): | $(NET_GEN_H)

api_server: $(API_SERVER_OBJS)
	$(CXX) -o $@ $^ $(DDS_LIBS) $(OPENSSL_LIBS) -pthread

game_server: $(GAME_SERVER_OBJS)
	$(CXX) -o $@ $^ $(DDS_LIBS) -pthread

net_tests: $(NET_TESTS_OBJS) api_server game_server
	$(CXX) -o $@ $(NET_TESTS_OBJS) $(DDS_LIBS) $(OPENSSL_LIBS) -pthread

# Headless LAN probe (cross-machine / Windows<->WSL checks); also built by CMake.
lan_probe: $(BUILD)/net/tests/lan_probe.o $(NET_CLIENT_OBJS)
	$(CXX) -o $@ $^ $(DDS_LIBS) $(OPENSSL_LIBS) -pthread

.PHONY: net-check
# Builds everything net/ owns and runs the end-to-end tests.
net-check: net_tests
	./net_tests
