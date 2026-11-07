#!/bin/bash
# End-to-end check on a stock QEMU guest (see tools/run-guest.sh):
#   start qdev-host with the tb-vga plugin -> hot-plug it behind the root port ->
#   the guest's bochs-drm binds -> the guest draws a pattern -> the frame is read
#   back through org.qemu.Display1 by qemu-display-probe (QSM Direct's tool) and
#   through SIGUSR1 -> unplug / replug with state kept.
# Env: RUN (socket dir), PROBE (qemu-display-probe binary, optional), OUT (results dir)
set -u
HERE=$(cd "$(dirname "$0")"/.. && pwd); RUN=${RUN:-/tmp/qdev}; OUT=${OUT:-$HERE/results}; mkdir -p "$RUN" "$OUT"
QMP=$RUN/qmp0.sock; SER=$RUN/serial0.sock; SOCK=$RUN/tbvga0.sock; DISP=$RUN/tbvga0.display
G() { python3 -I "$HERE/tools/guest_cmd.py" "$SER" "$@"; }
T() { python3 -I "$HERE/tools/tbctl.py" "$QMP" "$@"; }
step() { echo; echo "##### $*"; }

step "0. qdev-host + tb-vga (cleanup first)"
python3 -I "$HERE/tools/tbctl.py" "$QMP" pci | grep -q 'id=tbvga0' && T unplug tbvga0
for p in $(pgrep -x qdev-host); do kill $p; done; sleep 0.5
rm -f "$SOCK" "$DISP"
"$HERE/qdev-host" -v --plugin "$HERE/plugins/tb-vga/tb-vga.so" --socket "$SOCK" \
  --plugin-args "edid=$HERE/plugins/tb-vga/edid-1280x800.bin" --display-p2p "$DISP" \
  --dump-dir "$OUT" > "$OUT/qdev-host.log" 2>&1 &
HOST=$!; for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done; echo "qdev-host pid $HOST"

step "1. plug behind tbrp0"
T plug tbvga0 "$SOCK" tbrp0; sleep 4
G "lspci -nn -s 01:00.0" "lspci -vv -s 01:00.0 | grep -E 'Region|Kernel driver'" "dmesg | grep -iE 'bochs|drm' | tail -5"

step "2. guest draws 1280x800 on the device"
B64=$(base64 -w0 "$HERE/tools/guest-draw.py")
G "echo $B64 | base64 -d > /tmp/guest-draw.py; python3 /tmp/guest-draw.py 0000:01:00.0"
sleep 0.5; grep 'scanout' "$OUT/qdev-host.log" | tail -1

step "3. frame via org.qemu.Display1 (QSM Direct probe) and via SIGUSR1"
if [ -n "${PROBE:-}" ] && [ -x "$PROBE" ]; then
  "$PROBE" --dbus-address "unix:path=$DISP" --no-audio --duration-ms 2000 --snapshot "$OUT/probe.ppm" 2>&1 | grep -E 'scanout|updates|session errors'
  python3 -I "$HERE/tools/ppm2png.py" "$OUT/probe.ppm" "$OUT/probe.png"
else
  echo "PROBE not set, skipping qemu-display-probe"
fi
kill -USR1 $HOST; sleep 1; L=$(ls -t "$OUT"/qdev-fb-*.ppm | head -1); python3 -I "$HERE/tools/ppm2png.py" "$L" "$OUT/sigusr1.png"

step "4. unplug / replug (device state lives in qdev-host)"
T unplug tbvga0; sleep 2; G "lspci -nn -s 01:00.0 || echo 'gone from guest'"
T plug tbvga0 "$SOCK" tbrp0; sleep 4; G "lspci -nn -s 01:00.0; dmesg | grep -i bochs | tail -2"
grep -c 'client detached' "$OUT/qdev-host.log"

step "5. disconnect / connect (Thunderbolt 'connected' toggle)"
T disconnect tbrp0; sleep 2; T connect tbrp0 tbvga0="$SOCK"; sleep 3; G "lspci -nn -s 01:00.0"
echo; tail -12 "$OUT/qdev-host.log"
