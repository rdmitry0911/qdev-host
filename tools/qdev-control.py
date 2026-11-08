#!/usr/bin/env python3
"""Send commands to a qdev-host control socket (--control) and print the replies.
usage: qdev-control.py SOCK "screendump /tmp/a.ppm" "frame" "stats" ...
With --check, a screendump reply is checked against its PPM file: size, and for
test-v2 the frame seq in pixel (0,0) (R,G,B = seq bits 23..0) not older than
the reply's seq. Exit status 1 on an "err" reply or a failed check."""
import socket, sys

args = [a for a in sys.argv[2:] if a != "--check"]
check = "--check" in sys.argv
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
f = s.makefile("rw")
rc = 0
for cmd in args:
    f.write(cmd + "\n"); f.flush()
    reply = f.readline().strip()
    print(f"> {cmd}\n< {reply}")
    if reply.startswith("err"):
        rc = 1
    elif check and cmd.startswith("screendump"):
        path = cmd.split(None, 1)[1]
        _, seq, mode = reply.split()[:3]
        w, h = map(int, mode.split("x"))
        data = open(path, "rb").read()
        hdr = f"P6\n{w} {h}\n255\n".encode()
        ok = data.startswith(hdr) and len(data) == len(hdr) + w * h * 3
        px = data[len(hdr):len(hdr) + 3]
        pseq = px[0] << 16 | px[1] << 8 | px[2]
        ok_seq = pseq >= (int(seq) & 0xffffff)
        print(f"  {'PASS' if ok else 'FAIL'}: PPM {w}x{h}, {len(data)} bytes; pixel(0,0) seq {pseq} vs reply seq {seq}: "
              f"{'PASS' if ok_seq else 'FAIL'}")
        rc |= 0 if ok and ok_seq else 1
sys.exit(rc)
