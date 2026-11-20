#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""qdev-host-reboot-relay@VMID: a guest reboot of a Proxmox VE VM becomes a cold restart (`qdevctl reboot-policy VMID cold`).

With the reboot policy `cold` the VM runs with `-action reboot=shutdown` and a dedicated QMP socket
(/run/qdev-host/reboot-relay-VMID.qmp), so QEMU turns every reset request into a SHUTDOWN event that carries its cause
(stock QEMU >= 6.0). PVE starts QEMU with -no-shutdown: the VM pauses, qmeventd sends `quit`, and `qm cleanup` runs the
post-stop hook and then starts the VM again when PVE's own reboot request /run/qemu-server/VMID.reboot exists (the empty
file `qm reboot` creates, PVE::QemuServer::create_reboot_request). The new start gets a new QEMU, new device processes
and a new GPU context: the cold-start path.

This relay creates that request for

    reason guest-reset             the guest rebooted (in-guest Restart, ACPI/CF9/keyboard reset, triple fault)
    reason host-qmp-system-reset   QMP system_reset (PVE's Reset button)

and for nothing else: guest-shutdown, host-qmp-quit, guest-panic, host-signal, subsystem-reset, ... stay off, and
`qm reboot` (an ACPI shutdown plus PVE's own request) is unchanged. Only the first SHUTDOWN of a QEMU process counts.

Race fallback: when the VM is not running again FALLBACK_S (30) seconds after the SHUTDOWN and nothing holds the VM's
config lock (a cleanup or a start in progress; waits at most CAP_S more), the relay removes a request that is still
there (it would otherwise restart the VM at a later, unrelated stop), stops the VM's device units that are still active
(qdev-host-device@VMID-*, except devices marked persistent: a cleanup that did not finish skipped post-stop, and the
start must get new device processes) and runs `qm start VMID` once.

The relay keeps running while the VM's policy is `cold` (/etc/qdev-host/vms/VMID.json "reboot_policy"); with another
policy it exits once no QEMU serves its socket.

  qdev_reboot_relay.py VMID [--fallback-s 30] [--cap-s 300] [--once]
Paths (tests): --sock-dir /run/qdev-host --run-dir /run/qemu-server --lock-dir /run/lock/qemu-server
               --conf-dir /etc/qdev-host/vms --qm /usr/sbin/qm --systemctl systemctl
"""
from __future__ import annotations
import argparse, fcntl, json, os, socket, struct, subprocess, sys, time
from pathlib import Path

RESTART_REASONS = ("guest-reset", "host-qmp-system-reset")
SOCK_DIR = "/run/qdev-host"
RUN_DIR = "/run/qemu-server"            # pid files and reboot requests (PVE writes /var/run/..., a link to /run)
LOCK_DIR = "/run/lock/qemu-server"      # PVE::QemuConfig config_file_lock: lock-VMID.conf (flock)
CONF_DIR = "/etc/qdev-host/vms"
QM = "/usr/sbin/qm"
SYSTEMCTL = "systemctl"


def relay_socket(vmid: int, sock_dir: str = SOCK_DIR) -> Path:
    return Path(sock_dir) / f"reboot-relay-{vmid}.qmp"


def peer_pid(s: socket.socket) -> int | None:
    try:
        pid, _uid, _gid = struct.unpack("3i", s.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
        return pid or None
    except OSError:
        return None


class Relay:
    def __init__(self, vmid: int, sock_dir=SOCK_DIR, run_dir=RUN_DIR, lock_dir=LOCK_DIR, conf_dir=CONF_DIR, qm=QM,
                 fallback_s: float = 30.0, cap_s: float = 300.0, poll_s: float = 1.0, out=None, systemctl=SYSTEMCTL):
        self.vmid = int(vmid)
        self.sock = relay_socket(self.vmid, sock_dir)
        self.run_dir, self.lock_dir, self.conf_dir, self.qm = Path(run_dir), Path(lock_dir), Path(conf_dir), qm
        self.systemctl = systemctl
        self.fallback_s, self.cap_s, self.poll_s = fallback_s, cap_s, poll_s
        self.out = out or sys.stdout

    def log(self, msg: str) -> None:
        print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} VM {self.vmid}: {msg}", file=self.out, flush=True)

    # ---------------------------------------------------------------- PVE state
    def request_path(self) -> Path:
        return self.run_dir / f"{self.vmid}.reboot"

    def create_request(self) -> bool:
        """PVE's create_reboot_request: an empty file. Idempotent; returns True when it already existed."""
        p = self.request_path()
        existed = p.exists()
        with open(p, "w"):
            pass
        return existed

    def running_pid(self) -> int | None:
        """The VM's QEMU pid when it runs, checked as qemu-server's vm_running_locally does: the pid file names a live,
        non-zombie process whose command line carries that pid file."""
        pidfile = self.run_dir / f"{self.vmid}.pid"
        try:
            pid = int(pidfile.read_text().strip())
            args = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
            stat = Path(f"/proc/{pid}/stat").read_text()
            i = args.index(b"-pidfile")
        except (OSError, ValueError):
            return None
        if i + 1 >= len(args) or os.path.realpath(args[i + 1].decode(errors="replace")) != os.path.realpath(pidfile):
            return None
        state = stat.rsplit(")", 1)[-1].split()
        return pid if state and state[0] != "Z" else None

    def lock_busy(self) -> bool:
        """True while someone holds the VM's PVE config lock (qm cleanup, a start, a migration...)."""
        try:
            fd = os.open(self.lock_dir / f"lock-{self.vmid}.conf", os.O_RDONLY)
        except OSError:
            return False
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        finally:
            os.close(fd)        # closing drops our shared lock at once
        return False

    def conf(self) -> dict:
        try:
            return json.loads((self.conf_dir / f"{self.vmid}.json").read_text())
        except (OSError, ValueError):
            return {}

    def policy(self) -> str:
        return self.conf().get("reboot_policy", "reset")

    def stop_stale_units(self) -> list[str]:
        """Stop the VM's device units that are still active while it does not run (not the persistent ones)."""
        keep = {f"qdev-host-device@{self.vmid}-{d.get('id')}.service" for d in self.conf().get("devices", [])
                if d.get("persistent")}
        try:
            r = subprocess.run([self.systemctl, "list-units", "--plain", "--no-legend", "--state=active",
                                f"qdev-host-device@{self.vmid}-*"], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            return []
        stopped = []
        for unit in (l.split()[0] for l in r.stdout.splitlines() if l.strip()):
            if not unit.startswith(f"qdev-host-device@{self.vmid}-") or unit in keep:
                continue
            subprocess.run([self.systemctl, "stop", unit], capture_output=True, timeout=120)
            stopped.append(unit)
        return stopped

    # ---------------------------------------------------------------- QMP
    def connect(self) -> socket.socket | None:
        if not self.sock.exists():
            return None
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.settimeout(10)
            s.connect(str(self.sock))
        except OSError:
            s.close()
            return None
        return s

    def session(self, s: socket.socket) -> tuple[str | None, int | None]:
        """Serve one QEMU process until its socket closes. Returns (reason of its first SHUTDOWN, QEMU pid)."""
        pid = peer_pid(s) or self.running_pid()
        f = s.makefile("r", encoding="utf-8", errors="replace", newline="\n")
        greeting = f.readline()
        if '"QMP"' not in greeting:
            self.log(f"no QMP greeting on {self.sock}: {greeting.strip()[:120]!r}")
            return None, pid
        s.sendall(b'{"execute": "qmp_capabilities"}\n')
        s.settimeout(None)
        self.log(f"connected to QEMU pid {pid} on {self.sock}")
        first = None
        for line in f:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("event") != "SHUTDOWN" or first is not None:
                continue
            data = obj.get("data") or {}
            first = str(data.get("reason", ""))
            if first in RESTART_REASONS:
                existed = self.create_request()
                self.log(f"SHUTDOWN reason={first} guest={data.get('guest')} -> reboot request {self.request_path()}"
                         + (" (already there)" if existed else ""))
            else:
                self.log(f"SHUTDOWN reason={first} guest={data.get('guest')} -> no action (the VM stays off)")
        self.log(f"QEMU pid {pid} closed its QMP socket")
        return first, pid

    # ---------------------------------------------------------------- fallback
    def fallback(self, old_pid: int | None, t_event: float) -> str:
        """After a restart reason: wait until a new QEMU runs; after FALLBACK_S with the config lock free and the VM
        stopped, drop a leftover request and start the VM once."""
        deadline, cap = t_event + self.fallback_s, t_event + self.fallback_s + self.cap_s
        waited = False
        while True:
            pid = self.running_pid()
            if pid and pid != old_pid:
                self.log(f"restarted: new QEMU pid {pid} {round(time.monotonic() - t_event, 1)} s after the SHUTDOWN")
                return "restarted"
            now = time.monotonic()
            if now >= deadline:
                busy = self.lock_busy()
                if (busy or pid) and now < cap:
                    waited = waited or busy
                    time.sleep(self.poll_s)
                    continue
                if pid:
                    self.log(f"old QEMU pid {pid} still runs {round(now - t_event)} s after the SHUTDOWN; no start")
                    return "old-qemu-running"
                left = self.request_path().exists()
                if left:
                    self.request_path().unlink(missing_ok=True)
                stale = self.stop_stale_units()
                self.log(f"fallback: VM {self.vmid} not running {round(now - t_event, 1)} s after the SHUTDOWN"
                         f"{' (waited for the config lock)' if waited else ''}; "
                         f"{'removed the leftover reboot request; ' if left else ''}"
                         f"{'stopped still-active ' + ', '.join(stale) + '; ' if stale else ''}qm start {self.vmid}")
                try:
                    r = subprocess.run([self.qm, "start", str(self.vmid)], capture_output=True, text=True, timeout=600)
                    tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
                    self.log(f"fallback: qm start rc={r.returncode} {' | '.join(tail)[:300]}")
                    return f"fallback-start rc={r.returncode}"
                except (OSError, subprocess.TimeoutExpired) as e:
                    self.log(f"fallback: qm start failed: {e}")
                    return "fallback-start failed"
            time.sleep(self.poll_s)

    # ---------------------------------------------------------------- main loop
    def run(self, once: bool = False) -> int:
        self.sock.parent.mkdir(parents=True, exist_ok=True)
        self.log(f"relay up: socket {self.sock}, restart reasons {', '.join(RESTART_REASONS)}, fallback {self.fallback_s:g} s,"
                 f" policy {self.policy()}")
        while True:
            s = self.connect()
            if s is None:
                if self.policy() != "cold":
                    self.log("reboot policy is not cold and no QEMU serves the relay socket: exiting")
                    return 0
                time.sleep(self.poll_s)
                continue
            try:
                reason, pid = self.session(s)
            finally:
                s.close()
            if reason in RESTART_REASONS:
                self.fallback(pid, time.monotonic())
            if once and reason is not None:
                return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("vmid", type=int)
    p.add_argument("--fallback-s", type=float, default=30.0)
    p.add_argument("--cap-s", type=float, default=300.0)
    p.add_argument("--poll-s", type=float, default=1.0)
    p.add_argument("--once", action="store_true", help="return after the first QEMU process that sent a SHUTDOWN")
    p.add_argument("--sock-dir", default=SOCK_DIR)
    p.add_argument("--run-dir", default=RUN_DIR)
    p.add_argument("--lock-dir", default=LOCK_DIR)
    p.add_argument("--conf-dir", default=CONF_DIR)
    p.add_argument("--qm", default=QM)
    p.add_argument("--systemctl", default=SYSTEMCTL)
    a = p.parse_args(argv)
    r = Relay(a.vmid, a.sock_dir, a.run_dir, a.lock_dir, a.conf_dir, a.qm, a.fallback_s, a.cap_s, a.poll_s,
              systemctl=a.systemctl)
    return r.run(once=a.once)


if __name__ == "__main__":
    sys.exit(main())
