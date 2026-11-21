#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Off-guest tests: the guest GL bundle (UniqVGPUGL.bundle) and the static desktop picture
step (manifest "wallpaper" block) in uniq-vgpu-mkbundle and uniq-vgpu-guest-install.

  python3 tests/uniq-vgpu/test_guest_gl_bundle.py

mkbundle runs as is. uniq-vgpu-guest-install runs as is through its test-root seam
(UNIQVGPU_GUEST_TEST_ROOT): every macOS tool it calls by absolute path resolves to a
DOUBLE under a per-test root. The doubles (kmutil, ditto, PlistBuddy, shasum,
dwarfdump, sw_vers, csrutil, chown, reboot, `id`, install-gpu-bundle.sh and the
wallpaper tool uniqvgpu-wallpaper.sh) are
scoped to this file: they model only the call/output contract the installer relies
on and are NOT evidence for the real macOS tools, the AuxKC, or the real GL bundle
layout; the live guest install is the gate for those.

Overrides (fail-before runs): UNIQVGPU_BIN_DIR = the bin/ under test;
UNIQVGPU_FIXTURE_MKBUNDLE = the mkbundle that builds the guest-install fixtures
(default: the one under test). UNIQVGPU_REF_BIN_DIR = an older bin/ whose mkbundle
output for a no-gl bundle must be byte-identical (no-gl path unchanged).
"""
import hashlib, io, json, os, shutil, stat, subprocess, sys, tarfile, tempfile, textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
BIN_DIR = os.environ.get("UNIQVGPU_BIN_DIR") or os.path.abspath(os.path.join(HERE, "..", "..", "bin"))
MKBUNDLE = os.path.join(BIN_DIR, "uniq-vgpu-mkbundle")
INSTALL = os.path.join(BIN_DIR, "uniq-vgpu-guest-install")
FIX_MKBUNDLE = os.environ.get("UNIQVGPU_FIXTURE_MKBUNDLE") or MKBUNDLE
REF_BIN = os.environ.get("UNIQVGPU_REF_BIN_DIR")

PASS = FAIL = 0
KUUID = "429FA8C4-7FDA-3357-9681-A276EFA2B2C6"


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name}  {str(detail)[-1500:]}")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(p):
    return sha(open(p, "rb").read())


def write(p, data, mode=None):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)
    if mode:
        os.chmod(p, mode)


# ------------------------------------------------------------------ fixtures
def make_kext(d):
    k = os.path.join(d, "UniqVGPU.kext")
    write(os.path.join(k, "Contents", "Info.plist"),
          "<plist><dict>\n<key>CFBundleIdentifier</key><string>dev.uniqvgpu.driver</string>\n"
          "<key>CFBundleExecutable</key><string>UniqVGPU</string>\n</dict></plist>\n")
    write(os.path.join(k, "Contents", "MacOS", "UniqVGPU"), b"\xcf\xfa\xed\xfe fake kext", 0o755)
    tarp = os.path.join(d, "UniqVGPU.kext.tar")
    with tarfile.open(tarp, "w") as t:
        t.add(k, arcname="UniqVGPU.kext")
    return k, tarp, sha_file(os.path.join(k, "Contents", "MacOS", "UniqVGPU"))


def make_gl(d, payload=b"\xcf\xfa\xed\xfe fake UniqVGPUGL proxy"):
    g = os.path.join(d, "UniqVGPUGL.bundle")
    write(os.path.join(g, "Contents", "Info.plist"), "<plist><dict/></plist>\n")
    write(os.path.join(g, "Contents", "_CodeSignature", "CodeResources"), "<plist/>\n")
    write(os.path.join(g, "Contents", "MacOS", "UniqVGPUGL"), payload, 0o755)
    return g, sha(payload)


# DOUBLE of apple-virgl tools/guest/install-gpu-bundle.sh: its argument/output contract
# (install <Name> <src> <sha> -> GPU_BUNDLE_OK <Name> <sha>; retire <Name> ->
# GPU_BUNDLE_RETIRED <Name>; option-A layout), rooted at UNIQVGPU_GUEST_TEST_ROOT.
FAKE_GL_INSTALLER = textwrap.dedent("""\
    #!/bin/bash
    # TEST DOUBLE of install-gpu-bundle.sh (tests/uniq-vgpu/test_guest_gl_bundle.py only)
    set -eu
    R="$UNIQVGPU_GUEST_TEST_ROOT"; MODE=$1; NAME=$2
    echo "glinst $*" >> "$FAKE_CALLS"
    REAL=$R/Library/Apple/System/Library/PrivateFrameworks/$NAME.bundle
    LINK=$R/Library/GPUBundles/$NAME.bundle
    case $MODE in
    install)
        [ "${FAKE_GL_FAIL:-0}" = 1 ] && { echo "sha256 mismatch (double)" >&2; exit 1; }
        mkdir -p "$(dirname "$REAL")" "$(dirname "$LINK")"
        rm -rf "$REAL"; cp -a "$3" "$REAL"; rm -rf "$LINK"; ln -s "$REAL" "$LINK"
        echo "GPU_BUNDLE_OK $NAME $(sha256sum "$REAL/Contents/MacOS/$NAME" | cut -d' ' -f1)" ;;
    retire)
        mkdir -p "$R/var/db/uniqvgpu-retired"
        for p in "$LINK" "$REAL"; do
            if [ -L "$p" ]; then rm -f "$p"
            elif [ -e "$p" ]; then rm -rf "$R/var/db/uniqvgpu-retired/$NAME.bundle"; mv "$p" "$R/var/db/uniqvgpu-retired/$NAME.bundle"; fi
        done
        echo "GPU_BUNDLE_RETIRED $NAME" ;;
    *) exit 2 ;;
    esac
""")

# DOUBLE of apple-virgl tools/guest/uniqvgpu-wallpaper.sh: its argument/output contract
# (apply PICTURE -> copies itself to the support dir, WALLPAPER_APPLY_OK; revert ->
# WALLPAPER_REVERT_OK, support dir removed), rooted at UNIQVGPU_GUEST_TEST_ROOT. The real
# tool has its own test (apple-virgl tools/tests/run_uniqvgpu_wallpaper_tests.sh).
FAKE_WP_TOOL = textwrap.dedent("""\
    #!/bin/bash
    # TEST DOUBLE of uniqvgpu-wallpaper.sh (tests/uniq-vgpu/test_guest_gl_bundle.py only)
    R="$UNIQVGPU_GUEST_TEST_ROOT"; SUP="$R/Library/Application Support/UniqVGPU/wallpaper"
    echo "wp $*" >> "$FAKE_CALLS"
    case $1 in
    apply)
        mkdir -p "$SUP"; cp "$0" "$SUP/uniqvgpu-wallpaper.sh"
        [ "${FAKE_WP_FAIL:-0}" = 1 ] && { echo "WALLPAPER_USER devops FAIL WALLPAPER_FAIL double"; echo "WALLPAPER_APPLY_FAIL picture=$2"; exit 1; }
        echo "WALLPAPER_USER devops live WALLPAPER_SET_OK 0=$2"; echo "WALLPAPER_APPLY_OK picture=$2" ;;
    revert) rm -rf "$SUP"; echo "WALLPAPER_REVERT_OK" ;;
    *) exit 2 ;;
    esac
""")
FAKE_WP_SETTER = "// TEST DOUBLE of uniqvgpu-wallpaper-set.js\n"
PIC = "/System/Library/Desktop Pictures/iMac Blue.heic"

# macOS tool doubles (paths relative to the test root)
DOUBLES = {
    "usr/bin/kmutil": """#!/bin/sh
echo "kmutil $1" >> "$FAKE_CALLS"
cmd=$1; shift
aux=""; kexts=""
while [ $# -gt 0 ]; do case "$1" in -A) aux=$2; shift ;; -p) kexts="$kexts $2"; shift ;; esac; shift; done
case "$cmd" in
  create)  mkdir -p "$(dirname "$aux")"; : > "$aux"
           for k in $kexts; do echo "dev.uniqvgpu.driver $(basename "$k") (FAKE)" >> "$aux"; done ;;
  inspect) cat "$aux" 2>/dev/null ;;
esac
exit 0
""",
    "usr/bin/ditto": """#!/bin/sh
mkdir -p "$(dirname "$2")"
if [ -d "$2" ]; then cp -a "$1"/. "$2"/; else cp -a "$1" "$2"; fi
""",
    "usr/bin/shasum": """#!/bin/sh
shift 2; sha256sum "$@"
""",
    "usr/bin/dwarfdump": """#!/bin/sh
echo "UUID: ${FAKE_UUID:-429FA8C4-7FDA-3357-9681-A276EFA2B2C6} (x86_64) $2"
""",
    "usr/bin/sw_vers": """#!/bin/sh
case "$1" in -buildVersion) echo 25C56 ;; *) echo 26.2 ;; esac
""",
    "usr/bin/csrutil": """#!/bin/sh
echo "System Integrity Protection status: disabled."
""",
    "usr/libexec/PlistBuddy": """#!/bin/sh
key=${2#Print :}
sed -n "s:.*<key>$key</key><string>\\([^<]*\\)</string>.*:\\1:p" "$3"
""",
    "usr/sbin/chown": "#!/bin/sh\nexit 0\n",
    "sbin/reboot": "#!/bin/sh\necho REBOOT >> \"$FAKE_CALLS\"\n",
    "pathbin/id": "#!/bin/sh\necho 0\n",
}
REAL_LINKS = {"usr/bin/mktemp": "mktemp", "usr/bin/tar": "tar", "usr/bin/find": "find",
              "bin/cp": "cp", "bin/mv": "mv", "bin/bash": "bash"}
# a PATH without python3 (for the fail-closed gl-block read)
MIN_PATH_TOOLS = ["awk", "sed", "grep", "head", "mkdir", "rm", "ln", "readlink", "basename", "dirname",
                  "date", "cat", "tr", "ls", "sha256sum", "cut", "cp", "mv", "sh", "bash", "rmdir"]


def make_root(tmp, tag):
    r = os.path.join(tmp, "root-" + tag)
    for rel, body in DOUBLES.items():
        write(os.path.join(r, rel), body, 0o755)
    for rel, tool in REAL_LINKS.items():
        os.makedirs(os.path.dirname(os.path.join(r, rel)), exist_ok=True)
        os.symlink(shutil.which(tool), os.path.join(r, rel))
    for d in ("Library/Extensions", "Library/KernelCollections", "tmp", "var/db"):
        os.makedirs(os.path.join(r, d), exist_ok=True)
    for kc in ("BootKernelExtensions.kc", "SystemKernelExtensions.kc"):
        write(os.path.join(r, "System/Library/KernelCollections", kc), "fake kc\n")
    mp = os.path.join(r, "minpath")
    os.makedirs(mp)
    os.symlink(os.path.join(r, "pathbin", "id"), os.path.join(mp, "id"))
    for t in MIN_PATH_TOOLS:
        os.symlink(shutil.which(t), os.path.join(mp, t))
    return r


def run_install(root, args, no_python=False, extra_env=None):
    calls = os.path.join(root, "calls.log")
    env = dict(os.environ)
    env.update({"UNIQVGPU_GUEST_TEST_ROOT": root, "FAKE_CALLS": calls,
                "PATH": (os.path.join(root, "minpath") if no_python
                         else os.path.join(root, "pathbin") + os.pathsep + os.environ["PATH"])})
    env.update(extra_env or {})
    p = subprocess.run(["/bin/bash", INSTALL] + args, capture_output=True, text=True, env=env)
    log = open(calls).read() if os.path.exists(calls) else ""
    return p.returncode, p.stdout + p.stderr, log


def mkb(tool, outdir, *args):
    p = subprocess.run([sys.executable, tool, outdir] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def sums(d):
    out = {}
    for line in open(os.path.join(d, "SHA256SUMS")):
        h, _, rel = line.rstrip("\n").partition("  ")
        out[rel] = h
    return out


def files_of(d):
    res = set()
    for root, _, fs in os.walk(d):
        for f in fs:
            if f != "SHA256SUMS":
                res.add("./" + os.path.relpath(os.path.join(root, f), d))
    return res


def main():
    tmp = tempfile.mkdtemp(prefix="uniq-vgpu-gl-test-")
    src = os.path.join(tmp, "src")
    os.makedirs(src)
    kext, ktar, ksha = make_kext(src)
    gl, glsha = make_gl(src)
    inst = os.path.join(src, "install-gpu-bundle.sh")
    write(inst, FAKE_GL_INSTALLER, 0o755)
    backing = os.path.join(src, "backing_B.so")
    write(backing, b"\x7fELF fake backing")
    base = ["--name", "t", "--version", "1", "--envelope", "pvg-envelope-1"]
    GLARGS = ["--gl-bundle", gl, "--gl-sha", glsha, "--gl-installer", inst]

    # ================================================================ mkbundle
    print("[mkbundle: no --gl-bundle -> no gl block, SHA256SUMS over every file]")
    b0 = os.path.join(tmp, "b-nogl")
    rc, out = mkb(MKBUNDLE, b0, *base, "--backing", backing, "--kext", ktar,
                  "--kext-sha", ksha, "--kext-uuid", KUUID, "--kext-build", "25C56")
    m0 = json.load(open(os.path.join(b0, "uniq-vgpu-bundle.json"))) if rc == 0 else {}
    check("no-gl mkbundle rc 0", rc == 0, out)
    check("no-gl manifest keys unchanged",
          list(m0) == ["schema", "name", "version", "envelope", "kind", "backend_reload", "backing", "kext"], m0)
    check("no-gl SHA256SUMS covers exactly the bundle files",
          rc == 0 and set(sums(b0)) == files_of(b0)
          and all(sha_file(os.path.join(b0, r)) == h for r, h in sums(b0).items()), out)
    if REF_BIN:
        bref = os.path.join(tmp, "b-nogl-ref")
        rrc, rout = mkb(os.path.join(REF_BIN, "uniq-vgpu-mkbundle"), bref, *base, "--backing", backing,
                        "--kext", ktar, "--kext-sha", ksha, "--kext-uuid", KUUID, "--kext-build", "25C56")
        same = rrc == 0 and files_of(bref) | {"./SHA256SUMS"} == files_of(b0) | {"./SHA256SUMS"} and all(
            open(os.path.join(bref, r), "rb").read() == open(os.path.join(b0, r), "rb").read()
            for r in files_of(b0) | {"./SHA256SUMS"})
        check("no-gl bundle byte-identical to the reference (pre-change) mkbundle", same, rout)

    print("[mkbundle: --gl-bundle DIR -> gl block + gl/ payload in SHA256SUMS]")
    b1 = os.path.join(tmp, "b-gl")
    rc, out = mkb(MKBUNDLE, b1, *base, "--kext", ktar, "--kext-sha", ksha, *GLARGS, "--gl-origin", "test origin")
    m1 = json.load(open(os.path.join(b1, "uniq-vgpu-bundle.json"))) if rc == 0 else {}
    check("gl mkbundle rc 0", rc == 0, out)
    check("gl manifest block exact", m1.get("gl") == {
        "name": "UniqVGPUGL", "bundle": "gl/UniqVGPUGL.bundle.tar",
        "installer": "gl/install-gpu-bundle.sh", "binary_sha256": glsha, "origin": "test origin"}, m1)
    s1 = sums(b1) if rc == 0 else {}
    check("gl tar + installer in SHA256SUMS with correct sums",
          rc == 0 and set(s1) == files_of(b1)
          and {"./gl/UniqVGPUGL.bundle.tar", "./gl/install-gpu-bundle.sh"} <= set(s1)
          and all(sha_file(os.path.join(b1, r)) == h for r, h in s1.items()), s1)
    ok_tar = False
    if rc == 0:
        with tarfile.open(os.path.join(b1, "gl", "UniqVGPUGL.bundle.tar")) as t:
            mem = t.getmember("UniqVGPUGL.bundle/Contents/MacOS/UniqVGPUGL")
            ok_tar = (sha(t.extractfile(mem).read()) == glsha and mem.mode & 0o111
                      and all(m.uid == 0 and m.gid == 0 for m in t.getmembers()))
    check("gl tar holds UniqVGPUGL.bundle/Contents/MacOS/UniqVGPUGL (sha, exec, root:wheel)", ok_tar)
    check("gl installer copied verbatim + executable",
          rc == 0 and open(os.path.join(b1, "gl", "install-gpu-bundle.sh")).read() == FAKE_GL_INSTALLER
          and os.stat(os.path.join(b1, "gl", "install-gpu-bundle.sh")).st_mode & 0o111)
    b1b = os.path.join(tmp, "b-gl-again")
    mkb(MKBUNDLE, b1b, *base, "--kext", ktar, *GLARGS)
    check("gl tar deterministic across runs", rc == 0 and os.path.exists(os.path.join(b1b, "gl", "UniqVGPUGL.bundle.tar"))
          and sha_file(os.path.join(b1, "gl", "UniqVGPUGL.bundle.tar"))
          == sha_file(os.path.join(b1b, "gl", "UniqVGPUGL.bundle.tar")))

    print("[mkbundle: --gl-bundle TAR, gates]")
    b2 = os.path.join(tmp, "b-gltar")
    rc, out = mkb(MKBUNDLE, b2, *base, "--gl-bundle", os.path.join(b1, "gl", "UniqVGPUGL.bundle.tar"),
                  "--gl-sha", glsha, "--gl-installer", inst)
    check("gl from .tar: rc 0, same gl block, tar copied verbatim",
          rc == 0 and json.load(open(os.path.join(b2, "uniq-vgpu-bundle.json"))).get("gl", {}).get("binary_sha256") == glsha
          and sha_file(os.path.join(b2, "gl", "UniqVGPUGL.bundle.tar")) == sha_file(os.path.join(b1, "gl", "UniqVGPUGL.bundle.tar")), out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-badsha"), *base, "--gl-bundle", gl, "--gl-sha", "0" * 64,
                  "--gl-installer", inst)
    check("wrong --gl-sha refused", rc != 0 and "mismatch" in out
          and not os.path.exists(os.path.join(tmp, "b-badsha", "uniq-vgpu-bundle.json")), out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-nosha"), *base, "--gl-bundle", gl, "--gl-installer", inst)
    check("--gl-bundle without --gl-sha refused", rc != 0, out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-noinst"), *base, "--gl-bundle", gl, "--gl-sha", glsha)
    check("--gl-bundle without --gl-installer refused", rc != 0, out)

    print("[mkbundle: guest-only bundle, kext block carried verbatim from an older manifest]")
    oldm = os.path.join(src, "old-manifest.json")
    oldk = {"id": "dev.uniqvgpu.driver", "bundle": "UniqVGPU.kext.tar", "binary_sha256": ksha, "uuid": KUUID,
            "validated_builds": ["25C56", "25E253"], "origin": "o", "validation": {"semantics": "s", "25C56": "v"}}
    json.dump({"schema": "uniq-vgpu-bundle/1", "name": "old", "version": "0", "kext": oldk}, open(oldm, "w"))
    b3 = os.path.join(tmp, "b-guestonly")
    rc, out = mkb(MKBUNDLE, b3, "--name", "new", "--version", "1+gl", "--kext", ktar, "--kext-manifest", oldm, *GLARGS)
    m3 = json.load(open(os.path.join(b3, "uniq-vgpu-bundle.json"))) if rc == 0 else {}
    want_k = dict(oldk, bundle="kext/UniqVGPU.kext.tar")
    check("guest-only: keys schema,name,version,kext,gl (no envelope/kind)",
          list(m3) == ["schema", "name", "version", "kext", "gl"], m3 or out)
    check("guest-only: kext block verbatim (order + values; bundle path re-derived)",
          list(m3.get("kext", {}).items()) == list(want_k.items()), m3.get("kext"))
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-conflict"), "--name", "n", "--version", "1", "--kext", ktar,
                  "--kext-manifest", oldm, "--kext-sha", "f" * 64)
    check("--kext-manifest conflicting --kext-sha refused", rc != 0 and "conflicts" in out, out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-noenv"), "--name", "n", "--version", "1", "--backing", backing)
    check("--envelope still required with --backing", rc != 0, out)

    # ====================================================== guest-install fixtures
    print("[guest-install fixtures (built with the fixture mkbundle)]")
    fx_nogl = os.path.join(tmp, "fx-nogl")
    fx_gl = os.path.join(tmp, "fx-gl")
    r1, o1 = mkb(FIX_MKBUNDLE, fx_nogl, "--name", "n", "--version", "1", "--envelope", "e", "--kext", ktar,
                 "--kext-sha", ksha, "--kext-uuid", KUUID, "--kext-build", "25C56")
    r2, o2 = mkb(FIX_MKBUNDLE, fx_gl, "--name", "n", "--version", "1", "--envelope", "e", "--kext", ktar,
                 "--kext-sha", ksha, "--kext-uuid", KUUID, "--kext-build", "25C56", *GLARGS)
    check("fixtures built", r1 == 0 and r2 == 0, o1 + o2)

    def variant(tag, mutate):
        d = os.path.join(tmp, "fx-" + tag)
        shutil.copytree(fx_gl, d, symlinks=True)
        mutate(d)
        return d

    def set_gl_sha(d, v):
        mp = os.path.join(d, "uniq-vgpu-bundle.json")
        m = json.load(open(mp))
        m["gl"]["binary_sha256"] = v
        json.dump(m, open(mp, "w"), indent=2)

    fx_badsha = variant("badsha", lambda d: set_gl_sha(d, "e" * 64))

    def swap_tar(d):  # a different GL exe in the tar, SHA256SUMS left as built
        g2, _ = make_gl(os.path.join(d, "..", "other-gl-" + os.path.basename(d)), b"tampered")
        with tarfile.open(os.path.join(d, "gl", "UniqVGPUGL.bundle.tar"), "w") as t:
            t.add(g2, arcname="UniqVGPUGL.bundle")
    fx_badtar = variant("badtar", swap_tar)
    fx_noinst = variant("noinst", lambda d: os.remove(os.path.join(d, "gl", "install-gpu-bundle.sh")))

    # ================================================================ dry-run
    print("[guest-install --dry-run]")
    root = make_root(tmp, "dry")
    rc, out, calls = run_install(root, [fx_nogl, "--dry-run"])
    check("no-gl dry-run passes, no GL step", rc == 0 and "DRY-RUN" in out and "GL" not in out and "glinst" not in calls, out)
    rc, out, calls = run_install(root, [fx_gl, "--dry-run"])
    check("gl dry-run passes the GL STOP-IF checks",
          rc == 0 and f"OK  GL bundle UniqVGPUGL sha256 {glsha}" in out
          and "GL tar + installer match SHA256SUMS" in out, out)
    check("gl dry-run makes no change (no installer call, no GPUBundles)",
          "glinst" not in calls and not os.path.lexists(os.path.join(root, "Library/GPUBundles")), calls)
    rc, out, _ = run_install(root, [fx_badsha, "--dry-run"])
    check("gl dry-run: manifest sha != tar exe sha -> STOP-IF (rc 2)",
          rc == 2 and "STOP-IF: GL binary sha256" in out, out)
    rc, out, _ = run_install(root, [fx_badtar, "--dry-run"])
    check("gl dry-run: tar not matching SHA256SUMS -> STOP-IF (rc 2)",
          rc == 2 and "STOP-IF: gl/UniqVGPUGL.bundle.tar sha256" in out, out)
    rc, out, _ = run_install(root, [fx_noinst, "--dry-run"])
    check("gl dry-run: installer missing -> STOP-IF (rc 2)", rc == 2 and "STOP-IF: GL installer missing" in out, out)
    rc, out, _ = run_install(root, [fx_badsha, "--dry-run", "--no-gl"])
    check("--no-gl skips the GL gate (kext only)", rc == 0 and "GL step SKIPPED" in out, out)
    # without python3 the (pre-existing) kext grep fallback cannot read the validated_builds
    # list, so STOP-IF 4 is bypassed with --force-build to reach the GL gate
    rc, out, _ = run_install(root, [fx_gl, "--dry-run", "--force-build"], no_python=True)
    check("gl block unreadable without python3 -> STOP-IF, not a silent skip",
          rc == 2 and "STOP-IF: manifest gl block lacks" in out, out)
    rc, out, _ = run_install(root, [fx_nogl, "--dry-run", "--force-build"], no_python=True)
    check("no-gl dry-run without python3 still passes (kext grep fallback)", rc == 0 and "DRY-RUN" in out, out)

    # ================================================================ install / rollback
    GLREAL = "Library/Apple/System/Library/PrivateFrameworks/UniqVGPUGL.bundle"
    GLLINK = "Library/GPUBundles/UniqVGPUGL.bundle"
    state_rel = "var/db/uniqvgpu-kext-backups/rollback-state.sh"

    print("[guest-install: gl bundle, fresh guest -> kext, AuxKC, then GL; --rollback removes GL]")
    root = make_root(tmp, "fresh")
    rc, out, calls = run_install(root, [fx_gl, "--no-reboot", "--yes"])
    seq = [l.split()[0] + " " + l.split()[1] for l in calls.splitlines()]
    check("install rc 0", rc == 0, out)
    check("GL installer called with install UniqVGPUGL <extracted> <sha>",
          any(l.startswith("glinst install UniqVGPUGL ") and l.endswith(" " + glsha)
              and "/gl/UniqVGPUGL.bundle " in l for l in calls.splitlines()), calls)
    check("GL step runs AFTER kmutil create + inspect",
          "glinst install" in seq and "kmutil create" in seq and "kmutil inspect" in seq
          and seq.index("glinst install") > seq.index("kmutil create")
          and seq.index("glinst install") > max(i for i, s in enumerate(seq) if s == "kmutil inspect"), seq)
    lk = os.path.join(root, GLLINK)
    check("layout: link -> real dir with the bundle's exe",
          os.path.islink(lk) and os.readlink(lk) == os.path.join(root, GLREAL)
          and sha_file(os.path.join(root, GLREAL, "Contents/MacOS/UniqVGPUGL")) == glsha, out)
    st = open(os.path.join(root, state_rel)).read() if os.path.exists(os.path.join(root, state_rel)) else ""
    check("rollback state records GL prior = none + GL_MANAGED=1",
          "GL_PRIOR_REAL_KIND=none" in st and "GL_PRIOR_LINK_KIND=none" in st and st.rstrip().endswith("GL_MANAGED=1"), st)
    check("no reboot with --no-reboot; no retire without AppleVirglGL",
          "REBOOT" not in calls and "glinst retire" not in calls, calls)
    rc, out, _ = run_install(root, ["--rollback", "--no-reboot"])
    check("rollback removes the GL link + real dir and the kext",
          rc == 0 and not os.path.lexists(lk) and not os.path.lexists(os.path.join(root, GLREAL))
          and not os.path.exists(os.path.join(root, "Library/Extensions/UniqVGPU.kext")), out)

    print("[guest-install: a prior UniqVGPUGL existed -> backed up; --rollback restores it]")
    root = make_root(tmp, "prior")
    old_real = os.path.join(root, GLREAL)
    make_gl(os.path.dirname(old_real), b"OLD GL exe")
    os.makedirs(os.path.join(root, "Library/GPUBundles"))
    os.symlink(old_real, os.path.join(root, GLLINK))
    rc, out, _ = run_install(root, [fx_gl, "--no-reboot", "--yes"])
    check("install over a prior GL rc 0, new exe live",
          rc == 0 and sha_file(os.path.join(old_real, "Contents/MacOS/UniqVGPUGL")) == glsha, out)
    st = open(os.path.join(root, state_rel)).read() if rc == 0 else ""
    check("state: prior real dir backed up, prior link target recorded",
          "GL_PRIOR_REAL_KIND=dir" in st and f'GL_PRIOR_LINK_TARGET="{old_real}"' in st, st)
    rc, out, _ = run_install(root, ["--rollback", "--no-reboot"])
    check("rollback restores the prior GL bundle + link",
          rc == 0 and open(os.path.join(old_real, "Contents/MacOS/UniqVGPUGL"), "rb").read() == b"OLD GL exe"
          and os.readlink(os.path.join(root, GLLINK)) == old_real, out)

    print("[guest-install: leftover AppleVirglGL is retired; failing GL installer stops before reboot]")
    root = make_root(tmp, "retire")
    make_gl(os.path.join(root, "Library/GPUBundles"), b"x")
    os.rename(os.path.join(root, "Library/GPUBundles/UniqVGPUGL.bundle"),
              os.path.join(root, "Library/GPUBundles/AppleVirglGL.bundle"))
    rc, out, calls = run_install(root, [fx_gl, "--no-reboot", "--yes"])
    check("retire AppleVirglGL called + recorded",
          rc == 0 and "glinst retire AppleVirglGL" in calls
          and not os.path.lexists(os.path.join(root, "Library/GPUBundles/AppleVirglGL.bundle"))
          and 'GL_RETIRED="AppleVirglGL"' in open(os.path.join(root, state_rel)).read(), out)
    root = make_root(tmp, "glfail")
    rc, out, calls = run_install(root, [fx_gl, "--yes"], extra_env={"FAKE_GL_FAIL": "1"})
    check("GL installer failure -> STOP-IF rc 2, no reboot",
          rc == 2 and "STOP-IF: GL install did not report 'GPU_BUNDLE_OK UniqVGPUGL" in out and "REBOOT" not in calls, out)
    rc, out, _ = run_install(root, ["--rollback", "--no-reboot"])
    check("rollback after the GL failure restores the guest (kext + GL absent)",
          rc == 0 and not os.path.lexists(os.path.join(root, GLLINK))
          and not os.path.exists(os.path.join(root, "Library/Extensions/UniqVGPU.kext")), out)

    print("[guest-install: no-gl bundle and --no-gl install the kext only, exactly as before]")
    for tag, args in (("nogl", [fx_nogl]), ("optout", [fx_gl, "--no-gl"])):
        root = make_root(tmp, "k-" + tag)
        rc, out, calls = run_install(root, args + ["--no-reboot", "--yes"])
        st = open(os.path.join(root, state_rel)).read() if os.path.exists(os.path.join(root, state_rel)) else ""
        check(f"{tag}: kext installed, no GL call, no GL state",
              rc == 0 and os.path.exists(os.path.join(root, "Library/Extensions/UniqVGPU.kext"))
              and "glinst" not in calls and "GL_" not in st
              and not os.path.lexists(os.path.join(root, "Library/GPUBundles")), out)
        rc, out, _ = run_install(root, ["--rollback", "--no-reboot"])
        check(f"{tag}: rollback as before", rc == 0 and "GL:" not in out
              and not os.path.exists(os.path.join(root, "Library/Extensions/UniqVGPU.kext")), out)

    # ================================================================ static desktop picture
    wpd = os.path.join(src, "wp")
    write(os.path.join(wpd, "uniqvgpu-wallpaper.sh"), FAKE_WP_TOOL, 0o644)
    write(os.path.join(wpd, "uniqvgpu-wallpaper-set.js"), FAKE_WP_SETTER, 0o644)
    WPARGS = ["--wallpaper-tool", wpd]
    print("[mkbundle: --wallpaper-tool -> wallpaper block + wallpaper/ files in SHA256SUMS]")
    bw = os.path.join(tmp, "b-wp")
    rc, out = mkb(MKBUNDLE, bw, *base, "--kext", ktar, "--kext-sha", ksha, *GLARGS, *WPARGS, "--wallpaper-origin", "o")
    mw = json.load(open(os.path.join(bw, "uniq-vgpu-bundle.json"))) if rc == 0 else {}
    check("wp mkbundle rc 0", rc == 0, out)
    check("wp manifest block exact (default static picture)", mw.get("wallpaper") == {
        "tool": "wallpaper/uniqvgpu-wallpaper.sh", "setter": "wallpaper/uniqvgpu-wallpaper-set.js",
        "picture": PIC, "origin": "o"}, mw)
    sw = sums(bw) if rc == 0 else {}
    check("wp tool + setter in SHA256SUMS, verbatim, tool executable",
          rc == 0 and {"./wallpaper/uniqvgpu-wallpaper.sh", "./wallpaper/uniqvgpu-wallpaper-set.js"} <= set(sw)
          and set(sw) == files_of(bw)
          and open(os.path.join(bw, "wallpaper/uniqvgpu-wallpaper.sh")).read() == FAKE_WP_TOOL
          and os.stat(os.path.join(bw, "wallpaper/uniqvgpu-wallpaper.sh")).st_mode & 0o111, sw)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-wppic"), *base, *WPARGS, "--wallpaper-picture", "/x/y.png")
    check("--wallpaper-picture recorded",
          rc == 0 and json.load(open(os.path.join(tmp, "b-wppic", "uniq-vgpu-bundle.json")))["wallpaper"]["picture"] == "/x/y.png", out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-wpnotool"), *base, "--wallpaper-picture", "/x/y.png")
    check("--wallpaper-picture without --wallpaper-tool refused", rc != 0, out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-wpbad"), *base, "--wallpaper-tool", src)
    check("--wallpaper-tool dir without the tool refused", rc != 0 and "has no uniqvgpu-wallpaper" in out, out)
    rc, out = mkb(MKBUNDLE, os.path.join(tmp, "b-wpguest"), "--name", "n", "--version", "1", *WPARGS)
    check("wallpaper-only guest bundle needs no envelope", rc == 0, out)
    check("gl bundle without --wallpaper-tool has no wallpaper block", "wallpaper" not in m1, m1)

    print("[guest-install: wallpaper step]")
    fx_wp = os.path.join(tmp, "fx-wp")
    r3, o3 = mkb(FIX_MKBUNDLE, fx_wp, "--name", "n", "--version", "1", "--envelope", "e", "--kext", ktar,
                 "--kext-sha", ksha, "--kext-uuid", KUUID, "--kext-build", "25C56", *GLARGS, *WPARGS)
    check("wp fixture built", r3 == 0, o3)
    root = make_root(tmp, "wpdry")
    rc, out, calls = run_install(root, [fx_wp, "--dry-run"])
    check("wp dry-run passes the wallpaper STOP-IF and makes no change",
          rc == 0 and "wallpaper tool + setter match SHA256SUMS" in out and f"would run: bash wallpaper/uniqvgpu-wallpaper.sh apply '{PIC}'" in out
          and "wp " not in calls, out + calls)
    fx_wpbad = os.path.join(tmp, "fx-wpbad")
    shutil.copytree(fx_wp, fx_wpbad, symlinks=True)
    with open(os.path.join(fx_wpbad, "wallpaper", "uniqvgpu-wallpaper.sh"), "a") as f:
        f.write("# tampered\n")
    rc, out, calls = run_install(root, [fx_wpbad, "--dry-run"])
    check("wp tool not matching SHA256SUMS -> STOP-IF (rc 2)",
          rc == 2 and "STOP-IF: wallpaper/uniqvgpu-wallpaper.sh sha256" in out, out)
    rc, out, calls = run_install(root, [fx_wpbad, "--no-reboot", "--yes"])
    check("wp tampered tool stops BEFORE any change (no kext, no GL, no wp call)",
          rc == 2 and "kmutil create" not in calls and "glinst" not in calls and "wp " not in calls
          and not os.path.exists(os.path.join(root, "Library/Extensions/UniqVGPU.kext")), out + calls)
    rc, out, _ = run_install(root, [fx_wp, "--dry-run", "--force-build"], no_python=True)
    check("wp block unreadable without python3 -> STOP-IF, not a silent skip",
          rc == 2 and ("STOP-IF: manifest gl block lacks" in out or "STOP-IF: manifest wallpaper block lacks" in out), out)

    root = make_root(tmp, "wp")
    rc, out, calls = run_install(root, [fx_wp, "--no-reboot", "--yes"])
    seq = [" ".join(l.split()[:2]) for l in calls.splitlines()]
    check("wp install rc 0 + OK line", rc == 0 and "OK  wallpaper: static picture set" in out, out)
    check("wp tool called with apply <picture> from the bundle",
          f"wp apply {PIC}" in calls.splitlines(), calls)
    check("wp step runs AFTER the GL install",
          "wp apply" in seq and "glinst install" in seq and seq.index("wp apply") > seq.index("glinst install"), seq)
    st = open(os.path.join(root, state_rel)).read() if os.path.exists(os.path.join(root, state_rel)) else ""
    check("rollback state records WALLPAPER_MANAGED=1", "WALLPAPER_MANAGED=1" in st, st)
    rc, out, calls = run_install(root, ["--rollback", "--no-reboot"])
    check("rollback runs the installed tool's revert, then the kext rollback",
          rc == 0 and "wp revert" in calls and "OK  wallpaper: prior desktop pictures restored" in out
          and not os.path.exists(os.path.join(root, "Library/Application Support/UniqVGPU/wallpaper"))
          and not os.path.exists(os.path.join(root, "Library/Extensions/UniqVGPU.kext")), out)

    root = make_root(tmp, "wpopt")
    rc, out, calls = run_install(root, [fx_wp, "--no-reboot", "--yes", "--no-wallpaper"])
    st = open(os.path.join(root, state_rel)).read() if os.path.exists(os.path.join(root, state_rel)) else ""
    check("--no-wallpaper: kext + GL installed, no wp call, no wallpaper state",
          rc == 0 and "wallpaper" in out and "SKIPPED" in out and "wp " not in calls and "WALLPAPER" not in st
          and os.path.lexists(os.path.join(root, GLLINK)), out + calls)
    rc, out, calls = run_install(root, ["--rollback", "--no-reboot"])
    check("--no-wallpaper: rollback makes no wp call", rc == 0 and "wp " not in calls, out + calls)

    root = make_root(tmp, "wpfail")
    rc, out, calls = run_install(root, [fx_wp, "--yes"], extra_env={"FAKE_WP_FAIL": "1"})
    st = open(os.path.join(root, state_rel)).read() if os.path.exists(os.path.join(root, state_rel)) else ""
    check("wp failure is a WARN: rc 0, kext + GL installed, reboot proceeds, state lets --rollback revert",
          rc == 0 and "WARN wallpaper step rc=1" in out and "REBOOT" in calls and "WALLPAPER_MANAGED=1" in st
          and os.path.lexists(os.path.join(root, GLLINK)), out + calls)

    root = make_root(tmp, "wpnone")
    rc, out, calls = run_install(root, [fx_gl, "--no-reboot", "--yes"])
    check("bundle without a wallpaper block: no wp call, no wallpaper output",
          rc == 0 and "wp " not in calls and "wallpaper" not in out, out + calls)

    shutil.rmtree(tmp, ignore_errors=True)
    print()
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
