# audio/ -- owned by the audio module (miniaudio backend, procedural engines).
#
#   AUDIO_OBJS   linked into 'drifto' (needs AUDIO_LIBS on Linux)
#   audio_demo   offline renders -> build/audio_*.wav + level/spectrum report
#   audio_assets recorded layers (tyre squeal, impacts) -> assets/audio/
#                (part of `make assets`; fetched, never committed)
#
# Windows/MSVC: compile audio/src/*.cpp with third_party/ on the include
# path; miniaudio needs no extra libraries there (WASAPI is loaded at run time).
AUDIO_SRCS := $(wildcard audio/src/*.cpp)
AUDIO_OBJS := $(AUDIO_SRCS:%.cpp=$(BUILD)/%.o)
AUDIO_LIBS := -ldl -pthread -lm

audio_demo: $(BUILD)/audio/demo/audio_demo.o $(AUDIO_OBJS)
	$(CXX) -o $@ $^ $(AUDIO_LIBS)

AUDIO_ASSETS_STAMP := assets/audio/CREDITS.txt
audio_assets: $(AUDIO_ASSETS_STAMP)
$(AUDIO_ASSETS_STAMP): tools/fetch_audio.py
	python3 tools/fetch_audio.py --out assets --build $(BUILD)
assets: $(AUDIO_ASSETS_STAMP)

.PHONY: audio_assets
