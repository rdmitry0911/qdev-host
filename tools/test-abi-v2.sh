#!/bin/bash
# No-VM test of the qdev-host plugin ABI: test-v2 (ABI 2) and tb-vga (ABI 1)
# driven by tools/vfu-test-client, a Display1 listener and the control socket.
#   tools/test-abi-v2.sh OUTDIR [TBVGA_SO]
# TBVGA_SO: a tb-vga.so to check instead of the in-tree one (e.g. a v1 binary
# built from an older tree). Env: QDEV_HOST (host binary, default ./qdev-host),
# ONLY_TBVGA=1 (skip the test-v2 steps, e.g. for an older host). Runs from the
# repo root after "make". Every qdev-host is started with a hard timeout and
# stopped with SIGTERM by PID.
set -u
OUT=${1:?outdir}; TBVGA=${2:-plugins/tb-vga/tb-vga.so}
HOST=${QDEV_HOST:-./qdev-host}
RUN=${RUN:-/tmp/qdev-test-$$}      # short: unix socket paths are limited to 108 bytes
mkdir -p "$OUT" "$RUN"
rc=0
start_host() {   # name args...
    local name=$1; shift
    timeout 120 "$HOST" "$@" > "$OUT/$name.host.log" 2>&1 &
    PID=$!
    for _ in $(seq 50); do [ -S "$RUN/dev.sock" ] && break; sleep 0.1; done
}
stop_host() { kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; rm -f "$RUN"/*.sock; }
step() { echo "== $*" | tee -a "$OUT/summary.txt"; }
result() { local r=$1 what=$2; echo "   $what: $([ "$r" = 0 ] && echo PASS || echo "FAIL ($r)")" | tee -a "$OUT/summary.txt"; [ "$r" = 0 ] || rc=1; }
: > "$OUT/summary.txt"

if [ -z "${ONLY_TBVGA:-}" ]; then
step "test-v2: DMA regions with fd/offset, MSI from a plugin thread, eventfd swaps, remap under load, reset"
start_host t2-vfu --plugin plugins/test-v2/test-v2.so --socket "$RUN/dev.sock" -v
timeout 60 ./tools/vfu-test-client "$RUN/dev.sock" info cfg expect32 bar0 0 0x7e570002 dma msi irqs 3 1000 \
    swap 200 5000 remap 100 2000 reset expect32 bar0 0x34 0 > "$OUT/t2-vfu.client.log" 2>&1
result $? "vfu-test-client"
sleep 0.2; stop_host
grep -q "violations 0" "$OUT/t2-vfu.host.log"; result $? "plugin teardown reports 0 host rule violations"
grep -q "lost connection with 4 regions still mapped" "$OUT/t2-vfu.host.log"
result $? "reset(LOST_CONN) delivered while the 4 DMA regions are still mapped"

step "test-v2: frames (plugin damage), cursor, control socket"
start_host t2-display --plugin plugins/test-v2/test-v2.so --plugin-args fps=30 --socket "$RUN/dev.sock" \
    --control "$RUN/ctl.sock" --display-p2p "$RUN/d1.sock"
timeout 20 python3 tools/display1-listener-test.py "unix:path=$RUN/d1.sock" 3 --expect-cursor --expect-frames 60 \
    > "$OUT/t2-display.listener.log" 2>&1
result $? "Display1 listener: Scanout, Update per frame, CursorDefine, MouseSet"
timeout 20 python3 tools/qdev-control.py "$RUN/ctl.sock" frame "screendump $RUN/a.ppm" "screendump $RUN/b.ppm" frame stats \
    --check > "$OUT/t2-display.control.log" 2>&1
result $? "control socket: screendump waits for the next frame, PPM matches the reply seq"
stop_host

step "test-v2: initial get_scanout() after setup (no frame_ready), QDP_RESET_EXIT on the lost client"
start_host t2-exit --plugin plugins/test-v2/test-v2.so --plugin-args exit_on_disconnect=1 --socket "$RUN/dev.sock" \
    --control "$RUN/ctl.sock"
timeout 10 python3 tools/qdev-control.py "$RUN/ctl.sock" frame > "$OUT/t2-exit.control.log" 2>&1
grep -q "< ok 0 640x480" "$OUT/t2-exit.control.log"; result $? "scanout known before any frame: $(tail -1 "$OUT/t2-exit.control.log")"
timeout 20 ./tools/vfu-test-client "$RUN/dev.sock" dma msi > "$OUT/t2-exit.client.log" 2>&1
result $? "vfu-test-client (maps, then disconnects)"
t0=$(date +%s.%N); wait "$PID"; st=$?; t1=$(date +%s.%N)
echo "   host exit status $st after $(echo "$t1 - $t0" | bc) s" | tee -a "$OUT/summary.txt"
[ "$st" = 0 ] && grep -q "QDP_RESET_EXIT): exiting" "$OUT/t2-exit.host.log" && grep -q "teardown:.*regions at lost connection 4" "$OUT/t2-exit.host.log"
result $? "host tore the plugin down and exited 0 by itself"
rm -f "$RUN"/*.sock
fi

step "tb-vga ($TBVGA): load, config space, EDID/DISPI/QEXT registers, mode set, VRAM write, PPM"
start_host tbvga --plugin "$TBVGA" --plugin-args edid=plugins/tb-vga/edid-1280x800.bin --socket "$RUN/dev.sock" \
    --dump-dir "$RUN" ${TBVGA_CONTROL:+--control "$RUN/ctl.sock"}
timeout 30 ./tools/vfu-test-client "$RUN/dev.sock" info cfg expect32 cfg 0 0x11111234 expect32 cfg 8 0x03000000 \
    expect32 bar2 0 0xffffff00 expect32 bar2 4 0x00ffffff expect16 bar2 0x500 0xb0c5 expect16 bar2 0x514 256 \
    expect32 bar2 0x600 8 expect32 bar2 0x604 0x1e1e1e1e \
    w16 bar2 0x502 640 w16 bar2 0x504 480 w16 bar2 0x506 32 w16 bar2 0x508 0x41 expect16 bar2 0x508 0x41 \
    w32 bar0 0 0x00ff0000 w32 bar0 4 0x0000ff00 w32 bar0 2556 0x000000ff expect32 bar0 4 0x0000ff00 sleep 400 \
    > "$OUT/tbvga.client.log" 2>&1
result $? "vfu-test-client against tb-vga"
kill -USR1 "$(pgrep -P "$PID")"; sleep 0.5      # the qdev-host child of timeout(1)
python3 - "$RUN/qdev-fb-0.ppm" > "$OUT/tbvga.ppm.log" 2>&1 <<'PY'
import sys
d = open(sys.argv[1], "rb").read(); h = b"P6\n640 480\n255\n"
ok = d.startswith(h) and len(d) == len(h) + 640 * 480 * 3 and d[len(h):len(h) + 6] == bytes([255, 0, 0, 0, 255, 0]) \
     and d[len(h) + 639 * 3:len(h) + 640 * 3] == bytes([0, 0, 255])
print("PPM", len(d), "bytes, first pixels", d[len(h):len(h) + 6].hex(), "pixel 639", d[len(h) + 639 * 3:len(h) + 640 * 3].hex())
sys.exit(0 if ok else 1)
PY
result $? "SIGUSR1 PPM of the 640x480 mode shows the written pixels"
cp "$RUN/qdev-fb-0.ppm" "$OUT/tbvga-fb-0.ppm" 2>/dev/null
stop_host
rm -rf "$RUN"
echo "overall: $([ $rc = 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $rc
