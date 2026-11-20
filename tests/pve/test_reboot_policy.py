#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Off-node tests of `qdevctl reboot-policy VMID cold|reset` (args round-trip) and the pre-start stop of a device
process left from the previous run.

  python3 tests/pve/test_reboot_policy.py

qdevctl and qdev_pve run as is. `qm` and `systemctl` are DOUBLES scoped to this file (executables first in PATH):
the qm double keeps one VM config (with PVE's [PENDING] semantics reduced to "qm config prints the pending value")
and implements `config` and `set --KEY V / --delete KEY`; the systemctl double records argv and answers `is-active`
from a state file. qdev_pve's node paths (CONF_DIR, RUN_DIR, the snippets storage, the hook source) point into a
temporary directory. Not evidence for PVE itself; the live gates on a disposable PVE VM are.
"""
import importlib.machinery, importlib.util, io, json, os, shutil, sys, tempfile, unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
PVE = os.path.abspath(os.path.join(HERE, "..", "..", "pve"))
VMID = 118

# VM118's args as found on hz-main (2026-11-20, read-only): JSON in single quotes, QSM groups, two QMP monitors
ARGS_118 = ("-smbios type=2 -device usb-kbd,bus=ehci.0,port=2 -global nec-usb-xhci.msi=off "
            "-global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off -cpu Haswell-noTSX,vendor=GenuineIntel,+invtsc,"
            "+hypervisor,kvm=on,vmware-cpuid-freq=on -machine vmport=off -object memory-backend-memfd,id=uniq-vgpu-mem,"
            "size=32768M,share=on -machine memory-backend=uniq-vgpu-mem -chardev socket,id=uniq-vgpu-qmp,"
            "path=/run/uniq-vgpu/118-qmp.sock,server=on,wait=off -mon chardev=uniq-vgpu-qmp,mode=control "
            "-device '{\"driver\":\"vfio-user-pci\",\"id\":\"pvg0\",\"bus\":\"pcie.0\",\"addr\":\"0x5\",\"socket\":"
            "{\"type\":\"unix\",\"path\":\"/run/qdev-host/118-pvg0/vfio.sock\"},\"romfile\":"
            "\"/opt/apple-virgl/roms/ApplePVG-stock-eeee.rom\"}' -display dbus,p2p=yes,audiodev=qsm-direct-audio")

QM = r'''#!/usr/bin/env python3
import json, os, sys
st = os.environ["FAKE_PVE_STATE"]
cfg = json.load(open(st))
a = sys.argv[1:]
with open(os.environ["FAKE_PVE_LOG"], "a") as f:
    f.write(json.dumps(["qm"] + a) + "\n")
if a[0] == "config":
    for k in sorted(cfg):
        print(f"{k}: {cfg[k]}")
elif a[0] == "set":
    i = 2
    while i < len(a):
        if a[i] == "--delete":
            for k in a[i + 1].split(","):
                cfg.pop(k, None)
        else:
            cfg[a[i][2:]] = a[i + 1]
        i += 2
    json.dump(cfg, open(st, "w"))
else:
    sys.exit(2)
'''

SYSTEMCTL = r'''#!/usr/bin/env python3
import json, os, sys
a = sys.argv[1:]
with open(os.environ["FAKE_PVE_LOG"], "a") as f:
    f.write(json.dumps(["systemctl"] + a) + "\n")
active = json.load(open(os.environ["FAKE_ACTIVE"]))
if a[0] == "is-active":
    print("active" if active.get(a[1]) else "inactive")
    sys.exit(0 if active.get(a[1]) else 3)
if a[0] == "stop":
    active[a[1]] = False
    json.dump(active, open(os.environ["FAKE_ACTIVE"], "w"))
if a[0] == "list-units":
    sys.exit(0)
'''


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class Node:
    def __init__(self, t, args=ARGS_118, hookscript="local:snippets/qdev-host-hook.pl"):
        self.d = Path(tempfile.mkdtemp(prefix="rpolicy-"))
        t.addCleanup(shutil.rmtree, self.d, True)
        b = self.d / "bin"; b.mkdir()
        for n, body in (("qm", QM), ("systemctl", SYSTEMCTL)):
            (b / n).write_text(body); (b / n).chmod(0o755)
        self.state, self.log, self.active = self.d / "vm.json", self.d / "calls.log", self.d / "active.json"
        cfg = {"machine": "q35", "bios": "ovmf"}
        if args:
            cfg["args"] = args
        if hookscript:
            cfg["hookscript"] = hookscript
        self.state.write_text(json.dumps(cfg)); self.active.write_text("{}")
        env = {"PATH": f"{b}:{os.environ['PATH']}", "FAKE_PVE_STATE": str(self.state), "FAKE_PVE_LOG": str(self.log),
               "FAKE_ACTIVE": str(self.active)}
        self.old_env = {k: os.environ.get(k) for k in env}
        os.environ.update(env)
        t.addCleanup(self.restore_env)
        sys.path.insert(0, PVE)
        t.addCleanup(lambda: sys.path.remove(PVE))
        for m in ("qdev_pve", "qdev_tb", "qdevctl"):
            sys.modules.pop(m, None)
        self.Q = load("qdev_pve", os.path.join(PVE, "qdev_pve.py")); sys.modules["qdev_pve"] = self.Q
        self.Q.CONF_DIR = self.d / "etc-vms"; self.Q.UNIT_DIR = self.d / "etc-units"; self.Q.RUN_DIR = self.d / "run"
        snip = self.d / "snippets"; snip.mkdir()
        self.Q.HOOK_SRC = self.d / "hook-src.pl"; self.Q.HOOK_SRC.write_text("#!/usr/bin/perl\n")
        self.Q.snippets_dir = lambda: (snip, "local")
        self.Q.vm_node = lambda vmid: None
        self.ctl = load("qdevctl", os.path.join(PVE, "qdevctl"))

    def restore_env(self):
        for k, v in self.old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def qdevctl(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        old = sys.argv
        sys.argv = ["qdevctl", *map(str, argv)]
        try:
            with redirect_stdout(out), redirect_stderr(err):
                rc = self.ctl.main()
        finally:
            sys.argv = old
        return rc, out.getvalue(), err.getvalue()

    def cfg(self):
        return json.loads(self.state.read_text())

    def calls(self):
        return [json.loads(l) for l in self.log.read_text().splitlines()] if self.log.exists() else []

    def conf(self):
        return json.loads((self.Q.CONF_DIR / f"{VMID}.json").read_text())


GROUPS = f"-action reboot=shutdown -qmp unix:{{run}}/reboot-relay-{VMID}.qmp,server=on,wait=off"


class ArgsPure(unittest.TestCase):
    def setUp(self):
        self.n = Node(self)
        self.Q = self.n.Q
        self.g = GROUPS.format(run=self.Q.RUN_DIR)

    def test_round_trip(self):
        Q = self.Q
        cold = Q.apply_reboot_policy_args(ARGS_118, VMID, "cold")
        self.assertEqual(cold, ARGS_118 + " " + self.g)
        self.assertEqual(Q.apply_reboot_policy_args(cold, VMID, "cold"), cold, "set twice = once")
        self.assertEqual(Q.apply_reboot_policy_args(cold, VMID, "reset"), ARGS_118, "unset restores the args exactly")
        self.assertEqual(Q.apply_reboot_policy_args(ARGS_118, VMID, "reset"), ARGS_118, "unset twice = no change")
        self.assertEqual(Q.args_reboot_policy(cold, VMID), "cold")
        self.assertEqual(Q.args_reboot_policy(ARGS_118, VMID), "reset")

    def test_empty_and_middle(self):
        Q = self.Q
        self.assertEqual(Q.apply_reboot_policy_args("", VMID, "cold"), self.g)
        self.assertEqual(Q.apply_reboot_policy_args(self.g, VMID, "reset"), "")
        mid = "-smbios type=2 " + self.g + " -machine vmport=off"
        self.assertEqual(Q.apply_reboot_policy_args(mid, VMID, "reset"), "-smbios type=2 -machine vmport=off")
        self.assertEqual(Q.apply_reboot_policy_args(mid, VMID, "cold"), "-smbios type=2 -machine vmport=off " + self.g)

    def test_other_vm_socket_and_other_action_untouched(self):
        Q = self.Q
        other = f"-qmp unix:{Q.RUN_DIR}/reboot-relay-104.qmp,server=on,wait=off -action panic=pause"
        self.assertEqual(Q.apply_reboot_policy_args(other, VMID, "reset"), other)
        self.assertEqual(Q.args_reboot_policy(other, VMID), "reset")

    def test_bad_policy(self):
        with self.assertRaises(self.Q.QdevError):
            self.Q.apply_reboot_policy_args(ARGS_118, VMID, "warm")


class QdevctlRoundTrip(unittest.TestCase):
    def test_set_unset_idempotent(self):
        n = Node(self)
        g = GROUPS.format(run=n.Q.RUN_DIR)
        unit = f"qdev-host-reboot-relay@{VMID}.service"
        rc, out, err = n.qdevctl("reboot-policy", VMID, "cold")
        self.assertEqual(rc, 0, err)
        self.assertEqual(n.cfg()["args"], ARGS_118 + " " + g)
        self.assertEqual(n.conf()["reboot_policy"], "cold")
        self.assertIn(["systemctl", "enable", "--now", unit], n.calls())
        self.assertIn("effective at the next VM start", out)
        sets = [c for c in n.calls() if c[:2] == ["qm", "set"]]
        self.assertEqual(len(sets), 1, "hookscript already qdev-host's: only the args change")

        rc, out, _ = n.qdevctl("reboot-policy", VMID, "cold")
        self.assertEqual(rc, 0)
        self.assertIn("already carry reboot policy cold", out)
        self.assertEqual(len([c for c in n.calls() if c[:2] == ["qm", "set"]]), 1, "second cold: no config write")
        self.assertEqual(n.cfg()["args"], ARGS_118 + " " + g)

        rc, out, _ = n.qdevctl("reboot-policy", VMID)
        self.assertEqual(rc, 0)
        self.assertIn(f"VM {VMID}: reboot policy cold (args: cold, VM not running)", out)

        rc, out, _ = n.qdevctl("reboot-policy", VMID, "reset")
        self.assertEqual(rc, 0)
        self.assertEqual(n.cfg()["args"], ARGS_118)
        self.assertEqual(n.conf()["reboot_policy"], "reset")
        self.assertIn(["systemctl", "disable", unit], n.calls())
        self.assertIn(["systemctl", "stop", unit], n.calls())

        rc, out, _ = n.qdevctl("reboot-policy", VMID, "reset")
        self.assertIn("already carry reboot policy reset", out)
        self.assertEqual(n.cfg()["args"], ARGS_118)

    def test_cold_on_vm_without_args_or_hookscript(self):
        n = Node(self, args="", hookscript=None)
        g = GROUPS.format(run=n.Q.RUN_DIR)
        rc, out, err = n.qdevctl("reboot-policy", VMID, "cold")
        self.assertEqual(rc, 0, err)
        self.assertEqual(n.cfg()["args"], g)
        self.assertEqual(n.cfg()["hookscript"], "local:snippets/qdev-host-hook.pl")
        rc, out, _ = n.qdevctl("reboot-policy", VMID, "reset")
        self.assertNotIn("args", n.cfg(), "reset of an args that held only our groups deletes the key")


class PreStart(unittest.TestCase):
    def test_stops_stale_non_persistent_processes_and_starts_relay(self):
        n = Node(self)
        n.Q.CONF_DIR.mkdir(parents=True)
        conf = {"vmid": VMID, "port_addr": "0x08", "previous_hookscript": None, "reboot_policy": "cold", "devices": [
            {"id": "pvg0", "plugin": "uniq-vgpu", "args": "", "bus": "pcie.0", "display": False, "persistent": False},
            {"id": "keep0", "plugin": "tb-vga", "args": "", "bus": "qdev-tb0", "display": False, "persistent": True}]}
        (n.Q.CONF_DIR / f"{VMID}.json").write_text(json.dumps(conf))
        n.active.write_text(json.dumps({f"qdev-host-device@{VMID}-pvg0.service": True,
                                        f"qdev-host-device@{VMID}-keep0.service": True}))
        rc, out, err = n.qdevctl("hook", VMID, "pre-start")
        self.assertEqual(rc, 0, err)
        calls = n.calls()
        self.assertIn(["systemctl", "stop", f"qdev-host-device@{VMID}-pvg0.service"], calls)
        self.assertNotIn(["systemctl", "stop", f"qdev-host-device@{VMID}-keep0.service"], calls)
        self.assertIn(["systemctl", "start", f"qdev-host-reboot-relay@{VMID}.service"], calls)
        self.assertIn("stopped the still-active device process", err)
        self.assertTrue(n.Q.RUN_DIR.is_dir(), "relay socket directory exists before QEMU starts")

    def test_nothing_to_stop_and_policy_reset(self):
        n = Node(self)
        n.Q.CONF_DIR.mkdir(parents=True)
        conf = {"vmid": VMID, "port_addr": "0x08", "previous_hookscript": None, "devices": [
            {"id": "pvg0", "plugin": "uniq-vgpu", "args": "", "bus": "pcie.0", "display": False, "persistent": False}]}
        (n.Q.CONF_DIR / f"{VMID}.json").write_text(json.dumps(conf))
        rc, out, err = n.qdevctl("hook", VMID, "pre-start")
        self.assertEqual(rc, 0, err)
        calls = n.calls()
        self.assertFalse([c for c in calls if c[:2] == ["systemctl", "stop"]])
        self.assertFalse([c for c in calls if c[:2] == ["systemctl", "start"]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
