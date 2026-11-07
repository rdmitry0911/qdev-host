# qdev-host: out-of-process device host for stock QEMU (vfio-user) with
# org.qemu.Display1-compatible display export. Plugins are shared objects.
#
# libvfio-user: either installed with a pkg-config file, or give its install
# prefix in VFU_PREFIX (include/vfio-user, lib*/libvfio-user.so).
VFU_PREFIX ?= /usr/local
ifneq ($(shell pkg-config --exists libvfio-user && echo yes),)
VFU_CFLAGS := $(shell pkg-config --cflags libvfio-user)
VFU_LIBS   := $(shell pkg-config --libs libvfio-user)
else
VFU_LIBDIR := $(firstword $(wildcard $(VFU_PREFIX)/lib/x86_64-linux-gnu $(VFU_PREFIX)/lib))
VFU_CFLAGS := -I$(VFU_PREFIX)/include/vfio-user
VFU_LIBS   := -L$(VFU_LIBDIR) -lvfio-user -Wl,-rpath,$(VFU_LIBDIR)
endif
GLIB_CFLAGS := $(shell pkg-config --cflags gio-unix-2.0 glib-2.0)
GLIB_LIBS   := $(shell pkg-config --libs gio-unix-2.0 glib-2.0)
CFLAGS ?= -O2 -g -Wall -Wno-unused-parameter
PLUGINS := plugins/tb-vga/tb-vga.so

all: qdev-host $(PLUGINS)

dbus/dbus-display1.c dbus/dbus-display1.h: dbus/dbus-display1.xml
	cd dbus && gdbus-codegen dbus-display1.xml --glib-min-required 2.64 --output-directory . \
	    --interface-prefix org.qemu. --c-namespace QemuDBus --generate-c-code dbus-display1

qdev-host: qdev-host.c dbus/dbus-display1.c dbus/dbus-display1.h qdp.h
	$(CC) $(CFLAGS) -I. $(VFU_CFLAGS) $(GLIB_CFLAGS) -o $@ qdev-host.c dbus/dbus-display1.c $(VFU_LIBS) $(GLIB_LIBS) -ldl

plugins/%.so: plugins/%.c qdp.h
	$(CC) $(CFLAGS) -fPIC -shared -I. $(VFU_CFLAGS) -o $@ $< $(VFU_LIBS)

install: all
	install -D -m 755 qdev-host $(DESTDIR)$(PREFIX)/bin/qdev-host
	install -D -m 644 qdp.h $(DESTDIR)$(PREFIX)/include/qdev-host/qdp.h
	install -D -m 755 plugins/tb-vga/tb-vga.so $(DESTDIR)$(PREFIX)/lib/qdev-host/tb-vga.so
	install -D -m 644 plugins/tb-vga/edid-1280x800.bin $(DESTDIR)$(PREFIX)/share/qdev-host/edid-1280x800.bin

clean:
	rm -f qdev-host plugins/*/*.so dbus/dbus-display1.c dbus/dbus-display1.h

PREFIX ?= /usr/local
.PHONY: all clean install
