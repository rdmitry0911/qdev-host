#!/usr/bin/env python3
"""Run shell commands in the guest over its serial console (root autologin on Debian nocloud).
usage: guest_cmd.py SERIAL_SOCK 'cmd1' 'cmd2' ...   (prints each command and its output)"""
import socket, sys, time
sock = sys.argv[1]; cmds = sys.argv[2:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sock); s.settimeout(1.0)
buf = b""
def wait_for(pat, timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            buf += d
        except socket.timeout:
            pass
        if pat in buf: return True
    return False
s.sendall(b"\x03\n"); wait_for(b"# ", 3)   # Ctrl-C resets any pending input line
if b"# " not in buf:
    s.sendall(b"root\n"); wait_for(b"# ", 20)
for c in cmds:
    buf = b""; marker = b"__END_%d__" % int(time.time() * 1000)
    s.sendall(c.encode() + b"; echo " + marker + b"\n")
    wait_for(b"\n" + marker, 90)
    out = buf.decode(errors="replace")
    # strip the echoed command line and the marker line
    lines = out.split("\n")
    body = [l.rstrip("\r") for l in lines[1:] if not l.startswith(marker.decode())]
    print("$ " + c); print("\n".join(body).rstrip())
