#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Off-node tests of the reboot relay (pve/qdev_reboot_relay.py, reboot policy cold).

  python3 tests/pve/test_reboot_relay.py

The relay runs as is (its CLI, in a subprocess) against DOUBLES scoped to this file:
- fake QEMU: a process started with `-pidfile PATH` (so the relay's vm_running check sees it as QEMU sees it on PVE)
  that serves one QMP client on the relay socket (greeting, qmp_capabilities, then the given SHUTDOWN events) and exits;
- fake PVE cleanup: a thread that, after the fake QEMU exited, consumes the reboot request (unlink) and starts a new
  fake QEMU, as `qm cleanup` -> clear_reboot_request -> vm_start does; or does not (race cases);
- fake `qm`: records its argv.
The doubles model only the contract the relay relies on (QMP event lines, the pid file, PVE's reboot-request file,
the config lock flock). They are NOT evidence for qmeventd, qm cleanup or QEMU; the live gates on a disposable PVE VM
are.
"""
import fcntl, json, os, shutil, socket, subprocess, sys, tempfile, threading, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
RELAY = os.path.abspath(os.path.join(HERE, "..", "..", "pve", "qdev_reboot_relay.py"))
VMID = 9001

FAKE_QEMU = r'''
import json, os, socket, sys, time
a = sys.argv[1:]
sock, events, hold = a[0], json.loads(a[1]), float(a[2])
pidfile = a[a.index("-pidfile") + 1]
with open(pidfile, "w") as f:
    f.write(f"{os.getpid()}\n")
try:
    os.unlink(sock)
except FileNotFoundError:
    pass
srv = socket.socket(socket.AF_UNIX); srv.bind(sock); srv.listen(1); srv.settimeout(20)
c, _ = srv.accept()
f = c.makefile("rw", newline="\n")
f.write(json.dumps({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}}) + "\n"); f.flush()
cmd = json.loads(f.readline())
assert cmd["execute"] == "qmp_capabilities", cmd
f.write('{"return": {}}\n'); f.flush()
time.sleep(0.2)
for ev in events:
    f.write(json.dumps({"event": "SHUTDOWN", "data": ev, "timestamp": {"seconds": int(time.time()), "microseconds": 0}}) + "\n")
    f.write(json.dumps({"event": "STOP", "timestamp": {"seconds": int(time.time()), "microseconds": 0}}) + "\n")
    f.flush()
time.sleep(hold)
c.close(); srv.close(); os.unlink(sock)
'''


class Env:
    def __init__(self, t):
        self.d = tempfile.mkdtemp(prefix="relaytest-")
        self.bin = os.path.join(self.d, "bin"); os.mkdir(self.bin)
        for sub in ("sock", "run", "lock", "conf"):
            os.mkdir(os.path.join(self.d, sub))
        self.sockdir, self.run, self.lockd, self.conf = (os.path.join(self.d, s) for s in ("sock", "run", "lock", "conf"))
        self.qemu_py = os.path.join(self.bin, "fake_qemu.py")
        with open(self.qemu_py, "w") as f:
            f.write(FAKE_QEMU)
        self.qmlog = os.path.join(self.d, "qm.log")
        self.qm = os.path.join(self.bin, "qm")
        with open(self.qm, "w") as f:
            f.write(f"#!/bin/sh\necho \"$(date +%s.%N) $*\" >> {self.qmlog}\n")
        os.chmod(self.qm, 0o755)
        self.sctl_log = os.path.join(self.d, "systemctl.log")
        self.sctl = os.path.join(self.bin, "systemctl")
        with open(self.sctl, "w") as f:
            # active device units of the VM: pvg0 (a cleanup left it) and keep0 (persistent in the VM's json)
            f.write("#!/bin/sh\necho \"$*\" >> " + self.sctl_log + "\n"
                    "[ \"$1\" = list-units ] && printf 'qdev-host-device@%s-pvg0.service loaded active running x\\n"
                    "qdev-host-device@%s-keep0.service loaded active running y\\n' " + f"{VMID} {VMID}" + "\nexit 0\n")
        os.chmod(self.sctl, 0o755)
        self.sock = os.path.join(self.sockdir, f"reboot-relay-{VMID}.qmp")
        self.pidfile = os.path.join(self.run, f"{VMID}.pid")
        self.request = os.path.join(self.run, f"{VMID}.reboot")
        self.procs = []
        self.set_policy("cold")
        t.addCleanup(self.close)

    def set_policy(self, p):
        with open(os.path.join(self.conf, f"{VMID}.json"), "w") as f:
            json.dump({"vmid": VMID, "devices": [{"id": "pvg0", "persistent": False}, {"id": "keep0", "persistent": True}],
                       "reboot_policy": p}, f)

    def sctl_calls(self):
        try:
            with open(self.sctl_log) as f:
                return f.read().splitlines()
        except FileNotFoundError:
            return []

    def qemu(self, events, hold=0.3):
        p = subprocess.Popen([sys.executable, "-I", self.qemu_py, self.sock, json.dumps(events), str(hold),
                              "-pidfile", self.pidfile])
        self.procs.append(p)
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.02)
        return p

    def idle_qemu(self):
        # a restarted VM: serves the socket, never shuts down within the test
        return self.qemu([], hold=30)

    def relay(self, fallback=1.0, extra=()):
        p = subprocess.Popen([sys.executable, "-I", RELAY, str(VMID), "--once", "--fallback-s", str(fallback),
                              "--cap-s", "5", "--poll-s", "0.1", "--sock-dir", self.sockdir, "--run-dir", self.run,
                              "--lock-dir", self.lockd, "--conf-dir", self.conf, "--qm", self.qm,
                              "--systemctl", self.sctl, *extra],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.procs.append(p)
        return p

    def cleanup_sim(self, qemu, restart=True, consume=True, delay=0.2):
        """PVE: qm cleanup after QEMU exit: clear_reboot_request -> vm_start if it was there."""
        def body():
            qemu.wait()
            time.sleep(delay)
            had = os.path.exists(self.request)
            if consume and had:
                os.unlink(self.request)
            self.cleanup_saw = had
            if restart and had:
                self.idle_qemu()
        self.cleanup_saw = None
        th = threading.Thread(target=body, daemon=True); th.start()
        return th

    def qm_calls(self):
        try:
            with open(self.qmlog) as f:
                return [l.split(" ", 1)[1].strip() for l in f]
        except FileNotFoundError:
            return []

    def close(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            try:
                p.wait(5)
            except Exception:
                pass
            if p.stdout:
                p.stdout.close()
        shutil.rmtree(self.d, ignore_errors=True)


def finish(t, relay, timeout=20):
    out, _ = relay.communicate(timeout=timeout)
    t.assertEqual(relay.returncode, 0, out)
    return out


class ReasonMatrix(unittest.TestCase):
    def test_restart_reasons_create_request_and_pve_restarts(self):
        for reason in ("guest-reset", "host-qmp-system-reset"):
            with self.subTest(reason=reason):
                e = Env(self)
                q = e.qemu([{"guest": reason == "guest-reset", "reason": reason}])
                r = e.relay(fallback=3.0)
                th = e.cleanup_sim(q)
                out = finish(self, r)
                th.join(5)
                self.assertTrue(e.cleanup_saw, f"no reboot request when PVE's cleanup ran\n{out}")
                self.assertIn(f"SHUTDOWN reason={reason}", out)
                self.assertIn("-> reboot request", out)
                self.assertIn("restarted: new QEMU pid", out)
                self.assertEqual(e.qm_calls(), [], "no fallback start when PVE restarted the VM")
                self.assertEqual(e.sctl_calls(), [], "no unit touched when PVE restarted the VM")
                self.assertFalse(os.path.exists(e.request), "request consumed by the cleanup")

    def test_other_reasons_leave_the_vm_off(self):
        for reason in ("guest-shutdown", "guest-panic", "host-qmp-quit", "host-signal", "subsystem-reset", "host-error"):
            with self.subTest(reason=reason):
                e = Env(self)
                q = e.qemu([{"guest": reason.startswith("guest"), "reason": reason}])
                t0 = time.monotonic()
                r = e.relay(fallback=1.0)
                th = e.cleanup_sim(q)
                out = finish(self, r)
                th.join(5)
                self.assertIn(f"SHUTDOWN reason={reason}", out)
                self.assertIn("no action (the VM stays off)", out)
                self.assertFalse(e.cleanup_saw, "a reboot request was created")
                self.assertFalse(os.path.exists(e.request))
                time.sleep(1.3)   # past the fallback window: still no start
                self.assertEqual(e.qm_calls(), [])
                self.assertLess(time.monotonic() - t0, 10)


class RaceFallback(unittest.TestCase):
    def test_cleanup_ran_before_the_request_vm_stays_down_then_fallback_start(self):
        # PVE's cleanup missed the request (it cleared before the relay wrote it): the VM stays down and the request
        # file is left over. The relay removes it (else a later, unrelated stop would restart the VM) and starts once.
        e = Env(self)
        q = e.qemu([{"guest": True, "reason": "guest-reset"}])
        r = e.relay(fallback=1.0)
        th = e.cleanup_sim(q, restart=False, consume=False)
        t0 = time.monotonic()
        out = finish(self, r)
        th.join(5)
        self.assertEqual(e.qm_calls(), [f"start {VMID}"], out)
        self.assertGreaterEqual(time.monotonic() - t0, 1.0)
        self.assertFalse(os.path.exists(e.request), "leftover request removed")
        self.assertIn("removed the leftover reboot request", out)
        self.assertIn("fallback: qm start rc=0", out)
        # the device unit a cleanup left active is stopped before the start; the persistent one is kept
        self.assertIn(f"stop qdev-host-device@{VMID}-pvg0.service", e.sctl_calls())
        self.assertNotIn(f"stop qdev-host-device@{VMID}-keep0.service", e.sctl_calls())
        self.assertIn(f"stopped still-active qdev-host-device@{VMID}-pvg0.service", out)

    def test_cleanup_consumed_but_did_not_restart_fallback_start(self):
        e = Env(self)
        q = e.qemu([{"guest": True, "reason": "guest-reset"}])
        r = e.relay(fallback=1.0)
        th = e.cleanup_sim(q, restart=False, consume=True)
        out = finish(self, r)
        th.join(5)
        self.assertTrue(e.cleanup_saw)
        self.assertEqual(e.qm_calls(), [f"start {VMID}"], out)
        self.assertNotIn("removed the leftover", out)

    def test_waits_for_the_config_lock(self):
        # a cleanup/start still holds PVE's config lock at the deadline: the relay waits for it before starting
        e = Env(self)
        lockf = os.path.join(e.lockd, f"lock-{VMID}.conf")
        fd = os.open(lockf, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        fcntl.flock(fd, fcntl.LOCK_EX)
        q = e.qemu([{"guest": True, "reason": "guest-reset"}])
        r = e.relay(fallback=0.5)
        th = e.cleanup_sim(q, restart=False, consume=False)
        q.wait(10)
        time.sleep(2.0)
        self.assertEqual(e.qm_calls(), [], "started while the config lock was held")
        t_release = time.time()
        os.close(fd)
        out = finish(self, r)
        th.join(5)
        with open(e.qmlog) as f:
            calls = f.read().split()
        self.assertEqual(e.qm_calls(), [f"start {VMID}"], out)
        self.assertGreaterEqual(float(calls[0]), t_release - 0.05)
        self.assertIn("waited for the config lock", out)

    def test_restart_within_the_window_needs_no_fallback(self):
        e = Env(self)
        q = e.qemu([{"guest": True, "reason": "guest-reset"}])
        r = e.relay(fallback=2.0)
        th = e.cleanup_sim(q, delay=1.0)    # a slow cleanup, still inside the window
        out = finish(self, r)
        th.join(5)
        self.assertEqual(e.qm_calls(), [])
        self.assertIn("restarted: new QEMU pid", out)


class IdempotentRequest(unittest.TestCase):
    def test_existing_request_is_kept_as_one_empty_file(self):
        e = Env(self)
        open(e.request, "w").close()
        q = e.qemu([{"guest": True, "reason": "guest-reset"}])
        r = e.relay(fallback=3.0)
        seen = {}

        def watch():
            q.wait()
            seen["listing"] = sorted(os.listdir(e.run))
            seen["size"] = os.path.getsize(e.request) if os.path.exists(e.request) else None
        w = threading.Thread(target=watch, daemon=True); w.start()
        th = e.cleanup_sim(q, delay=0.3)
        out = finish(self, r)
        w.join(5); th.join(5)
        self.assertIn("(already there)", out)
        self.assertEqual(seen["size"], 0)
        self.assertEqual([x for x in seen["listing"] if x.endswith(".reboot")], [f"{VMID}.reboot"])
        self.assertEqual(e.qm_calls(), [])

    def test_only_the_first_shutdown_of_a_qemu_counts(self):
        e = Env(self)
        q = e.qemu([{"guest": True, "reason": "guest-reset"}, {"guest": False, "reason": "host-qmp-quit"}])
        r = e.relay(fallback=3.0)
        th = e.cleanup_sim(q)
        out = finish(self, r)
        th.join(5)
        self.assertEqual(out.count("SHUTDOWN reason="), 1, out)
        self.assertTrue(e.cleanup_saw)
        self.assertEqual(e.qm_calls(), [])

    def test_first_shutdown_not_a_restart_then_reset_is_ignored(self):
        e = Env(self)
        q = e.qemu([{"guest": True, "reason": "guest-shutdown"}, {"guest": True, "reason": "guest-reset"}])
        r = e.relay(fallback=1.0)
        th = e.cleanup_sim(q)
        out = finish(self, r)
        th.join(5)
        self.assertFalse(e.cleanup_saw)
        time.sleep(1.2)
        self.assertEqual(e.qm_calls(), [])


class PolicyLifetime(unittest.TestCase):
    def test_exits_when_policy_is_not_cold_and_no_qemu(self):
        e = Env(self)
        e.set_policy("reset")
        r = e.relay()
        out = finish(self, r, timeout=10)
        self.assertIn("reboot policy is not cold", out)

    def test_keeps_waiting_with_policy_cold(self):
        e = Env(self)
        r = e.relay()
        time.sleep(1.5)
        self.assertIsNone(r.poll(), "the relay must wait for the VM's next QEMU")
        e.set_policy("reset")
        out = finish(self, r, timeout=10)
        self.assertIn("reboot policy is not cold", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
