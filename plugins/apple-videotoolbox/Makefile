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

LAVC_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil 2>/dev/null)
LAVC_LIBS   := $(if $(filter 0,$(VT_LAVC)),,$(shell pkg-config --libs libavcodec libavutil 2>/dev/null))
FMT_LIBS    := $(shell pkg-config --libs libavformat 2>/dev/null)

BUILD := build
HAVE_VA := $(if $(VA_LIBS),1,)
HAVE_LAVC := $(if $(LAVC_LIBS),$(HAVE_VA),)
ifeq ($(HAVE_LAVC),1)
PLUGIN_LAVC_SRC := vtpv_lavc.c
PLUGIN_LAVC_CFLAGS := -DVTPV_HAVE_LAVC $(LAVC_CFLAGS)
endif

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
AW_CORE := aw_wire.c aw_sbuf.c aw_bplist.c aw_session.c vt_scatter.c vt_land.c
$(BUILD)/test-aw-session: $(AW_CORE) test_aw_session.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(AW_CORE) test_aw_session.c

# H4: the binary-plist encoder — real Dpba/,hin/,dec bytes a VideoToolbox
# framework parses (validated by a re-parse). No libva.
$(BUILD)/test-aw-bplist: aw_bplist.c aw_bplist.h test_aw_bplist.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ aw_bplist.c test_aw_bplist.c

# debug: emit the engine's real decs ,dec bplist to a file for an independent
# CF-equivalent parse (python3 -c 'import plistlib; plistlib.load(...)').
#   make dump-decs && ./build/dump-decs /tmp/dec.plist
dump-decs: $(BUILD)/dump-decs
$(BUILD)/dump-decs: aw_wire.c aw_sbuf.c aw_bplist.c aw_session.c dump_decs.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ aw_wire.c aw_sbuf.c aw_bplist.c aw_session.c dump_decs.c

# Scattered-page landing of a decoded frame into a non-contiguous guest surface.
$(BUILD)/test-vt-scatter: vt_scatter.c vt_scatter.h test_vt_scatter.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ vt_scatter.c test_vt_scatter.c

# GVA-LAND-BOUNDS: a picture lands top-left, only what fits the lent surface.
$(BUILD)/test-vt-land: vt_land.c vt_land.h vt_scatter.c vt_scatter.h test_vt_land.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ vt_land.c vt_scatter.c test_vt_land.c

# The uniq-vgpu <-> apple-videotoolbox IOSurface-mapper bridge (header-only shm
# registry, writer in uniq-vgpu, reader here): publish/lookup/retract contract.
BRIDGE_DIR := ../uniq-vgpu
$(BUILD)/test-pvg-mapper-bridge: $(BRIDGE_DIR)/pvg_mapper_bridge.h $(BRIDGE_DIR)/pvg_mapper_bridge_test.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(BRIDGE_DIR)/pvg_mapper_bridge_test.c

$(BUILD)/test-virtq: vtpv_virtq.c vtpv_proto.c test_virtq.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ vtpv_virtq.c vtpv_proto.c test_virtq.c

# NVDEC backend (CLEAN-ROOM from NVIDIA's Video Codec SDK API; see the headers):
# libnvcuvid + libcuda are loaded at run time with dlopen, so this links only
# -ldl and builds on a host with no NVIDIA driver; the decode test skips
# (exit 77) where NVDEC is not present.
NVDEC_BACKEND := vtpv_proto.c vtpv_nvdec.c vtpv_h264.c vtpv_virtq.c

$(BUILD)/test-nvdec: $(NVDEC_BACKEND) h264_gen.c test_nvdec.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(NVDEC_BACKEND) h264_gen.c test_nvdec.c -ldl

BACKEND := vtpv_proto.c vtpv_vaapi.c vtpv_h264.c vtpv_virtq.c

$(BUILD)/test-vaapi: $(BACKEND) test_vaapi.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(BACKEND) test_vaapi.c $(VA_LIBS)

$(BUILD)/test-h264: $(BACKEND) h264_gen.c test_h264.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(BACKEND) h264_gen.c test_h264.c $(VA_LIBS)

# THE HOST MILESTONE: real Apple contract-B wire bytes -> aw_session engine ->
# live VA-API H.264 -> NV12 out, pixel-verified.
$(BUILD)/test-aw-decode: $(AW_CORE) $(BACKEND) h264_gen.c test_aw_decode.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(AW_CORE) $(BACKEND) h264_gen.c test_aw_decode.c $(VA_LIBS)

# L-GVA-DECODECONTENT: replay LIVE-captured Apple-wire bytes (avcC param sets +
# avcC-framed slice-only AU) through the engine + live VA-API. Not in `make test`
# (needs the captured fixtures); run manually:
#   make build/test-aw-replay && ./build/test-aw-replay \
#     /dev/dri/renderD128 <avcc.bin> <sample.bin> <luma_ref.bin> 352 288
$(BUILD)/test-aw-replay: $(AW_CORE) $(BACKEND) test_aw_replay.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) -o $@ $(AW_CORE) $(BACKEND) test_aw_replay.c $(VA_LIBS)

# The device links the NVDEC backend too (vtpv_nvdec.c); it loads libnvcuvid +
# libcuda at run time with dlopen, so the .so needs only -ldl and still loads on
# a host with no NVIDIA driver (NVDEC open then fails cleanly, VA-API is used).
# The stream decode core (vtpv_lavc) — libavcodec + VA-API/CUDA hwaccel, DPB
# kept across frames (plan U1) — is linked in when libavcodec is found.
$(BUILD)/test-lavc: vtpv_lavc.c vtpv_lavc.h test_lavc.c test_lavc_util.c test_lavc_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) $(LAVC_CFLAGS) -o $@ vtpv_lavc.c test_lavc.c test_lavc_util.c $(LAVC_LIBS) $(FMT_LIBS) $(VA_LIBS)

$(BUILD)/test-aw-replay-lavc: $(AW_CORE) vtpv_lavc.c test_aw_replay_lavc.c test_lavc_util.c test_lavc_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) $(LAVC_CFLAGS) -o $@ $(AW_CORE) vtpv_lavc.c test_aw_replay_lavc.c test_lavc_util.c $(LAVC_LIBS) $(FMT_LIBS) $(VA_LIBS)

# GVA 4K landing: decode + CPU-readable picture + scattered-page landing (shadow vs
# direct), byte-identical + software-oracle MD5, per-frame ms.
#   ./build/test-lavc-land [/dev/dri/renderD128] [frames]
$(BUILD)/test-lavc-land: vtpv_lavc.c vtpv_lavc.h vt_scatter.c vt_scatter.h test_lavc_land.c test_lavc_util.c test_lavc_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) $(LAVC_CFLAGS) -o $@ vtpv_lavc.c vt_scatter.c test_lavc_land.c test_lavc_util.c $(LAVC_LIBS) $(FMT_LIBS) $(VA_LIBS)

# GVA-LAND-BOUNDS on a real decoded picture taller than its description (production
# core, software decode): the pre-fix contiguous path's calls write past a one-page
# surface; the fixed landing keeps every guard byte.
$(BUILD)/test-vt-land-lavc: vtpv_lavc.c vt_land.c vt_scatter.c test_vt_land_lavc.c test_lavc_util.c test_lavc_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) $(LAVC_CFLAGS) -o $@ vtpv_lavc.c vt_land.c vt_scatter.c test_vt_land_lavc.c test_lavc_util.c $(LAVC_LIBS) $(FMT_LIBS) $(VA_LIBS)

# Stream core on a recorded payload per backend (VA-API or CUDA/NVDEC): every unit
# decoded + landed, packed NV12 MD5 == the payload's software-oracle MD5 per decode
# index. Needs no encoder: builds against the package's bundled libavcodec too
# (PKG_CONFIG_PATH=<bundled prefix>/lib/pkgconfig) for the NVIDIA hosts.
#   ./build/test-lavc-payload vaapi|cuda <render node|cuda ordinal> <payload dir>...
$(BUILD)/test-lavc-payload: vtpv_lavc.c vtpv_lavc.h vt_scatter.c vt_scatter.h vt_land.c vt_land.h test_lavc_payload.c | $(BUILD)
	$(CC) $(CFLAGS) $(VA_CFLAGS) $(LAVC_CFLAGS) -o $@ vtpv_lavc.c vt_scatter.c vt_land.c test_lavc_payload.c $(LAVC_LIBS) $(VA_LIBS)

# E2E payload writer (guest multi-frame decode check: record + samples + oracle MD5s).
#   make build/gva-payload && ./build/gva-payload <dir> h264|hevc|hevc10 <w> <h> <n>
$(BUILD)/gva-payload: gva_payload.c test_lavc_util.c test_lavc_util.h | $(BUILD)
	$(CC) $(CFLAGS) $(LAVC_CFLAGS) -o $@ gva_payload.c test_lavc_util.c $(LAVC_LIBS) $(FMT_LIBS)

$(BUILD)/apple-videotoolbox.so: apple-videotoolbox.c $(AW_CORE) $(BACKEND) vtpv_nvdec.c $(PLUGIN_LAVC_SRC) $(BRIDGE_DIR)/pvg_mapper_bridge.h vt_scatter.h vt_land.h | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter -fPIC -shared $(QDP_INC) $(VFU_INC) $(VA_CFLAGS) $(PLUGIN_LAVC_CFLAGS) \
	    -o $@ apple-videotoolbox.c $(AW_CORE) $(BACKEND) vtpv_nvdec.c $(PLUGIN_LAVC_SRC) $(VA_LIBS) $(if $(PLUGIN_LAVC_SRC),$(LAVC_LIBS)) -ldl

$(BUILD)/test-plugin: test_plugin.c $(BUILD)/apple-videotoolbox.so | $(BUILD)
	$(CC) $(CFLAGS) $(QDP_INC) $(VFU_INC) -o $@ test_plugin.c -ldl

plugin: $(BUILD)/apple-videotoolbox.so

test: $(BUILD)/test-proto $(BUILD)/test-virtq $(BUILD)/test-aw-wire $(BUILD)/test-aw-bplist $(BUILD)/test-aw-session $(BUILD)/test-vt-scatter \
      $(BUILD)/test-pvg-mapper-bridge $(BUILD)/test-vt-land
	@echo "== Apple real-wire codec =="; $(BUILD)/test-aw-wire
	@echo "== binary-plist encoder (H4) =="; $(BUILD)/test-aw-bplist
	@echo "== Apple-wire session engine (H3/H5/H6, mock backend) =="; $(BUILD)/test-aw-session
	@echo "== scattered-page landing =="; $(BUILD)/test-vt-scatter
	@echo "== landing bounds (GVA-LAND-BOUNDS) =="; $(BUILD)/test-vt-land
	@echo "== IOSurface-mapper bridge (uniq-vgpu writer / VT reader) =="; $(BUILD)/test-pvg-mapper-bridge
	@echo "== protocol handler =="; $(BUILD)/test-proto
	@echo "== virtio virtqueue =="; $(BUILD)/test-virtq
	@$(MAKE) --no-print-directory $(BUILD)/test-nvdec
	@echo "== NVDEC H.264 I_PCM decode (live NVDEC, pixel-verified) =="; \
	 $(BUILD)/test-nvdec; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(nvdec test skipped: no NVIDIA GPU / NVDEC)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
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
ifeq ($(HAVE_LAVC),1)
	@$(MAKE) --no-print-directory $(BUILD)/test-lavc $(BUILD)/test-aw-replay-lavc $(BUILD)/test-lavc-land \
	   $(BUILD)/test-lavc-payload $(BUILD)/gva-payload $(BUILD)/test-vt-land-lavc
	@echo "== landing bounds on a decoded picture taller than its description (GVA-LAND-BOUNDS) =="; \
	 $(BUILD)/test-vt-land-lavc > $(BUILD)/test-vt-land-lavc.out 2>&1; rc=$$?; grep -v '^\[lib' $(BUILD)/test-vt-land-lavc.out; \
	 if [ $$rc = 77 ]; then echo "(land-bounds lavc test skipped: no encoder)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== stream decode core (libavcodec + VA-API, decode order vs software oracle) =="; \
	 $(BUILD)/test-lavc; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(lavc test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== 4K landing: decode + direct scattered landing == shadow landing == software oracle =="; \
	 $(BUILD)/test-lavc-land; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(landing test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== stream core on recorded payloads (H.264 IPB 640x360, HEVC IPB 1280x720), VA-API then CUDA =="; \
	 mkdir -p $(BUILD)/pl-h264 $(BUILD)/pl-hevc $(BUILD)/pl-hevc10 $(BUILD)/pl-hevc10-4k; \
	 $(BUILD)/gva-payload $(BUILD)/pl-h264 h264 640 360 30 >/dev/null 2>&1 && \
	 $(BUILD)/gva-payload $(BUILD)/pl-hevc hevc 1280 720 16 >/dev/null 2>&1 && \
	 $(BUILD)/gva-payload $(BUILD)/pl-hevc10 hevc10 1280 720 16 >/dev/null 2>&1 && \
	 $(BUILD)/gva-payload $(BUILD)/pl-hevc10-4k hevc10 3840 2160 6 >/dev/null 2>&1 || { echo "(payload test skipped: no encoder)"; exit 0; }; \
	 $(BUILD)/test-lavc-payload vaapi /dev/dri/renderD128 $(BUILD)/pl-h264 $(BUILD)/pl-hevc $(BUILD)/pl-hevc10 $(BUILD)/pl-hevc10-4k; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(vaapi payload test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi; \
	 $(BUILD)/test-lavc-payload cuda 0 $(BUILD)/pl-h264 $(BUILD)/pl-hevc $(BUILD)/pl-hevc10 $(BUILD)/pl-hevc10-4k; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(cuda payload test skipped: no NVIDIA GPU, or not the bundled libavcodec)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
	@echo "== Apple-wire engine + stream core, multi-frame IPB/HEVC through dfrm/Dbuf/Demt =="; \
	 $(BUILD)/test-aw-replay-lavc; rc=$$?; \
	 if [ $$rc = 77 ]; then echo "(engine+lavc test skipped: no usable render node)"; \
	 elif [ $$rc != 0 ]; then exit $$rc; fi
else
	@echo "(libavcodec not found: stream decode core not built, legacy backends only)"
endif
else
	@echo "(libva not found via pkg-config: skipping VA-API backend + plugin tests)"
endif

clean:
	rm -rf $(BUILD)
