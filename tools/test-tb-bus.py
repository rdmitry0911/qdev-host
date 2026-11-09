#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline checks of the TB bus code (pve/qdev_tb.py): the AML encoder, the
argument parsers and the generated -readconfig file.  No PVE node needed.

  python3 tools/test-tb-bus.py            # IASL=/path/to/iasl also disassembles the SSDT
"""
import os, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "pve"))
import qdev_pve as Q
import qdev_tb as T

failures = 0
def check(name, cond, detail=""):
    global failures
    print(("ok   " if cond else "FAIL ") + name + (f": {detail}" if detail and not cond else ""))
    failures += 0 if cond else 1

def unpkglen(b: bytes) -> tuple[int, int]:
    n = b[0] >> 6
    if n == 0:
        return b[0] & 0x3F, 1
    v = b[0] & 0x0F
    for i in range(n):
        v |= b[1 + i] << (4 + 8 * i)
    return v, n + 1

# PkgLength: the encoded length includes its own bytes
for body in (0, 10, 62, 63, 100, 4000, 70000):
    enc = T._pkglen(body)
    total, size = unpkglen(enc)
    check(f"pkglen {body}", total == body + size and size == len(enc), f"{enc.hex()} -> {total}")

scopes = [T.acpi_slot_name(8)] + [T.acpi_slot_name(9, i) for i in range(4)]
check("ACPI names", scopes == ["\\_SB.PCI0.S40", "\\_SB.PCI0.S48", "\\_SB.PCI0.S49", "\\_SB.PCI0.S4A", "\\_SB.PCI0.S4B"], str(scopes))
aml = T.ssdt(scopes)
check("SSDT signature", aml[:4] == b"SSDT")
check("SSDT length", int.from_bytes(aml[4:8], "little") == len(aml))
check("SSDT checksum", sum(aml) % 256 == 0)
check("SSDT _DSD uuid", __import__("uuid").UUID(T.DSD_UUID).bytes_le in aml)
check("SSDT _DSM uuid", __import__("uuid").UUID(T.APPLE_DSM_UUID).bytes_le in aml)
check("SSDT scopes", all(s.encode().split(b".")[-1] + b"_" in aml for s in scopes))
check("SSDT properties", aml.count(b"PCI-Thunderbolt\x00") == 2 * len(scopes))
if os.environ.get("IASL"):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "tb.aml"; p.write_bytes(aml)
        r = subprocess.run([os.environ["IASL"], "-d", str(p)], capture_output=True, text=True)
        dsl = (Path(d) / "tb.dsl").read_text() if (Path(d) / "tb.dsl").exists() else ""
        check("iasl disassembles", r.returncode == 0 and "Scope (\\_SB.PCI0.S4B)" in dsl, r.stdout + r.stderr)
        check("iasl no warnings", "Warning" not in r.stdout + r.stderr and "Error" not in r.stdout + r.stderr, r.stdout)

# netN syntax
s = T.net_spec("virtio,bridge=lo,firewall=1")
check("net bare model", s["model"] == "virtio" and s["macaddr"] == "" and s["firewall_ignored"], str(s))
s = T.net_spec("virtio=bc:24:11:aa:bb:cc,bridge=lo,tag=5,mtu=9000,queues=4")
check("net normalize", T.net_args(s) == "virtio=BC:24:11:AA:BB:CC,bridge=lo,tag=5,mtu=9000,queues=4", T.net_args(s))
for bad, why in (("e1000,bridge=lo", "no IO window"), ("virtio,bridge=nosuchbr0", "bridge"),
                 ("virtio,bridge=lo,link_down=1", "not supported"), ("e1000e,bridge=lo,mtu=9000", "mtu")):
    try:
        T.net_spec(bad); check(f"net rejects {bad}", False, "accepted")
    except Q.QdevError as e:
        check(f"net rejects {bad}", why in str(e), str(e))
mac = T.random_mac()
check("random MAC", T.MAC_RE.match(mac) is not None and int(mac[:2], 16) & 1 == 0, mac)

# QMP wants typed values
q = T.qmp_typed({"vendorid": "0x1a86", "hostbus": "7", "multifunction": "on", "mac": "BC:24:11:00:00:01", "addr": "0.1"})
check("qmp types", q == {"vendorid": 0x1a86, "hostbus": 7, "multifunction": True, "mac": "BC:24:11:00:00:01", "addr": "0.1"}, str(q))

# the -readconfig file, with qemu-server's helpers stubbed
def fake_perl(action, vmid, value, extra=""):
    if action == "pci-resolve":
        return {"ids": ["0000:06:00.0", "0000:06:00.1"], "rombar": "0"}
    if action == "usb-resolve":
        return {"vendorid": "1a86", "productid": "7523"}
    raise AssertionError(action)
T.perl = fake_perl
with tempfile.TemporaryDirectory() as d:
    T.TB_DIR = Path(d)
    conf = {"vmid": 990, "port_addr": "0x08", "tb": {"ports": 4}, "devices": [
        {"id": "tbpci0", "plugin": "tb-pci", "args": "host=0000:06:00", "bus": "qdev-tb0"},
        {"id": "tbnet0", "plugin": "tb-net", "args": "virtio=BC:24:11:00:00:01,bridge=lo", "bus": "qdev-tb1", "tap": 32},
        {"id": "tbusb0", "plugin": "tb-usb", "args": "host=1a86:7523", "bus": "tbxhci0"},
        {"id": "tbxhci0", "plugin": "tb-xhci", "args": "", "bus": "qdev-tb2"},
    ]}
    notes = T.write_files(990, conf, {"args": "-device pcie-root-port,id=qdev-rp0,bus=pcie.0,addr=0x08"})
    cfg = (Path(d) / "990.cfg").read_text()
    order = [l.split('"')[1] for l in cfg.splitlines() if l.startswith("[")]
    check("cfg order", order == ["qdev-tb0", "qdev-tb1", "qdev-tb2", "qdev-tb3", "tbpci0", "tbpci0-f1", "tbnet0",
                                 "tbnet0", "tbxhci0", "tbusb0"], str(order))
    check("cfg ports", 'addr = "09.3"' in cfg and 'io-reserve = "0"' in cfg and 'pref64-reserve = "1G"' in cfg)
    check("cfg multifunction", cfg.count('multifunction = "on"') == 4 + 2 and 'rombar = "0"' in cfg)
    check("cfg tap", 'ifname = "tap990i32"' in cfg and f'script = "{T.IFUP}"' in cfg)
    check("cfg usb", 'bus = "tbxhci0.0"' in cfg and 'vendorid = "0x1a86"' in cfg)
    aml = (Path(d) / "990.aml").read_bytes()
    check("aml with qdev-rp0", b"S40_" in aml and b"S4B_" in aml and sum(aml) % 256 == 0)
    check("no notes", notes == [], str(notes))

print("PASS" if not failures else f"{failures} FAILED")
sys.exit(1 if failures else 0)
