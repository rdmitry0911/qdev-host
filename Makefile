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
PLUGINS := plugins/tb-vga/tb-vga.so

all: qdev-host $(PLUGINS) tools/display1-input-bench

vfu: $(VFU_LOCAL)/lib/libvfio-user.so.0
$(VFU_LOCAL)/lib/libvfio-user.so.0: $(wildcard $(VFU_SRC)/lib/*.[ch] $(VFU_SRC)/include/*.h $(VFU_SRC)/include/*/*.h)
	[ -f build/vfu-build/build.ninja ] || meson setup build/vfu-build $(VFU_SRC) --prefix=$(VFU_LOCAL) --libdir=lib -Dbuildtype=release
	ninja -C build/vfu-build install >/dev/null
	touch $@

dbus/dbus-display1.c dbus/dbus-display1.h: dbus/dbus-display1.xml
	cd dbus && gdbus-codegen dbus-display1.xml --glib-min-required 2.64 --output-directory . \
	    --interface-prefix org.qemu. --c-namespace QemuDBus --generate-c-code dbus-display1

qdev-host: qdev-host.c kms-sink.c kms-sink.h dbus/dbus-display1.c dbus/dbus-display1.h qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -I. $(VFU_CFLAGS) $(GLIB_CFLAGS) $(DRM_CFLAGS) -o $@ qdev-host.c kms-sink.c dbus/dbus-display1.c $(VFU_LIBS) $(GLIB_LIBS) $(DRM_LIBS) -ldl -lpthread

tools/display1-input-bench: tools/display1-input-bench.c
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -o $@ $< $(GLIB_LIBS)

plugins/%.so: plugins/%.c qdp.h $(VFU_DEP)
	$(CC) $(CFLAGS) -fPIC -shared -I. $(VFU_CFLAGS) -o $@ $< $(VFU_LIBS) -lpthread

install: all
	install -D -m 755 qdev-host $(DESTDIR)$(PREFIX)/bin/qdev-host
	install -D -m 644 qdp.h $(DESTDIR)$(PREFIX)/include/qdev-host/qdp.h
	install -D -m 755 plugins/tb-vga/tb-vga.so $(DESTDIR)$(PREFIX)/lib/qdev-host/tb-vga.so
	install -D -m 644 plugins/tb-vga/edid-1280x800.bin $(DESTDIR)$(PREFIX)/share/qdev-host/edid-1280x800.bin

clean:
	rm -f qdev-host plugins/*/*.so tools/display1-input-bench dbus/dbus-display1.c dbus/dbus-display1.h

distclean: clean
	rm -rf build

PREFIX ?= /usr/local
.PHONY: all clean distclean install vfu
