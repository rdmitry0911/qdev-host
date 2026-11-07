#!/usr/bin/env python3
# Runs inside the guest: print the last ABS_X/ABS_Y reported by an input device within N seconds.
import os, struct, sys, time, select
dev, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK); end = time.time() + secs; ax = ay = None
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if not r: continue
    try: data = os.read(fd, 24 * 64)
    except BlockingIOError: continue
    for i in range(0, len(data) - 23, 24):
        _, _, t, code, value = struct.unpack_from("<qqHHi", data, i)
        if t == 3 and code == 0: ax = value
        if t == 3 and code == 1: ay = value
print(f"ABS_X={ax} ABS_Y={ay}")
