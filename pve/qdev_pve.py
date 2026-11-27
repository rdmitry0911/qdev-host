#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Proxmox VE integration library for qdev-host.

Everything here uses stock PVE tooling only: `qm` for VM config edits, the
node's own PVE::QemuServer::Monitor (mon_cmd) for QMP, systemd for the device
processes, and the standard guest hookscript mechanism for VM start/stop.
"""
from __future__ import annotations
import json, os, re, shlex, shutil, socket, subprocess, sys, time
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
VUG_BINARY = Path("/usr/lib/kvm/vhost-user-gpu")   # QEMU's own out-of-process virtio-gpu, in pve-qemu-kvm
SHARED_MEM_ID = "qdev-mem"
QSM_GPU_ID = "qsm-direct-gpu"                     # the in-process GPU of QSM Direct's Display1 VirGL profile

# Plugins that are not a qdev-host .so: their device process is another program.
BUILTIN_PLUGINS: dict[str, dict] = {
    "virgl-gpu": {
        "kind": "vhost-user-gpu",
        "title": "VirGL 3D GPU (QEMU vhost-user-gpu)",
        "description": ("A virtio-gpu with VirGL 3D acceleration whose rendering runs in its own process, "
                        "QEMU's vhost-user-gpu on a render node of this node. The VM's own D-Bus display "
                        "shows it (QSM Direct). Present from the next VM start; needs shared guest memory, "
                        "which qdev-host adds unless the VM has hugepages or a virtiofs device."),
        "fields": {"kms": False, "display": False, "persistent": False},
        "args": [
            {"name": "render", "label": "Render node", "type": "render", "default": "",
             "hint": "empty: the first render node of this node"},
            {"name": "display", "label": "Display", "type": "choice", "default": "primary",
             "choices": [["primary", "Primary display (virtio-gpu, no VGA)"],
                         ["vga", "Primary display, VGA-compatible (vhost-user-vga)"],
                         ["secondary", "Additional GPU next to the VM's display"]],
             # devices saved before 0.5.5 carry vga=1|0
             "legacy": {"name": "vga", "map": {"1": "vga", "0": "secondary"}},
             "hint": "primary: the VM's only display (vhost-user-gpu-pci); it replaces the VM's own display "
                     "device and QSM's in-process VirGL GPU until it is removed. The firmware screen needs "
                     "OVMF (SeaBIOS shows nothing until the guest's driver). vga: the same with a VGA part "
                     "(vhost-user-vga), for guests or firmware that need one; QEMU 11 crashes when such a "
                     "guest reboots while a console is open. secondary: next to the VM's own display, "
                     "which stays the one the console shows"},
            {"name": "outputs", "label": "Outputs", "type": "int", "default": 1, "min": 1, "max": 4},
            {"name": "xres", "label": "Width", "type": "int", "default": 1280, "min": 640, "max": 7680},
            {"name": "yres", "label": "Height", "type": "int", "default": 800, "min": 480, "max": 4320},
        ],
    },
    # QEMU's own devices on the TB bus (pve/qdev_tb.py); "dialog" names the PVE dialog that adds them,
    # so the qdev-host Device dialog does not offer them (it still edits a USB controller's ports)
    "tb-xhci": {
        "kind": "tb", "dialog": "usb",
        "title": "USB controller on the TB bus (qemu-xhci)",
        "description": ("QEMU's USB 3 controller on a hot-plug port of the VM's TB bus, a PCIe root port with "
                        "Thunderbolt ACPI properties. USB devices of this node are then added to it "
                        "(Add > USB Device, Bus) and removed while the VM runs, also for guests such as "
                        "macOS that do not take PVE's own USB hot-plug."),
        "fields": {"kms": False, "display": False, "persistent": False},
        "args": [{"name": "ports", "label": "Ports", "type": "int", "default": 8, "min": 1, "max": 15,
                  "hint": "USB 2 and USB 3 ports, each"}],
    },
    "tb-pci": {"kind": "tb", "dialog": "pci", "title": "PCI Device on the TB bus", "description": "",
               "fields": {"kms": False, "display": False, "persistent": False}, "args": []},
    "tb-net": {"kind": "tb", "dialog": "net", "title": "Network Device on the TB bus", "description": "",
               "fields": {"kms": False, "display": False, "persistent": False}, "args": []},
    "tb-usb": {"kind": "tb", "dialog": "usb", "title": "USB Device on the TB bus", "description": "",
               "fields": {"kms": False, "display": False, "persistent": False}, "args": []},
}

def plugin_kind(plugin: str) -> str:
    return BUILTIN_PLUGINS.get(plugin, {}).get("kind", "vfio-user")

def dev_kind(dev: dict) -> str:
    return plugin_kind(dev["plugin"])

# Sidecar keys a plugin's NAME.json may carry besides title/description/args:
#   "fields": {"display": false, "kms": false, "persistent": false} - dialog fields the device does not use
#   "shared_memory": true - the plugin reaches guest memory directly (DMA, queues in guest RAM): qdevctl
#       shares the VM's memory as it does for its GPUs, unless PVE or the VM's args already do
#   "env": {"NAME": "value"} - environment of the device's process; {vmid} and {id} are replaced
# Defaults for plugins whose sidecar does not carry the keys yet (the sidecar's own keys win).
# uniq-vgpu has none: its VMs keep the environment they were set up with.
PLUGIN_DEFAULTS = {
    "v-airport": {"fields": {"display": False, "kms": False}, "shared_memory": True},
    "v-aux": {"fields": {"display": False, "kms": False}, "shared_memory": True},
    # the mapper bridge uniq-vgpu opens without PVG_MAPPER_BRIDGE_SHM: the video device of the same VM meets
    # the GPU there (a VM-scoped name needs the same "env" in both sidecars, e.g. /uniqvgpu-{vmid}-mapper)
    "apple-videotoolbox": {"fields": {"display": False, "kms": False}, "shared_memory": True,
                           "env": {"PVG_MAPPER_BRIDGE_SHM": "/pvg-mapper-bridge"}},
}
PLUGIN_FIELDS = ("kms", "display", "persistent")
ENV_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

def plugin_meta(plugin: str) -> dict:
    """A .so plugin's sidecar NAME.json over PLUGIN_DEFAULTS ({} for built-in plugins)."""
    if plugin in BUILTIN_PLUGINS:
        return {}
    meta = {}
    side = PLUGIN_DIR / f"{plugin}.json"
    if side.exists():
        try:
            meta = json.loads(side.read_text())
        except ValueError:
            meta = {}
    if not isinstance(meta, dict):
        meta = {}
    return {**PLUGIN_DEFAULTS.get(plugin, {}), **meta}

def plugin_fields(plugin: str) -> dict:
    """The dialog fields a plugin's devices use."""
    if plugin in BUILTIN_PLUGINS:
        return dict(BUILTIN_PLUGINS[plugin]["fields"])
    own = plugin_meta(plugin).get("fields") or {}
    return {k: bool(own.get(k, True)) for k in PLUGIN_FIELDS}

def clamp_to_fields(dev: dict) -> None:
    """Drop the settings of fields the plugin does not use (the dialog does not send hidden fields)."""
    f = plugin_fields(dev["plugin"])
    if not f["display"]:
        dev["display"] = False
    if not f["kms"]:
        dev["kms"] = None
    if not f["persistent"]:
        dev["persistent"] = False

def needs_shared_memory(dev: dict) -> bool:
    return dev_kind(dev) == "vhost-user-gpu" or (dev_kind(dev) == "vfio-user"
                                                 and bool(plugin_meta(dev["plugin"]).get("shared_memory")))

def plugin_env(vmid: int, dev: dict) -> dict[str, str]:
    env = plugin_meta(dev["plugin"]).get("env") or {}
    out = {}
    for k, v in (env.items() if isinstance(env, dict) else []):
        v = str(v).replace("{vmid}", str(vmid)).replace("{id}", dev["id"])
        if ENV_NAME_RE.match(k) and k != "QDEV_ARGS" and "\n" not in v:
            out[k] = v
    return out

def plugin_available(plugin: str) -> bool:
    if plugin_kind(plugin) == "tb":
        return True
    if plugin in BUILTIN_PLUGINS:
        return VUG_BINARY.exists()
    return (PLUGIN_DIR / f"{plugin}.so").exists()

def parse_args(args: str) -> dict[str, str]:
    out = {}
    for kv in (args or "").split(","):
        if kv:
            k, _, v = kv.partition("=")
            out[k] = v
    return out

class QdevError(Exception):
    pass

def log(msg: str) -> None:
    print(f"qdevctl: {msg}", file=sys.stderr)

def run(cmd: list[str], check: bool = True, quiet: bool = False) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        # drop PVE's storage-plugin deprecation chatter, keep the real reason
        err = "\n".join(l for l in (r.stderr.strip() or r.stdout.strip()).splitlines()
                         if "older storage API" not in l)
        raise QdevError(f"{' '.join(shlex.quote(c) for c in cmd)} failed: {err}")
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

def unreadable_settings(vmid: int, stderr: str) -> list[str]:
    """Keys `qm config` could not parse in this VM's own config.  Warnings about
    other files (a storage plugin's storage.cfg properties, say) do not count."""
    pattern = re.compile(rf"^vm {vmid} - unable to parse value of '([^']+)'", re.M)
    return sorted(set(pattern.findall(stderr)))

def check_editable(vmid: int) -> None:
    """Refuse to save a config this node's qemu-server cannot fully read.

    `qm set` writes back only the settings it parsed, so a key from another
    qemu-server build (dbus-display of the QSM native preview, say) would be
    dropped silently by any change qdev-host makes."""
    keys = unreadable_settings(vmid, run(["qm", "config", str(vmid)]).stderr)
    if keys:
        raise QdevError(f"VM {vmid} has settings this node's qemu-server cannot read ({', '.join(keys)}); "
                        "saving its configuration would drop them, so qdev-host leaves the VM unchanged")

def read_vm_config(vmid: int) -> dict[str, str]:
    """The VM's config as `qm config` prints it (pending changes applied), read
    from pmxcfs without starting qm, which costs about 0.6 s of perl per call.
    For the panel's frequent reads; changes still go through qm."""
    try:
        text = Path(f"/etc/pve/qemu-server/{vmid}.conf").read_text()
    except (OSError, UnicodeDecodeError):
        return vm_config(vmid)
    current: dict[str, str] = {}
    pending: dict[str, str] = {}
    section: dict[str, str] | None = current
    for line in text.splitlines():
        if line.startswith("["):
            section = pending if line.strip() == "[PENDING]" else None  # snapshots are not the VM's config
            continue
        if section is None or line.startswith("#") or ":" not in line:
            continue
        k, v = line.split(":", 1)
        section[k.strip()] = v.strip()
    deleted = pending.pop("delete", "")
    current.update(pending)
    for key in deleted.split(","):
        current.pop(key.strip().lstrip("!"), None)
    return current

def vm_running(vmid: int) -> bool:
    """`qm status` = running, checked as qemu-server's vm_running_locally does:
    the pid file names a live, non-zombie process started with that pid file."""
    pidfile = f"/var/run/qemu-server/{vmid}.pid"
    try:
        pid = int(Path(pidfile).read_text().strip())
        args = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        stat = Path(f"/proc/{pid}/stat").read_text()
    except (OSError, ValueError):
        return False
    try:
        i = args.index(b"-pidfile")
    except ValueError:
        return False
    if i + 1 >= len(args) or args[i + 1].decode(errors="replace") != pidfile:
        return False
    state = stat.rsplit(")", 1)[-1].split()
    return bool(state) and state[0] != "Z"

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
    try:
        text = Path("/etc/pve/storage.cfg").read_text()
    except FileNotFoundError:
        # PVE's built-in default when the file was never written: no snippets content
        text = "dir: local\n\tpath /var/lib/vz\n\tcontent iso,vztmpl,backup\n"
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
                # PciBusInfo.number is the bridge's primary bus; children live on `secondary`
                walk(br["devices"], br["bus"].get("secondary", br["bus"]["number"]), d.get("qdev_id", ""))
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
def unit_name(vmid: int, dev_id: str, kind: str = "vfio-user") -> str:
    template = "qdev-host-vugpu" if kind == "vhost-user-gpu" else "qdev-host-device"
    return f"{template}@{vmid}-{dev_id}.service"

def dev_unit(vmid: int, dev: dict) -> str:
    return unit_name(vmid, dev["id"], dev_kind(dev))

def unit_run_dir(vmid: int, dev_id: str) -> Path:
    return RUN_DIR / f"{vmid}-{dev_id}"

def device_socket(vmid: int, dev_id: str) -> Path:
    return unit_run_dir(vmid, dev_id) / "vfio.sock"

def device_display(vmid: int, dev_id: str) -> Path:
    return unit_run_dir(vmid, dev_id) / "display.sock"

def device_control(vmid: int, dev_id: str) -> Path:
    return unit_run_dir(vmid, dev_id) / "control.sock"

# ---------------------------------------------- qdev-host control socket client
def control_cmd(control_path, line: str, timeout: float = 5.0) -> str:
    """Send one command line to a qdev-host --control socket and return its one
    reply line (without the trailing newline). Raises QdevError on failure."""
    control_path = str(control_path)
    if not os.path.exists(control_path):
        raise QdevError(f"control socket not found: {control_path} "
                        "(the device process must be started with --control)")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(control_path)
        s.sendall((line.rstrip("\n") + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace").split("\n", 1)[0].strip()
    except OSError as e:
        raise QdevError(f"control socket {control_path}: {e}")
    finally:
        s.close()

def device_abi(control_path) -> str:
    """Report the device's FROZEN envelope/transport ABI version (warm-vs-reboot
    gate), plugin name and version. Reads it from the live device."""
    reply = control_cmd(control_path, "abi")
    if not reply.startswith("ok"):
        raise QdevError(f"device-abi: {reply}")
    return reply[len("ok"):].strip()

def device_swap(control_path, backing: str,
                reset=None, vmid: int = 0, dev_id: str = "") -> list[str]:
    """Warm-swap the device's VARIABLE backing in process (quiesce -> dlopen-swap
    -> resume) WITHOUT dropping the vfio-user process. `backing` is a backing
    tag (A/B/builtin:X) or a path to a backing .so. `reset` triggers the guest
    re-handshake that makes the swap visible to a live guest:
      None       - stage only (caller resets, or a later reset picks it up);
      "hotplug"  - QMP device_del + device_add of the vfio-user-pci (process
                   stays alive, backing persists); the guest re-enumerates and
                   re-handshakes. Needs vmid/dev_id.
    Returns progress lines."""
    out = []
    reply = control_cmd(control_path, f"swap {backing}")
    if not reply.startswith("ok"):
        raise QdevError(f"device-swap: {reply}")
    out.append(f"backing swapped in process: {reply[len('ok'):].strip()}")
    if reset == "hotplug":
        if not vmid or not dev_id:
            raise QdevError("device-swap --reset hotplug needs VMID and ID")
        dev = find_device(load_conf(vmid), dev_id)
        if not dev:
            raise QdevError(f"device {dev_id} not found in VM {vmid} config")
        if not device_del(vmid, dev_id):
            raise QdevError(f"device_del {dev_id} timed out")
        out.append(f"hot-unplugged {dev_id} (qdev-host process kept alive)")
        device_add(vmid, dev, device_socket(vmid, dev_id))
        out.append(f"hot-replugged {dev_id}: guest re-enumerates + re-handshakes")
    return out

def _parse_backend_report(reply: str) -> dict:
    """Parse the qdev-host 'backend' reply ("ok key=value ... banner='...'")."""
    out = {}
    s = reply[len("ok"):].strip() if reply.startswith("ok") else reply
    # banner is quoted and may contain spaces/=, so pull it out first
    if "banner='" in s:
        head, _, tail = s.partition("banner='")
        out["banner"] = tail[:-1] if tail.endswith("'") else tail
        s = head
    for tok in s.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
    return out

def device_backend(control_path) -> dict:
    """Read the device's backend health/identity report (reload health-gate)."""
    reply = control_cmd(control_path, "backend")
    if not reply.startswith("ok"):
        raise QdevError(f"device-backend: {reply}")
    return _parse_backend_report(reply)

def device_reload(control_path, path: str, reset: bool = True,
                  expect_gpurestart: int = 0, rollback: bool = True,
                  allow_guest_gpu_loss: bool = False,
                  allow_unflushed: bool = False) -> list[str]:
    """In-process REAL-backend reload (v4): destroy the live backend Session and
    re-create it on the freshly loaded backend at `path`, re-adopting device
    state, with the vfio-user process kept alive (stock QEMU, device BOUND).

    SAME gates as device-swap plus the reload-specific ones:
      - ABI gate: the FROZEN envelope (abi_tag) must be unchanged across the
        reload (equal envelope => WARM; a change would require a guest reboot);
      - health gate: 0 new QMU [ERROR] lines; gpuRestart delta within
        `expect_gpurestart`; HandshakeGeneration must bump;
      - auto-rollback: on a failed reload or a failed health gate, reload the
        PREVIOUS backend (read from the pre-reload report) to restore the device.
    Returns progress lines; raises QdevError if the (post-rollback) device is
    not healthy."""
    out = []
    envelope_before = device_abi(control_path)
    rep0 = device_backend(control_path)
    prev_path = rep0.get("path")
    reloads0 = int(rep0.get("reloads", 0))
    gen0 = int(rep0.get("handshake_gen", 0))
    errs0 = int(rep0.get("qmu_errors", 0))
    gpur0 = int(rep0.get("gpu_restarts", 0))
    out.append(f"pre-reload: envelope={envelope_before.split()[0] if envelope_before else '?'} "
               f"reloads={reloads0} handshake_gen={gen0} qmu_errors={errs0} prev={prev_path}")

    # qdev-host refuses a reload while a guest is attached (the re-created backend Session
    # does not carry the guest-programmed device state: RELOAD-LIVE-REINIT) unless the
    # operator accepts the guest GPU loss explicitly.
    # A backend state hand-over without the suspend-time flush (state_handover=1,
    # handover_flush=0) additionally needs --allow-unflushed for a transparent reload.
    flags = ((" --reset" if reset else "") + (" --allow-guest-gpu-loss" if allow_guest_gpu_loss else "")
             + (" --allow-unflushed" if allow_unflushed else ""))
    arg = f"reload {path}" + flags
    reply = control_cmd(control_path, arg, timeout=60.0)
    if reply.startswith("err reload refused"):
        # refused before any quiesce: nothing changed, nothing to roll back
        raise QdevError(f"device-reload: {reply}")
    reload_ok = reply.startswith("ok")
    if reload_ok:
        out.append(f"backend reloaded in process: {reply[len('ok'):].strip()}")

    def _rollback(reason: str):
        if not (rollback and prev_path):
            raise QdevError(f"device-reload failed ({reason}); no rollback target")
        out.append(f"HEALTH-GATE FAIL ({reason}); auto-rollback -> {prev_path}")
        rb = control_cmd(control_path, f"reload {prev_path}" + flags,
                         timeout=60.0)
        if not rb.startswith("ok"):
            raise QdevError(f"device-reload: rollback to {prev_path} FAILED: {rb}")
        out.append(f"rolled back to {prev_path}: {rb[len('ok'):].strip()}")

    if not reload_ok:
        _rollback(reply)
        return out

    # health + ABI gates
    envelope_after = device_abi(control_path)
    rep1 = device_backend(control_path)
    errs1 = int(rep1.get("qmu_errors", 0))
    gen1 = int(rep1.get("handshake_gen", 0))
    gpur1 = int(rep1.get("gpu_restarts", 0))
    problems = []
    if envelope_after != envelope_before:
        problems.append(f"envelope changed {envelope_before!r}->{envelope_after!r} (reboot required)")
    if reset and gen1 <= gen0:
        problems.append(f"HandshakeGeneration did not bump ({gen0}->{gen1})")
    if errs1 > errs0:
        problems.append(f"{errs1 - errs0} new QMU [ERROR] line(s)")
    if gpur1 - gpur0 > expect_gpurestart:
        problems.append(f"gpuRestart delta {gpur1 - gpur0} > expected {expect_gpurestart}")
    if problems:
        _rollback("; ".join(problems))
        return out
    out.append(f"HEALTH-GATE PASS: envelope frozen, handshake_gen {gen0}->{gen1}, "
               f"0 new QMU errors, gpuRestart delta {gpur1 - gpur0} (<= {expect_gpurestart})")
    out.append(f"backend now: {rep1.get('banner', '?')}")
    return out

def gpu_socket(vmid: int, dev_id: str) -> Path:
    return unit_run_dir(vmid, dev_id) / "gpu.sock"

def build_args(vmid: int, dev: dict) -> list[str]:
    if dev_kind(dev) == "vhost-user-gpu":
        opts = vug_options(dev)
        args = [f"--socket-path={gpu_socket(vmid, dev['id'])}", f"--render-node={opts['render']}"]
        return args + (["--virgl"] if opts["virgl"] else [])
    plugin = PLUGIN_DIR / f"{dev['plugin']}.so"
    if not plugin.exists():
        raise QdevError(f"plugin not installed: {plugin}")
    args = ["--plugin", str(plugin), "--socket", str(device_socket(vmid, dev["id"])),
            "--control", str(device_control(vmid, dev["id"])),
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
    lines = ["QDEV_ARGS=" + " ".join(build_args(vmid, dev))]
    if dev_kind(dev) == "vfio-user":
        for k, v in plugin_env(vmid, dev).items():
            lines.append(f'{k}="' + v.replace("\\", "\\\\").replace('"', '\\"') + '"')
    p.write_text("\n".join(lines) + "\n")
    p.chmod(0o640)

def start_process(vmid: int, dev: dict) -> None:
    write_unit_env(vmid, dev)
    unit = dev_unit(vmid, dev)
    run(["systemctl", "reset-failed", unit], check=False)
    run(["systemctl", "start", unit])
    sock = gpu_socket(vmid, dev["id"]) if dev_kind(dev) == "vhost-user-gpu" else device_socket(vmid, dev["id"])
    for _ in range(50):
        if sock.exists():
            return
        time.sleep(0.1)
    raise QdevError(f"{unit} did not create {sock}; see journalctl -u {unit}")

def stop_process(vmid: int, dev_id: str, kind: str = "vfio-user") -> None:
    run(["systemctl", "stop", unit_name(vmid, dev_id, kind)], check=False)
    run(["systemctl", "reset-failed", unit_name(vmid, dev_id, kind)], check=False)

def process_active(vmid: int, dev_id: str, kind: str = "vfio-user") -> bool:
    return run(["systemctl", "is-active", unit_name(vmid, dev_id, kind)], check=False).stdout.strip() == "active"

def stop_stale_processes(vmid: int, conf: dict) -> list[str]:
    """pre-start: every VM start gets new device processes. A device process that is still active while its VM is
    not running is left from the previous run (a stop that did not finish, a restart that skipped post-stop), and
    `systemctl start` on an active unit is a no-op, so stop it first (SIGTERM, SIGKILL after the unit's TimeoutStopSec).
    Persistent devices keep their process by definition."""
    notes = []
    for d in conf["devices"]:
        kind = dev_kind(d)
        if kind == "tb" or d.get("persistent"):
            continue
        if process_active(vmid, d["id"], kind):
            unit = unit_name(vmid, d["id"], kind)
            t0 = time.monotonic()
            stop_process(vmid, d["id"], kind)
            notes.append(f"{d['id']}: stopped the still-active device process {unit} of the previous run "
                         f"({time.monotonic() - t0:.1f} s) before the start")
    return notes

# ----------------------------------------------------------- reboot policy
# `cold`: an in-guest reboot (or QMP system_reset) becomes a VM-level cold restart. QEMU runs with -action
# reboot=shutdown (stock, QEMU >= 6.0), so a reset request is a SHUTDOWN with reason guest-reset /
# host-qmp-system-reset; PVE's -no-shutdown pauses the VM and qmeventd quits QEMU; the relay unit
# (qdev_reboot_relay.py) reads the reason on its own QMP socket and creates PVE's reboot request, so `qm cleanup`
# starts the VM again: new QEMU, new device processes, new GPU context. `reset` (default): QEMU's in-process reset.
REBOOT_POLICIES = ("cold", "reset")

def relay_socket(vmid: int) -> Path:
    return RUN_DIR / f"reboot-relay-{vmid}.qmp"

def relay_unit(vmid: int) -> str:
    return f"qdev-host-reboot-relay@{vmid}.service"

def reboot_policy_groups(vmid: int) -> str:
    return f"-action reboot=shutdown -qmp unix:{relay_socket(vmid)},server=on,wait=off"

def reboot_policy_patterns(vmid: int) -> list[str]:
    return [r"-action\s+reboot=shutdown", rf"-qmp\s+unix:{re.escape(str(relay_socket(vmid)))},\S*"]

def apply_reboot_policy_args(args: str, vmid: int, policy: str) -> str:
    """The VM's args with the cold-policy groups present exactly once (cold) or absent (reset); everything else as it
    was. Idempotent."""
    if policy not in REBOOT_POLICIES:
        raise QdevError(f"reboot policy must be one of {', '.join(REBOOT_POLICIES)}")
    for pattern in reboot_policy_patterns(vmid):
        args = _drop(args or "", pattern)
    if policy == "cold":
        args = (args + " " + reboot_policy_groups(vmid)).strip()
    return args

def args_reboot_policy(args: str, vmid: int) -> str:
    return "cold" if all(_find(args or "", p) for p in reboot_policy_patterns(vmid)) else "reset"

def live_reboot_policy(vmid: int) -> str | None:
    """The policy the RUNNING QEMU was started with (None when the VM does not run)."""
    try:
        pid = int(Path(f"/var/run/qemu-server/{vmid}.pid").read_text().strip())
        cmd = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    except (OSError, ValueError):
        return None
    if not vm_running(vmid):
        return None
    sock = str(relay_socket(vmid)).encode()
    cold = any(a.startswith(b"reboot=shutdown") for a in cmd) and any(a.startswith(b"unix:" + sock + b",") for a in cmd)
    return "cold" if cold else "reset"

def ensure_relay(vmid: int) -> None:
    """The relay's socket directory exists and its unit runs (pre-start of a cold-policy VM; idempotent)."""
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    run(["systemctl", "start", relay_unit(vmid)], check=False)

def set_reboot_policy(vmid: int, policy: str) -> list[str]:
    """Write the policy into the VM's args (a pending change while it runs: effective at its next start), record it
    in /etc/qdev-host/vms/VMID.json and enable (cold) or disable (reset) the relay unit."""
    if policy not in REBOOT_POLICIES:
        raise QdevError(f"reboot policy must be one of {', '.join(REBOOT_POLICIES)}")
    node = vm_node(vmid)
    if node and node != local_node():
        raise QdevError(f"VM {vmid} is on node {node}; run qdevctl there")
    check_editable(vmid)
    cfg = vm_config(vmid)
    conf = load_conf(vmid)
    notes = []
    old = cfg.get("args", "")
    new = apply_reboot_policy_args(old, vmid, policy)
    if new != old:
        qm_set(vmid, "args", new or None)
        notes.append(f"VM {vmid} args: " + ("-action reboot=shutdown + relay QMP socket "
                                            f"{relay_socket(vmid)} added" if policy == "cold" else
                                            "-action reboot=shutdown + relay QMP socket removed"))
    else:
        notes.append(f"VM {vmid} args already carry reboot policy {policy}")
    if policy == "cold":
        if not cfg.get("hookscript"):
            # qdev-host's pre-start keeps the relay socket directory and the relay unit in place before QEMU starts
            ensure_hookscript(vmid, cfg, conf)
        elif not cfg["hookscript"].endswith("/" + HOOK_NAME):
            # a VM's own hookscript stays as it is (taking it over would also start qdev-host's TB/attach hooks);
            # the relay unit, enabled at boot before pve-guests, keeps the socket directory, and its fallback stops
            # device units a cleanup left active
            notes.append(f"VM {vmid} hookscript {cfg['hookscript']} kept (not qdev-host's)")
    conf["reboot_policy"] = policy
    save_conf(conf)
    unit = relay_unit(vmid)
    live = live_reboot_policy(vmid)
    if policy == "cold":
        RUN_DIR.mkdir(parents=True, exist_ok=True)
        run(["systemctl", "enable", "--now", unit])
        notes.append(f"{unit} enabled and running")
    else:
        run(["systemctl", "disable", unit], check=False)
        if live == "cold":
            notes.append(f"{unit} disabled; it keeps serving the running VM (started with the cold policy) and exits "
                         "after that QEMU is gone")
        else:
            run(["systemctl", "stop", unit], check=False)
            notes.append(f"{unit} disabled and stopped")
    if live is None:
        notes.append(f"reboot policy {policy}: effective at the next VM start (VM not running)")
    elif live == policy:
        notes.append(f"reboot policy {policy}: in effect (the running VM was started with it)")
    else:
        notes.append(f"reboot policy {policy}: armed, effective at the next VM start "
                     f"(the running VM keeps {live} until it stops)")
    return notes

# --------------------------------------------------------------- flows
def attach(vmid: int, dev: dict) -> str:
    """Start the device process (if needed) and hot-plug it. Returns a status line."""
    if dev_kind(dev) == "tb":
        import qdev_tb
        if not vm_running(vmid):
            return f"{dev['id']}: present from the next VM start"
        return qdev_tb.plug(vmid, load_conf(vmid), dev)
    if dev_kind(dev) == "vhost-user-gpu":
        # on the QEMU command line from the VM start: QEMU's D-Bus display exports only
        # the consoles that exist when it starts, so this GPU is never hot-plugged
        if vm_running(vmid) and device_present(vmid, dev["id"]):
            return f"{dev['id']}: attached"
        return f"{dev['id']}: present from the next VM start"
    if not process_active(vmid, dev["id"]):
        start_process(vmid, dev)
    if device_present(vmid, dev["id"]):
        return f"{dev['id']}: already attached"
    if not bus_present(vmid, dev["bus"]):
        return (f"{dev['id']}: bus {dev['bus']} is not in the running VM "
                f"(the root port is added at the next VM start); process running, not attached")
    if dev["bus"].startswith("qdev-tb"):
        import qdev_tb
        qdev_tb.power_slot(vmid, load_conf(vmid), vm_config(vmid), dev["bus"])
    device_add(vmid, dev, device_socket(vmid, dev["id"]))
    return f"{dev['id']}: attached to {dev['bus']} (plugin {dev['plugin']})"

def detach(vmid: int, dev: dict, keep_process: bool) -> str:
    if dev_kind(dev) == "tb":
        import qdev_tb
        return qdev_tb.unplug(vmid, dev) if vm_running(vmid) else f"{dev['id']}: VM not running"
    if dev_kind(dev) == "vhost-user-gpu":
        if vm_running(vmid):
            return f"{dev['id']}: stays in the running VM until it stops"
        stop_process(vmid, dev["id"], "vhost-user-gpu")
        return f"{dev['id']}: process stopped"
    gone = True
    if vm_running(vmid):
        gone = device_del(vmid, dev["id"])
    if not gone:
        # the guest still has the device: its process stays until the VM stops (post-stop)
        return (f"{dev['id']}: device_del sent, the guest has not released it yet; its process runs until "
                "the device is gone or the VM stops")
    if not keep_process:
        stop_process(vmid, dev["id"])
    return f"{dev['id']}: detached" + ("" if keep_process else ", process stopped")

# ------------------------------------------------------------ validation
ID_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,31}$")
# plugin args are a comma separated k=v list; keep them free of shell/quote characters
ARGS_RE = re.compile(r"^[A-Za-z0-9_=,./:+@-]*$")
KMS_RE = re.compile(r"^/dev/dri/card[0-9]+:[A-Za-z0-9-]+$")
BUS_RE = re.compile(r"^[A-Za-z0-9_.-]{1,32}$")
ADDR_RE = re.compile(r"^0x[0-9a-fA-F]{1,2}$")

def validate_device(dev_id: str, plugin: str, args: str, bus: str, kms: str | None) -> None:
    if not ID_RE.match(dev_id or ""):
        raise QdevError("device id must be 1-32 characters: letters, digits, '-' or '_' (starting with a letter)")
    if not plugin_available(plugin):
        where = VUG_BINARY if plugin in BUILTIN_PLUGINS else PLUGIN_DIR / (plugin + ".so")
        raise QdevError(f"plugin not installed: {plugin} (looked for {where})")
    # PVE's netN syntax of a TB network device starts with a bare model ("virtio,bridge=vmbr0")
    bare_ok = plugin_kind(plugin) == "tb"
    if args and (not ARGS_RE.match(args) or
                 any(not kv.split("=", 1)[0] or ("=" not in kv and not bare_ok) for kv in args.split(","))):
        raise QdevError("plugin args must be a comma separated list of key=value pairs")
    if not BUS_RE.match(bus or ""):
        raise QdevError("invalid bus id")
    if kms:
        if not KMS_RE.match(kms):
            raise QdevError("KMS output must look like /dev/dri/cardN:CONNECTOR")
        card, conn = kms.split(":", 1)
        if not Path(card).exists() or not Path(f"/sys/class/drm/{Path(card).name}-{conn}").exists():
            raise QdevError(f"KMS output {kms} does not exist on this node")
    if plugin_kind(plugin) == "vhost-user-gpu":
        if kms:
            raise QdevError(f"{plugin} has no KMS output: QEMU's display shows it")
        vug_options({"plugin": plugin, "args": args})

def is_q35(cfg: dict[str, str]) -> bool:
    first = cfg.get("machine", "").split(",")[0]
    return first == "q35" or first.startswith("pc-q35")

# -------------------------------------------------------------- cluster
def local_node() -> str:
    return socket.gethostname().split(".")[0]

def vm_node(vmid: int) -> str | None:
    """Node that owns VMID according to pmxcfs (None if unknown)."""
    try:
        ids = json.loads(Path("/etc/pve/.vmlist").read_text()).get("ids", {})
    except (OSError, ValueError):
        return None
    ent = ids.get(str(vmid))
    return ent.get("node") if ent else None

# ------------------------------------------------- vhost-user-gpu devices
RENDER_RE = re.compile(r"^/dev/dri/renderD[0-9]+$")

def list_render_nodes() -> list[dict]:
    out = []
    for node in sorted(Path("/dev/dri").glob("renderD*")):
        sysdev = Path("/sys/class/drm") / node.name / "device"
        try:
            driver = Path(os.readlink(sysdev / "driver")).name
        except OSError:
            driver = "?"
        pci = sysdev.resolve().name if sysdev.exists() else ""
        out.append({"path": str(node), "driver": driver, "pci": pci})
    return out

def vug_options(dev: dict) -> dict:
    """Validated settings of a vhost-user-gpu device from its plugin args."""
    a = parse_args(dev.get("args", ""))
    unknown = set(a) - {"render", "display", "vga", "outputs", "xres", "yres", "virgl"}
    if unknown:
        raise QdevError(f"{dev['plugin']}: unknown setting(s) {', '.join(sorted(unknown))}")
    def number(key: str, default: int, low: int, high: int) -> int:
        try:
            v = int(a.get(key) or default)
        except ValueError:
            raise QdevError(f"{key} must be a number") from None
        if not low <= v <= high:
            raise QdevError(f"{key} must be {low}..{high}")
        return v
    def flag(key: str, default: bool) -> bool:
        v = a.get(key, "")
        if v == "":
            return default
        if v not in ("0", "1"):
            raise QdevError(f"{key} must be 0 or 1")
        return v == "1"
    render = a.get("render") or ""
    if not render:
        nodes = list_render_nodes()
        if not nodes:
            raise QdevError("this node has no DRM render node (/dev/dri/renderD*)")
        render = nodes[0]["path"]
    if not RENDER_RE.match(render) or not Path(render).exists():
        raise QdevError(f"render node {render} does not exist on this node")
    display = a.get("display", "")
    if display and "vga" in a:
        raise QdevError("give display=primary|vga|secondary or the older vga=1|0, not both")
    if not display:
        legacy = a.get("vga", "")
        if legacy not in ("", "0", "1"):
            raise QdevError("vga must be 0 or 1")
        display = {"": "primary", "1": "vga", "0": "secondary"}[legacy]
    if display not in ("primary", "vga", "secondary"):
        raise QdevError("display must be primary, vga or secondary")
    return {"render": render, "display": display, "primary": display != "secondary",
            "vga": display == "vga", "virgl": flag("virgl", True),
            "outputs": number("outputs", 1, 1, 4), "xres": number("xres", 1280, 640, 7680),
            "yres": number("yres", 800, 480, 4320)}

def _drop(args: str, pattern: str) -> str:
    """args without the whitespace-free option groups matching PATTERN (an -option plus its value)."""
    return re.sub(r"(?:^|\s+)" + pattern + r"(?=\s|$)", "", args).strip()

def _find(args: str, pattern: str) -> str | None:
    m = re.search(r"(?:^|\s)(" + pattern + r")(?=\s|$)", args)
    return m.group(1) if m else None

def vug_groups(vmid: int, dev: dict) -> str:
    o = vug_options(dev)
    chardev = f"qdev-{dev['id']}"
    driver = "vhost-user-vga" if o["display"] == "vga" else "vhost-user-gpu-pci"
    return (f"-chardev socket,id={chardev},path={gpu_socket(vmid, dev['id'])} "
            f"-device {driver},id={dev['id']},chardev={chardev},bus={dev['bus']},"
            f"max_outputs={o['outputs']},xres={o['xres']},yres={o['yres']}")

def vug_patterns(dev_id: str) -> list[str]:
    i = re.escape(dev_id)
    return [rf"-chardev\s+socket,id=qdev-{i},\S*", rf"-device\s+vhost-user-(?:vga|gpu-pci),id={i},\S*"]

SHARED_MEM_PATTERNS = [rf"-object\s+memory-backend-memfd,id={SHARED_MEM_ID},\S*",
                       rf"-machine\s+memory-backend={SHARED_MEM_ID}"]
QSM_GPU_PATTERN = rf"-device\s+virtio-vga-gl,id={QSM_GPU_ID}\S*"

def vm_memory(cfg: dict[str, str]) -> int:
    value = cfg.get("memory", "")
    m = re.match(r"^(?:current=)?([0-9]+)", value)
    if not m:
        raise QdevError("set the VM's memory explicitly: the shared memory backend needs its size")
    return int(m.group(1))

def args_share_memory(args: str) -> bool:
    """The VM's args already back its RAM with a shared memory backend of their own (not qdev-host's)."""
    for m in re.finditer(r"-machine\s+(?:\S*,)?memory-backend=([^,\s]+)", args or ""):
        bid = m.group(1)
        if bid == SHARED_MEM_ID:
            continue
        for o in re.finditer(r"-object\s+memory-backend-(?:memfd|file),(\S+)", args or ""):
            opts = dict(x.split("=", 1) for x in o.group(1).split(",") if "=" in x)
            if opts.get("id") == bid and opts.get("share") in ("on", "true", "yes"):
                return True
    return False

def memory_is_shared(cfg: dict[str, str]) -> bool:
    return (bool(cfg.get("hugepages")) or any(re.match(r"^virtiofs\d+$", k) for k in cfg)
            or args_share_memory(cfg.get("args", "")))

def running_memory_shared(vmid: int) -> bool:
    """The running QEMU's RAM is in a shared memory backend (hugepages, virtiofs, a memfd with share=on)."""
    try:
        pid = int(Path(f"/var/run/qemu-server/{vmid}.pid").read_text().strip())
        cmd = Path(f"/proc/{pid}/cmdline").read_bytes().decode(errors="replace").split("\0")
    except (OSError, ValueError):
        return False
    return any(re.match(r"memory-backend-(?:memfd|file),", a) and re.search(r"(?:^|,)share=(?:on|true|yes)(?:,|$)", a)
               for a in cmd)

def shared_memory_groups(cfg: dict[str, str]) -> str:
    """The args that give the VM shared memory, or '' when PVE already does."""
    if memory_is_shared(cfg):
        return ""
    if cfg.get("numa") == "1":
        raise QdevError("the VM uses NUMA nodes; give it hugepages or a virtiofs device, which share its memory")
    if "memory" in cfg.get("hotplug", "network,disk,usb").split(","):
        raise QdevError("the VM hot-plugs memory; give it hugepages or a virtiofs device, which share its memory")
    return (f"-object memory-backend-memfd,id={SHARED_MEM_ID},size={vm_memory(cfg)}M,share=on "
            f"-machine memory-backend={SHARED_MEM_ID}")

def shared_memory_mismatch(cfg: dict[str, str]) -> str | None:
    """A reason the VM would not start with its shared memory args, or None."""
    obj = _find(cfg.get("args", ""), SHARED_MEM_PATTERNS[0])
    if not obj:
        return None
    if memory_is_shared(cfg):
        return "the VM now has hugepages or a virtiofs device, and also qdev-host's memory backend"
    m = re.search(r"size=([0-9]+)M", obj)
    try:
        want = vm_memory(cfg)
    except QdevError as e:
        return str(e)
    if not m or int(m.group(1)) != want:
        return f"the VM's memory is {want} MiB, qdev-host's shared memory backend {m.group(1) if m else '?'} MiB"
    return None

def apply_vug_args(vmid: int, conf: dict, cfg: dict[str, str], gpu_notes: bool = True) -> list[str]:
    """Rewrite the qdev-host parts of the VM's args (and vga) for its vhost-user-gpu devices.

    Removes every group qdev-host owns, then puts back the current ones after the
    root port, so a re-run gives the same result.  Returns notes for the user; with
    gpu_notes False (another kind of device changed) none about the VM's GPUs."""
    notes = []
    args = cfg.get("args", "")
    had_mem = bool(_find(args, SHARED_MEM_PATTERNS[0]))
    for dev in conf["devices"]:
        for pat in vug_patterns(dev["id"]):
            args = _drop(args, pat)
    for pat in SHARED_MEM_PATTERNS:
        args = _drop(args, pat)
    vugs = [d for d in conf["devices"] if dev_kind(d) == "vhost-user-gpu"]
    primary = [d for d in vugs if vug_options(d)["primary"]]
    if len(primary) > 1:
        raise QdevError("only one qdev-host GPU can be the primary display")
    # QSM's in-process VirGL GPU gives way to a primary qdev-host GPU and comes back without one
    qsm_gpu = _find(args, QSM_GPU_PATTERN)
    if primary and qsm_gpu:
        conf["replaced_qsm_gpu"] = qsm_gpu
        # remember the option group in front of it, to put it back in the same place
        before = args[:args.find(qsm_gpu)].rstrip()
        cut = before.rfind(" -")
        conf["replaced_qsm_gpu_after"] = before[cut + 1:] if cut >= 0 else before
        args = _drop(args, QSM_GPU_PATTERN)
        notes.append("QSM Direct's in-process VirGL GPU gives way to this GPU while it exists "
                     "(qsm-pve-direct 0.4.0+stock4.14~exp233 or later shows it and keeps it so)")
    elif not primary and conf.get("replaced_qsm_gpu"):
        gpu, after = conf.pop("replaced_qsm_gpu"), conf.pop("replaced_qsm_gpu_after", "")
        if after and after in args:
            args = args.replace(after, f"{after} {gpu}", 1)
        else:
            args = f"{gpu} {args}".strip()
    if vugs or any(needs_shared_memory(d) for d in conf["devices"]):
        mem = shared_memory_groups({**cfg, "args": args})
        groups = " ".join(vug_groups(vmid, d) for d in vugs)
        args = " ".join(x for x in (args, mem, groups) if x)
        if mem and not had_mem:
            notes.append(f"the VM's {vm_memory(cfg)} MiB of memory are shared with qdev-host's device processes "
                         "(memory-backend-memfd in args; save the device again after changing the memory)")
    qm_set(vmid, "args", args or None)
    # the VM's own display device would come first, so QSM would not show the GPU
    vga_now = cfg.get("vga")
    if primary and cfg.get("dbus-display") in ("1", "on", "yes", "true"):
        raise QdevError("the VM uses the native shared display of the older QSM preview (dbus-display), which "
                        "needs a graphical Display: switch it to QSM Display1 VirGL in Hardware > Display and its "
                        "sound to QSM Console first, or add this GPU with display=secondary")
    if primary and "saved_vga" not in conf:
        if vga_now != "none":
            conf["saved_vga"] = vga_now
            qm_set(vmid, "vga", "none")
            notes.append(f"Display set to none (was {vga_now or 'default'}); it comes back when the GPU is removed")
    elif not primary and "saved_vga" in conf:
        qm_set(vmid, "vga", conf.pop("saved_vga"))
    if gpu_notes and vugs and not _find(args, r"-display\s+\S*gl=(?:on|core|es)\S*"):
        notes.append("the VM has no OpenGL display (QSM Display1 VirGL): the GPU renders, but its picture is not shown")
    return notes

# ---------------------------------------------------------- inventories
def list_plugins() -> list[dict]:
    """Installed plugins with their optional sidecar metadata (NAME.json next to NAME.so)."""
    out = []
    for so in sorted(PLUGIN_DIR.glob("*.so")):
        meta = {}
        side = so.with_suffix(".json")
        if side.exists():
            try:
                meta = json.loads(side.read_text())
            except ValueError:
                meta = {}
        meta = {**PLUGIN_DEFAULTS.get(so.stem, {}), **(meta if isinstance(meta, dict) else {})}
        out.append({"name": so.stem, "kind": "vfio-user", "title": meta.get("title", so.stem),
                    "description": meta.get("description", ""), "args": meta.get("args", []),
                    "fields": plugin_fields(so.stem), "shared_memory": bool(meta.get("shared_memory"))})
    for name, meta in BUILTIN_PLUGINS.items():  # after the .so plugins: tb-vga stays the dialog's default
        if plugin_available(name):
            entry = {"name": name, "kind": meta["kind"], "title": meta["title"],
                     "description": meta["description"], "args": meta["args"], "fields": meta["fields"]}
            if meta.get("dialog"):
                entry["dialog"] = meta["dialog"]
            out.append(entry)
    return out

def list_kms_outputs() -> list[dict]:
    """DRM connectors of this node as /dev/dri/cardN:CONNECTOR specs with their status."""
    out = []
    for status in sorted(Path("/sys/class/drm").glob("card*-*/status")):
        name = status.parent.name  # card1-HDMI-A-1
        card, conn = name.split("-", 1)
        if conn.startswith("Writeback"):
            continue
        try:
            st = status.read_text().strip()
        except OSError:
            continue
        out.append({"spec": f"/dev/dri/{card}:{conn}", "status": st})
    return out

# ------------------------------------------------------------- operations
def add_device(vmid: int, dev_id: str, plugin: str, args: str = "", bus: str = "", display: bool = True,
               kms: str | None = None, persistent: bool = False, port_addr: str | None = None,
               extra: list[str] | None = None) -> list[str]:
    """Define a device for VMID (and hot-plug it if the VM runs). Returns status lines.

    BUS "" or "tb" puts the device on a free port of the VM's TB bus (from 0.6.0);
    qdev-rp0 (the root port of earlier versions) and PVE's ports stay possible."""
    import qdev_tb
    msgs = []
    bus = bus or "tb"
    validate_device(dev_id, plugin, args or "", bus, kms)
    if port_addr and not ADDR_RE.match(port_addr):
        raise QdevError("port address must be like 0x08")
    conf = load_conf(vmid)
    if find_device(conf, dev_id):
        raise QdevError(f"device {dev_id} already defined for VM {vmid}")
    cfg = vm_config(vmid)
    if not is_q35(cfg):
        raise QdevError(f"VM {vmid} is not a q35 machine (machine: {cfg.get('machine', 'i440fx')}); PCIe hot-plug needs q35")
    dev = {"id": dev_id, "plugin": plugin, "args": args or "", "bus": bus, "display": bool(display),
           "kms": kms or None, "persistent": bool(persistent), "extra": extra or []}
    if plugin_kind(plugin) == "tb":
        if kms:
            raise QdevError(f"{plugin} has no KMS output")
        dev.update(display=False, kms=None, persistent=False, extra=[])
        return qdev_tb.add(vmid, conf, cfg, dev)
    had_tb = bool(conf.get("tb"))
    if bus == "tb":
        conf.setdefault("tb", {"ports": qdev_tb.DEFAULT_PORTS})
        bus = dev["bus"] = qdev_tb.free_port(conf)
    taken = next((d["id"] for d in conf["devices"] if d["bus"] == bus), None)
    if taken and bus != "pcie.0":
        raise QdevError(f"bus {bus} already holds device {taken}: a PCIe root port has one slot "
                        "(remove that device first, or give this one another bus)")
    conf["port_addr"] = port_addr or conf.get("port_addr", "0x08")
    # every check that can fail comes before the first change to the VM
    check_editable(vmid)
    snippets_dir()
    if plugin_kind(plugin) == "vhost-user-gpu":
        return add_vug_device(vmid, conf, cfg, dev, had_tb)
    clamp_to_fields(dev)
    shared = needs_shared_memory(dev)
    if shared:
        shared_memory_groups(cfg)          # refuses NUMA/memory hot-plug without hugepages/virtiofs
    changed_port = False
    try:
        if bus == PORT_ID:
            changed_port = ensure_port_in_args(vmid, cfg, conf["port_addr"])
        elif qdev_tb.is_port(bus):
            changed_port = qdev_tb.ensure_bus(vmid, conf, cfg)
        ensure_hookscript(vmid, cfg, conf)
    except Exception:
        if bus == PORT_ID and changed_port:
            remove_port_from_args(vmid, vm_config(vmid))
        elif not had_tb:
            qdev_tb.purge(vmid, conf, vm_config(vmid))
        raise
    conf["devices"].append(dev)
    if shared:
        msgs += [f"note: {n}" for n in apply_vug_args(vmid, conf, vm_config(vmid), gpu_notes=False)]
    save_conf(conf)
    write_unit_env(vmid, dev)
    msgs.append(f"VM {vmid}: device {dev_id} ({plugin}) defined on bus {bus}")
    if vm_running(vmid) and shared and not running_memory_shared(vmid):
        msgs.append(f"{dev_id}: not hot-plugged - {plugin} works in the guest's memory, which the running VM does not "
                    "share yet: stop and start the VM (the hookscript starts and attaches the device then)")
    elif vm_running(vmid):
        msgs.append(attach(vmid, dev))
        if changed_port:
            msgs.append(f"note: root port {PORT_ID} was added to args; it appears at the next VM start" if bus == PORT_ID
                        else "note: the TB bus was added to the VM's args; it appears at the next VM start")
    else:
        msgs.append("VM is stopped: the device will be started and attached by the hookscript at VM start")
    return msgs

def add_vug_device(vmid: int, conf: dict, cfg: dict[str, str], dev: dict, had_tb: bool = True) -> list[str]:
    import qdev_tb
    dev.update(display=False, kms=None, persistent=False, extra=[])
    shared_memory_groups(cfg)              # refuses NUMA/memory hot-plug without hugepages/virtiofs
    had_devices = bool(conf["devices"])
    changed_port = False
    try:
        # the TB bus goes in front of args: its ports must exist before the GPU's -device
        if dev["bus"] == PORT_ID:
            changed_port = ensure_port_in_args(vmid, cfg, conf["port_addr"])
        elif qdev_tb.is_port(dev["bus"]):
            qdev_tb.ensure_bus(vmid, conf, cfg)
        ensure_hookscript(vmid, cfg, conf)
        conf["devices"].append(dev)
        notes = apply_vug_args(vmid, conf, vm_config(vmid))
    except Exception:
        conf["devices"] = [d for d in conf["devices"] if d["id"] != dev["id"]]
        try:
            fresh = vm_config(vmid)
            for pat in vug_patterns(dev["id"]):
                fresh["args"] = _drop(fresh.get("args", ""), pat)
            apply_vug_args(vmid, conf, fresh)  # puts back what this call replaced
            if changed_port:
                remove_port_from_args(vmid, vm_config(vmid))
            if not had_tb:
                qdev_tb.purge(vmid, conf, vm_config(vmid))
            if not had_devices:
                restore_hookscript(vmid, conf)
        except Exception as e:  # report the first error, not the cleanup's
            log(f"cleanup after a failed add: {e}")
        raise
    save_conf(conf)
    write_unit_env(vmid, dev)
    msgs = [f"VM {vmid}: GPU {dev['id']} ({dev['plugin']}) defined on bus {dev['bus']}"] + [f"note: {n}" for n in notes]
    msgs.append("the GPU is present from the next VM start" + (": stop and start the VM" if vm_running(vmid) else ""))
    return msgs

def update_device(vmid: int, dev_id: str, args: str | None = None, kms: str | None = None,
                  display: bool | None = None, persistent: bool | None = None) -> list[str]:
    """Change a device's settings: kms '' removes the KMS output, None keeps a value."""
    conf = load_conf(vmid)
    dev = find_device(conf, dev_id)
    if not dev:
        raise QdevError(f"no device {dev_id} on VM {vmid}")
    new = dict(dev)
    if args is not None:
        new["args"] = args
    if kms is not None:
        new["kms"] = kms or None
    if display is not None:
        new["display"] = bool(display)
    if persistent is not None:
        new["persistent"] = bool(persistent)
    if dev_kind(dev) == "vfio-user":
        clamp_to_fields(new)
    validate_device(dev_id, new["plugin"], new.get("args", ""), new["bus"], new.get("kms"))
    if dev_kind(dev) == "tb":
        import qdev_tb
        if kms:
            raise QdevError(f"{dev['plugin']} has no KMS output")
        return qdev_tb.update(vmid, conf, dev, new["args"])
    replaced = lambda d: new if d["id"] == dev_id else d
    if dev_kind(dev) == "vhost-user-gpu":
        new.update(display=False, kms=None, persistent=False)
        check_editable(vmid)
        conf["devices"] = [replaced(d) for d in conf["devices"]]
        notes = apply_vug_args(vmid, conf, vm_config(vmid))
        save_conf(conf)
        write_unit_env(vmid, new)
        return ([f"VM {vmid}: GPU {dev_id} updated"] + [f"note: {n}" for n in notes] +
                ["the change applies at the next VM start" + (": stop and start the VM" if vm_running(vmid) else "")])
    # a vfio-user device gets a new process; a running VM sees an unplug and a replug
    running = vm_running(vmid)
    was_attached = running and device_present(vmid, dev_id)
    msgs = []
    if was_attached or process_active(vmid, dev_id):
        msgs.append(detach(vmid, dev, keep_process=False))
    conf["devices"] = [replaced(d) for d in conf["devices"]]
    save_conf(conf)
    write_unit_env(vmid, new)
    if running:
        msgs.append(attach(vmid, new))
    elif new.get("persistent"):
        start_process(vmid, new)
        msgs.append(f"{dev_id}: process started with the new settings")
    return [f"VM {vmid}: device {dev_id} updated" + (", unplugged and plugged again" if was_attached else "")] + msgs

def remove_device(vmid: int, dev_id: str, keep_process: bool = False, purge_port: bool = False) -> list[str]:
    msgs = []
    conf = load_conf(vmid)
    dev = find_device(conf, dev_id)
    if not dev:
        raise QdevError(f"no device {dev_id} on VM {vmid}")
    check_editable(vmid)
    if dev_kind(dev) == "tb":
        import qdev_tb
        msgs += qdev_tb.remove(vmid, conf, dev)
    elif dev_kind(dev) == "vhost-user-gpu":
        running = vm_running(vmid)
        cfg = vm_config(vmid)
        for pat in vug_patterns(dev_id):
            cfg["args"] = _drop(cfg.get("args", ""), pat)
        conf["devices"] = [d for d in conf["devices"] if d["id"] != dev_id]
        apply_vug_args(vmid, conf, cfg)
        if running:
            msgs.append(f"{dev_id}: removed from the VM config; the running VM keeps the GPU until it stops")
        else:
            stop_process(vmid, dev_id, "vhost-user-gpu")
            msgs.append(f"{dev_id}: removed")
    else:
        msgs.append(detach(vmid, dev, keep_process))
        conf["devices"] = [d for d in conf["devices"] if d["id"] != dev_id]
        if needs_shared_memory(dev) and _find(vm_config(vmid).get("args", ""), SHARED_MEM_PATTERNS[0]):
            apply_vug_args(vmid, conf, vm_config(vmid))
    env = UNIT_DIR / f"{vmid}-{dev_id}.env"
    if env.exists() and not keep_process:
        env.unlink()
    if not conf["devices"] and not keep_process:
        cfg = vm_config(vmid)
        restore_hookscript(vmid, conf)
        if purge_port:
            remove_port_from_args(vmid, vm_config(vmid))
            if conf.get("tb"):
                import qdev_tb
                qdev_tb.purge(vmid, conf, vm_config(vmid))
        if conf.get("tb") and not purge_port:
            # the bus stays in args: keep its settings for the files written at the next start
            conf["previous_hookscript"] = None
            save_conf(conf)
        else:
            conf_path(vmid).unlink(missing_ok=True)
        msgs.append(f"VM {vmid}: no devices left, hookscript restored" +
                    (", root port and TB bus removed from args" if purge_port else
                     " (root port and TB bus kept in args; --purge-port removes them)"))
    else:
        save_conf(conf)
    return msgs

def describe_vm(vmid: int) -> dict:
    """Everything the UI needs for one VM: devices with live state, plugins, KMS outputs."""
    node = vm_node(vmid)
    me = local_node()
    if node and node != me:
        return {"vmid": vmid, "node": node, "local": False, "devices": []}
    conf = load_conf(vmid)
    cfg = read_vm_config(vmid)
    running = vm_running(vmid)
    present: dict[str, str] = {}
    buses: set[str] = set()
    if running:
        for d in pci_devices(vmid):
            if d["qdev_id"]:
                present[d["qdev_id"]] = f"{d['bus']:02x}:{d['slot']:02x}.0"
                buses.add(d["qdev_id"])
    devices = []
    mismatch = shared_memory_mismatch(cfg)
    import qdev_tb
    tb_info, tb_where = qdev_tb.describe(vmid, conf, cfg, running)
    for d in conf["devices"]:
        kind = dev_kind(d)
        if kind == "tb":
            attached = d["id"] in tb_where
            target = d["bus"] in present or d["bus"] in tb_where
            devices.append({**{k: d.get(k) for k in ("id", "plugin", "args", "bus", "display", "kms", "persistent")},
                            "kind": kind, "state": "attached" if attached else "stopped", "process_active": False,
                            "pci": present.get(d["id"]),
                            # a USB device whose controller is detached waits for the controller, not a restart
                            "needs_restart": bool(running and not attached and not target and qdev_tb.is_port(d["bus"])),
                            "warning": None})
            continue
        active = process_active(vmid, d["id"], kind)
        if d["id"] in present:
            state = "attached"
        elif active:
            state = "process"
        else:
            state = "stopped"
        restart = (d["id"] not in present) if kind == "vhost-user-gpu" else (d["bus"] not in buses)
        devices.append({**{k: d.get(k) for k in ("id", "plugin", "args", "bus", "display", "kms", "persistent")},
                        "kind": kind, "state": state, "process_active": active, "pci": present.get(d["id"]),
                        "needs_restart": bool(running and restart),
                        "warning": mismatch if kind == "vhost-user-gpu" else None})
    return {"vmid": vmid, "node": me, "local": True, "running": running, "q35": is_q35(cfg),
            "machine": cfg.get("machine", ""), "port_addr": conf.get("port_addr", "0x08"),
            "port_in_args": f"id={PORT_ID}" in cfg.get("args", ""), "port_in_vm": PORT_ID in buses,
            "hookscript": cfg.get("hookscript"), "devices": devices, "tb": tb_info,
            "plugins": list_plugins(), "kms_outputs": list_kms_outputs(), "render_nodes": list_render_nodes()}
