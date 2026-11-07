#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the qdev-host Hardware panel overlay into a supported Proxmox VE 9 web UI.

PVE has no stable web UI plug-in API.  Like QSM Direct, this tool therefore
adds one script tag to ``/usr/share/pve-manager/index.html.tpl`` through a
package-owned ``dpkg-divert``: the stock template is kept at a fixed
diversion path, its exact pve-manager version and SHA-256 must be in the
reviewed allow-list (``pve-manager-index-template.sha256``), and an unknown
PVE upgrade restores the untouched stock template instead of patching it.

Coexistence with qsm-pve-direct: a file can only be diverted once.  When
qsm-pve-direct already owns the diversion, this tool verifies *its* stock
copy against the allow-list and inserts the qdev-host block into the current
(qsm-rendered) template instead ("piggyback" mode).  qsm's own reconcile
rewrites that file without our block; the ``qdev-host-ui-reconcile.path``
unit re-adds it.  When nothing owns the diversion yet but qsm-pve-direct is
installed, qsm is asked to divert first so that a later qsm install does not
find a foreign diversion.

Commands: install | reconcile | remove | status   (reconcile is idempotent)
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, stat, subprocess, sys, tempfile
from pathlib import Path

PACKAGE = "qdev-host-pve"
QSM_PACKAGE = "qsm-pve-direct"
PVE_MANAGER = "pve-manager"
TEMPLATE = Path("usr/share/pve-manager/index.html.tpl")
OVERLAY = Path("usr/share/pve-manager/js/qdev-host-hardware.js")
MANIFEST = Path("usr/share/qdev-host/pve/pve-manager-index-template.sha256")
STATE = Path("var/lib/qdev-host/pve-ui-state.json")
UI_CONFIG = Path("etc/qdev-host/ui.json")
QSM_UI_TOOL = Path("usr/sbin/qsm-pve-direct-ui")
DIVERSION_SUFFIX = f".{PACKAGE}-orig"
QSM_DIVERSION_SUFFIX = f".{QSM_PACKAGE}-orig"
BEGIN = b"    <!-- qdev-host-pve PVE 9 hardware overlay begin; package-managed, do not edit -->\n"
END = b"    <!-- qdev-host-pve PVE 9 hardware overlay end -->\n"
ANCHOR = (b'    <script type="text/javascript" src="/pve2/js/pvemanagerlib.js?ver=[% version %]"></script>\n')
OVERLAY_HEAD = b"/*\n * qdev-host Hardware panel integration"
MAX_TEMPLATE = 256 * 1024
MAX_OVERLAY = 1024 * 1024
MAX_MANIFEST = 128 * 1024
_VERSION = re.compile(r"\A[A-Za-z0-9.+:~_-]{1,128}\Z")
_HASH = re.compile(r"\A[0-9a-f]{64}\Z")
DEFAULT_PORT = 8017

class UiError(RuntimeError):
    pass

class UnsupportedPve(UiError):
    pass

class UnsafeState(UiError):
    pass

# ------------------------------------------------------------------ helpers
def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def read_regular(path: Path, maximum: int, root_owner: bool) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as e:
        raise UnsafeState(f"{path}: {e.strerror}") from e
    try:
        st = os.fstat(fd)
        if (not stat.S_ISREG(st.st_mode) or st.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or (root_owner and st.st_uid != 0) or st.st_size > maximum):
            raise UnsafeState(f"{path}: not a safe regular file")
        data = b""
        while len(data) <= maximum:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            data += chunk
        if len(data) > maximum:
            raise UnsafeState(f"{path}: too large")
        return data
    finally:
        os.close(fd)

def atomic_write(path: Path, content: bytes, mode: int, root_owner: bool) -> None:
    st = path.parent.lstat()
    if not stat.S_ISDIR(st.st_mode) or st.st_mode & (stat.S_IWGRP | stat.S_IWOTH) or (root_owner and st.st_uid != 0):
        raise UnsafeState(f"{path.parent}: unsafe directory")
    fd, tmp = tempfile.mkstemp(prefix=f".{PACKAGE}-", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        if root_owner:
            os.fchown(fd, 0, 0)
        os.write(fd, content)
        os.fsync(fd)
        os.close(fd)
        fd = -1
        os.replace(tmp, path)
        dfd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    except OSError as e:
        raise UiError(f"cannot write {path}: {e.strerror}") from e
    finally:
        if fd >= 0:
            os.close(fd)
        try:
            Path(tmp).unlink()
        except FileNotFoundError:
            pass

def load_manifest(path: Path, root_owner: bool) -> dict[str, str]:
    try:
        text = read_regular(path, MAX_MANIFEST, root_owner).decode("ascii")
    except UnicodeDecodeError as e:
        raise UnsafeState("manifest is not ASCII") from e
    out: dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not _VERSION.match(fields[0]) or not _HASH.match(fields[1]) or fields[0] in out:
            raise UnsafeState("manifest is invalid")
        out[fields[0]] = fields[1]
    if not out:
        raise UnsafeState("manifest is empty")
    return out

def strip_block(template: bytes) -> bytes:
    """Remove our block (if any); tolerate a truncated one by cutting BEGIN..END or BEGIN..anchor-free."""
    while True:
        b = template.find(BEGIN)
        if b < 0:
            return template
        e = template.find(END, b)
        if e < 0:
            raise UnsafeState("qdev-host overlay block in the PVE template is damaged (begin without end)")
        template = template[:b] + template[e + len(END):]

def render_block(overlay_digest: str, endpoint: dict) -> bytes:
    if not _HASH.match(overlay_digest):
        raise UnsafeState("overlay digest is invalid")
    encoded = json.dumps(endpoint, separators=(",", ":"), ensure_ascii=True)
    encoded = encoded.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    return (BEGIN
            + b'    <script type="text/javascript">window.QDEV_HOST_ENDPOINT=' + encoded.encode("ascii") + b";</script>\n"
            + b'    <script type="text/javascript" src="/pve2/js/qdev-host-hardware.js?ver=[% version %]-qdev-overlay-'
            + overlay_digest[:16].encode("ascii") + b'"></script>\n'
            + END)

def render(base: bytes, overlay_digest: str, endpoint: dict) -> bytes:
    """Insert our block after the pvemanagerlib.js tag of BASE (which may already be qsm-rendered)."""
    base = strip_block(base)
    if base.count(ANCHOR) != 1:
        raise UnsupportedPve("PVE web UI layout is unsupported (pvemanagerlib.js anchor not found exactly once)")
    return base.replace(ANCHOR, ANCHOR + render_block(overlay_digest, endpoint), 1)

# ---------------------------------------------------------------- dpkg
class Diversions:
    def __init__(self, exe: str = "/usr/bin/dpkg-divert") -> None:
        self.exe = exe

    def _run(self, args: list[str]) -> str:
        try:
            r = subprocess.run([self.exe, *args], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        except OSError as e:
            raise UiError("dpkg-divert is unavailable") from e
        if r.returncode != 0:
            raise UiError(f"dpkg-divert failed: {r.stderr.strip()}")
        return r.stdout.strip()

    def owner(self, target: Path) -> str | None:
        return self._run(["--listpackage", str(target)]) or None

    def add(self, target: Path, diverted: Path) -> None:
        self._run(["--package", PACKAGE, "--divert", str(diverted), "--rename", "--add", str(target)])

    def remove(self, target: Path, diverted: Path) -> None:
        self._run(["--package", PACKAGE, "--divert", str(diverted), "--rename", "--remove", str(target)])

class SandboxDiversions:
    """File-only diversion backend for --root tests (never touches the dpkg database)."""

    def __init__(self, state: Path) -> None:
        self.state = state

    def owner(self, target: Path) -> str | None:
        try:
            return self.state.read_text().strip() or None
        except FileNotFoundError:
            return None

    def add(self, target: Path, diverted: Path) -> None:
        os.replace(target, diverted)
        self.state.parent.mkdir(parents=True, exist_ok=True)
        self.state.write_text(PACKAGE + "\n")

    def remove(self, target: Path, diverted: Path) -> None:
        if target.exists():
            raise UiError("target still present")
        os.replace(diverted, target)
        self.state.unlink()

def pve_manager_version() -> str:
    try:
        r = subprocess.run(["/usr/bin/dpkg-query", "-W", "-f=${db:Status-Status}\t${Version}", PVE_MANAGER],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True)
    except OSError as e:
        raise UnsupportedPve("pve-manager is unavailable") from e
    fields = r.stdout.strip().split("\t", 1)
    if r.returncode != 0 or len(fields) != 2 or fields[0] not in ("installed", "triggers-pending", "triggers-awaited") \
            or not _VERSION.match(fields[1]):
        raise UnsupportedPve("pve-manager is unavailable")
    return fields[1]

def package_installed(name: str) -> bool:
    r = subprocess.run(["/usr/bin/dpkg-query", "-W", "-f=${db:Status-Status}", name],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    return r.returncode == 0 and r.stdout.strip() == "installed"

# ---------------------------------------------------------------- manager
class Integration:
    def __init__(self, root: Path, diversions, version_fn=pve_manager_version, root_owner: bool = True) -> None:
        self.root = root
        self.div = diversions
        self.version_fn = version_fn
        self.root_owner = root_owner
        self.template = root / TEMPLATE
        self.diversion = Path(f"{self.template}{DIVERSION_SUFFIX}")
        self.qsm_diversion = Path(f"{self.template}{QSM_DIVERSION_SUFFIX}")
        self.overlay = root / OVERLAY
        self.manifest = root / MANIFEST
        self.state = root / STATE

    # -- inputs
    def endpoint(self) -> dict:
        port = DEFAULT_PORT
        cfg = self.root / UI_CONFIG
        if cfg.exists():
            try:
                data = json.loads(cfg.read_text())
                if isinstance(data, dict) and isinstance(data.get("port"), int):
                    port = data["port"]
            except ValueError as e:
                raise UnsafeState(f"{cfg}: {e}") from e
        return {"port": port}

    def overlay_digest(self) -> str:
        content = read_regular(self.overlay, MAX_OVERLAY, self.root_owner)
        if not content.startswith(OVERLAY_HEAD):
            raise UnsafeState("qdev-host overlay asset is invalid")
        return sha256(content)

    def check_stock(self, stock_path: Path) -> str:
        version = self.version_fn()
        want = load_manifest(self.manifest, self.root_owner).get(version)
        if want is None:
            raise UnsupportedPve(f"pve-manager {version} is not in the reviewed allow-list")
        stock = read_regular(stock_path, MAX_TEMPLATE, self.root_owner)
        if sha256(stock) != want:
            raise UnsupportedPve(f"stock PVE template hash does not match the allow-list entry for {version}")
        return version

    def write_state(self, version: str, mode: str) -> None:
        self.state.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(self.state, json.dumps({"pve_manager_version": version, "mode": mode, "version": 1},
                                            sort_keys=True).encode() + b"\n", 0o600, self.root_owner)

    def current(self) -> bytes:
        return read_regular(self.template, MAX_TEMPLATE, self.root_owner)

    # -- operations
    def install(self) -> str:
        owner = self.div.owner(self.template)
        if owner is None and package_installed(QSM_PACKAGE) and (self.root / QSM_UI_TOOL).exists():
            # let qsm own the diversion so that both overlays can coexist in either install order
            subprocess.run([str(self.root / QSM_UI_TOOL), "reconcile"], stdin=subprocess.DEVNULL,
                           capture_output=True, text=True)
            owner = self.div.owner(self.template)
        digest = self.overlay_digest()
        endpoint = self.endpoint()
        if owner == PACKAGE:
            version = self.check_stock(self.diversion)
            cur = self.current()
            new = render(read_regular(self.diversion, MAX_TEMPLATE, self.root_owner), digest, endpoint)
            if cur != new:
                atomic_write(self.template, new, 0o644, self.root_owner)
                self.write_state(version, "own")
                return "refreshed"
            return "unchanged"
        if owner == QSM_PACKAGE:
            version = self.check_stock(self.qsm_diversion)
            cur = self.current()
            new = render(cur, digest, endpoint)
            if cur != new:
                atomic_write(self.template, new, 0o644, self.root_owner)
                self.write_state(version, "piggyback")
                return "refreshed (alongside qsm-pve-direct)"
            return "unchanged"
        if owner is not None:
            raise UnsafeState(f"PVE web UI template is diverted by another package ({owner})")
        version = self.check_stock(self.template)
        stock = self.current()
        self.div.add(self.template, self.diversion)
        try:
            diverted = read_regular(self.diversion, MAX_TEMPLATE, self.root_owner)
            if diverted != stock:
                raise UnsafeState("template changed while diverting")
            atomic_write(self.template, render(diverted, digest, endpoint), 0o644, self.root_owner)
            self.write_state(version, "own")
        except BaseException:
            try:
                self._restore_own()
            except UiError:
                pass
            raise
        return "installed"

    def _restore_own(self) -> None:
        if self.div.owner(self.template) != PACKAGE:
            return
        patched = None
        if self.template.exists():
            patched = self.current()
            if BEGIN not in patched:
                raise UnsafeState("refusing to remove an unmanaged PVE UI template")
            self.template.unlink()
        try:
            self.div.remove(self.template, self.diversion)
        except UiError:
            if patched is not None:
                atomic_write(self.template, patched, 0o644, self.root_owner)
            raise

    def remove(self) -> str:
        owner = self.div.owner(self.template)
        if owner == PACKAGE:
            self._restore_own()
        elif owner == QSM_PACKAGE and self.template.exists():
            cur = self.current()
            new = strip_block(cur)
            if new != cur:
                atomic_write(self.template, new, 0o644, self.root_owner)
        try:
            self.state.unlink()
        except FileNotFoundError:
            pass
        return "removed"

    def reconcile(self) -> str:
        try:
            return self.install()
        except UnsupportedPve as e:
            # a pve-manager upgrade we have not reviewed: hand the stock template back untouched
            self.remove()
            return f"disabled ({e})"

    def status(self) -> str:
        owner = self.div.owner(self.template)
        if owner == PACKAGE:
            return "active"
        if owner == QSM_PACKAGE:
            try:
                return "active (alongside qsm-pve-direct)" if BEGIN in self.current() else "inactive (qsm-pve-direct owns the template)"
            except UiError:
                return "inactive"
        return "inactive" if owner is None else f"foreign diversion ({owner})"

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("install", "reconcile", "remove", "status"))
    ap.add_argument("--root", type=Path, default=Path("/"), help=argparse.SUPPRESS)
    ap.add_argument("--package-version", help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    root = a.root.resolve()
    testing = root != Path("/")
    if a.command != "status" and not testing and os.geteuid() != 0:
        print(f"{PACKAGE} UI: must run as root", file=sys.stderr)
        return 2
    version_fn = (lambda: a.package_version) if a.package_version else pve_manager_version
    div = SandboxDiversions(root / "var/lib/qdev-host/test-diversion-owner") if testing else Diversions()
    integ = Integration(root, div, version_fn=version_fn, root_owner=not testing)
    try:
        out = {"install": integ.install, "reconcile": integ.reconcile, "remove": integ.remove, "status": integ.status}[a.command]()
    except UiError as e:
        print(f"{PACKAGE} UI: {e}", file=sys.stderr)
        return 1
    print(f"{PACKAGE} UI: {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
