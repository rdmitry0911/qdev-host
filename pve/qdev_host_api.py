#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""qdev-host node service: the HTTPS backend of the Proxmox VE Hardware panel overlay.

Stock PVE has no server-side plug-in API, so (like QSM Direct) the browser talks
to this small service on the node instead.  Every request carries the user's
PVE ticket (Authorization: Bearer) and user name (X-QDev-User); the service
relays them to the node's own pveproxy (/access/ticket with the VM path and
VM.Config.HWType) and additionally requires the user to be in the allow-list
(root@pam by default: the operations edit the VM's `args`, which PVE itself
reserves to root).  Nothing is registered inside pveproxy or pvedaemon.

  GET    /                                   plain page (open once to accept the certificate)
  GET    /api/v1/vms/VMID                    devices with live state, plugins, KMS outputs
  POST   /api/v1/vms/VMID/devices            {id, plugin, args, kms, display, persistent}
  DELETE /api/v1/vms/VMID/devices/ID[?purge_port=1&keep_process=1]
  POST   /api/v1/vms/VMID/devices/ID/attach  start the device process and hot-plug
  POST   /api/v1/vms/VMID/devices/ID/detach  hot-unplug and stop the process

Configuration (optional): /etc/qdev-host/ui.json
  {"port": 8017, "listen": "", "users": ["root@pam"], "privilege": "VM.Config.HWType"}
"""
from __future__ import annotations
import argparse, http.server, json, os, re, socket, ssl, sys, threading, time, urllib.error, urllib.parse, urllib.request
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qdev_pve as Q  # noqa: E402

CONFIG_PATH = Path("/etc/qdev-host/ui.json")
DEFAULTS: dict[str, Any] = {"port": 8017, "listen": "", "users": ["root@pam"], "privilege": "VM.Config.HWType"}
MAX_BODY = 64 * 1024
MAX_TICKET = 4096
REQUEST_TIMEOUT = 10
_USER = re.compile(r"^[A-Za-z0-9._@+-]{3,128}$")
_ORIGIN = re.compile(r"^https://[A-Za-z0-9.\-:\[\]]{1,255}$")
_VM_PATH = re.compile(r"^/api/v1/vms/(?P<vmid>[1-9][0-9]{2,8})(?P<rest>/.*)?$")
_DEV_PATH = re.compile(r"^/devices/(?P<id>[A-Za-z][A-Za-z0-9_-]{0,31})(?:/(?P<action>attach|detach))?$")

def load_config(path: Path = CONFIG_PATH) -> dict[str, Any]:
    cfg = dict(DEFAULTS)
    if path.exists():
        try:
            data = json.loads(path.read_text())
        except ValueError as e:
            raise SystemExit(f"qdev-host-api: {path}: {e}")
        if not isinstance(data, dict):
            raise SystemExit(f"qdev-host-api: {path}: must be a JSON object")
        cfg.update({k: v for k, v in data.items() if k in DEFAULTS})
    if not isinstance(cfg["port"], int) or not 1 <= cfg["port"] <= 65535:
        raise SystemExit("qdev-host-api: port must be 1..65535")
    if not isinstance(cfg["users"], list) or not all(isinstance(u, str) for u in cfg["users"]):
        raise SystemExit("qdev-host-api: users must be a list of strings")
    return cfg

class ApiError(Exception):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status

class PveAuthority:
    """Authentication and ACL checks are delegated to the node's own pveproxy."""

    def __init__(self, pve_url: str, ca_file: Path) -> None:
        self._url = pve_url.rstrip("/") + "/api2/json/access/ticket"
        ctx = ssl.create_default_context()
        try:
            ctx.load_verify_locations(cafile=str(ca_file))
        except OSError:
            pass  # a custom/ACME pveproxy certificate verifies against the system store
        ctx.check_hostname = False  # loopback: the CA signature is the meaningful check
        # Python 3.13 enables X509_STRICT by default; older PVE cluster CAs lack the
        # key-usage extension and fail it ("CA cert does not include key usage").
        # The chain is still verified against the cluster CA / system store.
        ctx.verify_flags &= ~getattr(ssl, "VERIFY_X509_STRICT", 0)
        self._ctx = ctx

    def authorize(self, user: str, ticket: str, vmid: int, privilege: str) -> str:
        if not _USER.match(user or "") or not 1 <= len(ticket or "") <= MAX_TICKET:
            raise ApiError(401, "authentication failure")
        form = urllib.parse.urlencode({"username": user, "password": ticket, "path": f"/vms/{vmid}",
                                       "privs": privilege}).encode("ascii")
        req = urllib.request.Request(self._url, data=form, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT, context=self._ctx) as r:
                body = json.loads(r.read(MAX_BODY).decode("utf-8"))
        except urllib.error.HTTPError as e:
            raise ApiError(401, f"authentication failure (pveproxy: {e.code})") from e
        except (OSError, ValueError) as e:
            raise ApiError(502, "pveproxy unavailable") from e
        data = body.get("data") if isinstance(body, dict) else None
        confirmed = data.get("username") if isinstance(data, dict) else None
        if not isinstance(confirmed, str) or not _USER.match(confirmed):
            raise ApiError(401, "authentication failure")
        return confirmed

class Service:
    def __init__(self, config: dict[str, Any], authority: PveAuthority) -> None:
        self.config = config
        self.authority = authority
        self._locks: dict[int, threading.Lock] = {}
        self._locks_guard = threading.Lock()

    def lock(self, vmid: int) -> threading.Lock:
        with self._locks_guard:
            return self._locks.setdefault(vmid, threading.Lock())

    def check_user(self, confirmed: str) -> None:
        users = self.config["users"]
        if "*" in users or confirmed in users:
            return
        raise ApiError(403, f"user {confirmed} is not allowed to manage qdev-host devices (see /etc/qdev-host/ui.json)")

    def handle(self, method: str, path: str, query: dict[str, list[str]], body: dict[str, Any],
               user: str, ticket: str) -> Any:
        m = _VM_PATH.match(path)
        if not m:
            raise ApiError(404, "not found")
        vmid = int(m.group("vmid"))
        rest = m.group("rest") or ""
        confirmed = self.authority.authorize(user, ticket, vmid, self.config["privilege"])
        self.check_user(confirmed)
        node = Q.vm_node(vmid)
        if node is None:
            raise ApiError(404, f"VM {vmid} does not exist in this cluster")
        if node != Q.local_node():
            raise ApiError(409, f"VM {vmid} is on node {node}: open the web UI of that node to manage its qdev-host devices")
        with self.lock(vmid):
            if rest == "" and method == "GET":
                return Q.describe_vm(vmid)
            if rest == "/devices" and method == "POST":
                return self.add(vmid, body, confirmed)
            d = _DEV_PATH.match(rest)
            if d and d.group("action") is None and method == "DELETE":
                flags = {k: query.get(k, ["0"])[0] in ("1", "true") for k in ("purge_port", "keep_process")}
                log(f"{confirmed}: remove {d.group('id')} from VM {vmid} {flags}")
                return {"messages": Q.remove_device(vmid, d.group("id"), flags["keep_process"], flags["purge_port"])}
            if d and d.group("action") and method == "POST":
                conf = Q.load_conf(vmid)
                dev = Q.find_device(conf, d.group("id"))
                if not dev:
                    raise ApiError(404, f"no device {d.group('id')} on VM {vmid}")
                log(f"{confirmed}: {d.group('action')} {dev['id']} on VM {vmid}")
                if d.group("action") == "attach":
                    if not Q.vm_running(vmid):
                        raise ApiError(409, f"VM {vmid} is not running")
                    return {"messages": [Q.attach(vmid, dev)]}
                return {"messages": [Q.detach(vmid, dev, keep_process=False)]}
        raise ApiError(404, "not found")

    def add(self, vmid: int, body: dict[str, Any], confirmed: str) -> Any:
        def text(key: str, default: str = "") -> str:
            v = body.get(key, default)
            if v is None:
                return ""
            if not isinstance(v, str) or len(v) > 512:
                raise ApiError(400, f"{key}: must be a string")
            return v.strip()
        dev_id, plugin, args, kms = text("id"), text("plugin"), text("args"), text("kms")
        bus = text("bus", Q.PORT_ID) or Q.PORT_ID
        display = bool(body.get("display", True))
        persistent = bool(body.get("persistent", False))
        log(f"{confirmed}: add {dev_id} ({plugin}, args='{args}', kms={kms or '-'}) to VM {vmid}")
        return {"messages": Q.add_device(vmid, dev_id, plugin, args, bus, display, kms or None, persistent)}

def log(msg: str) -> None:
    print(f"qdev-host-api: {msg}", file=sys.stderr, flush=True)

INDEX_HTML = b"""<!doctype html><meta charset="utf-8"><title>qdev-host node service</title>
<body style="font-family:sans-serif;max-width:40em;margin:3em auto"><h1>qdev-host node service</h1>
<p>This endpoint serves the qdev-host entries of the Proxmox VE Hardware panel.
If you were sent here to accept the node certificate, you can close this tab now
and reload the Proxmox VE web UI.</p></body>"""

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "qdev-host-api"
    sys_version = ""
    timeout = 30

    def setup(self) -> None:
        super().setup()
        self.connection.do_handshake()  # on this connection's thread, not the accept loop

    @property
    def service(self) -> Service:
        return self.server.service  # type: ignore[attr-defined]

    def _cors(self) -> None:
        origin = self.headers.get("Origin", "")
        if _ORIGIN.match(origin):
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-QDev-User")
        self.send_header("Access-Control-Max-Age", "600")

    def _send(self, status: int, payload: dict[str, Any] | None = None, raw: bytes | None = None,
              ctype: str = "application/json") -> None:
        data = raw if raw is not None else json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode()
        self.send_response(status)
        self._cors()
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def _body(self) -> dict[str, Any]:
        length = self.headers.get("Content-Length")
        if not length:
            return {}
        try:
            size = int(length)
        except ValueError:
            raise ApiError(400, "invalid request")
        if size < 0 or size > MAX_BODY:
            raise ApiError(413, "request too large")
        raw = self.rfile.read(size) if size else b""
        if not raw:
            return {}
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise ApiError(400, "invalid JSON body")
        if not isinstance(value, dict):
            raise ApiError(400, "JSON body must be an object")
        return value

    def _dispatch(self, method: str) -> None:
        try:
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == "/" and method == "GET":
                self._send(200, raw=INDEX_HTML, ctype="text/html; charset=utf-8")
                return
            body = self._body()
            auth = self.headers.get("Authorization", "")
            ticket = auth[len("Bearer "):] if auth.startswith("Bearer ") else ""
            user = self.headers.get("X-QDev-User", "")
            result = self.service.handle(method, parsed.path, urllib.parse.parse_qs(parsed.query), body, user, ticket)
            self._send(200, {"data": result})
        except ApiError as e:
            self._send(e.status, {"errors": str(e), "message": str(e)})
        except Q.QdevError as e:
            self._send(422, {"errors": str(e), "message": str(e)})
        except Exception as e:  # noqa: BLE001
            log(f"internal error: {e!r}")
            self._send(500, {"errors": "internal error", "message": "internal error (see journalctl -u qdev-host-api)"})

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self._cors()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        self._dispatch("GET")

    def do_POST(self) -> None:  # noqa: N802
        self._dispatch("POST")

    def do_DELETE(self) -> None:  # noqa: N802
        self._dispatch("DELETE")

    def log_message(self, *_args: Any) -> None:
        return  # request lines could carry credentials; actions are logged by the service

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], service: Service, context: ssl.SSLContext) -> None:
        self.address_family = socket.AF_INET6 if ":" in address[0] or address[0] == "" else socket.AF_INET
        if address[0] == "":
            address = ("::", address[1])
        super().__init__(address, Handler)
        self.service = service
        self.socket = context.wrap_socket(self.socket, server_side=True, do_handshake_on_connect=False)

    def server_bind(self) -> None:
        if self.address_family == socket.AF_INET6:
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()

    def handle_error(self, request: Any, client_address: Any) -> None:
        if not isinstance(sys.exc_info()[1], OSError):
            log("a request failed")

def node_certificate() -> tuple[Path, Path]:
    custom = Path("/etc/pve/local/pveproxy-ssl.pem"), Path("/etc/pve/local/pveproxy-ssl.key")
    if custom[0].exists() and custom[1].exists():
        return custom
    return Path("/etc/pve/local/pve-ssl.pem"), Path("/etc/pve/local/pve-ssl.key")

def watch_certificate(context: ssl.SSLContext, cert: Path, key: Path) -> None:
    """Reload the certificate when PVE renews it (new connections pick it up)."""
    last = None
    while True:
        try:
            stamp = (cert.stat().st_mtime_ns, key.stat().st_mtime_ns)
            if last is not None and stamp != last:
                context.load_cert_chain(certfile=str(cert), keyfile=str(key))
                log("certificate reloaded")
            last = stamp
        except (OSError, ssl.SSLError) as e:
            log(f"certificate check failed: {e}")
        time.sleep(300)

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", type=Path, default=CONFIG_PATH)
    ap.add_argument("--listen", default=None, help="bind address (default: all)")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--cert", type=Path, default=None)
    ap.add_argument("--key", type=Path, default=None)
    ap.add_argument("--ca", type=Path, default=Path("/etc/pve/pve-root-ca.pem"))
    ap.add_argument("--pve-url", default="https://127.0.0.1:8006")
    a = ap.parse_args()
    config = load_config(a.config)
    listen = a.listen if a.listen is not None else config["listen"]
    port = a.port if a.port is not None else config["port"]
    cert, key = (a.cert, a.key) if a.cert and a.key else node_certificate()
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(certfile=str(cert), keyfile=str(key))
    service = Service(config, PveAuthority(a.pve_url, a.ca))
    server = Server((listen, port), service, context)
    threading.Thread(target=watch_certificate, args=(context, cert, key), daemon=True).start()
    log(f"listening on {listen or '*'}:{port} (users: {', '.join(config['users'])}, privilege {config['privilege']})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
