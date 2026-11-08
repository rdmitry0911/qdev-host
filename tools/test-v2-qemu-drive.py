#!/usr/bin/env python3
"""Drive a stock QEMU (qtest accelerator, no guest code) that has the qdev-host
test-v2 device cold-plugged at 00:05.0 through vfio-user-pci.
usage: test-v2-qemu-drive.py RUNDIR (started by tools/test-v2-qemu.sh)
RUNDIR holds qtest.sock, qmp2.sock (a second QMP monitor) and d1.sock (qdev-host Display1).
Checks: config space through QEMU, BAR0 MMIO, plugin DMA writes into QEMU's guest
RAM, plugin reads of RAM written by QEMU (vaddr == mmap(fd, fd_offset)), INTx then
MSI from the plugin's thread (MSI aimed at guest RAM so the write is visible),
a memory map change (PAM) -> quiesce, system_reset -> DEVICE_RESET, input relay
Display1 -> qdev-host -> QMP input-send-event (seen in QEMU's trace log)."""
import json, socket, sys, time, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

R = sys.argv[1]
ok = True
def check(cond, what):
    global ok
    print(("PASS: " if cond else "FAIL: ") + what, flush=True)
    ok &= bool(cond)

qt = socket.socket(socket.AF_UNIX); qt.connect(f"{R}/qtest.sock"); qf = qt.makefile("rw")
def q(cmd):
    qf.write(cmd + "\n"); qf.flush()
    while True:
        line = qf.readline().strip()
        if line.startswith("OK") or line.startswith("FAIL") or line.startswith("ERR"):
            return line
def qv(cmd): return int(q(cmd).split()[1], 16)

DEV = 0x80000000 | (5 << 11)
def cfg_rd(off): q(f"outl 0xcf8 {DEV | (off & ~3):#x}"); return qv("inl 0xcfc")
def cfg_wr(off, v): q(f"outl 0xcf8 {DEV | (off & ~3):#x}"); q(f"outl 0xcfc {v:#x}")

qmp = socket.socket(socket.AF_UNIX); qmp.connect(f"{R}/qmp2.sock"); mf = qmp.makefile("rw")
mf.readline()
def qmp_cmd(cmd, **args):
    mf.write(json.dumps({"execute": cmd, "arguments": args} if args else {"execute": cmd}) + "\n"); mf.flush()
    while True:
        r = json.loads(mf.readline())
        if "return" in r or "error" in r: return r
qmp_cmd("qmp_capabilities")

# config space and BAR0 through QEMU
check(cfg_rd(0) == 0x7e571234, f"config 00:05.0 vendor/device {cfg_rd(0):#010x} == 0x7e571234")
cfg_wr(0x10, 0xfe000000)
cfg_wr(0x04, 0x0006)                       # memory decode + bus master
check(qv("readl 0xfe000000") == 0x7e570002, "BAR0 ID register through QEMU MMIO")

# plugin -> guest RAM: IRQ thread writes {seq, t} at GPA 0x2000 before every IRQ (INTx while MSI is off)
q("writeq 0x2000 0"); q("writeq 0x2008 0")
q("writel 0xfe000010 0x2000"); q("writel 0xfe000014 0"); q("writel 0xfe000004 200")
time.sleep(0.5)
seq = qv("readq 0x2000"); t = qv("readq 0x2008")
check(seq > 20 and t > 0, f"plugin DMA writes into QEMU guest RAM: record seq {seq}, t {t}")

# guest RAM -> plugin: QEMU writes, the plugin reads through vaddr and mmap(fd, fd_offset)
for gpa, val in ((0x3000, 0x1122334455667788), (0x8000000 + 0x40, 0x0badc0ffee0ddf00d & 0xffffffffffffffff)):
    q(f"writeq {gpa:#x} {val:#x}")
    q(f"writel 0xfe000018 {gpa & 0xffffffff:#x}"); q(f"writel 0xfe00001c {gpa >> 32:#x}")
    st = qv("readl 0xfe000028"); got = qv("readl 0xfe000020") | qv("readl 0xfe000024") << 32
    check(st == 1 and got == val, f"plugin probe of GPA {gpa:#x}: status {st} value {got:#x} == {val:#x}")

# MSI aimed at guest RAM 0x1000 (msi_notify is a memory write), data 0xabcd
q("writel 0x1000 0")
cfg_wr(0x44, 0x1000); cfg_wr(0x48, 0); cfg_wr(0x4c, 0xabcd)
cfg_wr(0x40, cfg_rd(0x40) | 0x10000)       # MSI enable (MC bit 0)
time.sleep(0.3)
check(qv("readl 0x1000") == 0xabcd, f"MSI from the plugin thread delivered by QEMU: [0x1000] = {qv('readl 0x1000'):#x}")
irqs = qv("readl 0xfe000008"); drop = qv("readl 0xfe00000c")
print(f"plugin irq_trigger ok {irqs}, dropped {drop}", flush=True)

# memory map change: PAM0 (0xf0000-0xfffff) RAM <-> PCI/ROM -> DMA unmap/map -> quiesce
q0 = qv("readl 0xfe000038")
pam = 0x80000000 | 0x90
q(f"outl 0xcf8 {pam:#x}"); v = qv("inl 0xcfc")
for i in range(4):
    q(f"outl 0xcf8 {pam:#x}"); q(f"outl 0xcfc {(v & ~0x30) | (0x30 if i % 2 == 0 else 0):#x}")
time.sleep(0.2)
q1 = qv("readl 0xfe000038")
check(q1 > q0, f"PAM toggles changed the DMA map: plugin quiesces {q0} -> {q1}")
check(qv("readl 0xfe000034") == 0, "plugin saw no host rule violation")

# input relay: Display1 on qdev-host -> QMP input-send-event
c = Gio.DBusConnection.new_for_address_sync(f"unix:path={R}/d1.sock", Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
def d1(iface, method, sig, args):
    c.call_sync(None, "/org/qemu/Display1/Console_0", iface, method, GLib.Variant(sig, args), None, Gio.DBusCallFlags.NONE, 5000, None)
absprop = c.call_sync(None, "/org/qemu/Display1/Console_0", "org.freedesktop.DBus.Properties", "Get",
                      GLib.Variant("(ss)", ("org.qemu.Display1.Mouse", "IsAbsolute")), None, Gio.DBusCallFlags.NONE, 5000, None)
print("exported Mouse.IsAbsolute:", absprop.unpack()[0], flush=True)
d1("org.qemu.Display1.Keyboard", "Press", "(u)", (0x1e,)); d1("org.qemu.Display1.Keyboard", "Release", "(u)", (0x1e,))
d1("org.qemu.Display1.Mouse", "SetAbsPosition", "(uu)", (320, 240))
d1("org.qemu.Display1.Mouse", "Press", "(u)", (0,)); d1("org.qemu.Display1.Mouse", "Release", "(u)", (0,))
d1("org.qemu.Display1.Keyboard", "Press", "(u)", (0x9d,)); d1("org.qemu.Display1.Keyboard", "Release", "(u)", (0x9d,))
time.sleep(0.5)

# device reset through QEMU
r = qmp_cmd("system_reset")
check("return" in r, "QMP system_reset")
time.sleep(0.5)
q("outl 0xcf8 0x80002800")
check(qv("inl 0xcfc") == 0x7e571234, "device still answers after system_reset")
qmp_cmd("quit")
sys.exit(0 if ok else 1)
