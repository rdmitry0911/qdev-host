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


def mkbundle(outdir, name, version, envelope, backing=None, kind="host-backend",
             kext=None, kext_uuid=None, kext_sha=None, kext_builds=None):
    args = [MKBUNDLE, outdir, "--name", name, "--version", version,
            "--envelope", envelope, "--kind", kind]
    if backing:
        args += ["--backing", backing]
    if kext:
        args += ["--kext", kext]
        if kext_uuid:
            args += ["--kext-uuid", kext_uuid]
        if kext_sha:
            args += ["--kext-sha", kext_sha]
        for b in (kext_builds or []):
            args += ["--kext-build", b]
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

    # ---- apply: host warm + guest kext orchestration -----------------------
    print("[apply: warm host + guest kext (no channel -> print one-liner)]")
    # a fake kext payload with a known uuid
    fakekext = os.path.join(tmp, "UniqVGPU.kext")
    os.makedirs(os.path.join(fakekext, "Contents", "MacOS"), exist_ok=True)
    open(os.path.join(fakekext, "Contents", "MacOS", "UniqVGPU"), "wb").write(b"\x7fELF fake kext")
    open(os.path.join(fakekext, "Contents", "Info.plist"), "w").write(
        "<plist><dict><key>CFBundleIdentifier</key><string>dev.uniqvgpu.driver</string></dict></plist>")
    KUUID = "AACD3BB1-145E-3246-81CD-C70B581D32E5"
    bk = mkbundle(os.path.join(tmp, "apply-warm"), "pvg-warm", "0.2-warmswapB",
                  "pvg-envelope-1", backing, kext=fakekext, kext_uuid=KUUID,
                  kext_builds=["25G342", "25C56"])
    check("mkbundle embeds kext block",
          json.load(open(os.path.join(bk, "uniq-vgpu-bundle.json"))).get("kext", {}).get("uuid") == KUUID)

    sda = os.path.join(tmp, "state-apply")
    hlog2 = os.path.join(tmp, "apply-health.log")
    open(hlog2, "w").write("qdev-host: WARM-SWAP backing -> tag 'B'\n")
    rc, out, err = run(["apply", bk, "--state-dir", sda, "--health-log", hlog2] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "a1.log")})
    check("apply warm host commits", "COMMITTED" in out, out + err)
    check("apply exit 21 (guest kext install required)", rc == 21, f"rc={rc}\n{out}")
    check("apply prints guest one-liner", "sudo uniq-vgpu-guest-install UniqVGPU.kext" in out, out)
    check("apply verdict: WARM-UPDATED + guest required",
          "WARM-UPDATED (host, no reboot). GUEST KEXT INSTALL REQUIRED" in out, out)

    print("[apply: guest kext already current (via --guest-kextstat-file) -> no guest action]")
    ksfile = os.path.join(tmp, "guest-kextstat.txt")
    open(ksfile, "w").write(
        f"dev.uniqvgpu.driver\t0.1.0\t/Library/Extensions/UniqVGPU.kext\t({KUUID})\n")
    sda2 = os.path.join(tmp, "state-apply2")
    rc, out, err = run(["apply", bk, "--state-dir", sda2, "--health-log", hlog2,
                        "--guest-kextstat-file", ksfile] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "a2.log")})
    check("apply warm + guest current -> exit 0", rc == 0, f"rc={rc}\n{out}")
    check("apply says no guest action / WARM-UPDATED no reboot",
          "already current" in out and "WARM-UPDATED, no reboot" in out, out)

    print("[apply --dry-run: plan shows warm + guest-needed]")
    rc, out, err = run(["apply", bk, "--dry-run", "--state-dir", os.path.join(tmp, "s-ad")] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1"})
    check("apply dry-run exit 0 + WARM plan", rc == 0 and "host: WARM (no reboot)" in out, out)
    check("apply dry-run flags guest install required",
          "guest kext: install required" in out and "guest command: sudo uniq-vgpu-guest-install" in out, out)

    print("[apply: envelope changed -> REBOOT required + guest command, NO swap]")
    bkr = mkbundle(os.path.join(tmp, "apply-reboot"), "pvg-next", "1.0-env2",
                   "pvg-envelope-2", backing, kext=fakekext, kext_uuid=KUUID)
    sdr = os.path.join(tmp, "state-apply-reboot")
    alog = os.path.join(tmp, "a3.log")
    rc, out, err = run(["apply", bkr, "--state-dir", sdr, "--health-log", hlog2] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": alog})
    check("apply reboot path exit 20", rc == 20, f"rc={rc}\n{out}")
    check("apply reboot path did NOT swap", not os.path.exists(alog), out)
    check("apply reboot emits guest kext command + reboot note",
          "REBOOT REQUIRED" in out and "Install the guest kext then reboot" in out, out)

    print("[apply --rollback: host symmetric swap + guest rollback note]")
    rc, out, err = run(["apply", "--rollback", "--state-dir", sda, "--health-log", hlog2] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "a4.log")})
    check("apply --rollback exit 0", rc == 0, f"rc={rc}\n{out}")
    check("apply --rollback mentions guest-install --rollback",
          "uniq-vgpu-guest-install --rollback" in out, out)

    print("[apply: host-only bundle (no kext) -> pure warm, no guest]")
    bho = mkbundle(os.path.join(tmp, "apply-hostonly"), "pvg-hostonly", "0.2-B",
                   "pvg-envelope-1", backing)
    rc, out, err = run(["apply", bho, "--state-dir", os.path.join(tmp, "s-ho"),
                        "--health-log", hlog2] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "a5.log")})
    check("apply host-only exit 0 + no guest", rc == 0 and "ships NO kext" in out, out)

    # ---- backend-process reload seam (real renderer libmlapi/QMetal/air) -----
    print("[backend_reload=backend-process: device-swap updates identity, renderer reload PENDING]")
    bbp = mkbundle(os.path.join(tmp, "backend-proc"), "pvg-renderer", "0.3-mlapiX",
                   "pvg-envelope-1", backing)
    # flip the manifest to backend-process + re-sum
    mpath = os.path.join(bbp, "uniq-vgpu-bundle.json")
    mani = json.load(open(mpath)); mani["backend_reload"] = "backend-process"
    json.dump(mani, open(mpath, "w"), indent=2)
    subprocess.run([sys.executable, MKBUNDLE, bbp, "--name", "pvg-renderer",
                    "--version", "0.3-mlapiX", "--envelope", "pvg-envelope-1",
                    "--backing", backing, "--backend-reload", "backend-process"],
                   capture_output=True, text=True)
    sdbp = os.path.join(tmp, "state-bp")
    rc, out, err = run(["update", bbp, "--state-dir", sdbp, "--health-log", hlog] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "bp.log")})
    check("backend-process without hook -> exit 22 (pending)", rc == 22, f"rc={rc}\n{out}")
    check("backend-process prints REAL-BACKEND RELOAD PENDING", "REAL-BACKEND RELOAD PENDING" in out, out)
    check("backend-process still device-swapped the identity (1 swap)",
          open(os.path.join(tmp, "bp.log")).read().count("\n") == 1, out)
    # status shows the pending-backend marker
    rc, out, err = run(["status", "--state-dir", sdbp] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1"})
    check("status shows PENDING BACKEND RELOAD", "PENDING BACKEND RELOAD" in out, out)

    print("[backend-process WITH --backend-reload-cmd: renderer reloaded -> commit]")
    sdbp2 = os.path.join(tmp, "state-bp2")
    reload_cmd = f"{sys.executable} -c \"print('backend-process restarted + vfio reconnect')\""
    rc, out, err = run(["update", bbp, "--state-dir", sdbp2, "--health-log", hlog,
                        "--backend-reload-cmd", reload_cmd] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "bp2.log")})
    check("backend-process with hook -> exit 0 COMMITTED", rc == 0 and "COMMITTED" in out, f"rc={rc}\n{out}")
    check("backend-process ran the reload hook", "backend-process reload hook" in out, out)

    print("[backend-process reload hook FAILS -> rollback]")
    sdbp3 = os.path.join(tmp, "state-bp3")
    bad_reload = f"{sys.executable} -c \"import sys; sys.exit(1)\""
    rc, out, err = run(["update", bbp, "--state-dir", sdbp3, "--health-log", hlog,
                        "--backend-reload-cmd", bad_reload] + base,
                       extra_env={"FAKE_ENVELOPE": "pvg-envelope-1", "FAKE_LOG": os.path.join(tmp, "bp3.log")})
    check("backend-process reload fail -> exit 3 + rollback", rc == 3 and "rolled back identity" in out, f"rc={rc}\n{out}")

    print()
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
