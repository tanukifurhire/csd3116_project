# gfx/ -- owned by the graphics module. The only module that touches OpenGL.
#
#   make assets    fetch JohnnyHowe/drifto-old into build/ and convert its
#                  cars / trees / skyboxes into assets/, and fetch the
#                  Initial D arcade HUD sprites into assets/hud/initiald/
#                  (both gitignored -- the sources have no license, nothing
#                  is committed)
#   make gfx_demo  standalone viewer: ./gfx_demo [--model NAME] [--skybox NAME]
#                  [--screenshot out.png]
GFX_SRCS := $(wildcard gfx/src/*.cpp)
GFX_OBJS += $(GFX_SRCS:%.cpp=$(BUILD)/%.o)
MODULE_CXXFLAGS += $(GLFW_CFLAGS)

GFX_DEMO_OBJS := $(BUILD)/gfx/demo/gfx_demo.o

gfx_demo: $(GFX_DEMO_OBJS) $(GFX_OBJS)
	$(CXX) -o $@ $^ $(GLFW_LIBS) -ldl -lm

# The stamp is rewritten on every successful conversion; re-run when the
# converter changes. `make assets FORCE_ASSETS=1` regenerates from scratch.
ASSETS_STAMP := assets/.stamp
HUD_THEME := assets/hud/initiald/theme.txt
assets: $(ASSETS_STAMP) $(HUD_THEME)
$(ASSETS_STAMP): tools/convert_drifto_assets.py
	python3 tools/convert_drifto_assets.py --out assets --build $(BUILD) $(if $(FORCE_ASSETS),--force)
# Initial D arcade HUD sprites (xmatheusgc/Initial-D-HUD, no license -> fetched,
# never committed). Never fails: without it the procedural HUD theme is used.
$(HUD_THEME): tools/fetch_initiald_hud.py
	python3 tools/fetch_initiald_hud.py --out assets --build $(BUILD)

.PHONY: assets
