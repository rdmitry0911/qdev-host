# apple-videotoolbox - AppleGVA host VideoToolbox codec device (increment 1).
# Standalone build + unit tests for the module, independent of the qdev-host
# top build. The plugin .so links against libva; the protocol-handler test
# needs no libva. CLEAN-ROOM (see the headers). Lane L-M2V03, 2026-11-16.
#
# Targets:
#   make test        - build + run every unit test (proto always; vaapi+plugin
#                      if their deps are present; vaapi skips w/o a render node)
#   make plugin      - build apple-videotoolbox.so (needs libva + the vendored
#                      libvfio-user header at ../../third_party/libvfio-user)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
QDP_INC := -I../..
VFU_INC := -I../../third_party/libvfio-user/include
VA_CFLAGS := $(shell pkg-config --cflags libva libva-drm 2>/dev/null)
VA_LIBS   := $(shell pkg-config --libs libva libva-drm 2>/dev/null)

BUILD := build
HAVE_VA := $(if $(VA_LIBS),1,)

.PHONY: all test plugin clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

# --- unit tests -------------------------------------------------------

$(BUILD)/test-proto: vtpv_proto.c vtpv_proto.h test_proto.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ vtpv_proto.c test_proto.c

$(BUILD)/test-vaapi: vtpv_proto.c vtpv_vaapi.c test_vaapi.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ vtpv_proto.c vtpv_vaapi.c test_vaapi.c $(VA_LIBS)

$(BUILD)/apple-videotoolbox.so: apple-videotoolbox.c vtpv_proto.c vtpv_vaapi.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter -fPIC -shared $(QDP_INC) $(VFU_INC) $(VA_CFLAGS) \
	    -o $@ apple-videotoolbox.c vtpv_proto.c vtpv_vaapi.c $(VA_LIBS)

$(BUILD)/test-plugin: test_plugin.c $(BUILD)/apple-videotoolbox.so | $(BUILD)
	$(CC) $(CFLAGS) $(QDP_INC) $(VFU_INC) -o $@ test_plugin.c -ldl

plugin: $(BUILD)/apple-videotoolbox.so

test: $(BUILD)/test-proto
	@echo "== protocol handler =="; $(BUILD)/test-proto
ifeq ($(HAVE_VA),1)
	@$(MAKE) --no-print-directory $(BUILD)/test-vaapi $(BUILD)/test-plugin
	@echo "== VA-API backend (live render node) =="; \
	 $(BUILD)/test-vaapi; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(vaapi test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== plugin ABI smoke =="; $(BUILD)/test-plugin $(BUILD)/apple-videotoolbox.so
else
	@echo "(libva not found via pkg-config: skipping VA-API backend + plugin tests)"
endif

clean:
	rm -rf $(BUILD)
