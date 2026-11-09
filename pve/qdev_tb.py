#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The TB bus of qdev-host: hot-plug PCIe root ports with Thunderbolt ACPI
properties, and the QEMU devices qdev-host puts on them.

Device kinds (the `plugin` of a device in /etc/qdev-host/vms/VMID.json):
  tb-pci   PCI pass-through (vfio-pci); args in PVE's hostpciN syntax (host=... or mapping=...)
  tb-net   network card on a tap of a node bridge; args in PVE's netN syntax (virtio=MAC,bridge=vmbr0,...)
  tb-xhci  QEMU USB controller (qemu-xhci)
  tb-usb   USB device on a tb-xhci controller (its `bus`); args host=VID:PID|BUS-PORT or mapping=NAME

The bus is a set of pcie-root-ports qdev-tb0..N-1 at pcie.0 slot 0x09
(functions 0..N-1).  The ports and the devices present at VM start come from
/var/lib/qdev-host/tb/VMID.cfg (QEMU -readconfig), the ACPI properties from
/var/lib/qdev-host/tb/VMID.aml (-acpitable); both are written again at every
VM start by the hookscript, so the VM's args only name the two files.  While
the VM runs, devices are hot-plugged and unplugged over QMP.

PCI pass-through devices are hot-plugged right after the start (post-start)
instead: QEMU pins all of the VM's memory for the first one before it
daemonizes, and qemu-server's start timeout grows only for its own hostpciN,
so a 32 GiB VM with one in -readconfig was killed after 32 s.
"""
from __future__ import annotations
import json, os, random, re, subprocess, time, uuid
from pathlib import Path

import qdev_pve as Q

TB_DIR = Q.STATE_DIR / "tb"
NET_RUN_DIR = Q.RUN_DIR / "tb-net"
LIB_DIR = Path("/usr/lib/qdev-host")
IFUP = LIB_DIR / "qdev-tb-ifup"
IFDOWN = LIB_DIR / "qdev-tb-ifdown"
PORT_PREFIX = "qdev-tb"
TB_SLOT = 0x09            # free on pcie.0 in PVE's maps (except hostpci4bus0 of Windows 7 guests)
DEFAULT_PORTS = 4
MAX_PORTS = 8
CHASSIS_BASE = 61         # qdev-rp0 is chassis 60, PVE's own ports 1..16
TAP_BASE = 32             # PVE's NICs net0..net31 are tapVMIDi0..31
KINDS = ("tb-pci", "tb-net", "tb-xhci", "tb-usb")
PORT_KINDS = ("tb-pci", "tb-net", "tb-xhci")
# no IO window on TB ports (the q35 IO space is small); these models work without port IO
NET_MODELS = {"virtio": "virtio-net-pci", "e1000e": "e1000e", "vmxnet3": "vmxnet3"}
NET_KEYS = {"bridge", "tag", "rate", "mtu", "queues", "firewall"}
# ACPI properties of the ports, per VM (conf["tb"]["acpi"]):
#   thunderbolt  the qemu-thunderbolt fork's: macOS treats the port as a Thunderbolt tunnel root
#   hotplug      a native PCIe hot-plug port for macOS (PCIHotplugCapable)
#   none         plain ports
ACPI_MODES = {
    "thunderbolt": [("PCI-Thunderbolt", 1), ("pci-supports-link-change", 1)],
    "hotplug": [("PCIHotplugCapable", 1), ("pci-supports-link-change", 1)],
    "none": [],
}
DEFAULT_ACPI = "thunderbolt"
TB_PROPS = ACPI_MODES[DEFAULT_ACPI]
DSD_UUID = "daffd814-6eba-4d8c-8a91-bc9bbf4aa301"     # ACPI device properties
APPLE_DSM_UUID = "a0b5b7c6-1318-441c-b0c9-fe695eaf949b"  # macOS device properties (_DSM)

def is_tb(dev: dict) -> bool:
    return dev.get("plugin") in KINDS

def tb_devices(conf: dict) -> list[dict]:
    return [d for d in conf["devices"] if is_tb(d)]

def port_id(i: int) -> str:
    return f"{PORT_PREFIX}{i}"

def acpi_mode(conf: dict, vmcfg: dict | None = None) -> str:
    """The ports' ACPI properties; auto (unset) = none on macOS ports with slot power
    (Thunderbolt ports there would need link reporting the handshake turns off),
    thunderbolt otherwise."""
    mode = conf.get("tb", {}).get("acpi", "auto")
    if mode in ACPI_MODES:
        return mode
    return "none" if vmcfg is not None and slot_handshake(conf, vmcfg) else DEFAULT_ACPI

def port_count(conf: dict) -> int:
    return int(conf.get("tb", {}).get("ports", DEFAULT_PORTS))

def cfg_path(vmid: int) -> Path:
    return TB_DIR / f"{vmid}.cfg"

def aml_path(vmid: int) -> Path:
    return TB_DIR / f"{vmid}.aml"

def tap_name(vmid: int, dev: dict) -> str:
    return f"tap{vmid}i{dev['tap']}"

# ------------------------------------------------------------------ AML
def _pkglen(body: int) -> bytes:
    for extra in range(1, 5):
        total = body + extra
        if extra == 1 and total < 0x40:
            return bytes([total])
        if extra > 1 and total < 1 << (4 + 8 * (extra - 1)):
            out = [((extra - 1) << 6) | (total & 0x0F)]
            rest = total >> 4
            for _ in range(extra - 1):
                out.append(rest & 0xFF); rest >>= 8
            return bytes(out)
    raise ValueError("AML object too large")

def _seg(name: str) -> bytes:
    assert 1 <= len(name) <= 4
    return name.ljust(4, "_").encode()

def _path(path: str) -> bytes:
    segs = [_seg(s) for s in path.lstrip("\\").split(".")]
    prefix = b"\\" if path.startswith("\\") else b""
    if len(segs) == 1:
        return prefix + segs[0]
    if len(segs) == 2:
        return prefix + b"\x2e" + b"".join(segs)
    return prefix + b"\x2f" + bytes([len(segs)]) + b"".join(segs)

def _int(v: int) -> bytes:
    if v in (0, 1):
        return bytes([v])
    for op, size in ((0x0A, 1), (0x0B, 2), (0x0C, 4), (0x0E, 8)):
        if v < 1 << (8 * size):
            return bytes([op]) + v.to_bytes(size, "little")
    raise ValueError(v)

def _str(s: str) -> bytes:
    return b"\x0d" + s.encode() + b"\x00"

def _pkg(*elems: bytes) -> bytes:
    body = bytes([len(elems)]) + b"".join(elems)
    return b"\x12" + _pkglen(len(body)) + body

def _buf(data: bytes) -> bytes:
    body = (b"\x0a" + bytes([len(data)]) if len(data) < 256 else _int(len(data))) + data
    return b"\x11" + _pkglen(len(body)) + body

def _uuid(u: str) -> bytes:
    return _buf(uuid.UUID(u).bytes_le)      # ToUUID () byte order

def _if(pred: bytes, *terms: bytes) -> bytes:
    body = pred + b"".join(terms)
    return b"\xa0" + _pkglen(len(body)) + body

def _lequal(a: bytes, b: bytes) -> bytes:
    return b"\x93" + a + b

def _ret(v: bytes) -> bytes:
    return b"\xa4" + v

ARG0, ARG2 = b"\x68", b"\x6a"

def _scope(path: str, *terms: bytes) -> bytes:
    body = _path(path) + b"".join(terms)
    return b"\x10" + _pkglen(len(body)) + body

def _name(name: str, value: bytes) -> bytes:
    return b"\x08" + _seg(name) + value

def _method(name: str, nargs: int, *terms: bytes) -> bytes:
    body = _seg(name) + bytes([nargs & 7]) + b"".join(terms)
    return b"\x14" + _pkglen(len(body)) + body

def port_properties(props: list[tuple[str, int]] | None = None) -> list[bytes]:
    """_DSD (ACPI device properties) and macOS's _DSM with the port properties."""
    props = TB_PROPS if props is None else props
    dsd = _name("_DSD", _pkg(_uuid(DSD_UUID), _pkg(*[_pkg(_str(k), _int(v)) for k, v in props])))
    flat = [x for k, v in props for x in (_str(k), _int(v))]
    dsm = _method("_DSM", 4,
                  _if(_lequal(ARG0, _uuid(APPLE_DSM_UUID)),
                      _if(_lequal(ARG2, _int(0)), _ret(_buf(b"\x03"))),
                      _ret(_pkg(*flat))),
                  _ret(_buf(b"\x00")))
    return [dsd, dsm]

def ssdt(scopes: list[str], oem_table: str = "QDEVTB", props: list[tuple[str, int]] | None = None) -> bytes:
    """An SSDT that gives every ACPI path in SCOPES the port properties (default: Thunderbolt)."""
    props = TB_PROPS if props is None else props
    body = b"".join(_scope(s, *port_properties(props)) for s in scopes) if props else b""
    head = bytearray(b"SSDT" + (36 + len(body)).to_bytes(4, "little") + b"\x02\x00" + b"QDEVHT" +
                     oem_table.encode().ljust(8, b" ") + (1).to_bytes(4, "little") + b"QDEV" +
                     (1).to_bytes(4, "little"))
    table = bytearray(head + body)
    table[9] = (-sum(table)) & 0xFF
    return bytes(table)

def acpi_slot_name(slot: int, func: int = 0) -> str:
    # QEMU names the devices on pcie.0 S<devfn in hex>
    return f"\\_SB.PCI0.S{(slot << 3 | func):02X}"

# ------------------------------------------------------------- PVE perl
PERL = r'''
use strict; use warnings; use JSON::PP;
use PVE::QemuServer::PCI; use PVE::QemuServer::USB;
my ($action, $vmid, $value, $extra) = @ARGV;
my $out = {};
if ($action eq 'pci-resolve' || $action eq 'pci-prepare') {
    my $d = PVE::QemuServer::PCI::parse_hostpci($value);
    die "mediated devices (mdev) are not supported on the TB bus\n" if $d->{mdev} || $d->{nvidia};
    die "legacy-igd needs the standard bus\n" if $d->{'legacy-igd'};
    my $chosen;
    if ($action eq 'pci-prepare') {
        my $err;
        for my $alt ($d->{ids}->@*) {
            my $ids = [map { $_->{id} } @$alt];
            eval { PVE::QemuServer::PCI::reserve_pci_usage($ids, $vmid, $extra || 60, undef) };
            if ($err = $@) { next; }
            $chosen = $alt; last;
        }
        die $err // "no free PCI device\n" if !$chosen;
        PVE::QemuServer::PCI::prepare_pci_device($vmid, $_->{id}, 0, $d) for @$chosen;
    } else {
        $chosen = $d->{ids}->[0];
    }
    $out->{ids} = [map { $_->{id} } @$chosen];
    $out->{$_} = $d->{$_} for grep { defined $d->{$_} } qw(rombar romfile vendor-id device-id sub-vendor-id sub-device-id x-vga);
} elsif ($action eq 'pci-own') {
    my $ids = decode_json($value);
    eval { PVE::QemuServer::PCI::reserve_pci_usage($ids, $vmid, undef, $extra) };
    die $@ if $@ && $@ !~ /in use by running VMID '\Q$vmid\E'/;
} elsif ($action eq 'pci-release') {
    PVE::QemuServer::PCI::remove_pci_reservation($vmid, decode_json($value));
} elsif ($action eq 'usb-resolve') {
    my ($host, $mapping) = $value =~ /^mapping=(.*)$/ ? (undef, $1) : ($value, undef);
    my $d = PVE::QemuServer::USB::parse_usb_device($host, $mapping);
    die "not a USB device of this node: $value\n" if !$d || $d->{spice} || !($d->{vendorid} || $d->{hostbus});
    $out = $d;
} else { die "unknown action $action\n"; }
print JSON::PP->new->canonical->encode($out);
'''

def perl(action: str, vmid: int, value: str, extra: str = "") -> dict:
    r = subprocess.run(["perl", "-e", PERL, action, str(vmid), value, extra], capture_output=True, text=True)
    if r.returncode != 0:
        err = "\n".join(l for l in r.stderr.strip().splitlines() if "older storage API" not in l)
        raise Q.QdevError(err or f"{action} failed")
    return json.loads(r.stdout or "{}")

# ------------------------------------------------------------ device specs
MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")

def mac_prefix() -> str:
    try:
        for line in Path("/etc/pve/datacenter.cfg").read_text().splitlines():
            if line.startswith("mac_prefix:"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "BC:24:11"

def random_mac() -> str:
    prefix = [int(x, 16) for x in mac_prefix().split(":") if x]
    rest = [random.randrange(256) for _ in range(6 - len(prefix))]
    b = prefix + rest
    b[0] = (b[0] & 0xFE)                     # unicast
    return ":".join(f"{x:02X}" for x in b)

def net_spec(args: str) -> dict:
    """PVE netN syntax -> {model, macaddr, bridge, tag, rate, mtu, queues}."""
    a = Q.parse_args(args)
    if "model" in a:
        model, mac = a.pop("model"), a.pop("macaddr", "")
    else:
        models = [k for k in a if k in NET_MODELS or k in ("e1000", "rtl8139", "ne2k_pci", "pcnet", "i82551",
                                                             "i82557b", "i82559er", "ne2k_isa")
                  or k.startswith("e1000-")]
        if len(models) != 1:
            raise Q.QdevError("a network device needs one model: virtio, e1000e or vmxnet3")
        model = models[0]; mac = a.pop(model)
    if model not in NET_MODELS:
        raise Q.QdevError(f"model {model} is not available on the TB bus (its ports have no IO window): "
                          f"use {', '.join(NET_MODELS)}")
    unknown = set(a) - NET_KEYS
    if unknown:
        raise Q.QdevError(f"network option(s) not supported on the TB bus: {', '.join(sorted(unknown))}")
    if mac and not MAC_RE.match(mac):
        raise Q.QdevError(f"invalid MAC address {mac}")
    bridge = a.get("bridge", "")
    if not re.match(r"^[A-Za-z0-9_.-]{1,15}$", bridge):
        raise Q.QdevError("a network device needs bridge=NAME")
    if not Path(f"/sys/class/net/{bridge}").exists():
        raise Q.QdevError(f"bridge {bridge} does not exist on this node")
    spec = {"model": model, "macaddr": mac.upper(), "bridge": bridge}
    for key, low, high in (("tag", 1, 4094), ("mtu", 576, 65520), ("queues", 1, 64)):
        if a.get(key):
            try:
                v = int(a[key])
            except ValueError:
                raise Q.QdevError(f"{key} must be a number") from None
            if not low <= v <= high:
                raise Q.QdevError(f"{key} must be {low}..{high}")
            spec[key] = v
    if a.get("rate"):
        try:
            spec["rate"] = float(a["rate"])
        except ValueError:
            raise Q.QdevError("rate must be a number (MB/s)") from None
    if "mtu" in spec and model != "virtio":
        raise Q.QdevError("mtu is for virtio network devices")
    if "queues" in spec and model != "virtio":
        raise Q.QdevError("queues is for virtio network devices")
    spec["firewall_ignored"] = a.get("firewall", "0") not in ("", "0")
    return spec

def net_args(spec: dict) -> str:
    out = [f"{spec['model']}={spec['macaddr']}", f"bridge={spec['bridge']}"]
    for key in ("tag", "rate", "mtu", "queues"):
        if key in spec:
            v = spec[key]
            out.append(f"{key}={v:g}" if isinstance(v, float) else f"{key}={v}")
    return ",".join(out)

PCI_KEYS = {"host", "mapping", "rombar", "romfile", "pcie", "vendor-id", "device-id", "sub-vendor-id", "sub-device-id"}

def pci_check(args: str) -> None:
    a = Q.parse_args(args)
    unknown = set(a) - PCI_KEYS
    if unknown:
        raise Q.QdevError(f"PCI option(s) not supported on the TB bus: {', '.join(sorted(unknown))} "
                          "(x-vga, mdev and legacy-igd need the standard bus)")
    if ("host" in a) == ("mapping" in a):
        raise Q.QdevError("a PCI device needs host=DOMAIN:BUS:SLOT[.FUNC] or mapping=NAME")

def usb_check(args: str) -> None:
    a = Q.parse_args(args)
    unknown = set(a) - {"host", "mapping", "usb3"}
    if unknown or ("host" in a) == ("mapping" in a):
        raise Q.QdevError("a USB device needs host=VENDOR:PRODUCT, host=BUS-PORT or mapping=NAME")

def usb_value(args: str) -> str:
    a = Q.parse_args(args)
    return f"mapping={a['mapping']}" if "mapping" in a else a["host"]

def xhci_check(args: str) -> int:
    a = Q.parse_args(args)
    if set(a) - {"ports"}:
        raise Q.QdevError("a USB controller takes only ports=N (USB 2 and USB 3 ports each, 1..15)")
    try:
        n = int(a.get("ports") or 8)
    except ValueError:
        raise Q.QdevError("ports must be a number") from None
    if not 1 <= n <= 15:
        raise Q.QdevError("ports must be 1..15")
    return n

# ------------------------------------------------------- QEMU device options
def _opts(d: dict) -> str:
    return ",".join(f"{k}={v}" for k, v in d.items())

def macos_guest(vmcfg: dict) -> bool:
    return "isa-applesmc" in (vmcfg or {}).get("args", "")

def power_controller(conf: dict, vmcfg: dict | None) -> str | None:
    """on/off for the ports' slot power controller, None for QEMU's default (on).
    off: macOS takes hot-plugged devices but cannot release one (QEMU waits for the
    slot to be switched off), so a device leaves only when the VM stops (0.6.1)."""
    value = conf.get("tb", {}).get("power-controller")
    if value in ("on", "off"):
        return value
    if (vmcfg is not None and macos_guest(vmcfg) and hotplug_mode(conf, vmcfg) == "native"
            and conf.get("tb", {}).get("acpi") == "thunderbolt"):
        return "off"    # Thunderbolt ports in macOS: the 0.6.1 hot-plug-only mode
    return None

def slot_handshake(conf: dict, vmcfg: dict | None) -> bool:
    """macOS ports run with slot power: hot-plug through the power handshake below,
    hot-unplug through macOS's own release (attention button, power off)."""
    return (vmcfg is not None and macos_guest(vmcfg) and hotplug_mode(conf, vmcfg) == "native"
            and power_controller(conf, vmcfg) != "off" and conf.get("tb", {}).get("acpi") != "thunderbolt")

def port_devices(conf: dict, vmcfg: dict | None = None) -> list[tuple[str, dict]]:
    out = []
    pcp = power_controller(conf, vmcfg)
    handshake = slot_handshake(conf, vmcfg)
    for i in range(port_count(conf)):
        p = {"driver": "pcie-root-port", "bus": "pcie.0", "addr": f"{TB_SLOT:02x}.{i}",
             "multifunction": "on", "chassis": str(CHASSIS_BASE + i), "port": str(CHASSIS_BASE + i),
             "io-reserve": "0", "mem-reserve": "32M", "pref64-reserve": "1G"}
        if pcp:
            p["power_controller_present"] = pcp
        if handshake:
            # no Data Link Layer Link Active reporting (QEMU ties it to speeds above
            # 2.5 GT/s): the handshake's key device must arrive with its link down
            p["x-speed"] = "2_5"
        for key in ("io-reserve", "mem-reserve", "pref64-reserve"):
            v = conf.get("tb", {}).get(key)
            if v is None and vmcfg is not None and macos_guest(vmcfg):
                v = ""      # macOS sizes hot-plug port windows itself: QEMU's defaults, as checked
            if v == "":
                p.pop(key)
            elif v:
                p[key] = v
        for key, v in (conf.get("tb", {}).get("port-props") or {}).items():
            p[str(key)] = str(v)
        out.append((port_id(i), p))
    return out

def pci_functions(dev: dict, info: dict) -> list[tuple[str, dict]]:
    """(qdev id, properties) for each function of a pass-through device; function 0 first."""
    out = []
    ids = info["ids"]
    multi = len(ids) > 1
    for j, host in enumerate(ids):
        p = {"driver": "vfio-pci", "host": host, "bus": dev["bus"], "addr": f"0.{j}" if multi else "0.0"}
        if multi:
            p["multifunction"] = "on"
        if j == 0:
            if str(info.get("rombar", "1")) == "0":
                p["rombar"] = "0"
            if info.get("romfile"):
                p["romfile"] = f"/usr/share/kvm/{info['romfile']}"
            for key in ("vendor-id", "device-id", "sub-vendor-id", "sub-device-id"):
                if info.get(key):
                    p[f"x-pci-{key}"] = info[key]
        out.append((dev["id"] if j == 0 else f"{dev['id']}-f{j}", p))
    return out

def net_netdev(vmid: int, dev: dict, spec: dict) -> dict:
    nd = {"type": "tap", "ifname": tap_name(vmid, dev), "script": str(IFUP), "downscript": str(IFDOWN)}
    if spec["model"] == "virtio":
        if Path("/dev/vhost-net").exists():
            nd["vhost"] = "on"
        if spec.get("queues", 1) > 1:
            nd["queues"] = str(spec["queues"])
    return nd

def net_device(dev: dict, spec: dict) -> dict:
    p = {"driver": NET_MODELS[spec["model"]], "netdev": dev["id"], "mac": spec["macaddr"], "bus": dev["bus"], "addr": "0.0"}
    if spec["model"] == "virtio":
        if spec.get("mtu"):
            p["host_mtu"] = str(spec["mtu"])
        if spec.get("queues", 1) > 1:
            p["mq"] = "on"; p["vectors"] = str(2 * spec["queues"] + 2)
    return p

def xhci_device(dev: dict) -> dict:
    n = xhci_check(dev.get("args", ""))
    return {"driver": "qemu-xhci", "bus": dev["bus"], "addr": "0.0", "p2": str(n), "p3": str(n)}

def usb_device(vmid: int, dev: dict) -> dict:
    u = perl("usb-resolve", vmid, usb_value(dev["args"]))
    p = {"driver": "usb-host", "bus": f"{dev['bus']}.0"}
    if u.get("vendorid"):
        p.update(vendorid=f"0x{u['vendorid']}", productid=f"0x{u['productid']}")
    else:
        p.update(hostbus=str(u["hostbus"]), hostport=str(u["hostport"]))
    return p

def write_net_settings(vmid: int, dev: dict, spec: dict) -> None:
    NET_RUN_DIR.mkdir(parents=True, exist_ok=True)
    p = NET_RUN_DIR / f"{tap_name(vmid, dev)}.json"
    data = {k: spec[k] for k in ("bridge", "tag", "rate") if k in spec}
    data.update(vmid=vmid, id=dev["id"], macaddr=spec["macaddr"])
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(data) + "\n"); tmp.chmod(0o600)
    os.replace(tmp, p)

# ------------------------------------------------------------- files
def _config_file(groups: list[tuple[str, str, dict]]) -> str:
    out = ["# written by qdev-host at every start of the VM; edit the devices with qdevctl or the Hardware panel"]
    for kind, name, props in groups:
        out.append(f'\n[{kind} "{name}"]')
        for k, v in props.items():
            out.append(f'  {k} = "{v}"')
    return "\n".join(out) + "\n"

def ssdt_scopes(conf: dict, vmcfg: dict) -> list[str]:
    scopes = [acpi_slot_name(TB_SLOT, i) for i in range(port_count(conf))]
    # the vfio-user port of qdev-host (tb-vga) is a hot-plug port of the same kind
    if f"id={Q.PORT_ID}," in vmcfg.get("args", ""):
        try:
            scopes.insert(0, acpi_slot_name(int(conf.get("port_addr", "0x08"), 16)))
        except ValueError:
            pass
    return scopes

def write_files(vmid: int, conf: dict, vmcfg: dict, starting: bool = False) -> list[str]:
    """Write VMID.cfg and VMID.aml (STARTING: at pre-start, with the taps' settings).
    PCI pass-through devices are not in VMID.cfg: post-start hot-plugs them."""
    notes = []
    groups: list[tuple[str, str, dict]] = [("device", n, p) for n, p in port_devices(conf, vmcfg)]
    for dev in tb_devices(conf):
        kind = dev["plugin"]
        if kind == "tb-net":
            spec = net_spec(dev["args"])
            groups.append(("netdev", dev["id"], net_netdev(vmid, dev, spec)))
            groups.append(("device", dev["id"], net_device(dev, spec)))
            if starting:
                write_net_settings(vmid, dev, spec)
        elif kind == "tb-xhci":
            groups.append(("device", dev["id"], xhci_device(dev)))
    for dev in tb_devices(conf):   # after their controllers
        if dev["plugin"] == "tb-usb":
            try:
                groups.append(("device", dev["id"], usb_device(vmid, dev)))
            except Q.QdevError as e:
                notes.append(f"{dev['id']}: {e}; left out")
    TB_DIR.mkdir(parents=True, exist_ok=True)
    props = ACPI_MODES[acpi_mode(conf, vmcfg)]
    for path, data in ((cfg_path(vmid), _config_file(groups).encode()),
                       (aml_path(vmid), ssdt(ssdt_scopes(conf, vmcfg), props=props))):
        tmp = path.with_suffix(".tmp")
        tmp.write_bytes(data); tmp.chmod(0o644)
        os.replace(tmp, path)
    return notes

# -------------------------------------------------------------- VM args
# QEMU's q35 hands every cold-plugged bridge's hot-plug to ACPI (since 6.1).  macOS
# ignores those ACPI events on its Thunderbolt ports: it waits for the port's own
# PCIe hot-plug interrupts ("linkInterrupts 1" in its IOPCIFamily log).
NATIVE_HOTPLUG_GLOBAL = "-global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off"
NATIVE_HOTPLUG_PATTERN = r"-global\s+ICH9-LPC\.acpi-pci-hotplug-with-bridge-support=off"
HOTPLUG_MODES = ("auto", "native", "acpi")

def hotplug_mode(conf: dict, vmcfg: dict) -> str:
    """native or acpi; auto = native for a macOS guest (isa-applesmc in args) or
    when the VM's args already switch ACPI PCI hot-plug off."""
    mode = conf.get("tb", {}).get("hotplug", "auto")
    if mode in ("native", "acpi"):
        return mode
    args = vmcfg.get("args", "")
    return "native" if (macos_guest(vmcfg) or Q._find(args, NATIVE_HOTPLUG_PATTERN)) else "acpi"

def arg_groups(vmid: int, conf: dict | None = None, vmcfg: dict | None = None) -> str:
    groups = f"-readconfig {cfg_path(vmid)} -acpitable file={aml_path(vmid)}"
    if conf is not None and vmcfg is not None and hotplug_mode(conf, vmcfg) == "native":
        groups += " " + NATIVE_HOTPLUG_GLOBAL
    return groups

def arg_patterns(vmid: int) -> list[str]:
    return [rf"-readconfig\s+{re.escape(str(cfg_path(vmid)))}", rf"-acpitable\s+file={re.escape(str(aml_path(vmid)))}"]

def in_args(vmid: int, vmcfg: dict, conf: dict | None = None) -> bool:
    args = vmcfg.get("args", "")
    if not all(Q._find(args, p) for p in arg_patterns(vmid)):
        return False
    if conf is not None:
        return (hotplug_mode(conf, vmcfg) == "native") == bool(Q._find(args, NATIVE_HOTPLUG_PATTERN))
    return True

def ensure_args(vmid: int, vmcfg: dict, conf: dict | None = None) -> bool:
    """Put the TB bus in front of the VM's args: devices in args (a qdev-host GPU)
    may sit on its ports, and QEMU creates devices in command line order."""
    if in_args(vmid, vmcfg, conf):
        return False
    groups = arg_groups(vmid, conf, vmcfg)
    args = vmcfg.get("args", "")
    for p in arg_patterns(vmid) + [NATIVE_HOTPLUG_PATTERN]:
        args = Q._drop(args, p)
    Q.qm_set(vmid, "args", (groups + " " + args).strip())
    return True

def is_port(bus: str) -> bool:
    return bool(re.match(rf"^{PORT_PREFIX}[0-9]$", bus or ""))

def ensure_bus(vmid: int, conf: dict, vmcfg: dict) -> bool:
    """The VM's TB bus in its files and args. Returns True if args changed."""
    conf.setdefault("tb", {"ports": DEFAULT_PORTS})
    write_files(vmid, conf, vmcfg)
    return ensure_args(vmid, vmcfg, conf)

def remove_args(vmid: int, vmcfg: dict) -> None:
    args = vmcfg.get("args", "")
    new = args
    for p in arg_patterns(vmid) + [NATIVE_HOTPLUG_PATTERN]:
        new = Q._drop(new, p)
    if new != args:
        Q.qm_set(vmid, "args", new or None)

# -------------------------------------------------------------- QMP
def peripherals(vmid: int) -> set[str]:
    try:
        return {c["name"] for c in Q.mon_cmd(vmid, "qom-list", path="/machine/peripheral")}
    except Q.QdevError:
        return set()

def vm_pid(vmid: int) -> str:
    try:
        return Path(f"/var/run/qemu-server/{vmid}.pid").read_text().strip()
    except OSError:
        return ""

QMP_INTS = {"vendorid", "productid", "hostbus", "p2", "p3", "rombar", "host_mtu", "vectors", "queues",
            "x-pci-vendor-id", "x-pci-device-id", "x-pci-sub-vendor-id", "x-pci-sub-device-id"}
QMP_BOOLS = {"multifunction", "mq", "vhost"}

def qmp_typed(props: dict) -> dict:
    """The -readconfig strings as the typed values QMP's device_add and netdev_add want."""
    out = {}
    for k, v in props.items():
        if k in QMP_INTS:
            out[k] = int(str(v), 0)
        elif k in QMP_BOOLS:
            out[k] = str(v) in ("on", "true", "1", "True")
        else:
            out[k] = v
    return out

def _device_add(vmid: int, qid: str, props: dict) -> None:
    extra = {}
    if props.get("driver") == "vfio-pci":
        extra["timeout"] = 600          # the first one pins all of the VM's memory
    Q.mon_cmd(vmid, "device_add", id=qid, **qmp_typed(props), **extra)

def _wait_gone(vmid: int, ids: list[str], timeout: float) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        if not set(ids) & peripherals(vmid):
            return True
        time.sleep(0.5)
    return False

# ------------------------------------------------- macOS slot power handshake
# macOS (IOPCIFamily's IOPCI2PCIBridge) drives a PCIe slot that has a power controller
# with the attention-button state machine.  It releases a device only that way: blink,
# driver teardown, slot power off, indicator off -- the write that completes QEMU's
# device_del.  It switches a slot on only when the device's link is down at the
# hot-plug event, but QEMU reports the link up at once, so a device hot-plugged into a
# slot that macOS switched off stays unpowered.  The handshake: a key device without
# link-active reporting makes macOS switch the slot on; the attention button takes
# macOS out of its hot-add state; the key leaves on the host side (unrealized, moved to
# function 7 and deleted: QEMU removes a function without function 0 at once); the
# real device then arrives in a powered slot with its link up, and macOS takes it.
# The state is read from the port's config space through the PCIe MMCONFIG window
# (HMP xp: plain reads, no side effects).
KEY_DRIVER = "pvpanic-pci"
SLTCTL_PCC, SLTCTL_PIC, SLTCTL_HPIE = 0x400, 0x300, 0x20
PIC_NAMES = {0x100: "on", 0x200: "blink", 0x300: "off", 0: "-"}
_mmcfg: dict[int, tuple[int, int]] = {}

def _hmp(vmid: int, line: str) -> str:
    return Q.mon_cmd(vmid, "human-monitor-command", **{"command-line": line})

def _xp_words(vmid: int, addr: int, n: int) -> list[int]:
    out = _hmp(vmid, f"xp /{n}wx {addr:#x}")
    return [int(w, 16) for line in out.splitlines() if ":" in line for w in line.split(":", 1)[1].split()][:n]

def _port_cfg(vmid: int, bus: str) -> int:
    """MMCONFIG address of the port's config space (TB ports sit on bus 0)."""
    i = int(bus[len(PORT_PREFIX):])
    return (TB_SLOT << 15) | (i << 12)

def _mmcfg_info(vmid: int, bus: str, fresh: bool = False) -> tuple[int, int]:
    """(MMCONFIG base, PCIe capability offset), checked on the port's vendor ID."""
    if not fresh and vmid in _mmcfg:
        return _mmcfg[vmid]
    base = None
    for line in _hmp(vmid, "info mtree").splitlines():
        if "pcie-mmcfg-mmio" in line:
            base = int(line.strip().split("-", 1)[0], 16)
            break
    if base is None:
        raise Q.QdevError("the VM has no PCIe MMCONFIG window yet")
    words = _xp_words(vmid, base + _port_cfg(vmid, bus), 64)
    if len(words) < 64 or words[0] & 0xffff != 0x1b36:
        raise Q.QdevError(f"{bus}: config space not readable yet")
    cfg = b"".join(w.to_bytes(4, "little") for w in words)
    pos, cap = cfg[0x34], None
    for _ in range(48):
        if not pos or pos >= 0xff:
            break
        if cfg[pos] == 0x10:
            cap = pos
            break
        pos = cfg[pos + 1]
    if cap is None:
        raise Q.QdevError(f"{bus}: no PCIe capability")
    _mmcfg[vmid] = (base, cap)
    return base, cap

def slot_state(vmid: int, bus: str) -> dict:
    """The port's Slot Control as the guest set it: power, indicator, interrupts."""
    for fresh in (False, True):
        try:
            base, cap = _mmcfg_info(vmid, bus, fresh)
            w = _xp_words(vmid, base + _port_cfg(vmid, bus) + (cap & ~3) + 0x18, 1)
            w0 = _xp_words(vmid, base + _port_cfg(vmid, bus), 1)
            if not w or not w0 or w0[0] & 0xffff != 0x1b36:
                raise Q.QdevError(f"{bus}: config space not readable")
            ctl = (w[0] >> (8 * ((cap + 0x18) & 3))) & 0xffff
            return {"ctl": ctl, "power": not ctl & SLTCTL_PCC, "pic": PIC_NAMES[ctl & SLTCTL_PIC],
                    "ints": bool(ctl & SLTCTL_HPIE)}
        except Q.QdevError:
            _mmcfg.pop(vmid, None)
            if fresh:
                raise
    raise Q.QdevError(f"{bus}: no slot state")

def wait_port_ready(vmid: int, bus: str, timeout: float) -> bool:
    """Until the guest's PCIe driver runs the port (hot-plug interrupts enabled)."""
    end = time.time() + timeout
    while time.time() < end:
        if not Q.vm_running(vmid):
            return False
        try:
            if slot_state(vmid, bus)["ints"]:
                return True
        except Q.QdevError:
            pass
        time.sleep(2)
    return False

def _drop_key(vmid: int, key: str) -> None:
    path = f"/machine/peripheral/{key}"
    Q.mon_cmd(vmid, "qom-set", path=path, property="realized", value=False)
    Q.mon_cmd(vmid, "qom-set", path=path, property="addr", value="0.7")
    for _ in range(40):
        try:
            Q.mon_cmd(vmid, "device_del", id=key)
            break
        except Q.QdevError as e:
            if "process of unplug" not in str(e):
                raise
            time.sleep(0.25)
    _wait_gone(vmid, [key], 3.0)

def power_slot(vmid: int, conf: dict, vmcfg: dict, bus: str) -> str | None:
    """Have macOS switch the port's slot on before a device is hot-plugged into it."""
    if not slot_handshake(conf, vmcfg) or not is_port(bus):
        return None
    st = slot_state(vmid, bus)
    if not st["ints"]:
        raise Q.QdevError(f"{bus}: the guest has not started this port yet (macOS still booting?)")
    if st["power"]:
        if st["pic"] == "blink":
            raise Q.QdevError(f"{bus}: the guest is busy with this slot (indicator blinking); try again")
        return None
    key = f"{bus}-key"
    if key in peripherals(vmid):
        Q.mon_cmd(vmid, "device_del", id=key)      # a leftover in a switched-off slot: removed at once
        _wait_gone(vmid, [key], 3.0)
    Q.mon_cmd(vmid, "device_add", driver=KEY_DRIVER, id=key, bus=bus, addr="0.0",
              **{"x-pcie-lnksta-dllla": False})
    end = time.time() + 20
    while time.time() < end:
        st = slot_state(vmid, bus)
        if st["power"] and st["pic"] == "on":
            break
        time.sleep(0.25)
    else:
        try:
            Q.mon_cmd(vmid, "device_del", id=key)  # still switched off: QEMU removes it without the guest
            _wait_gone(vmid, [key], 3.0)
        except Q.QdevError:
            pass
        raise Q.QdevError(f"{bus}: the guest did not switch the slot on (indicator {st['pic']})")
    Q.mon_cmd(vmid, "device_del", id=key)          # attention button: macOS leaves its hot-add state
    time.sleep(5.3)                                # QEMU takes the next device_del of the key after 5 s
    _drop_key(vmid, key)
    return f"{bus}: slot switched on by the guest"

def target_present(vmid: int, dev: dict, present: set[str]) -> bool:
    return dev["bus"] in present

def plug(vmid: int, conf: dict, dev: dict) -> str:
    """Hot-plug DEV into the running VM. Returns a status line."""
    present = peripherals(vmid)
    if dev["id"] in present:
        return f"{dev['id']}: already attached"
    if not target_present(vmid, dev, present):
        what = "controller" if dev["plugin"] == "tb-usb" else "TB port"
        return f"{dev['id']}: {what} {dev['bus']} is not in the running VM; present from the next VM start"
    kind = dev["plugin"]
    if kind in PORT_KINDS:
        power_slot(vmid, conf, Q.vm_config(vmid), dev["bus"])
    if kind == "tb-pci":
        # QEMU 11 (hw/vfio/container-legacy.c) casts every VFIO container of the address
        # space to a legacy one when a vfio-pci device connects, and aborts on the
        # container of a vfio-user device.  At VM start vfio-pci comes first: no problem.
        users = [d["id"] for d in conf["devices"] if Q.dev_kind(d) == "vfio-user" and d["id"] in present]
        if users:
            raise Q.QdevError(f"QEMU 11 aborts when a PCI pass-through device is hot-plugged while a vfio-user "
                              f"device is attached ({', '.join(users)}): {dev['id']} is present from the next VM "
                              f"start (or unplug {users[0]} first: qdevctl stop {vmid} {users[0]})")
        info = perl("pci-prepare", vmid, dev["args"], "60")
        funcs = pci_functions(dev, info)
        try:
            # the slot shows up when function 0 arrives: the others go first
            for qid, props in funcs[1:] + funcs[:1]:
                _device_add(vmid, qid, props)
        except Q.QdevError:
            for qid, _ in funcs:
                try:
                    Q.mon_cmd(vmid, "device_del", id=qid)
                except Q.QdevError:
                    pass
            perl("pci-release", vmid, json.dumps(info["ids"]))
            raise
        perl("pci-own", vmid, json.dumps(info["ids"]), vm_pid(vmid))
        dev["_ids"] = info["ids"]
        return f"{dev['id']}: {' '.join(info['ids'])} attached to {dev['bus']}"
    if kind == "tb-net":
        spec = net_spec(dev["args"])
        write_net_settings(vmid, dev, spec)
        nd = net_netdev(vmid, dev, spec)
        params = qmp_typed({k: v for k, v in nd.items() if k != "type"})
        err = NET_RUN_DIR / f"{tap_name(vmid, dev)}.err"
        err.unlink(missing_ok=True)
        try:
            Q.mon_cmd(vmid, "netdev_add", type="tap", id=dev["id"], **params)
        except Q.QdevError as e:
            why = err.read_text().strip() if err.exists() else ""
            raise Q.QdevError(f"{e}: {why}" if why else str(e)) from None
        try:
            _device_add(vmid, dev["id"], net_device(dev, spec))
        except Q.QdevError:
            Q.mon_cmd(vmid, "netdev_del", id=dev["id"])
            raise
        return f"{dev['id']}: {spec['model']} {spec['macaddr']} on {spec['bridge']} attached to {dev['bus']}"
    if kind == "tb-xhci":
        _device_add(vmid, dev["id"], xhci_device(dev))
        msgs = [f"{dev['id']}: USB controller attached to {dev['bus']}"]
        for child in [d for d in tb_devices(conf) if d["plugin"] == "tb-usb" and d["bus"] == dev["id"]]:
            try:
                _device_add(vmid, child["id"], usb_device(vmid, child))
                msgs.append(f"{child['id']}: attached")
            except Q.QdevError as e:
                msgs.append(f"{child['id']}: {e}")
        return "; ".join(msgs)
    if kind == "tb-usb":
        _device_add(vmid, dev["id"], usb_device(vmid, dev))
        return f"{dev['id']}: attached to {dev['bus']}"
    raise Q.QdevError(f"unknown TB device kind {kind}")

def unplug(vmid: int, dev: dict, timeout: float = 30.0) -> str:
    kind = dev["plugin"]
    present = peripherals(vmid)
    ids = [q for q in present if q == dev["id"] or q.startswith(f"{dev['id']}-f")]
    if not ids:
        if kind == "tb-net":
            try:
                Q.mon_cmd(vmid, "netdev_del", id=dev["id"])
            except Q.QdevError:
                pass
        return f"{dev['id']}: not attached"
    Q.mon_cmd(vmid, "device_del", id=dev["id"])
    gone = _wait_gone(vmid, ids, 3.0 if kind == "tb-usb" else timeout)
    if not gone:
        conf = Q.load_conf(vmid)
        if kind != "tb-usb" and power_controller(conf, Q.read_vm_config(vmid)) == "off":
            return (f"{dev['id']}: device_del sent; the guest cannot switch off a port without a power "
                    "controller, so QEMU removes the device when the VM stops")
        return f"{dev['id']}: device_del sent, the guest has not released it yet"
    if kind == "tb-net":
        try:
            Q.mon_cmd(vmid, "netdev_del", id=dev["id"])
        except Q.QdevError:
            pass
        (NET_RUN_DIR / f"{tap_name(vmid, dev)}.json").unlink(missing_ok=True)
    if kind == "tb-pci":
        try:
            info = perl("pci-resolve", vmid, dev["args"])
            perl("pci-release", vmid, json.dumps(info["ids"]))
        except Q.QdevError as e:
            Q.log(f"{dev['id']}: releasing the PCI reservation: {e}")
    return f"{dev['id']}: detached"

# ------------------------------------------------------------ operations
def on_port(conf: dict) -> list[dict]:
    """Devices that take a TB port: QEMU's own and qdev-host's (tb-vga, virgl-gpu)."""
    return [d for d in conf["devices"] if d.get("plugin") != "tb-usb" and is_port(d.get("bus", ""))]

def free_port(conf: dict) -> str:
    used = {d["bus"] for d in on_port(conf)}
    for i in range(port_count(conf)):
        if port_id(i) not in used:
            return port_id(i)
    raise Q.QdevError(f"all {port_count(conf)} TB ports are taken (qdevctl tb VMID --ports N gives the bus more, "
                      f"up to {MAX_PORTS}, from the next VM start)")

def next_tap(conf: dict) -> int:
    used = {d.get("tap") for d in tb_devices(conf)}
    i = TAP_BASE
    while i in used:
        i += 1
    if i > 99:
        raise Q.QdevError("too many TB network devices")
    return i

def normalize(vmid: int, conf: dict, dev: dict, notes: list[str]) -> None:
    """Check DEV's args and fill in what qdev-host chooses (bus, MAC, tap)."""
    kind = dev["plugin"]
    if kind == "tb-pci":
        pci_check(dev["args"])
        info = perl("pci-resolve", vmid, dev["args"])
        if info.get("x-vga"):
            raise Q.QdevError("x-vga (primary GPU) needs the standard bus")
        others = [d for d in tb_devices(conf) if d["plugin"] == "tb-pci" and d["id"] != dev["id"]]
        for o in others:
            if set(perl("pci-resolve", vmid, o["args"])["ids"]) & set(info["ids"]):
                raise Q.QdevError(f"{o['id']} already passes through {', '.join(info['ids'])}")
    elif kind == "tb-net":
        spec = net_spec(dev["args"])
        if not spec["macaddr"]:
            spec["macaddr"] = random_mac()
        if spec.pop("firewall_ignored"):
            notes.append("the PVE firewall does not filter TB bus network devices (firewall ignored)")
        dev["args"] = net_args(spec)
        dev.setdefault("tap", next_tap(conf))
    elif kind == "tb-xhci":
        xhci_check(dev["args"])
    elif kind == "tb-usb":
        usb_check(dev["args"])
        perl("usb-resolve", vmid, usb_value(dev["args"]))
        ctrls = [d["id"] for d in tb_devices(conf) if d["plugin"] == "tb-xhci"]
        if dev["bus"] in ("", "tb", Q.PORT_ID, None):
            if len(ctrls) != 1:
                raise Q.QdevError("give the USB controller (--bus ID of a tb-xhci device)" if ctrls else
                                  "the VM has no USB controller on the TB bus: add a tb-xhci device first")
            dev["bus"] = ctrls[0]
        elif dev["bus"] not in ctrls:
            raise Q.QdevError(f"{dev['bus']} is not a TB USB controller of VM {vmid}")
    if kind in PORT_KINDS:
        if dev["bus"] in ("", "tb", Q.PORT_ID, None):
            dev["bus"] = free_port(conf)
        elif not is_port(dev["bus"]) or int(dev["bus"][len(PORT_PREFIX):]) >= port_count(conf):
            raise Q.QdevError(f"{dev['bus']} is not a TB port of VM {vmid} (qdev-tb0..qdev-tb{port_count(conf) - 1})")
        else:
            taken = next((d["id"] for d in on_port(conf) if d["bus"] == dev["bus"] and d["id"] != dev["id"]), None)
            if taken:
                raise Q.QdevError(f"{dev['bus']} already holds {taken}")

def add(vmid: int, conf: dict, vmcfg: dict, dev: dict) -> list[str]:
    notes: list[str] = []
    if any(o["id"] == dev["id"] or dev["id"].startswith(o["id"] + "-f") or o["id"].startswith(dev["id"] + "-f")
           for o in conf["devices"]):
        raise Q.QdevError(f"device id {dev['id']} clashes with a device of VM {vmid}")
    if re.match(r"^(net|hostpci|usb)\d+$", dev["id"]):
        raise Q.QdevError("PVE uses netN, hostpciN and usbN itself: pick another id")
    conf.setdefault("tb", {"ports": DEFAULT_PORTS})
    normalize(vmid, conf, dev, notes)
    Q.check_editable(vmid)
    Q.snippets_dir()
    had_hook = vmcfg.get("hookscript")
    conf["devices"].append(dev)
    try:
        write_files(vmid, conf, vmcfg)
        changed = ensure_args(vmid, vmcfg, conf)
        Q.ensure_hookscript(vmid, vmcfg, conf)
    except Exception:
        conf["devices"].remove(dev)
        raise
    Q.save_conf(conf)
    msgs = [f"VM {vmid}: {dev['plugin']} {dev['id']} defined on {dev['bus']}"] + [f"note: {n}" for n in notes]
    if Q.vm_running(vmid):
        try:
            msgs.append(plug(vmid, conf, dev))
        except Q.QdevError as e:
            msgs.append(f"{dev['id']}: hot-plug failed: {e}")
        if changed:
            msgs.append("note: the TB bus was added to the VM's args; it appears at the next VM start")
    else:
        msgs.append("VM is stopped: the device is present from the next VM start")
    return msgs

def update(vmid: int, conf: dict, dev: dict, args: str) -> list[str]:
    new = dict(dev, args=args)
    notes: list[str] = []
    others = dict(conf, devices=[d for d in conf["devices"] if d["id"] != dev["id"]])
    normalize(vmid, others, new, notes)
    Q.check_editable(vmid)
    msgs = []
    running = Q.vm_running(vmid)
    attached = running and dev["id"] in peripherals(vmid)
    if attached:
        msgs.append(unplug(vmid, dev))
    conf["devices"] = [new if d["id"] == dev["id"] else d for d in conf["devices"]]
    Q.save_conf(conf)
    write_files(vmid, conf, Q.vm_config(vmid))
    if running:
        try:
            msgs.append(plug(vmid, conf, new))
        except Q.QdevError as e:
            msgs.append(f"{dev['id']}: hot-plug failed: {e}")
    return [f"VM {vmid}: {dev['id']} updated" + (", unplugged and plugged again" if attached else "")] + \
        [f"note: {n}" for n in notes] + msgs

def remove(vmid: int, conf: dict, dev: dict) -> list[str]:
    if dev["plugin"] == "tb-xhci":
        kids = [d["id"] for d in tb_devices(conf) if d["plugin"] == "tb-usb" and d["bus"] == dev["id"]]
        if kids:
            raise Q.QdevError(f"remove the USB devices on {dev['id']} first: {', '.join(kids)}")
    Q.check_editable(vmid)
    msgs = []
    if Q.vm_running(vmid):
        msgs.append(unplug(vmid, dev))
    conf["devices"] = [d for d in conf["devices"] if d["id"] != dev["id"]]
    write_files(vmid, conf, Q.vm_config(vmid))
    return msgs

def purge(vmid: int, conf: dict, vmcfg: dict) -> None:
    """The VM has no TB devices left and the user asked to remove the bus."""
    remove_args(vmid, vmcfg)
    conf.pop("tb", None)
    for p in (cfg_path(vmid), aml_path(vmid)):
        p.unlink(missing_ok=True)

def set_ports(vmid: int, conf: dict, n: int, acpi: str | None = None, hotplug: str | None = None,
              power_controller: str | None = None) -> list[str]:
    if acpi is not None and acpi not in ACPI_MODES and acpi != "auto":
        raise Q.QdevError(f"ACPI properties: auto, {', '.join(ACPI_MODES)}")
    if hotplug is not None and hotplug not in HOTPLUG_MODES:
        raise Q.QdevError(f"hot-plug: {', '.join(HOTPLUG_MODES)}")
    if power_controller is not None and power_controller not in ("on", "off", "default"):
        raise Q.QdevError("power controller: on, off or default")
    if not 1 <= n <= MAX_PORTS:
        raise Q.QdevError(f"the TB bus has 1..{MAX_PORTS} ports")
    used = [d for d in on_port(conf) if int(d["bus"][len(PORT_PREFIX):]) >= n]
    if used:
        raise Q.QdevError(f"{', '.join(d['id'] for d in used)} use ports beyond {n}")
    Q.check_editable(vmid)
    conf.setdefault("tb", {})["ports"] = n
    if acpi == "auto":
        conf["tb"].pop("acpi", None)
    elif acpi is not None:
        conf["tb"]["acpi"] = acpi
    if hotplug is not None:
        conf["tb"]["hotplug"] = hotplug
    if power_controller == "default":
        conf["tb"].pop("power-controller", None)
    elif power_controller is not None:
        conf["tb"]["power-controller"] = power_controller
    vmcfg = Q.vm_config(vmid)
    write_files(vmid, conf, vmcfg)
    changed = ensure_args(vmid, vmcfg, conf)
    Q.ensure_hookscript(vmid, vmcfg, conf)
    Q.save_conf(conf)
    mode = ""
    if macos_guest(vmcfg):
        mode = (", macOS slot power (hot-plug and hot-unplug)" if slot_handshake(conf, vmcfg)
                else ", macOS: hot-plug only (a device leaves when the VM stops)")
    return [f"VM {vmid}: TB bus with {n} port(s), ACPI properties {acpi_mode(conf, vmcfg)}, "
            f"{hotplug_mode(conf, vmcfg)} hot-plug{mode}" + (", args updated" if changed else "") +
            (": the change applies at the next VM start" if Q.vm_running(vmid) else "")]

# --------------------------------------------------------------- hooks
def pre_start(vmid: int, conf: dict) -> list[str]:
    if not conf.get("tb"):
        return []
    vmcfg = Q.vm_config(vmid)
    notes = [] if in_args(vmid, vmcfg, conf) else [
        f"VM {vmid}'s args do not match its TB bus settings (edited by hand?); run: qdevctl tb {vmid}"]
    return notes + write_files(vmid, conf, vmcfg, starting=True)

BOOT_UNIT = "qdev-host-tb-boot-{vmid}"

def deferred(vmid: int, conf: dict, vmcfg: dict) -> list[dict]:
    """Devices hot-plugged once the guest runs its TB ports (macOS slot power): plugged
    while the firmware runs, a device sits in a switched-off slot and macOS, starting
    the port, finds it unpowered and then releases it.  Order: PCI pass-through first,
    then vfio-user devices (QEMU 11)."""
    if not slot_handshake(conf, vmcfg):
        return []
    later = [d for d in tb_devices(conf) if d["plugin"] == "tb-pci"]
    users = [d for d in conf["devices"] if Q.dev_kind(d) == "vfio-user"]
    return later + (users if later else [d for d in users if is_port(d["bus"])])

def post_start(vmid: int, conf: dict) -> list[str]:
    """Hot-plug the PCI pass-through devices; before qdev-host's vfio-user devices,
    which QEMU 11 must not have when a vfio-pci device connects."""
    later = deferred(vmid, conf, Q.vm_config(vmid))
    if later:
        unit = BOOT_UNIT.format(vmid=vmid)
        Q.run(["systemctl", "stop", unit], check=False)
        Q.run(["systemctl", "reset-failed", unit], check=False)
        Q.run(["systemd-run", f"--unit={unit}", "--collect", "--quiet",
               "/usr/sbin/qdevctl", "tb-boot", str(vmid)], check=False)
        return [f"{', '.join(d['id'] for d in later)}: hot-plugged once the guest has started its TB ports"]
    msgs = []
    for dev in tb_devices(conf):
        if dev["plugin"] == "tb-pci":
            try:
                msgs.append(plug(vmid, conf, dev))
            except Q.QdevError as e:
                msgs.append(f"{dev['id']}: {e}")
    return msgs

def boot_attach(vmid: int) -> int:
    """qdevctl tb-boot: the deferred hot-plugs of a VM start (macOS slot power)."""
    conf = Q.load_conf(vmid)
    rc = 0
    for dev in deferred(vmid, conf, Q.vm_config(vmid)):
        if is_port(dev["bus"]) and not wait_port_ready(vmid, dev["bus"], 900):
            print(f"{dev['id']}: the guest did not start {dev['bus']}; not attached", flush=True)
            rc = 1
            continue
        try:
            print(Q.attach(vmid, dev), flush=True)
        except Q.QdevError as e:
            print(f"{dev['id']}: {e}", flush=True)
            rc = 1
    return rc

def post_stop(vmid: int, conf: dict) -> None:
    Q.run(["systemctl", "stop", BOOT_UNIT.format(vmid=vmid)], check=False)
    ids = []
    for dev in tb_devices(conf):
        if dev["plugin"] == "tb-pci":
            try:
                ids += perl("pci-resolve", vmid, dev["args"])["ids"]
            except Q.QdevError:
                pass
        if dev["plugin"] == "tb-net":
            (NET_RUN_DIR / f"{tap_name(vmid, dev)}.json").unlink(missing_ok=True)
    if ids:
        perl("pci-release", vmid, json.dumps(ids))

def describe(vmid: int, conf: dict, vmcfg: dict, running: bool) -> tuple[dict, dict[str, str]]:
    """The bus for the panel, and the attached TB devices (id -> where)."""
    present = peripherals(vmid) if running and tb_devices(conf) else set()
    where = {}
    for d in tb_devices(conf):
        if d["id"] in present:
            where[d["id"]] = d["bus"]
    ports_in_vm = running and port_id(0) in (present or peripherals(vmid))
    info = {"enabled": bool(conf.get("tb")), "ports": port_count(conf), "acpi": acpi_mode(conf, vmcfg), "in_args": in_args(vmid, vmcfg, conf), "hotplug": hotplug_mode(conf, vmcfg),
            "power_controller": power_controller(conf, vmcfg) or "on", "slot_power": slot_handshake(conf, vmcfg),
            "in_vm": bool(ports_in_vm)}
    return info, where
