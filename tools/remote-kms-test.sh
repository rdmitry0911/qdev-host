#!/bin/bash
# Live-display test on a host with a GPU + monitor (run there). Needs: a stock QEMU
# >= 10.1 (QEMU), the prepared guest (GUEST dir with guest-autologin.qcow2, vmlinuz,
# initrd.img), qdev-host built with VFU_PREFIX, DRM master free on the card.
#   KMS=/dev/dri/card0:eDP-1 QEMU=... GUEST=... VFU_PREFIX=... tools/remote-kms-test.sh
set -u
HERE=$(cd "$(dirname "$0")"/.. && pwd); RUN=${RUN:-/tmp/qdev}; OUT=${OUT:-$HERE/results}; mkdir -p "$RUN" "$OUT"
QEMU=${QEMU:?}; GUEST=${GUEST:?}; KMS=${KMS:?}
QMP=$RUN/qmp0.sock; SER=$RUN/serial0.sock; SOCK=$RUN/tbvga0.sock; DISP=$RUN/tbvga0.display; VMBUS=$RUN/vm-display1.bus
G() { python3 -I "$HERE/tools/guest_cmd.py" "$SER" "$@"; }
T() { python3 -I "$HERE/tools/tbctl.py" "$QMP" "$@"; }
step() { echo; echo "##### $(date +%T) $*"; }

step "0. cleanup (only what a previous run of this script started: pid files in $RUN)"
for f in "$RUN"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null; rm -f "$f"; done
sleep 1; rm -f "$RUN"/*.sock "$RUN"/*.display "$RUN"/*.bus
[ -f "$GUEST/test2.qcow2" ] || "$(dirname "$QEMU")/qemu-img" create -q -f qcow2 -b "$GUEST/guest-autologin.qcow2" -F qcow2 "$GUEST/test2.qcow2" 20G

step "1. private bus for the VM's own Display1 (what QSM Direct's stock profile does)"
dbus-daemon --session --fork --print-address=0 --print-pid=1 --address=unix:path=$VMBUS > "$RUN/dbus.pid"; sleep 0.3

step "2. guest on stock QEMU with -display dbus (VM console: boot VGA), q35, root port with reserved windows"
nohup "$QEMU" -machine q35,accel=kvm -cpu host -m 2G -smp 2 -nodefaults \
  -object memory-backend-memfd,id=m0,size=2G,share=on -machine memory-backend=m0 \
  -drive file="$GUEST/test2.qcow2",if=virtio,format=qcow2 \
  -device VGA,id=bootvga,bus=pcie.0,addr=0x06,vgamem_mb=4 -device usb-ehci,id=ehci -device usb-tablet,bus=ehci.0 -device usb-kbd,bus=ehci.0 \
  -display dbus,addr=unix:path=$VMBUS,gl=off \
  -device pcie-root-port,id=tbrp0,bus=pcie.0,chassis=1,slot=1,addr=0x05,pref64-reserve=${PREF64:-256M},mem-reserve=8M \
  -acpitable file="$HERE/acpi/ssdt-tb.aml" \
  -kernel "$GUEST/vmlinuz" -initrd "$GUEST/initrd.img" -append "console=ttyS0,115200n8 root=/dev/vda1 rw net.ifnames=0 systemd.firstboot=0" \
  -serial unix:$SER,server,nowait -qmp unix:$QMP,server,nowait > "$OUT/qemu.log" 2>&1 &
echo $! > "$RUN/qemu.pid"
for i in $(seq 1 90); do sleep 2; OUT1=$(timeout 15 python3 -I "$HERE/tools/guest_cmd.py" "$SER" "echo READY" 2>/dev/null | grep -c '^READY'); [ "$OUT1" = "1" ] && break; done
echo "guest ready after ~$((i*2)) s"; head -3 "$OUT/qemu.log"

step "3. qdev-host: tb-vga, KMS on $KMS (EDID of that monitor to the guest), Display1 p2p, input forwarded to the VM"
"$HERE/qdev-host" -v --plugin "$HERE/plugins/tb-vga/tb-vga.so" --socket "$SOCK" --plugin-args vram=${VRAM:-64} \
  --kms "$KMS" --display-p2p "$DISP" --qemu-display "unix:path=$VMBUS" --qemu-display-dest org.qemu --qemu-console 0 \
  --dump-dir "$OUT" --fps ${FPS:-60} --stats-interval 10 > "$OUT/qdev-host.log" 2>&1 &
HOST=$!; echo $HOST > "$RUN/qdev-host.pid"; sleep 1; cat "$OUT/qdev-host.log"

step "4. plug, guest driver binds, modes from the real EDID"
T plug tbvga0 "$SOCK" tbrp0; sleep 4
G "lspci -nn -s 01:00.0" "dmesg | grep -iE 'bochs|drm' | tail -4" "cat /sys/class/drm/card1-Virtual-1/modes | head -3"

step "5. static frame -> monitor"
B64=$(base64 -w0 "$HERE/tools/guest-draw.py"); G "echo $B64 | base64 -d > /tmp/guest-draw.py; (nohup python3 /tmp/guest-draw.py 0000:01:00.0 > /tmp/draw.log 2>&1 &); echo started"
for i in $(seq 1 24); do sleep 5; G "cat /tmp/draw.log" | grep -q wrote && break; done; G "cat /tmp/draw.log"
sleep 1; grep -E 'scanout|kms:' "$OUT/qdev-host.log" | tail -4
kill -USR1 $HOST; sleep 3; L=$(ls -t "$OUT"/qdev-fb-*.ppm | head -1); python3 -I "$HERE/tools/ppm2png.py" "$L" "$OUT/kms-static-vram.png" 4
K=$(ls -t "$OUT"/qdev-kms-*.ppm 2>/dev/null | head -1); [ -n "$K" ] && python3 -I "$HERE/tools/ppm2png.py" "$K" "$OUT/kms-static-screen.png" 4

step "6. animation: moving bar 600 frames, then full-frame 300 frames; host stats after each"
B64=$(base64 -w0 "$HERE/tools/guest-animate.py"); G "echo $B64 | base64 -d > /tmp/guest-animate.py"
kill -USR2 $HOST; sleep 0.2
G "python3 /tmp/guest-animate.py 0000:01:00.0 600"; sleep 0.5; kill -USR2 $HOST; sleep 0.5; grep -E 'stats:|kms:' "$OUT/qdev-host.log" | tail -2
G "python3 /tmp/guest-animate.py 0000:01:00.0 300 full"; sleep 0.5; kill -USR2 $HOST; sleep 0.5; grep -E 'stats:|kms:' "$OUT/qdev-host.log" | tail -2

step "7. input forwarding: Display1 probe --input-smoke against qdev-host, guest listens on its input devices"
G "rm -f /tmp/ev-*.txt; for e in /dev/input/event*; do (timeout 12 head -c 48 \$e 2>/dev/null | od -An -tx1 > /tmp/ev-\$(basename \$e).txt &); done; echo listening"
if [ -n "${PROBE:-}" ] && [ -x "$PROBE" ]; then "$PROBE" --dbus-address "unix:path=$DISP" --no-audio --duration-ms 1500 --input-smoke --no-snapshot 2>&1 | grep -E 'input|session errors'; fi
"$HERE/tools/display1-input-bench" "unix:path=$DISP" - 200 2>&1 | tail -1
sleep 12; G "for f in /tmp/ev-*.txt; do [ -s \$f ] && echo \"\$f: \$(head -c 60 \$f)\"; done; ls -l /dev/input/by-path/ | grep -E 'event' | sed 's/.*by-path.//'"
grep -c 'forwarded' "$OUT/qdev-host.log"

step "8. input latency: direct to QEMU vs through qdev-host (2000 round trips each)"
"$HERE/tools/display1-input-bench" "unix:path=$VMBUS" org.qemu 2000
"$HERE/tools/display1-input-bench" "unix:path=$DISP" - 2000

step "9. unplug / replug, monitor blanks and comes back"
T unplug tbvga0; sleep 2; grep -E 'kms:|scanout' "$OUT/qdev-host.log" | tail -2
T plug tbvga0 "$SOCK" tbrp0; sleep 4; G "python3 /tmp/guest-animate.py 0000:01:00.0 60 full" | tail -1; sleep 1; grep -E 'kms:|scanout' "$OUT/qdev-host.log" | tail -2
echo; echo "=== qdev-host log ==="; cat "$OUT/qdev-host.log"
