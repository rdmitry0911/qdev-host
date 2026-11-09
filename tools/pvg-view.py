#!/usr/bin/env python3
"""Minimal interactive viewer for a qdev-host display when QEMU has no graphic
console (the GPU is the external device): frames come from the qdev-host
control socket (`screendump FILE`, which waits for the next presented frame),
keyboard and mouse go to the VM's QEMU through QMP input-send-event
(absolute pointer for usb-tablet, keys as QEMU "number" values from evdev
keycodes).

Window size -> guest display size: the viewer asks qdev-host for a display of
the window's size in device pixels (`uiinfo W H WMM HMM` on the control
socket, as a Display1 SetUIInfo) once the size has not changed for 1 s; F11
toggles fullscreen and asks at once. The guest re-lays out at that size (an
ABI 3 plugin such as apple-pvg offers it as the guest's mode); until the guest
presents at the new size the frame is shown scaled with its aspect kept
(letterbox), and the pointer maps through the same rectangle. Physical size
sent: device pixels / (4.8 x scale factor) mm, a provisional policy (4.8 px/mm
is the reference 1920 px over 400 mm). With an older qdev-host (no uiinfo) the
viewer only scales.

usage: pvg-view.py --control CTL.sock --qmp QMP.sock [--size 1600x900]
                   [--guest-size WxH] [--no-resize] [--title T]
--size: initial window size (logical pixels). --guest-size: the display size
the guest was started with (plugin xres x yres); no request is sent while the
window shows exactly that. --no-resize: never send uiinfo.
QEMU's QMP socket serves one client: do not run another QMP user meanwhile.
"""
import argparse, json, math, os, socket, sys, tempfile, threading, time
import gi
gi.require_version('Gtk', '3.0'); gi.require_version('GdkPixbuf', '2.0')
from gi.repository import Gtk, Gdk, GdkPixbuf, GLib

ap = argparse.ArgumentParser()
ap.add_argument('--control', required=True); ap.add_argument('--qmp', required=True)
ap.add_argument('--size', default='1600x900'); ap.add_argument('--title', default='macOS guest (qdev-host)')
ap.add_argument('--guest-size', default=None); ap.add_argument('--no-resize', action='store_true')
a = ap.parse_args()
W, H = (int(v) for v in a.size.split('x'))
MIN_W, MIN_H = 800, 600          # device pixels; the apple-pvg plugin refuses less
SETTLE_S = 1.0                   # quiet time before a window size is requested

# evdev keycode -> QEMU qnum (AT set 1; extended keys get 0x80|code)
EXT = {96: 0x9c, 97: 0x9d, 98: 0xb5, 99: 0xb7, 100: 0xb8, 102: 0xc7, 103: 0xc8, 104: 0xc9, 105: 0xcb,
       106: 0xcd, 107: 0xcf, 108: 0xd0, 109: 0xd1, 110: 0xd2, 111: 0xd3, 125: 0xdb, 126: 0xdc, 127: 0xdd}
def qnum(evdev):
    if evdev in EXT: return EXT[evdev]
    return evdev if 0 < evdev <= 88 else None

def log(msg):
    print(f'pvg-view: {msg}', file=sys.stderr, flush=True)

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

def control(line):
    """One command on a fresh control connection; returns the reply line."""
    s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(a.control)
    try:
        s.sendall((line + '\n').encode()); return s.makefile().readline().strip()
    finally:
        s.close()

qmp = QMP(a.qmp)
shot = os.path.join(tempfile.mkdtemp(prefix='pvg-view-', dir='/dev/shm'), 'frame.ppm')
state = {'pb': None, 'gw': 1920, 'gh': 1080, 'seq': None, 'run': True}
guest = tuple(int(v) for v in a.guest_size.split('x')) if a.guest_size else None
ui = {'enabled': not a.no_resize, 'last_sent': guest, 'timer': 0, 'immediate': False, 'fullscreen': False}

def grab_loop():
    while state['run']:
        try:
            r = control(f'screendump {shot}')
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
    if (pb.get_width(), pb.get_height()) != (state['gw'], state['gh']):
        log(f'guest display {pb.get_width()}x{pb.get_height()}')
    state['gw'], state['gh'] = pb.get_width(), pb.get_height()
    state['pb'] = pb
    area.queue_draw(); return False

def frame_rect():
    """Where the frame goes in the drawing area (logical pixels): as large as
    fits with its aspect kept, centred; 1:1 device pixels when the guest size
    equals the window's device size."""
    aw, ah = max(1, area.get_allocated_width()), max(1, area.get_allocated_height())
    gw, gh = max(1, state['gw']), max(1, state['gh'])
    k = min(aw / gw, ah / gh)
    dw, dh = gw * k, gh * k
    return (aw - dw) / 2, (ah - dh) / 2, dw, dh

def draw(w, cr):
    cr.set_source_rgb(0, 0, 0); cr.paint()
    pb = state['pb']
    if pb is not None:
        ox, oy, dw, dh = frame_rect()
        cr.translate(ox, oy); cr.scale(dw / pb.get_width(), dh / pb.get_height())
        Gdk.cairo_set_source_pixbuf(cr, pb, 0, 0); cr.paint()
    return False

# ---------------------------------------------------------------- resizing
def device_size():
    sf = area.get_scale_factor()
    return area.get_allocated_width() * sf, area.get_allocated_height() * sf, sf

def send_uiinfo(w, h, wmm, hmm):
    def run():
        try:
            r = control(f'uiinfo {w} {h} {wmm} {hmm}')
        except Exception as e:
            r = f'err {e}'
        log(f'uiinfo {w}x{h} {wmm}x{hmm}mm -> {r}')
        if r.startswith('err unknown command'):
            ui['enabled'] = False
            log('this qdev-host has no uiinfo: the window only scales the guest display')
    threading.Thread(target=run, daemon=True).start()

def request_size():
    ui['timer'] = 0
    if not ui['enabled']: return False
    w, h, sf = device_size()
    if w < MIN_W or h < MIN_H or (w, h) == ui['last_sent']: return False
    ui['last_sent'] = (w, h)
    send_uiinfo(w, h, round(w / (4.8 * sf)), round(h / (4.8 * sf)))
    return False

def on_allocate(widget, alloc):
    if ui['timer']:
        GLib.source_remove(ui['timer']); ui['timer'] = 0
    if ui['immediate']:
        ui['immediate'] = False; request_size()
    else:
        ui['timer'] = GLib.timeout_add(int(SETTLE_S * 1000), request_size)

def on_window_state(w, e):
    if e.changed_mask & Gdk.WindowState.FULLSCREEN:
        ui['fullscreen'] = bool(e.new_window_state & Gdk.WindowState.FULLSCREEN)
        GLib.idle_add(request_size)     # fullscreen in or out: no settle time
    return False

def toggle_fullscreen():
    ui['immediate'] = True
    if ui['fullscreen']: win.unfullscreen()
    else: win.fullscreen()

# ------------------------------------------------------------------- input
def abs_ev(x, y):
    ox, oy, dw, dh = frame_rect()
    fx, fy = (x - ox) / max(dw, 1), (y - oy) / max(dh, 1)
    return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(min(max(fx, 0), 1) * 32767)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(min(max(fy, 0), 1) * 32767)}}]

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
    if e.keyval == Gdk.KEY_F11:          # the viewer's own: fullscreen, not sent to the guest
        if down: toggle_fullscreen()
        return True
    q = qnum(e.hardware_keycode - 8)
    if q is not None: qmp.events([{'type': 'key', 'data': {'down': down, 'key': {'type': 'number', 'data': q}}}])
    return True

win = Gtk.Window(title=a.title); win.set_default_size(W, H)
area = Gtk.DrawingArea(); area.set_can_focus(True)
area.add_events(Gdk.EventMask.POINTER_MOTION_MASK | Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
                Gdk.EventMask.SCROLL_MASK | Gdk.EventMask.KEY_PRESS_MASK | Gdk.EventMask.KEY_RELEASE_MASK)
area.connect('draw', draw); area.connect('motion-notify-event', motion)
area.connect('button-press-event', lambda w, e: button(w, e, True)); area.connect('button-release-event', lambda w, e: button(w, e, False))
area.connect('scroll-event', scroll); area.connect('size-allocate', on_allocate)
win.connect('key-press-event', lambda w, e: key(w, e, True)); win.connect('key-release-event', lambda w, e: key(w, e, False))
win.connect('window-state-event', on_window_state)
win.add(area); win.connect('destroy', lambda *_: (state.update(run=False), Gtk.main_quit()))
win.show_all(); area.grab_focus()
sf0 = area.get_scale_factor()
area.set_size_request(math.ceil(MIN_W / sf0), math.ceil(MIN_H / sf0))
log(f'window {W}x{H} at scale {sf0}; guest started at {a.guest_size or "unknown"}; '
    f'resize requests {"on" if ui["enabled"] else "off"} (F11: fullscreen)')
threading.Thread(target=grab_loop, daemon=True).start()
Gtk.main()
