#!/usr/bin/env python3
"""Clipboard client against a Display1 endpoint: Register, Grab, listen, Release, Unregister.
Exports its own org.qemu.Display1.Clipboard object so the server can call Grab/Release/Request back.
usage: display1-clipboard-test.py unix:path=SOCK [destination|-] [seconds-to-listen]"""
import sys, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
addr = sys.argv[1]; dest = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
listen = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
XML = """<node><interface name="org.qemu.Display1.Clipboard">
<method name="Register"/><method name="Unregister"/>
<method name="Grab"><arg type="u" name="selection" direction="in"/><arg type="u" name="serial" direction="in"/><arg type="as" name="mimes" direction="in"/></method>
<method name="Release"><arg type="u" name="selection" direction="in"/></method>
<method name="Request"><arg type="u" name="selection" direction="in"/><arg type="as" name="mimes" direction="in"/><arg type="s" name="mime" direction="out"/><arg type="ay" name="data" direction="out"/></method>
<property name="Interfaces" type="as" access="read"/></interface></node>"""
flags = (Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION) if dest else Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
c = Gio.DBusConnection.new_for_address_sync(addr, flags, None, None)
events = []
def on_call(conn, sender, path, iface, method, params, inv):
    events.append((method, params.unpack()))
    print(f"  <- server called {method}{params.unpack()}")
    if method == "Request":
        inv.return_value(GLib.Variant("(say)", ("text/plain", b"hello from the test client")))
    else:
        inv.return_value(None)
info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
c.register_object("/org/qemu/Display1/Clipboard", info, on_call, None, None)
def call(m, params=None):
    return c.call_sync(dest, "/org/qemu/Display1/Clipboard", "org.qemu.Display1.Clipboard", m, params, None, Gio.DBusCallFlags.NONE, 5000, None)
call("Register"); print("Register ok")
call("Grab", GLib.Variant("(uuas)", (0, 1, ["text/plain", "UTF8_STRING"]))); print("Grab ok (selection 0, serial 1)")
loop = GLib.MainLoop(); GLib.timeout_add(int(listen * 1000), loop.quit); loop.run()
call("Release", GLib.Variant("(u)", (0,))); print("Release ok")
call("Unregister"); print("Unregister ok")
print("callbacks received:", len(events))
