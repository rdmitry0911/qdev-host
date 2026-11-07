# qdev-host on Proxmox VE (qdev-host-pve)

Adds out-of-process devices (qdev-host plugins) to VMs on a stock Proxmox VE 9 node.
No Proxmox package is patched; the integration uses `qm`, the node's own QEMU
monitor API (`PVE::QemuServer::Monitor::mon_cmd`), the standard guest hookscript
and systemd.

```bash
apt install ./qdev-host_0.1.0_amd64.deb ./qdev-host-pve_0.1.0_all.deb
qdevctl add 128 tbvga0 --plugin tb-vga --args vram=16      # define + hookscript + root port in args
qm stop 128 && qm start 128                                  # the root port appears at the next start
qdevctl list 128                                             # tbvga0 ... attached
qdevctl remove 128 tbvga0                                    # hot-unplug, stop the process, forget it
```

What `add` does:

1. writes `/etc/qdev-host/vms/VMID.json` and `/etc/qdev-host/units/VMID-ID.env`;
2. sets the VM's `hookscript` to `local:snippets/qdev-host-hook.pl` (a VM's own
   hookscript is remembered and chained);
3. for the default bus `qdev-rp0` appends a `pcie-root-port` with reserved windows
   (`pref64-reserve=256M,mem-reserve=16M`) to the VM's `args` (root@pam only, like
   any `args` change). `--bus ich9-pcie-port-N` uses one of PVE's own q35 ports
   instead (no restart, but the firmware reserves only small windows there);
4. if the VM is running and the bus exists: starts
   `qdev-host-device@VMID-ID.service` and hot-plugs the device (`device_add
   vfio-user-pci`).

On every VM start the hookscript (post-start) starts the device processes and
hot-plugs them; on post-stop it stops them (`--persistent` keeps a process, and
its VRAM, across VM restarts).

Sockets: `/run/qdev-host/VMID-ID/vfio.sock` (vfio-user) and `display.sock`
(org.qemu.Display1 p2p). When QSM Direct runs the VM's Display1 bus
(`/run/qsm-pve-direct/VMID/qemu-display1.bus`), the device relays keyboard,
mouse, clipboard and audio to it automatically. A real monitor: `--kms
/dev/dri/cardN:CONNECTOR` (the node must not run a compositor on that card).

Logs: `journalctl -u qdev-host-device@VMID-ID`. Requirements: q35 machine type,
`pve-qemu-kvm` ≥ 10.1 (vfio-user-pci), a storage with `snippets` content.
