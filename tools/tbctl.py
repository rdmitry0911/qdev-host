#!/usr/bin/env python3
"""tbctl: drive the Thunderbolt-style hotplug flow on stock QEMU over QMP.

  tbctl.py QMP_SOCK plug ID SOCKET [BUS]   device_add vfio-user-pci behind BUS (default tbrp0)
  tbctl.py QMP_SOCK unplug ID              device_del + wait for DEVICE_DELETED
  tbctl.py QMP_SOCK disconnect BUS         unplug every device behind BUS (fork: connected=false)
  tbctl.py QMP_SOCK connect BUS ID=SOCKET...  re-add devices (fork: connected=true)
  tbctl.py QMP_SOCK pci                    list PCI devices (bus, slot, id, class)
  tbctl.py QMP_SOCK screendump FILE        PPM screenshot of the QEMU-side display
"""
import json, socket, sys, time

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); self.s.settimeout(60)
        self.f = self.s.makefile("rwb", buffering=0)
        self.f.readline()
        self.cmd("qmp_capabilities")
    def _recv(self):
        while True:
            line = self.f.readline()
            if not line: raise RuntimeError("QMP closed")
            m = json.loads(line)
            if "event" in m:
                self.events.append(m); continue
            return m
    events = []
    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        r = self._recv()
        if "error" in r: raise RuntimeError(f"{name}: {r['error']['desc']}")
        return r.get("return")
    def wait_event(self, ev, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for e in self.events:
                if e["event"] == ev: self.events.remove(e); return e
            self.s.settimeout(0.5)
            try:
                line = self.f.readline()
                if line:
                    m = json.loads(line)
                    if "event" in m: self.events.append(m)
            except socket.timeout: pass
            finally: self.s.settimeout(60)
        return None

def pci_list(q):
    """(bus, slot, func, qdev_id, class, vendor, device, parent_bridge_id)"""
    out = []
    def walk(devs, busnum, parent):
        for d in devs:
            out.append((busnum, d["slot"], d["function"], d.get("qdev_id", ""), d["class_info"]["desc"], d["id"]["vendor"], d["id"]["device"], parent))
            if "pci_bridge" in d and d["pci_bridge"].get("devices"):
                walk(d["pci_bridge"]["devices"], d["pci_bridge"]["bus"]["number"], d.get("qdev_id", ""))
    for root in q.cmd("query-pci"):
        walk(root["devices"], root["bus"], "")
    return out

def main():
    q = QMP(sys.argv[1]); op = sys.argv[2]; a = sys.argv[3:]
    if op == "plug":
        dev_id, sock = a[0], a[1]; bus = a[2] if len(a) > 2 else "tbrp0"
        q.cmd("device_add", driver="vfio-user-pci", id=dev_id, bus=bus,
              socket={"type": "unix", "path": sock})
        print(f"plugged {dev_id} on {bus} <- {sock}")
    elif op == "unplug":
        q.cmd("device_del", id=a[0])
        e = q.wait_event("DEVICE_DELETED", 30)
        print(f"unplugged {a[0]}: {'DEVICE_DELETED' if e else 'no DEVICE_DELETED event within 30 s'}")
    elif op == "disconnect":
        ids = [d[3] for d in pci_list(q) if d[3] and d[7] == a[0]]
        for i in ids: q.cmd("device_del", id=i)
        for i in ids: q.wait_event("DEVICE_DELETED", 30)
        print(f"disconnected {a[0]}: removed {ids}")
    elif op == "connect":
        bus = a[0]
        present = {d[3] for d in pci_list(q)}
        for spec in a[1:]:
            dev_id, sock = spec.split("=", 1)
            if dev_id in present:
                print(f"{dev_id} already present, skipping"); continue
            q.cmd("device_add", driver="vfio-user-pci", id=dev_id, bus=bus, socket={"type": "unix", "path": sock})
        print(f"connected {bus}: {a[1:]}")
    elif op == "pci":
        for busnum, slot, fn, qid, desc, ven, devid, parent in pci_list(q):
            print(f"bus {busnum:02x} slot {slot:02x}.{fn} id={qid or '-':10s} {ven:04x}:{devid:04x} {desc}" + (f"  (behind {parent})" if parent else ""))
    elif op == "screendump":
        q.cmd("screendump", filename=a[0]); print("screendump", a[0])
    else:
        print(__doc__); sys.exit(2)

if __name__ == "__main__":
    main()
