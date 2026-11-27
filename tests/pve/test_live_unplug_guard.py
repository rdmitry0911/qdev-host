#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Off-node tests of the live-unplug guard (D-VT-HOTREMOVE-ASSERT): qdevctl/API never send QMP device_del for a
vfio-user device that carries vfio-user's MSI-X PBA region, because stock QEMU aborts when it frees such a device
(hw/vfio-user/pci.c vfio_user_msix_teardown after QOM already finalized the region; system/memory.c
memory_region_del_subregion assertion; unmerged upstream fix
https://patchew.org/QEMU/20251010-vfio-v1-1-d7a6056539b7@rsg.ci.i.u-tokyo.ac.jp).

  python3 tests/pve/test_live_unplug_guard.py

qdevctl and qdev_pve run as is with the doubles of test_reboot_policy.py (qm, systemctl). The VM's QMP monitor is
a DOUBLE scoped to this file: Q.mon_cmd answers query-pci / qom-list from a small device table and records every
command; Q.vm_running answers from a flag. The QOM child names come from QEMU v11.0.3: vfio-user's
memory_region_init_io(..., "VFIO MSIX PBA", ...) -> child "VFIO MSIX PBA[0]" of the device (memory_region_do_init
escapes only / [ \\ ]); msix_init adds "msix-table[0]"/"msix-pba[0]" for any MSI-X device. Not evidence for QEMU
itself; the live gate on a disposable VM is.
"""
import json, os, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_reboot_policy import Node, VMID  # noqa: E402

MSIX_VFIO_USER = ["type", "parent_bus", "hotpluggable", "realized", "msix-table[0]", "msix-pba[0]",
                  "VFIO MSIX PBA[0]", "vfio-user-pci base BAR 0[0]"]
MSI_ONLY = ["type", "parent_bus", "hotpluggable", "realized", "vfio-user-pci base BAR 0[0]"]


class FakeQemu:
    """devices: qdev id -> list of QOM child names; records every monitor command."""

    def __init__(self, devices):
        self.devices = dict(devices)
        self.cmds = []

    def __call__(self, vmid, cmd, **params):
        self.cmds.append((cmd, params))
        if cmd == "query-pci":
            return [{"bus": 0, "devices": [
                {"slot": i + 1, "qdev_id": qid, "id": {"vendor": 0x1b36, "device": 0xeee0 + i}}
                for i, qid in enumerate(self.devices)]}]
        if cmd == "qom-list":
            qid = params["path"].rsplit("/", 1)[-1]
            if qid not in self.devices:
                raise QdevErrorHolder.cls(f"qom-list: Device '{qid}' not found")
            return [{"name": n, "type": "child<memory-region>" if "[" in n else "string"}
                    for n in self.devices[qid]]
        if cmd == "device_del":
            self.devices.pop(params["id"], None)  # the guest ejects at once
            return {}
        raise AssertionError(f"unexpected monitor command {cmd}")

    def sent(self, cmd):
        return [p for c, p in self.cmds if c == cmd]


class QdevErrorHolder:
    cls = Exception


def dev(dev_id, plugin):
    return {"id": dev_id, "plugin": plugin, "args": "", "bus": "qdev-tb0", "display": False, "kms": None,
            "persistent": False, "extra": []}


class Guard(unittest.TestCase):
    def setUp(self):
        self.n = Node(self)
        self.Q = self.n.Q
        QdevErrorHolder.cls = self.Q.QdevError
        self.qemu = FakeQemu({"vt0": MSIX_VFIO_USER, "gpu0": MSI_ONLY})
        self.Q.mon_cmd = self.qemu
        self.running = True
        self.Q.vm_running = lambda vmid: self.running
        self.Q.save_conf({"vmid": VMID, "devices": [dev("vt0", "apple-videotoolbox"), dev("gpu0", "uniq-vgpu")],
                          "port_addr": "0x08", "previous_hookscript": None, "tb": {"ports": 4}})

    def test_device_del_refuses_vfio_user_msix(self):
        with self.assertRaises(self.Q.QdevError) as cm:
            self.Q.device_del(VMID, "vt0")
        msg = str(cm.exception)
        self.assertIn("vt0", msg)
        self.assertIn("MSI-X", msg)
        self.assertIn("patchew.org/QEMU/20251010-vfio-v1-1-d7a6056539b7@rsg.ci.i.u-tokyo.ac.jp", msg)
        self.assertIn("stop the VM", msg)
        self.assertEqual(self.qemu.sent("device_del"), [], "no device_del may reach QEMU")
        self.assertIn("vt0", self.qemu.devices)

    def test_device_del_proceeds_without_msix(self):
        self.assertTrue(self.Q.device_del(VMID, "gpu0"))
        self.assertEqual(self.qemu.sent("device_del"), [{"id": "gpu0"}])

    def test_device_del_absent_device_is_gone(self):
        self.assertTrue(self.Q.device_del(VMID, "nope"))
        self.assertEqual(self.qemu.sent("device_del"), [])

    def test_remove_refused_keeps_config_and_process(self):
        rc, out, err = self.n.qdevctl("remove", VMID, "vt0")
        self.assertEqual(rc, 1, out + err)
        self.assertIn("MSI-X", err)
        self.assertEqual([d["id"] for d in self.n.conf()["devices"]], ["vt0", "gpu0"], "config unchanged")
        self.assertEqual(self.qemu.sent("device_del"), [])
        self.assertFalse([c for c in self.n.calls() if c[:2] == ["systemctl", "stop"]], "process kept")

    def test_stop_all_refuses_one_detaches_the_other(self):
        rc, out, err = self.n.qdevctl("stop", VMID)
        self.assertEqual(rc, 1, out + err)
        self.assertIn("vt0", err)
        self.assertIn("gpu0: detached", out)
        self.assertEqual(self.qemu.sent("device_del"), [{"id": "gpu0"}])

    def test_swap_hotplug_reset_refused_before_the_swap(self):
        swaps = []
        self.Q.control_cmd = lambda path, cmd: swaps.append(cmd) or "ok swapped"
        with self.assertRaises(self.Q.QdevError):
            self.Q.device_swap("/nonexistent.sock", "B", reset="hotplug", vmid=VMID, dev_id="vt0")
        self.assertEqual(swaps, [], "refused before the backing changes")
        self.assertEqual(self.qemu.sent("device_del"), [])

    def test_remove_after_vm_stop_works(self):
        self.running = False
        rc, out, err = self.n.qdevctl("remove", VMID, "vt0")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual([d["id"] for d in self.n.conf()["devices"]], ["gpu0"])
        self.assertEqual(self.qemu.cmds, [], "VM stopped: no monitor command")


if __name__ == "__main__":
    unittest.main(verbosity=2)
