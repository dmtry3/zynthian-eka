.DEFAULT_GOAL := all
CC ?= cc
CXX ?= c++
CFLAGS ?= -O3
WARN = -std=c11 -Wall -Wextra -Wpedantic -Werror
LV2_CFLAGS := $(shell pkg-config --cflags lv2)

# Private DSP-only aubio subset; portable Ooura FFT, no FFmpeg/runtime dependency.
TRACK_SOURCES = $(wildcard third_party/aubio/*.c third_party/aubio/pitch/*.c third_party/aubio/spectral/*.c third_party/aubio/utils/*.c)
TRACK_OBJECTS = $(patsubst %.c,build/tracker-objects/%.o,$(TRACK_SOURCES))
TRACK_LIBS = build/tracker/libaubio-dsp.a
build/tracker-objects/%.o: %.c $(wildcard third_party/aubio/*.h third_party/aubio/*/*.h)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=c11 -fPIC -fvisibility=hidden -DHAVE_CONFIG_H -Ithird_party/aubio -c $< -o $@
$(TRACK_LIBS): $(TRACK_OBJECTS)
	mkdir -p $(dir $@)
	ar rcs $@ $^

# Bungee Basic (standard granularity 0) pitch-shifting backend.
BUNGEE_LIB = build/bungee/libbungee.a
$(BUNGEE_LIB): scripts/build-bungee.py
	python3 scripts/build-bungee.py

BUNDLE = build/eka-weave4.lv2
COMMON_SOURCES = src/dsp/pitch.c src/dsp/character.c src/dsp/tone.c src/dsp/delay.c src/harmony/chords.c src/harmony/timing.c src/harmony/midi.c src/harmony/scale.c src/dsp/tracker.c src/lv2/tempo.c src/lv2/plugin.c
HEADERS = $(wildcard src/dsp/*.h src/harmony/*.h src/lv2/*.h)
C_OBJS = $(patsubst %.c,build/objects/%.o,$(COMMON_SOURCES))
BACKEND_OBJ = build/objects/backend_bungee.o

LV2_DIR ?= /usr/local/lib/lv2
ZYNTHIAN_UI_DIR ?= /zynthian/zynthian-ui

.PHONY: all install uninstall test status
all: $(BUNDLE)/eka_weave4.so

build/objects/%.o: %.c $(HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) $(LV2_CFLAGS) -fPIC -c $< -o $@

$(BACKEND_OBJ): src/dsp/backend_bungee.cpp src/dsp/backend.h $(BUNGEE_LIB) scripts/build-bungee.py
	mkdir -p $(dir $@)
	$(CXX) -O3 -std=c++20 -fPIC -fvisibility=hidden -DEKA_BUNGEE_GRANULARITY=0 -Ibuild/bungee/bungee -c $< -o $@

$(BUNDLE)/eka_weave4.so: $(TRACK_LIBS) $(C_OBJS) $(BACKEND_OBJ) $(BUNGEE_LIB) lv2/manifest.ttl lv2/eka_weave4.ttl scripts/factory-presets.py
	@pkg-config --exists lv2 || { echo "LV2 headers missing"; exit 1; }
	mkdir -p $(BUNDLE)
	$(CXX) -shared -Wl,-Bsymbolic $(C_OBJS) $(BACKEND_OBJ) $(BUNGEE_LIB) $(TRACK_LIBS) -Wl,--exclude-libs,ALL -lm -o $@
	cp lv2/manifest.ttl lv2/eka_weave4.ttl $(BUNDLE)/
	python3 scripts/factory-presets.py $(BUNDLE)

install: all
	install -d "$(DESTDIR)$(LV2_DIR)/eka-weave4.lv2"
	install -m 755 $(BUNDLE)/eka_weave4.so "$(DESTDIR)$(LV2_DIR)/eka-weave4.lv2/"
	install -m 644 $(BUNDLE)/*.ttl build/bungee/notices/* "$(DESTDIR)$(LV2_DIR)/eka-weave4.lv2/"
	install -m 644 third_party/aubio/COPYING "$(DESTDIR)$(LV2_DIR)/eka-weave4.lv2/LICENSE-aubio.txt"
	install -d "$(DESTDIR)$(ZYNTHIAN_UI_DIR)/zyngui/widgets_user"
	install -m 644 ui/zynthian_widget_eka_weave4.py "$(DESTDIR)$(ZYNTHIAN_UI_DIR)/zyngui/widgets_user/"
	install -m 644 ui/zynthian_widget_eka_weave4.py "$(DESTDIR)$(ZYNTHIAN_UI_DIR)/zyngui/widgets_user/zynthian_widget_eka_weave-4.py"
	@if [ -d "$${ZYNTHIAN_CONFIG_DIR:-/zynthian/config}" ]; then python3 scripts/register-factory-presets.py; fi

uninstall:
	rm -f "$(DESTDIR)$(ZYNTHIAN_UI_DIR)/zyngui/widgets_user/zynthian_widget_eka_weave4.py" "$(DESTDIR)$(ZYNTHIAN_UI_DIR)/zyngui/widgets_user/zynthian_widget_eka_weave-4.py"
	rm -rf "$(DESTDIR)$(LV2_DIR)/eka-weave4.lv2"

test: all build/tracker-rt build/bungee-rt build/bungee-stream
	./build/tracker-rt
	./build/bungee-rt
	./build/bungee-stream
	python3 test/check.py
	python3 test/factory_presets.py

build/bungee-rt: test/backend_rt.cpp $(BACKEND_OBJ) $(BUNGEE_LIB)
	$(CXX) -O3 -std=c++20 $^ -lm -o $@

build/bungee-stream: test/backend_stream.cpp $(BACKEND_OBJ) $(BUNGEE_LIB)
	$(CXX) -O3 -std=c++20 $^ -lm -o $@

build/tracker-rt: test/tracker_rt.c src/dsp/tracker.c src/dsp/tracker.h src/harmony/scale.c src/harmony/scale.h $(TRACK_LIBS)
	$(CC) $(CFLAGS) $(WARN) test/tracker_rt.c src/dsp/tracker.c src/harmony/scale.c $(TRACK_LIBS) -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -lm -o $@

status:
	@echo "Eka Weave-4 (Bungee Basic, granularity 0). Four voices, scenes, MIDI/Scale harmony."
