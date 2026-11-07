#!/usr/bin/env python3
"""Send Mouse.SetAbsPosition(x, y) to a Display1 endpoint (p2p or bus).
usage: display1-abs-test.py unix:path=SOCK [destination|-] X Y"""
import sys, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
addr, dest = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None)
x, y = int(sys.argv[3]), int(sys.argv[4])
flags = (Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION) if dest else Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
c = Gio.DBusConnection.new_for_address_sync(addr, flags, None, None)
c.call_sync(dest, "/org/qemu/Display1/Console_0", "org.qemu.Display1.Mouse", "SetAbsPosition",
            GLib.Variant("(uu)", (x, y)), None, Gio.DBusCallFlags.NONE, 5000, None)
print(f"sent SetAbsPosition({x}, {y})")
