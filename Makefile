# qdev-host: out-of-process device host for stock QEMU (vfio-user) with
# org.qemu.Display1-compatible display export. Plugins are shared objects.
#
# libvfio-user: the vendored copy in third_party/libvfio-user carries patches
# the host and v2 plugins need (DMA region fd/offset, thread-safe
# vfu_irq_trigger), so an upstream libvfio-user does not work. By default it is
# built with meson/ninja (needs json-c and cmocka headers) into build/vfu and
# linked with an rpath there ("make vfu" builds only the library). To use a
# copy installed elsewhere, give its prefix in VFU_PREFIX (include/vfio-user,
# lib/libvfio-user.so); PRIVLIB sets a different rpath (packages).
VFU_SRC    := third_party/libvfio-user
VFU_LOCAL  := $(CURDIR)/build/vfu
VFU_PREFIX ?= $(VFU_LOCAL)
VFU_LIBDIR := $(firstword $(wildcard $(VFU_PREFIX)/lib/x86_64-linux-gnu) $(VFU_PREFIX)/lib)
VFU_CFLAGS := -I$(VFU_PREFIX)/include/vfio-user
VFU_LIBS   := -L$(VFU_LIBDIR) -lvfio-user -Wl,-rpath,$(if $(PRIVLIB),$(PRIVLIB),$(VFU_LIBDIR))
ifeq ($(VFU_PREFIX),$(VFU_LOCAL))
VFU_DEP    := $(VFU_LOCAL)/lib/libvfio-user.so.0
endif
GLIB_CFLAGS := $(shell pkg-config --cflags gio-unix-2.0 glib-2.0)
GLIB_LIBS   := $(shell pkg-config --libs gio-unix-2.0 glib-2.0)
DRM_CFLAGS  := $(shell pkg-config --cflags libdrm)
DRM_LIBS    := $(shell pkg-config --libs libdrm)
CFLAGS ?= -O2 -g -Wall -Wno-unused-parameter
PLUGINS := plugins/tb-vga/tb-vga.so plugins/test-v2/test-v2.so plugins/test-v2/test-v2-reload.so

# uniq-vgpu (a GPU on the Apple ParavirtualizedGraphics protocol) links
# QMetal's libmlapi.so and is built only when QMETAL_INCLUDE is given:
#   make QMETAL_INCLUDE=<qmetal>/include QMETAL_LIBDIR=<dir of libmlapi.so>
# QMETAL_RPATH (default QMETAL_LIBDIR) is recorded as the plugin's rpath; with
# QMETAL_RPATH= empty, put libmlapi.so (and its libair2spirv.so.1) on
# LD_LIBRARY_PATH when starting qdev-host instead.
QMETAL_INCLUDE ?=
QMETAL_LIBDIR ?=
QMETAL_RPATH ?= $(QMETAL_LIBDIR)
UNIQ_VGPU_SRC := $(addprefix plugins/uniq-vgpu/,uniq-vgpu.c pvg-memory.c pvg-process-vm.c pvg-dmabuf.c)
UNIQ_VGPU_HDR := plugins/uniq-vgpu/pvg.h plugins/uniq-vgpu/pvg-process-vm.h \
                 plugins/uniq-vgpu/pvg_backing.h plugins/uniq-vgpu/pvg_mapper_bridge.h \
                 $(wildcard plugins/uniq-vgpu/qmu-private/*.h)
GLIB_ONLY_CFLAGS = $(shell pkg-config --cflags glib-2.0)
GLIB_ONLY_LIBS   = $(shell pkg-config --libs glib-2.0)
ifneq ($(QMETAL_INCLUDE),)
PLUGINS += plugins/uniq-vgpu/uniq-vgpu.so
endif

# apple-videotoolbox (paravirt VideoToolbox codec device, PCI 1b36:eee2; Apple's is 106b:1a03)
# is the AppleGVA host backend (increment 1). It links libva (VA-API decode)
# and is built only when VIDEOTOOLBOX=1 is given, so the default build is
# unchanged on hosts without libva:
#   make VIDEOTOOLBOX=1
# Its module unit tests (no VM) run with: make apple-videotoolbox-test
VT_DIR := plugins/apple-videotoolbox
VT_SRC := $(addprefix $(VT_DIR)/,apple-videotoolbox.c vtpv_vaapi.c vtpv_nvdec.c vtpv_h264.c \
          vtpv_virtq.c aw_wire.c aw_sbuf.c aw_bplist.c aw_session.c vt_scatter.c)
VT_HDR := $(addprefix $(VT_DIR)/,vtpv_proto.h vtpv_vaapi.h vtpv_nvdec.h vtpv_h264.h vtpv_virtq.h \
          aw_wire.h aw_sbuf.h aw_bplist.h aw_session.h vt_scatter.h) \
          plugins/uniq-vgpu/pvg_mapper_bridge.h
VT_VA_CFLAGS := $(shell pkg-config --cflags libva libva-drm 2>/dev/null)
VT_VA_LIBS   := $(shell pkg-config --libs libva libva-drm 2>/dev/null)
# The stream decode core (vtpv_lavc: libavcodec + VA-API/CUDA hwaccel, keeps the
# DPB across frames) is built in when libavcodec is found; without it the device
# falls back to the legacy intra-only backends. VT_LAVC=0 forces it off.
VT_LAVC_LIBS := $(if $(filter 0,$(VT_LAVC)),,$(shell pkg-config --libs libavcodec libavutil 2>/dev/null))
ifneq ($(VT_LAVC_LIBS),)
VT_SRC += $(VT_DIR)/vtpv_lavc.c
VT_HDR += $(VT_DIR)/vtpv_lavc.h
VT_VA_CFLAGS += -DVTPV_HAVE_LAVC $(shell pkg-config --cflags libavcodec libavutil 2>/dev/null)
VT_VA_LIBS += $(VT_LAVC_LIBS)
endif
ifneq ($(VIDEOTOOLBOX),)
PLUGINS += $(VT_DIR)/apple-videotoolbox.so
endif

all: qdev-host $(PLUGINS) tools/display1-input-bench tools/display1-uiinfo-test tools/vfu-test-client pve/libqdev-vugpu-guard.so

vfu: $(VFU_LOCAL)/lib/libvfio-user.so.0

# LD_PRELOAD library for QEMU's vhost-user-gpu (the virgl-gpu plugin's process):
# works around two faults of it (pve/vugpu-guard.c); needs the virglrenderer header.
pve/libqdev-vugpu-guard.so: pve/vugpu-guard.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(GLIB_ONLY_CFLAGS) $(shell pkg-config --cflags virglrenderer) -o $@ $< $(LDFLAGS) $(GLIB_ONLY_LIBS) -ldl
$(VFU_LOCAL)/lib/libvfio-user.so.0: $(wildcard $(VFU_SRC)/lib/*.[ch] $(VFU_SRC)/include/*.h $(VFU_SRC)/include/*/*.h)
	[ -f build/vfu-build/build.ninja ] || meson setup build/vfu-build $(VFU_SRC) --prefix=$(VFU_LOCAL) --libdir=lib -Dbuildtype=release
	ninja -C build/vfu-build install >/dev/null
	touch $@

dbus/dbus-display1.c dbus/dbus-display1.h: dbus/dbus-display1.xml
	cd dbus && gdbus-codegen dbus-display1.xml --glib-min-required 2.64 --output-directory . \
	    --interface-prefix org.qemu. --c-namespace QemuDBus --generate-c-code dbus-display1

qdev-host: qdev-host.c kms-sink.c kms-sink.h ui-info-reply.h dbus/dbus-display1.c dbus/dbus-display1.h qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -I. $(VFU_CFLAGS) $(GLIB_CFLAGS) $(DRM_CFLAGS) -o $@ qdev-host.c kms-sink.c dbus/dbus-display1.c $(VFU_LIBS) $(GLIB_LIBS) $(DRM_LIBS) -ldl -lpthread

tools/display1-input-bench: tools/display1-input-bench.c
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -o $@ $< $(GLIB_LIBS)

# Display1 SetUIInfo reply of qdev-host (ui-info-reply.h): unit cases without
# arguments ("make ui-info-reply-test"), a live call against a running
# qdev-host with ADDRESS W H EXPECT (tools/test-abi-v2.sh).
tools/display1-uiinfo-test: tools/display1-uiinfo-test.c ui-info-reply.h qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -I. $(VFU_CFLAGS) $(GLIB_CFLAGS) -o $@ $< $(GLIB_LIBS)

ui-info-reply-test: tools/display1-uiinfo-test
	tools/display1-uiinfo-test

# QEMU's Display1 Audio over its p2p dbus display through QMP (no guest; stock QEMU, dbus-daemon, python3-gi):
# make qemu-audio-p2p-test QEMU=/path/qemu-system-x86_64 QEMU_L=/path/pc-bios
qemu-audio-p2p-test: qdev-host plugins/tb-vga/tb-vga.so
	QEMU=$(QEMU) QEMU_L=$(QEMU_L) tools/test-qemu-audio-p2p.sh

# test client for plugins without a VM: speaks vfio-user itself, needs only the protocol header
tools/vfu-test-client: tools/vfu-test-client.c $(VFU_DEP)
	$(CC) $(CFLAGS) $(VFU_CFLAGS) -o $@ $< -lpthread

plugins/%.so: plugins/%.c qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -fPIC -shared -I. $(VFU_CFLAGS) -o $@ $< $(VFU_LIBS) -lpthread

# ABI 4 variant of test-v2 (counting reload) for tools/test-reload-guard.sh
plugins/test-v2/test-v2-reload.so: plugins/test-v2/test-v2.c qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -fPIC -shared -I. $(VFU_CFLAGS) -DTEST_V2_RELOAD -o $@ $< $(VFU_LIBS) -lpthread

plugins/uniq-vgpu/uniq-vgpu.so: $(UNIQ_VGPU_SRC) $(UNIQ_VGPU_HDR) qdp.h $(VFU_DEP)
	@test -n "$(QMETAL_INCLUDE)" -a -n "$(QMETAL_LIBDIR)" || \
	    { echo "uniq-vgpu needs QMETAL_INCLUDE and QMETAL_LIBDIR" >&2; exit 1; }
	$(CC) $(CFLAGS) -fPIC -shared -pthread -fvisibility=hidden -I. -Iplugins/uniq-vgpu \
	    -I$(QMETAL_INCLUDE) $(VFU_CFLAGS) $(GLIB_ONLY_CFLAGS) -o $@ $(UNIQ_VGPU_SRC) \
	    $(VFU_LIBS) -L$(QMETAL_LIBDIR) -Wl,--no-as-needed -lmlapi -Wl,--as-needed \
	    $(if $(QMETAL_RPATH),-Wl$(comma)-rpath$(comma)$(QMETAL_RPATH)) \
	    $(GLIB_ONLY_LIBS) -ldl

# Warm-swap VARIABLE backings (increment 2): tiny backend-identity .so's the
# plugin dlopen-swaps under a live guest (quiesce -> swap -> resume). The frozen
# envelope (PVG MMIO/FIFO contract) is untouched. See pvg_backing.h.
plugins/uniq-vgpu/backing_A.so: plugins/uniq-vgpu/backing_A.c plugins/uniq-vgpu/pvg_backing.h
	$(CC) $(CFLAGS) -fPIC -shared -Iplugins/uniq-vgpu -o $@ $<
plugins/uniq-vgpu/backing_B.so: plugins/uniq-vgpu/backing_B.c plugins/uniq-vgpu/pvg_backing.h
	$(CC) $(CFLAGS) -fPIC -shared -Iplugins/uniq-vgpu -o $@ $<
.PHONY: pvg-backings
pvg-backings: plugins/uniq-vgpu/backing_A.so plugins/uniq-vgpu/backing_B.so

# Self-test of the uniq-vgpu plugin on a vfu context without a VM (see
# plugins/uniq-vgpu/README.md). Runs the real libmlapi; select the Vulkan
# driver with VK_ICD_FILENAMES (e.g. lavapipe) when no GPU should be used.
plugins/uniq-vgpu/selftest: plugins/uniq-vgpu/selftest.c qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -pthread -rdynamic -I. -I$(QMETAL_INCLUDE) $(VFU_CFLAGS) -o $@ $< \
	    $(VFU_LIBS) -ldl

uniq-vgpu-selftest: plugins/uniq-vgpu/uniq-vgpu.so plugins/uniq-vgpu/selftest
	plugins/uniq-vgpu/selftest plugins/uniq-vgpu/uniq-vgpu.so
	plugins/uniq-vgpu/selftest plugins/uniq-vgpu/uniq-vgpu.so --personality 0xeee1

# Native unit test of the process-VM provider and the ordinary physical capture
# (production pvg-process-vm.c / pvg-memory.c / pvg-dmabuf.c; no VM, GPU or
# libmlapi; QMetal headers only): descriptor ownership per mapped range
# (PVM-RANGE-NOHANDLE-01), data identity, pin across the client's unregister.
PVM_TEST_SRC := $(addprefix plugins/uniq-vgpu/,pvg_process_vm_test.c pvg-process-vm.c pvg-memory.c pvg-dmabuf.c)
plugins/uniq-vgpu/pvg_process_vm_test: $(PVM_TEST_SRC) $(UNIQ_VGPU_HDR) qdp.h $(VFU_DEP)
	@test -n "$(QMETAL_INCLUDE)" || { echo "pvg_process_vm_test needs QMETAL_INCLUDE" >&2; exit 1; }
	$(CC) $(CFLAGS) -pthread -I. -Iplugins/uniq-vgpu -I$(QMETAL_INCLUDE) $(VFU_CFLAGS) $(GLIB_ONLY_CFLAGS) \
	    -o $@ $(PVM_TEST_SRC) $(GLIB_ONLY_LIBS)

uniq-vgpu-pvm-test: plugins/uniq-vgpu/pvg_process_vm_test
	plugins/uniq-vgpu/pvg_process_vm_test

# apple-videotoolbox: the multi-source codec device + libva backend. Explicit
# rule (overrides the generic plugins/%.so single-source rule).
$(VT_DIR)/apple-videotoolbox.so: $(VT_SRC) $(VT_HDR) qdp.h $(VFU_DEP)
	@test -n "$(VT_VA_LIBS)" || { echo "apple-videotoolbox needs libva (pkg-config libva libva-drm)" >&2; exit 1; }
	$(CC) $(CFLAGS) -fPIC -shared -I. -I$(VT_DIR) $(VFU_CFLAGS) $(VT_VA_CFLAGS) \
	    -o $@ $(VT_SRC) $(VFU_LIBS) $(VT_VA_LIBS) -lpthread -ldl

# Module unit tests without a VM (protocol handler always; VA-API backend live
# on a render node, skipped if none; plugin ABI smoke).
.PHONY: apple-videotoolbox-test
apple-videotoolbox-test:
	$(MAKE) -C $(VT_DIR) test

install: all
	install -D -m 755 qdev-host $(DESTDIR)$(PREFIX)/bin/qdev-host
	install -D -m 644 qdp.h $(DESTDIR)$(PREFIX)/include/qdev-host/qdp.h
	install -D -m 755 plugins/tb-vga/tb-vga.so $(DESTDIR)$(PREFIX)/lib/qdev-host/tb-vga.so
	install -D -m 644 plugins/tb-vga/edid-1280x800.bin $(DESTDIR)$(PREFIX)/share/qdev-host/edid-1280x800.bin

clean:
	rm -f qdev-host plugins/*/*.so pve/libqdev-vugpu-guard.so plugins/uniq-vgpu/selftest plugins/uniq-vgpu/pvg_process_vm_test tools/display1-input-bench tools/display1-uiinfo-test tools/vfu-test-client dbus/dbus-display1.c dbus/dbus-display1.h
	rm -rf $(VT_DIR)/build

distclean: clean
	rm -rf build

PREFIX ?= /usr/local
comma := ,
.PHONY: all clean distclean install vfu uniq-vgpu-selftest uniq-vgpu-pvm-test ui-info-reply-test qemu-audio-p2p-test
