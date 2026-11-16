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

# Apple real-wire codec (aw_wire): byte-exact struct/framing + async-framing
# tests against the frozen WIRE-CONTRACT. No libva/GPU/guest.
$(BUILD)/test-aw-wire: aw_wire.c aw_wire.h test_aw_wire.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ aw_wire.c test_aw_wire.c

# Apple-wire sbuf reader (aw_sbuf, H3) + session engine (aw_session, H5+H6):
# routing/status + decode-exchange wire ordering with a mock backend. No libva.
AW_CORE := aw_wire.c aw_sbuf.c aw_bplist.c aw_session.c
$(BUILD)/test-aw-session: $(AW_CORE) test_aw_session.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(AW_CORE) test_aw_session.c

# H4: the binary-plist encoder — real Dpba/,hin/,dec bytes a VideoToolbox
# framework parses (validated by a re-parse). No libva.
$(BUILD)/test-aw-bplist: aw_bplist.c aw_bplist.h test_aw_bplist.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ aw_bplist.c test_aw_bplist.c

$(BUILD)/test-virtq: vtpv_virtq.c vtpv_proto.c test_virtq.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ vtpv_virtq.c vtpv_proto.c test_virtq.c

BACKEND := vtpv_proto.c vtpv_vaapi.c vtpv_h264.c vtpv_virtq.c

$(BUILD)/test-vaapi: $(BACKEND) test_vaapi.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(BACKEND) test_vaapi.c $(VA_LIBS)

$(BUILD)/test-h264: $(BACKEND) h264_gen.c test_h264.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(BACKEND) h264_gen.c test_h264.c $(VA_LIBS)

# THE HOST MILESTONE: real Apple contract-B wire bytes -> aw_session engine ->
# live VA-API H.264 -> NV12 out, pixel-verified.
$(BUILD)/test-aw-decode: $(AW_CORE) $(BACKEND) h264_gen.c test_aw_decode.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(AW_CORE) $(BACKEND) h264_gen.c test_aw_decode.c $(VA_LIBS)

$(BUILD)/apple-videotoolbox.so: apple-videotoolbox.c $(AW_CORE) $(BACKEND) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter -fPIC -shared $(QDP_INC) $(VFU_INC) $(VA_CFLAGS) \
	    -o $@ apple-videotoolbox.c $(AW_CORE) $(BACKEND) $(VA_LIBS)

$(BUILD)/test-plugin: test_plugin.c $(BUILD)/apple-videotoolbox.so | $(BUILD)
	$(CC) $(CFLAGS) $(QDP_INC) $(VFU_INC) -o $@ test_plugin.c -ldl

plugin: $(BUILD)/apple-videotoolbox.so

test: $(BUILD)/test-proto $(BUILD)/test-virtq $(BUILD)/test-aw-wire $(BUILD)/test-aw-bplist $(BUILD)/test-aw-session
	@echo "== Apple real-wire codec =="; $(BUILD)/test-aw-wire
	@echo "== binary-plist encoder (H4) =="; $(BUILD)/test-aw-bplist
	@echo "== Apple-wire session engine (H3/H5/H6, mock backend) =="; $(BUILD)/test-aw-session
	@echo "== protocol handler =="; $(BUILD)/test-proto
	@echo "== virtio virtqueue =="; $(BUILD)/test-virtq
ifeq ($(HAVE_VA),1)
	@$(MAKE) --no-print-directory $(BUILD)/test-vaapi $(BUILD)/test-h264 $(BUILD)/test-plugin
	@echo "== VA-API backend (live render node) =="; \
	 $(BUILD)/test-vaapi; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(vaapi test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== H.264 I_PCM decode (live, pixel-verified) =="; \
	 $(BUILD)/test-h264; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(h264 test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@$(MAKE) --no-print-directory $(BUILD)/test-aw-decode
	@echo "== MILESTONE: Apple-wire bytes in -> NV12 out (live) =="; \
	 $(BUILD)/test-aw-decode; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(milestone test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== plugin ABI smoke =="; $(BUILD)/test-plugin $(BUILD)/apple-videotoolbox.so
else
	@echo "(libva not found via pkg-config: skipping VA-API backend + plugin tests)"
endif

clean:
	rm -rf $(BUILD)
