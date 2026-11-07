#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Proxmox VE integration library for qdev-host.

Everything here uses stock PVE tooling only: `qm` for VM config edits, the
node's own PVE::QemuServer::Monitor (mon_cmd) for QMP, systemd for the device
processes, and the standard guest hookscript mechanism for VM start/stop.
"""
from __future__ import annotations
import json, os, re, shlex, shutil, subprocess, sys, time
from pathlib import Path

CONF_DIR = Path("/etc/qdev-host/vms")
UNIT_DIR = Path("/etc/qdev-host/units")
RUN_DIR = Path("/run/qdev-host")
STATE_DIR = Path("/var/lib/qdev-host")
PLUGIN_DIR = Path("/usr/lib/qdev-host/plugins")
SHARE_DIR = Path("/usr/share/qdev-host")
HOOK_SRC = SHARE_DIR / "pve" / "qdev-host-hook.pl"
HOOK_NAME = "qdev-host-hook.pl"
PORT_ID = "qdev-rp0"
QSM_BUS = "/run/qsm-pve-direct/{vmid}/qemu-display1.bus"

class QdevError(Exception):
    pass

def log(msg: str) -> None:
    print(f"qdevctl: {msg}", file=sys.stderr)

def run(cmd: list[str], check: bool = True, quiet: bool = False) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise QdevError(f"{' '.join(shlex.quote(c) for c in cmd)} failed: {r.stderr.strip() or r.stdout.strip()}")
    return r

# ------------------------------------------------------------------ config
def conf_path(vmid: int) -> Path:
    return CONF_DIR / f"{vmid}.json"

def load_conf(vmid: int) -> dict:
    p = conf_path(vmid)
    if not p.exists():
        return {"vmid": vmid, "devices": [], "port_addr": "0x08", "previous_hookscript": None}
    return json.loads(p.read_text())

def save_conf(conf: dict) -> None:
    CONF_DIR.mkdir(parents=True, exist_ok=True)
    p = conf_path(conf["vmid"])
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(conf, indent=2) + "\n")
    tmp.chmod(0o640)
    os.replace(tmp, p)

def find_device(conf: dict, dev_id: str) -> dict | None:
    for d in conf["devices"]:
        if d["id"] == dev_id:
            return d
    return None

# ---------------------------------------------------------------- PVE: qm
def vm_config(vmid: int) -> dict[str, str]:
    r = run(["qm", "config", str(vmid)])
    out = {}
    for line in r.stdout.splitlines():
        if ":" in line and not line.startswith(" "):
            k, v = line.split(":", 1)
            out[k.strip()] = v.strip()
    return out

def vm_running(vmid: int) -> bool:
    r = run(["qm", "status", str(vmid)])
    return "running" in r.stdout

def qm_set(vmid: int, key: str, value: str | None) -> None:
    if value is None or value == "":
        run(["qm", "set", str(vmid), "--delete", key])
    else:
        run(["qm", "set", str(vmid), f"--{key}", value])

# ------------------------------------------------------------ root port
def port_device_arg(addr: str) -> str:
    # a root port of our own with reserved windows: hot-plugged BARs of tens of MiB need them
    return (f"-device pcie-root-port,id={PORT_ID},bus=pcie.0,addr={addr},chassis=60,slot=60,port=60,"
            f"pref64-reserve=256M,mem-reserve=16M")

def ensure_port_in_args(vmid: int, cfg: dict[str, str], addr: str) -> bool:
    """Add our root port to the VM's `args`. Returns True if the config changed."""
    args = cfg.get("args", "")
    if f"id={PORT_ID}" in args:
        return False
    new = (args + " " + port_device_arg(addr)).strip()
    qm_set(vmid, "args", new)
    return True

def remove_port_from_args(vmid: int, cfg: dict[str, str]) -> None:
    args = cfg.get("args", "")
    if f"id={PORT_ID}" not in args:
        return
    parts = args.split(" -device ")
    kept = [p for p in parts if f"id={PORT_ID}" not in p]
    new = " -device ".join(kept).strip()
    qm_set(vmid, "args", new if new else None)

# ------------------------------------------------------------- hookscript
def snippets_dir() -> Path:
    """Directory of the first storage with `snippets` content (usually local)."""
    text = Path("/etc/pve/storage.cfg").read_text()
    name = path = None; content = ""
    for line in text.splitlines() + ["dir: __end__"]:
        if re.match(r"^\w+:", line):
            if name and path and "snippets" in content:
                return Path(path) / "snippets", name
            name = line.split(":", 1)[1].strip(); path = None; content = ""
            if not line.startswith("dir:"):
                name = None
        elif name:
            s = line.strip()
            if s.startswith("path "): path = s.split(None, 1)[1]
            if s.startswith("content "): content = s.split(None, 1)[1]
    raise QdevError("no directory storage with 'snippets' content (enable it on 'local' in Datacenter > Storage)")

def ensure_hookscript(vmid: int, cfg: dict[str, str], conf: dict) -> None:
    d, storage = snippets_dir()
    d.mkdir(parents=True, exist_ok=True)
    dst = d / HOOK_NAME
    if not dst.exists() or dst.read_bytes() != HOOK_SRC.read_bytes():
        shutil.copyfile(HOOK_SRC, dst)
    dst.chmod(0o755)
    want = f"{storage}:snippets/{HOOK_NAME}"
    cur = cfg.get("hookscript")
    if cur == want:
        return
    if cur:
        # remember the VM's own hookscript so ours can chain it
        vol = run(["pvesm", "path", cur]).stdout.strip()
        conf["previous_hookscript"] = vol
        log(f"VM {vmid} already had hookscript {cur}; it will be chained after ours")
    qm_set(vmid, "hookscript", want)

def restore_hookscript(vmid: int, conf: dict) -> None:
    prev = conf.get("previous_hookscript")
    if prev:
        # map the path back to a volume id if it lives in a snippets dir
        d, storage = snippets_dir()
        p = Path(prev)
        qm_set(vmid, "hookscript", f"{storage}:snippets/{p.name}" if p.parent == d else None)
    else:
        qm_set(vmid, "hookscript", None)

# ------------------------------------------------------------- monitor
PERL = r'''
use strict; use warnings; use JSON::PP; use PVE::QemuServer::Monitor;
my ($vmid, $cmd, $json) = @ARGV;
my $params = decode_json($json);
my $res = eval { PVE::QemuServer::Monitor::mon_cmd($vmid, $cmd, %$params) };
if ($@) { print STDERR $@; exit 1; }
print JSON::PP->new->canonical->encode({ result => $res });
'''

def mon_cmd(vmid: int, cmd: str, **params):
    r = subprocess.run(["perl", "-e", PERL, str(vmid), cmd, json.dumps(params)], capture_output=True, text=True)
    if r.returncode != 0:
        raise QdevError(f"{cmd}: {r.stderr.strip()}")
    return json.loads(r.stdout)["result"]

def pci_devices(vmid: int) -> list[dict]:
    out = []
    def walk(devs, busnum, parent):
        for d in devs:
            out.append({"bus": busnum, "slot": d["slot"], "qdev_id": d.get("qdev_id", ""), "parent": parent,
                        "vendor": d["id"]["vendor"], "device": d["id"]["device"]})
            br = d.get("pci_bridge")
            if br and br.get("devices"):
                walk(br["devices"], br["bus"]["number"], d.get("qdev_id", ""))
    for root in mon_cmd(vmid, "query-pci"):
        walk(root["devices"], root["bus"], "")
    return out

def device_present(vmid: int, dev_id: str) -> bool:
    return any(d["qdev_id"] == dev_id for d in pci_devices(vmid))

def bus_present(vmid: int, bus: str) -> bool:
    return any(d["qdev_id"] == bus for d in pci_devices(vmid))

def device_add(vmid: int, dev: dict, sock: Path) -> None:
    mon_cmd(vmid, "device_add", driver="vfio-user-pci", id=dev["id"], bus=dev["bus"],
            socket={"type": "unix", "path": str(sock)})

def device_del(vmid: int, dev_id: str, timeout: float = 20.0) -> bool:
    if not device_present(vmid, dev_id):
        return True
    mon_cmd(vmid, "device_del", id=dev_id)
    end = time.time() + timeout
    while time.time() < end:
        if not device_present(vmid, dev_id):
            return True
        time.sleep(0.5)
    return False

# --------------------------------------------------------------- systemd
def unit_name(vmid: int, dev_id: str) -> str:
    return f"qdev-host-device@{vmid}-{dev_id}.service"

def unit_run_dir(vmid: int, dev_id: str) -> Path:
    return RUN_DIR / f"{vmid}-{dev_id}"

def device_socket(vmid: int, dev_id: str) -> Path:
    return unit_run_dir(vmid, dev_id) / "vfio.sock"

def device_display(vmid: int, dev_id: str) -> Path:
    return unit_run_dir(vmid, dev_id) / "display.sock"

def build_args(vmid: int, dev: dict) -> list[str]:
    plugin = PLUGIN_DIR / f"{dev['plugin']}.so"
    if not plugin.exists():
        raise QdevError(f"plugin not installed: {plugin}")
    args = ["--plugin", str(plugin), "--socket", str(device_socket(vmid, dev["id"])),
            "--dump-dir", str(STATE_DIR), "--vm-name", f"VM{vmid}-{dev['id']}"]
    if dev.get("args"):
        args += ["--plugin-args", dev["args"]]
    if dev.get("display", True):
        args += ["--display-p2p", str(device_display(vmid, dev["id"]))]
    if dev.get("kms"):
        args += ["--kms", dev["kms"]]
    qsm = Path(QSM_BUS.format(vmid=vmid))
    if dev.get("display", True) and qsm.exists():
        args += ["--qemu-display", f"unix:path={qsm}", "--qemu-display-dest", "org.qemu", "--qemu-console", "0"]
    for extra in dev.get("extra", []):
        args.append(extra)
    return args

def write_unit_env(vmid: int, dev: dict) -> None:
    UNIT_DIR.mkdir(parents=True, exist_ok=True)
    p = UNIT_DIR / f"{vmid}-{dev['id']}.env"
    p.write_text("QDEV_ARGS=" + " ".join(build_args(vmid, dev)) + "\n")
    p.chmod(0o640)

def start_process(vmid: int, dev: dict) -> None:
    write_unit_env(vmid, dev)
    run(["systemctl", "start", unit_name(vmid, dev["id"])])
    sock = device_socket(vmid, dev["id"])
    for _ in range(50):
        if sock.exists():
            return
        time.sleep(0.1)
    raise QdevError(f"{unit_name(vmid, dev['id'])} did not create {sock}; see journalctl -u {unit_name(vmid, dev['id'])}")

def stop_process(vmid: int, dev_id: str) -> None:
    run(["systemctl", "stop", unit_name(vmid, dev_id)], check=False)

def process_active(vmid: int, dev_id: str) -> bool:
    return run(["systemctl", "is-active", unit_name(vmid, dev_id)], check=False).stdout.strip() == "active"

# --------------------------------------------------------------- flows
def attach(vmid: int, dev: dict) -> str:
    """Start the device process (if needed) and hot-plug it. Returns a status line."""
    if not process_active(vmid, dev["id"]):
        start_process(vmid, dev)
    if device_present(vmid, dev["id"]):
        return f"{dev['id']}: already attached"
    if not bus_present(vmid, dev["bus"]):
        return (f"{dev['id']}: bus {dev['bus']} is not in the running VM "
                f"(the root port is added at the next VM start); process running, not attached")
    device_add(vmid, dev, device_socket(vmid, dev["id"]))
    return f"{dev['id']}: attached to {dev['bus']} (plugin {dev['plugin']})"

def detach(vmid: int, dev: dict, keep_process: bool) -> str:
    gone = True
    if vm_running(vmid):
        gone = device_del(vmid, dev["id"])
    if not keep_process:
        stop_process(vmid, dev["id"])
    return f"{dev['id']}: {'detached' if gone else 'device_del sent, guest has not released it yet'}" + ("" if keep_process else ", process stopped")
