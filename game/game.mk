# game/ -- owned by the game module. Pure C++ (no GL, no DDS): it only
# includes the gfx/net *headers* for their plain data structs.
GAME_SRCS := $(wildcard game/src/*.cpp)
GAME_OBJS += $(GAME_SRCS:%.cpp=$(BUILD)/%.o)

# Headless unit tests: no GL, no DDS, no window. `make game_tests` builds,
# `make check_game` builds and runs.
GAME_TEST_SRCS := $(wildcard game/tests/*.cpp)
GAME_TEST_OBJS := $(GAME_TEST_SRCS:%.cpp=$(BUILD)/%.o)

game_tests: $(GAME_TEST_OBJS) $(GAME_SRCS:%.cpp=$(BUILD)/%.o)
	$(CXX) -o $@ $^ -lm

.PHONY: check_game
check_game: game_tests
	./game_tests

# --- sanitizer builds (separate tree under build/asan; default build untouched)
#   make game_tests_asan   -> build/asan/game_tests   (ASan+UBSan headless tests)
#   make drifto_asan       -> build/asan/drifto       (client + gfx + net, same flags)
# (Other modules' variables aren't defined yet when this file is included, so
#  the source lists are spelled out here.)
ASAN_BUILD := $(BUILD)/asan
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1
ASAN_NET_GEN := $(BUILD)/net/gen
$(ASAN_BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(ASAN_FLAGS) $(GLFW_CFLAGS) $(DDS_CFLAGS) -I$(ASAN_NET_GEN) -c $< -o $@

ASAN_GAME_OBJS := $(GAME_SRCS:%.cpp=$(ASAN_BUILD)/%.o)
ASAN_CLIENT_OBJS := $(patsubst %.cpp,$(ASAN_BUILD)/%.o,$(wildcard client_folder/src/*.cpp))
ASAN_GFX_OBJS := $(patsubst %.cpp,$(ASAN_BUILD)/%.o,$(wildcard gfx/src/*.cpp))
ASAN_NET_OBJS := $(patsubst %.cpp,$(ASAN_BUILD)/%.o,$(wildcard net/src/*.cpp))
ASAN_AUDIO_OBJS := $(patsubst %.cpp,$(ASAN_BUILD)/%.o,$(wildcard audio/src/*.cpp))
$(ASAN_NET_OBJS): | $(ASAN_NET_GEN)/drifto_net.h

.PHONY: game_tests_asan drifto_asan
game_tests_asan: $(ASAN_BUILD)/game_tests
$(ASAN_BUILD)/game_tests: $(GAME_TEST_SRCS:%.cpp=$(ASAN_BUILD)/%.o) $(ASAN_GAME_OBJS)
	$(CXX) $(ASAN_FLAGS) -o $@ $^ -lm

drifto_asan: $(ASAN_BUILD)/drifto
$(ASAN_BUILD)/drifto: $(ASAN_CLIENT_OBJS) $(ASAN_GAME_OBJS) $(ASAN_GFX_OBJS) $(ASAN_NET_OBJS) $(ASAN_NET_GEN)/drifto_net.o $(ASAN_AUDIO_OBJS)
	$(CXX) $(ASAN_FLAGS) -o $@ $^ $(GLFW_LIBS) -lGL -ldl $(DDS_LIBS) $(OPENSSL_LIBS) -pthread -lm
