#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for `uniq-vgpu` (increment 3): verify / ABI-gate / dry-run / warm /
reboot / health-gate+auto-rollback / rollback / status.

Fully MOCKED: a fake qdevctl stands in for the increment-1 primitives, so this
runs OFF-VM with no qdev-host, no plugin, no QEMU. The end-to-end script
e2e-vt-warmswap.sh exercises the same logic against the real inc1 VT harness.

  python3 tests/uniq-vgpu/test_uniq_vgpu.py
"""
import json, os, subprocess, sys, tempfile, textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(os.path.join(HERE, "..", "..", "bin", "uniq-vgpu"))
MKBUNDLE = os.path.abspath(os.path.join(HERE, "..", "..", "bin", "uniq-vgpu-mkbundle"))

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name}  {detail}")


# A fake qdevctl: `device-abi` prints a controllable envelope; `device-swap`
# appends to a log and (optionally) fails. Behaviour is read from env so each
# test can steer it without rewriting the file.
FAKE_QDEVCTL = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    log = os.environ.get("FAKE_LOG", "/dev/null")
    env = os.environ.get("FAKE_ENVELOPE", "vtpv-envelope-1")
    if cmd == "device-abi":
        print(f"envelope={env} plugin=apple-videotoolbox version=0.3-warmswap qdp_abi=3")
        sys.exit(0)
    if cmd == "device-swap":
        backing = sys.argv[-1]
        if "--reset" in sys.argv:
            backing = sys.argv[-3]
        with open(log, "a") as f:
            f.write(backing + "\\n")
        if os.environ.get("FAKE_SWAP_FAIL") == "1":
            print("err swap: Input/output error", file=sys.stderr); sys.exit(1)
        print(f"backing swapped in process: swapped {backing}")
        sys.exit(0)
    print("unknown", file=sys.stderr); sys.exit(2)
""")


def run(args, env=None, extra_env=None):
    e = dict(os.environ)
    if extra_env:
        e.update(extra_env)
    p = subprocess.run([sys.executable, BIN] + args, capture_output=True, text=True, env=e)
    return p.returncode, p.stdout, p.stderr


def mkbundle(outdir, name, version, envelope, backing=None, kind="host-backend"):
    args = [MKBUNDLE, outdir, "--name", name, "--version", version,
            "--envelope", envelope, "--kind", kind]
    if backing:
        args += ["--backing", backing]
    p = subprocess.run([sys.executable] + args, capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    return outdir


def main():
    tmp = tempfile.mkdtemp(prefix="uniq-vgpu-test-")
    qdevctl = os.path.join(tmp, "fake-qdevctl")
    with open(qdevctl, "w") as f:
        f.write(FAKE_QDEVCTL)
    os.chmod(qdevctl, 0o755)

    # a dummy backing payload
    backing = os.path.join(tmp, "backing_B.so")
    with open(backing, "wb") as f:
        f.write(b"\x7fELF fake backing B payload")
    ctl = os.path.join(tmp, "ctl.sock")  # never connected; the fake ignores it

    base = ["--control", ctl, "--qdevctl", qdevctl]

    # ---- mkbundle + verify -------------------------------------------------
    print("[mkbundle + verify]")
    bwarm = mkbundle(os.path.join(tmp, "warm"), "vt-hevc", "0.3-warmswapB",
                     "vtpv-envelope-1", backing)
    check("manifest written", os.path.exists(os.path.join(bwarm, "uniq-vgpu-bundle.json")))
    check("SHA256SUMS written", os.path.exists(os.path.join(bwarm, "SHA256SUMS")))

    sd = os.path.join(tmp, "state-verify")
    rc, out, err = run(["update", bwarm, "--dry-run", "--state-dir", sd] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1"})
    check("verify OK in dry-run", rc == 0 and "verify: OK" in out, out + err)

    # tamper a file -> verify must fail
    with open(os.path.join(bwarm, "backing", "backing_B.so"), "ab") as f:
        f.write(b"tampered")
    rc, out, err = run(["update", bwarm, "--dry-run", "--state-dir", sd] + base)
    check("verify catches tamper", rc != 0 and "MISMATCH" in out, out + err)
    # restore
    mkbundle(bwarm, "vt-hevc", "0.3-warmswapB", "vtpv-envelope-1", backing)

    # ---- ABI-gate: warm vs reboot (dry-run) --------------------------------
    print("[ABI-gate / dry-run]")
    rc, out, err = run(["update", bwarm, "--dry-run", "--state-dir", sd] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1"})
    check("same envelope -> WARM", rc == 0 and "=> WARM" in out and "DRY-RUN" in out, out)

    breboot = mkbundle(os.path.join(tmp, "reboot"), "vt-next", "1.0-env2",
                       "vtpv-envelope-2", backing)
    rc, out, err = run(["update", breboot, "--dry-run", "--state-dir", sd] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1"})
    check("diff envelope -> REBOOT", rc == 0 and "REBOOT required" in out, out)

    # ---- WARM path: swap + health gate + commit ----------------------------
    print("[warm update: swap -> health -> commit]")
    swaplog = os.path.join(tmp, "swap.log")
    sd = os.path.join(tmp, "state-warm")
    hlog = os.path.join(tmp, "health-clean.log")
    open(hlog, "w").write("qdev-host: WARM-SWAP backing -> tag 'B'\nall good\n")
    rc, out, err = run(["update", bwarm, "--state-dir", sd, "--health-log", hlog] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": swaplog})
    check("warm update commits", rc == 0 and "COMMITTED" in out, out + err)
    check("health gate ran (QMU ERROR 0)", "QMU ERROR: 0  OK" in out, out)
    state = json.load(open(os.path.join(sd, "state.json")))
    check("state records active bundle", state["active"]["bundle"] == "vt-hevc", str(state))
    check("device-swap called once", open(swaplog).read().count("\n") == 1, open(swaplog).read())
    staged = state["active"]["backing"]
    check("backing staged atomically", os.path.exists(staged) and "backings" in staged, staged)

    # ---- health gate FAIL -> auto-rollback ---------------------------------
    print("[warm update: health FAIL -> auto-rollback]")
    swaplog2 = os.path.join(tmp, "swap2.log")
    sd2 = os.path.join(tmp, "state-fail")
    hbad = os.path.join(tmp, "health-bad.log")
    open(hbad, "w").write("QMU ERROR: fifo stall\nkernel panic - not syncing\n")
    rc, out, err = run(["update", bwarm, "--state-dir", sd2, "--health-log", hbad] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": swaplog2})
    check("health FAIL exit code 3", rc == 3, f"rc={rc}")
    check("auto-rollback happened", "AUTO-ROLLBACK" in out, out)
    swaps = open(swaplog2).read().strip().split("\n")
    check("swapped then rolled back (2 swaps)", len(swaps) == 2, str(swaps))
    check("rollback target is previous 'A'", swaps[1] == "A", str(swaps))
    check("no commit on health fail",
          not os.path.exists(os.path.join(sd2, "state.json")) or
          "active" not in json.load(open(os.path.join(sd2, "state.json"))), out)

    # ---- gpuRestart gate: +1 OK, +2 FAIL -----------------------------------
    print("[gpuRestart delta gate]")
    sd3 = os.path.join(tmp, "state-gpur")
    # health-cmd returns counters; simulate a +1 restart (baseline 0 -> post 1)
    hc_ok = f"{sys.executable} -c \"print('qmu_error=0 panics=0 gpu_hang=0 gpuRestart=1')\""
    # baseline read also returns 1... so use a stateful counter via a temp file
    cfile = os.path.join(tmp, "gpur_count")
    open(cfile, "w").write("0")
    hc = (f"{sys.executable} -c \""
          f"p='{cfile}';import io;"
          f"v=int(open(p).read());"
          f"print(f'qmu_error=0 panics=0 gpu_hang=0 gpuRestart={{v}}');"
          f"open(p,'w').write(str(v+1))\"")
    rc, out, err = run(["update", bwarm, "--state-dir", sd3, "--health-cmd", hc] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": os.path.join(tmp, "s3.log")})
    check("gpuRestart +1 passes", rc == 0 and "gpuRestart: 0->1 (+1)  OK" in out, out)

    open(cfile, "w").write("0")
    sd4 = os.path.join(tmp, "state-gpur2")
    hc2 = (f"{sys.executable} -c \""
           f"p='{cfile}';v=int(open(p).read());"
           f"print(f'qmu_error=0 panics=0 gpu_hang=0 gpuRestart={{v}}');"
           f"open(p,'w').write(str(v+3))\"")  # baseline 0 then post 3 -> +3 FAIL
    rc, out, err = run(["update", bwarm, "--state-dir", sd4, "--health-cmd", hc2] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": os.path.join(tmp, "s4.log")})
    check("gpuRestart +3 fails -> rollback", rc == 3 and "AUTO-ROLLBACK" in out, out)

    # ---- explicit rollback command -----------------------------------------
    print("[rollback command]")
    # first commit a warm update to establish active+previous
    sd5 = os.path.join(tmp, "state-rb")
    run(["update", bwarm, "--state-dir", sd5, "--health-log", hlog] + base,
        extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": os.path.join(tmp, "s5.log")})
    rblog = os.path.join(tmp, "rb.log")
    rc, out, err = run(["rollback", "--state-dir", sd5, "--health-log", hlog] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": rblog})
    check("rollback command OK", rc == 0 and "rollback complete" in out, out + err)
    check("rollback issued a device-swap", os.path.exists(rblog) and open(rblog).read().strip(), out)

    # ---- update --rollback (alias) -----------------------------------------
    rc, out, err = run(["update", "--rollback", "--state-dir", sd5, "--health-log", hlog] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": os.path.join(tmp, "s6.log")})
    check("update --rollback alias OK", rc == 0, out + err)

    # ---- status ------------------------------------------------------------
    print("[status]")
    iofile = os.path.join(tmp, "ioreg.txt")
    open(iofile, "w").write(
        'AGVAEnvelopeVersion="vtpv-envelope-1"\n'
        'AGVAGuestDriverVersion="dev.applevtpv.driver 1.0-warmswap"\n'
        'AGVAHandshakeGeneration=1\n')
    rc, out, err = run(["status", "--state-dir", sd, "--ioreg-file", iofile] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1"})
    check("status shows resident envelope", rc == 0 and "envelope=vtpv-envelope-1" in out, out)
    check("status shows ioreg triplet", "AGVAEnvelopeVersion" in out and "HandshakeGeneration" in out, out)

    # ---- reboot path writes a pending marker + exit 20 ---------------------
    print("[reboot path state]")
    sd6 = os.path.join(tmp, "state-reboot")
    rc, out, err = run(["update", breboot, "--state-dir", sd6] + base,
                       extra_env={"FAKE_ENVELOPE": "vtpv-envelope-1", "FAKE_LOG": os.path.join(tmp, "s7.log")})
    check("reboot path exit code 20", rc == 20, f"rc={rc}")
    check("reboot path did NOT swap", not os.path.exists(os.path.join(tmp, "s7.log")), out)
    st6 = json.load(open(os.path.join(sd6, "state.json")))
    check("pending_reboot recorded", "pending_reboot" in st6, str(st6))

    print()
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
