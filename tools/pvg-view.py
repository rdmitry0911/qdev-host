#!/usr/bin/env python3
"""Minimal interactive viewer for a qdev-host display when QEMU has no graphic
console (the GPU is the external device): frames come from the qdev-host
control socket (`screendump FILE`, which waits for the next presented frame),
keyboard and mouse go to the VM's QEMU through QMP input-send-event
(absolute pointer for usb-tablet, keys as QEMU "number" values from evdev
keycodes).

usage: pvg-view.py --control CTL.sock --qmp QMP.sock [--size 1600x900] [--title T]
QEMU's QMP socket serves one client: do not run another QMP user meanwhile.
"""
import argparse, json, os, socket, tempfile, threading, time
import gi
gi.require_version('Gtk', '3.0'); gi.require_version('GdkPixbuf', '2.0')
from gi.repository import Gtk, Gdk, GdkPixbuf, GLib

ap = argparse.ArgumentParser()
ap.add_argument('--control', required=True); ap.add_argument('--qmp', required=True)
ap.add_argument('--size', default='1600x900'); ap.add_argument('--title', default='macOS guest (qdev-host)')
a = ap.parse_args()
W, H = (int(v) for v in a.size.split('x'))

# evdev keycode -> QEMU qnum (AT set 1; extended keys get 0x80|code)
EXT = {96: 0x9c, 97: 0x9d, 98: 0xb5, 99: 0xb7, 100: 0xb8, 102: 0xc7, 103: 0xc8, 104: 0xc9, 105: 0xcb,
       106: 0xcd, 107: 0xcf, 108: 0xd0, 109: 0xd1, 110: 0xd2, 111: 0xd3, 125: 0xdb, 126: 0xdc, 127: 0xdd}
def qnum(evdev):
    if evdev in EXT: return EXT[evdev]
    return evdev if 0 < evdev <= 88 else None

class QMP:
    def __init__(self, path):
        self.lock = threading.Lock(); self.path = path; self.s = None
    def _connect(self):
        s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(self.path)
        f = s.makefile('rwb'); f.readline()
        f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
        self.s, self.f = s, f
    def cmd(self, name, args):
        with self.lock:
            try:
                if self.s is None: self._connect()
                self.f.write((json.dumps({'execute': name, 'arguments': args, 'id': 1}) + '\n').encode()); self.f.flush()
                while True:
                    r = json.loads(self.f.readline())
                    if r.get('id') == 1: return r
            except Exception:
                self.s = None
    def events(self, ev): self.cmd('input-send-event', {'events': ev})

qmp = QMP(a.qmp)
shot = os.path.join(tempfile.mkdtemp(prefix='pvg-view-', dir='/dev/shm'), 'frame.ppm')
state = {'pix': None, 'gw': 1920, 'gh': 1080, 'seq': None, 'run': True}

def grab_loop():
    while state['run']:
        try:
            s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(a.control)
            s.sendall(f'screendump {shot}\n'.encode()); r = s.makefile().readline().strip(); s.close()
            if r.startswith('ok'):
                parts = r.split()
                if len(parts) >= 3 and parts[1] != state['seq']:
                    state['seq'] = parts[1]
                    pb = GdkPixbuf.Pixbuf.new_from_file(shot)
                    GLib.idle_add(show, pb)
                    continue
            time.sleep(0.05)
        except Exception:
            time.sleep(0.5)

def show(pb):
    state['gw'], state['gh'] = pb.get_width(), pb.get_height()
    aw, ah = area.get_allocated_width(), area.get_allocated_height()
    state['pix'] = pb.scale_simple(max(1, aw), max(1, ah), GdkPixbuf.InterpType.BILINEAR)
    area.queue_draw(); return False

def draw(w, cr):
    if state['pix'] is not None:
        Gdk.cairo_set_source_pixbuf(cr, state['pix'], 0, 0); cr.paint()
    return False

def abs_ev(x, y):
    aw, ah = max(1, area.get_allocated_width()), max(1, area.get_allocated_height())
    return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(min(max(x / aw, 0), 1) * 32767)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(min(max(y / ah, 0), 1) * 32767)}}]

last_move = [0.0]
def motion(w, e):
    now = time.monotonic()
    if now - last_move[0] < 0.015: return True
    last_move[0] = now; threading.Thread(target=qmp.events, args=(abs_ev(e.x, e.y),), daemon=True).start(); return True
BTN = {1: 'left', 2: 'middle', 3: 'right'}
def button(w, e, down):
    if e.button in BTN:
        qmp.events(abs_ev(e.x, e.y) + [{'type': 'btn', 'data': {'down': down, 'button': BTN[e.button]}}])
    return True
def scroll(w, e):
    b = {Gdk.ScrollDirection.UP: 'wheel-up', Gdk.ScrollDirection.DOWN: 'wheel-down'}.get(e.direction)
    if b:
        qmp.events([{'type': 'btn', 'data': {'down': True, 'button': b}}]); qmp.events([{'type': 'btn', 'data': {'down': False, 'button': b}}])
    return True
def key(w, e, down):
    q = qnum(e.hardware_keycode - 8)
    if q is not None: qmp.events([{'type': 'key', 'data': {'down': down, 'key': {'type': 'number', 'data': q}}}])
    return True

win = Gtk.Window(title=a.title); win.set_default_size(W, H)
area = Gtk.DrawingArea(); area.set_can_focus(True)
area.add_events(Gdk.EventMask.POINTER_MOTION_MASK | Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
                Gdk.EventMask.SCROLL_MASK | Gdk.EventMask.KEY_PRESS_MASK | Gdk.EventMask.KEY_RELEASE_MASK)
area.connect('draw', draw); area.connect('motion-notify-event', motion)
area.connect('button-press-event', lambda w, e: button(w, e, True)); area.connect('button-release-event', lambda w, e: button(w, e, False))
area.connect('scroll-event', scroll)
win.connect('key-press-event', lambda w, e: key(w, e, True)); win.connect('key-release-event', lambda w, e: key(w, e, False))
win.add(area); win.connect('destroy', lambda *_: (state.update(run=False), Gtk.main_quit()))
win.show_all(); area.grab_focus()
threading.Thread(target=grab_loop, daemon=True).start()
Gtk.main()
