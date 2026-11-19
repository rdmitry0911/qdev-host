#!/usr/bin/env python3
"""Audio client against a Display1 endpoint, the way QSM Direct's worker is one: find
/org/qemu/Display1/Audio, RegisterOutListener with a socketpair fd, serve
org.qemu.Display1.AudioOutListener on our end of that pair and count what QEMU sends.

usage: display1-audio-test.py ADDRESS [DEST|-] [SECONDS] [--expect-absent] [--min-writes N] [--min-peak P]
  ADDRESS  unix:path=SOCK (p2p, DEST '-') or a bus address with DEST org.qemu
Prints one RESULT line; exit 0 when the expectation holds:
  default          Audio present, RegisterOutListener ok, >= 1 Init from QEMU
  --expect-absent  no Audio object and no Audio in VM.Interfaces
  --min-writes N   additionally >= N Write calls; --min-peak P: max |S16| sample >= P
"""
import os, socket, struct, sys, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

argv = [a for a in sys.argv[1:] if not a.startswith("--")]
flags_ = sys.argv[1:]
def opt(name, default):
    return type(default)(flags_[flags_.index(name) + 1]) if name in flags_ else default
addr = argv[0]; dest = argv[1] if len(argv) > 1 and argv[1] != "-" else None
listen = float(argv[2]) if len(argv) > 2 else 2.0
expect_absent = "--expect-absent" in flags_
min_writes, min_peak = opt("--min-writes", 0), opt("--min-peak", 0)
XML = os.environ.get("DISPLAY1_XML") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dbus", "dbus-display1.xml")
node = Gio.DBusNodeInfo.new_for_xml(open(XML).read())
out_iface = node.lookup_interface("org.qemu.Display1.AudioOutListener")

flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | (Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION if dest else 0)
c = Gio.DBusConnection.new_for_address_sync(addr, flags, None, None)

def get_all(path, iface):
    try:
        r = c.call_sync(dest, path, "org.freedesktop.DBus.Properties", "GetAll", GLib.Variant("(s)", (iface,)),
                        None, Gio.DBusCallFlags.NONE, 5000, None)
        return r.unpack()[0]
    except GLib.Error as e:
        return None

vm = get_all("/org/qemu/Display1/VM", "org.qemu.Display1.VM") or {}
vm_has_audio = "org.qemu.Display1.Audio" in vm.get("Interfaces", [])
audio = get_all("/org/qemu/Display1/Audio", "org.qemu.Display1.Audio")
print(f"VM.Interfaces={vm.get('Interfaces')} Audio={'present NSamples=%s' % audio.get('NSamples') if audio else 'absent'}")
if expect_absent:
    ok = audio is None and not vm_has_audio
    print(f"RESULT {'PASS' if ok else 'FAIL'} expect-absent audio={'present' if audio else 'absent'} vm_lists_audio={vm_has_audio}")
    sys.exit(0 if ok else 1)
if audio is None:
    print("RESULT FAIL Audio object absent"); sys.exit(1)

stats = {"init": [], "enabled": 0, "volume": 0, "writes": 0, "bytes": 0, "peak": 0, "fini": 0}
def on_call(conn, sender, path, iface, method, params, inv):
    p = params.unpack()
    if method == "Init":
        stats["init"].append(p); print(f"  Init id={p[0]:#x} bits={p[1]} signed={p[2]} float={p[3]} freq={p[4]} ch={p[5]}")
    elif method == "SetEnabled":
        stats["enabled"] += 1; print(f"  SetEnabled id={p[0]:#x} {p[1]}")
    elif method == "SetVolume":
        stats["volume"] += 1
    elif method == "Fini":
        stats["fini"] += 1
    elif method == "Write":
        data = bytes(p[1]); stats["writes"] += 1; stats["bytes"] += len(data)
        if len(data) >= 2:
            n = len(data) // 2
            stats["peak"] = max(stats["peak"], max(abs(v) for v in struct.unpack("<%dh" % n, data[:2 * n])))
    inv.return_value(None)
def on_get(conn, sender, path, iface, prop):
    return GLib.Variant("as", []) if prop == "Interfaces" else None

ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
fds = Gio.UnixFDList.new()
idx = fds.append(theirs.fileno())
try:
    c.call_with_unix_fd_list_sync(dest, "/org/qemu/Display1/Audio", "org.qemu.Display1.Audio", "RegisterOutListener",
                                  GLib.Variant("(h)", (idx,)), None, Gio.DBusCallFlags.NONE, 5000, fds, None)
except GLib.Error as e:
    print(f"RESULT FAIL RegisterOutListener: {e.message}"); sys.exit(1)
theirs.close()
print("RegisterOutListener ok")
sc = Gio.Socket.new_from_fd(ours.detach()).connection_factory_create_connection()
lc = Gio.DBusConnection.new_sync(sc, None, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
                                 Gio.DBusConnectionFlags.DELAY_MESSAGE_PROCESSING, None, None)
lc.register_object("/org/qemu/Display1/AudioOutListener", out_iface, on_call, on_get, None)
lc.start_message_processing()
loop = GLib.MainLoop(); GLib.timeout_add(int(listen * 1000), loop.quit); loop.run()
ok = len(stats["init"]) >= 1 and stats["writes"] >= min_writes and stats["peak"] >= min_peak
print(f"RESULT {'PASS' if ok else 'FAIL'} inits={len(stats['init'])} enabled_calls={stats['enabled']} volume_calls={stats['volume']} "
      f"writes={stats['writes']} bytes={stats['bytes']} peak_s16={stats['peak']} fini={stats['fini']}")
sys.exit(0 if ok else 1)
