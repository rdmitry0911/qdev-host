#!/usr/bin/env python3
"""Register a Display1 listener on a qdev-host (or QEMU) console, record what
arrives for SECONDS and print a summary: scanout, updates (with the frame seq
that test-v2 puts in pixel (0,0)), CursorDefine and MouseSet.

usage: display1-listener-test.py unix:path=SOCK SECONDS [--expect-cursor] [--expect-frames N]
Exit status 1 when an expectation fails."""
import os, socket, sys, json, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

addr, seconds = sys.argv[1], float(sys.argv[2])
expect_cursor = "--expect-cursor" in sys.argv
expect_frames = int(sys.argv[sys.argv.index("--expect-frames") + 1]) if "--expect-frames" in sys.argv else 0
xml = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dbus", "dbus-display1.xml")).read()
node = Gio.DBusNodeInfo.new_for_xml(xml)
iface = node.lookup_interface("org.qemu.Display1.Listener")

st = {"scanout": None, "updates": 0, "update_seqs": [], "disable": 0, "cursor_define": [], "mouse_set": [], "other": []}

def seq_of(data):
    return data[0] | data[1] << 8 | data[2] << 16 if len(data) >= 4 else None

def on_call(conn, sender, path, iface_name, method, params, inv):
    # unpack scalars only: large "ay" payloads go through get_data_as_bytes()
    n = params.n_children()
    a = [params.get_child_value(i) for i in range(n)]
    v = [x.get_data_as_bytes().get_data() if x.get_type_string() == "ay" else x.unpack() for x in a]
    if method == "Scanout":
        w, h, stride, fmt, data = v
        st["scanout"] = {"w": w, "h": h, "stride": stride, "format": hex(fmt), "bytes": len(data), "seq": seq_of(data)}
    elif method == "Update":
        x, y, w, h, stride, fmt, data = v
        st["updates"] += 1
        if x == 0 and y == 0: st["update_seqs"].append(seq_of(data))
    elif method == "Disable":
        st["disable"] += 1
    elif method == "CursorDefine":
        w, h, hx, hy, data = v
        opaque = sum(1 for i in range(3, len(data), 4) if data[i])
        st["cursor_define"].append({"w": w, "h": h, "hot": [hx, hy], "bytes": len(data), "opaque_px": opaque})
    elif method == "MouseSet":
        st["mouse_set"].append(v)
    else:
        st["other"].append(method)
    inv.return_value(None)

def on_get(conn, sender, path, iface_name, prop):
    return GLib.Variant("as", []) if prop == "Interfaces" else None

flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
c = Gio.DBusConnection.new_for_address_sync(addr, flags, None, None)
ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
fds = Gio.UnixFDList.new_from_array([theirs.fileno()])
c.call_with_unix_fd_list_sync(None, "/org/qemu/Display1/Console_0", "org.qemu.Display1.Console", "RegisterListener",
                              GLib.Variant("(h)", (0,)), None, Gio.DBusCallFlags.NONE, 5000, fds, None)
theirs.close()
sconn = Gio.Socket.new_from_fd(ours.detach()).connection_factory_create_connection()
lc = Gio.DBusConnection.new_sync(sconn, None, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
                                 Gio.DBusConnectionFlags.DELAY_MESSAGE_PROCESSING, None, None)
lc.register_object("/org/qemu/Display1/Listener", iface, on_call, on_get, None)
lc.start_message_processing()
loop = GLib.MainLoop()
GLib.timeout_add(int(seconds * 1000), loop.quit)
loop.run()

seqs = [s for s in st["update_seqs"] if s is not None]
summary = {"scanout": st["scanout"], "updates": st["updates"], "first_seq": seqs[0] if seqs else None,
           "last_seq": seqs[-1] if seqs else None, "seq_monotonic": all(b >= a for a, b in zip(seqs, seqs[1:])),
           "disable": st["disable"], "cursor_defines": st["cursor_define"][:3], "n_cursor_defines": len(st["cursor_define"]),
           "n_mouse_set": len(st["mouse_set"]), "mouse_set_first": st["mouse_set"][:3], "other": st["other"]}
print(json.dumps(summary, indent=1))
ok = True
def check(cond, what):
    global ok
    print(("PASS: " if cond else "FAIL: ") + what)
    ok &= bool(cond)
check(st["scanout"] is not None, "Scanout received")
if expect_frames:
    check(st["updates"] >= expect_frames and summary["seq_monotonic"], f"at least {expect_frames} updates, frame seq monotonic")
if expect_cursor:
    cd = st["cursor_define"]
    check(cd and cd[0]["w"] == 32 and cd[0]["h"] == 32 and cd[0]["bytes"] == 32 * 32 * 4 and cd[0]["opaque_px"] > 0,
          "CursorDefine 32x32 with an image")
    sc = st["scanout"] or {"w": 0, "h": 0}
    check(st["mouse_set"] and all(0 <= m[0] < sc["w"] and 0 <= m[1] < sc["h"] and m[2] == 1 for m in st["mouse_set"]),
          "MouseSet positions inside the scanout, visible")
sys.exit(0 if ok else 1)
