#!/bin/bash
# Zero-copy KMS, second-head pointer mapping, Clipboard/Audio proxies: live test on a GPU host.
#   KMS=/dev/dri/card0:eDP-1 QEMU=... GUEST=... PROBE=... tools/remote-features-test.sh
set -u
HERE=$(cd "$(dirname "$0")"/.. && pwd); RUN=${RUN:-/tmp/qdev}; OUT=${OUT:-$HERE/results}; mkdir -p "${RUN:?}" "${OUT:?}"
QEMU=${QEMU:?}; GUEST=${GUEST:?}; KMS=${KMS:?}; PROBE=${PROBE:-}
QMP=$RUN/qmp0.sock; SER=$RUN/serial0.sock; SOCK=$RUN/tbvga0.sock; DISP=$RUN/tbvga0.display; VMBUS=$RUN/vm-display1.bus
G() { python3 -I "$HERE/tools/guest_cmd.py" "$SER" "$@"; }
T() { python3 -I "$HERE/tools/tbctl.py" "$QMP" "$@"; }
step() { echo; echo "##### $(date +%T) $*"; }

if [ -n "${SKIP_VM:-}" ]; then
step "0. reuse the running guest; restart qdev-host only"
python3 -I "$HERE/tools/tbctl.py" "$QMP" pci | grep -q 'id=tbvga0' && T unplug tbvga0
[ -f "$RUN/qdev-host.pid" ] && kill "$(cat "$RUN/qdev-host.pid")" 2>/dev/null; sleep 1; rm -f "${RUN:?}"/tbvga0.sock "${RUN:?}"/tbvga0.display "${OUT:?}"/qdev-fb-*.ppm
else
step "0. cleanup (own pid files only)"
for f in "${RUN:?}"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null; rm -f "$f"; done
sleep 1; rm -f "${RUN:?}"/*.sock "${RUN:?}"/*.display "${RUN:?}"/*.bus "${OUT:?}"/qdev-fb-*.ppm "${OUT:?}"/qdev-kms-*.ppm
[ -f "$GUEST/test2.qcow2" ] || "$(dirname "$QEMU")/qemu-img" create -q -f qcow2 -b "$GUEST/guest-autologin.qcow2" -F qcow2 "$GUEST/test2.qcow2" 20G

step "1. VM: -display dbus on a private bus, dbus audio (ich9-hda), usb-tablet/usb-kbd, root port 256M window"
dbus-daemon --session --fork --print-address=0 --print-pid=1 --address=unix:path=$VMBUS > "$RUN/dbus.pid"; sleep 0.3
nohup "$QEMU" -machine q35,accel=kvm -cpu host -m 2G -smp 2 -nodefaults \
  -object memory-backend-memfd,id=m0,size=2G,share=on -machine memory-backend=m0 \
  -drive file="$GUEST/test2.qcow2",if=virtio,format=qcow2 \
  -device VGA,id=bootvga,bus=pcie.0,addr=0x06,vgamem_mb=4 -device usb-ehci,id=ehci -device usb-tablet,bus=ehci.0 -device usb-kbd,bus=ehci.0 \
  -audiodev dbus,id=snd0 -device ich9-intel-hda,id=hda -device hda-duplex,audiodev=snd0 \
  -display dbus,addr=unix:path=$VMBUS,gl=off,audiodev=snd0 \
  -device pcie-root-port,id=tbrp0,bus=pcie.0,chassis=1,slot=1,addr=0x05,pref64-reserve=256M,mem-reserve=8M \
  -acpitable file="$HERE/acpi/ssdt-tb.aml" \
  -kernel "$GUEST/vmlinuz" -initrd "$GUEST/initrd.img" -append "console=ttyS0,115200n8 root=/dev/vda1 rw net.ifnames=0 systemd.firstboot=0" \
  -serial unix:$SER,server,nowait -qmp unix:$QMP,server,nowait > "$OUT/qemu.log" 2>&1 &
echo $! > "$RUN/qemu.pid"
for i in $(seq 1 100); do sleep 2; R=$(timeout 15 python3 -I "$HERE/tools/guest_cmd.py" "$SER" "echo READY" 2>/dev/null | grep -c '^READY'); [ "$R" = "1" ] && break; done
echo "guest ready after ~$((i*2)) s"; head -3 "$OUT/qemu.log"
fi

step "2. qdev-host: zero-copy KMS, input forwarding (head right of the VM console), clipboard/audio proxies"
"$HERE/qdev-host" -v --plugin "$HERE/plugins/tb-vga/tb-vga.so" --socket "$SOCK" --plugin-args vram=${VRAM:-128} \
  --kms "$KMS" --display-p2p "$DISP" --qemu-display "unix:path=$VMBUS" --qemu-display-dest org.qemu --qemu-console 0 \
  --dump-dir "$OUT" --fps 60 --idle-fps 10 > "$OUT/qdev-host.log" 2>&1 &
HOST=$!; echo $HOST > "$RUN/qdev-host.pid"; sleep 1; cat "$OUT/qdev-host.log"

step "3. plug; guest sets the panel mode (raw path, driver detached); zero-copy scanout expected"
T plug tbvga0 "$SOCK" tbrp0; sleep 5
B64=$(base64 -w0 "$HERE/tools/guest-animate.py"); G "echo $B64 | base64 -d > /tmp/guest-animate.py; python3 /tmp/guest-animate.py 0000:01:00.0 60 full 30 raw" | grep -E 'guest:|raw'
sleep 1; grep -E 'zero-copy|scanout|kms:' "$OUT/qdev-host.log" | tail -4

step "4. load with zero-copy, no Display1 listener: 600 frames bar 60 fps, 600 full 60 fps (expect 0 MB copied, idle-like CPU)"
for spec in "600 bar 60" "600 full 60"; do set -- $spec
  kill -USR2 $HOST; sleep 0.3; G "python3 /tmp/guest-animate.py 0000:01:00.0 $1 $2 $3 raw" | grep guest:; sleep 0.5; kill -USR2 $HOST; sleep 0.5; grep -E 'stats:|kms: eDP' "$OUT/qdev-host.log" | tail -2
done
kill -USR1 $HOST; sleep 4; L=$(ls -t "$OUT"/qdev-fb-*.ppm | head -1); python3 -I "$HERE/tools/ppm2png.py" "$L" "$OUT/zc-screen.png" 4

step "5. Display1 probe with audio (listener attached -> polling resumes; audio out listener relayed to the VM)"
if [ -x "$PROBE" ]; then
  G "which aplay speaker-test; ls /dev/snd | tr '\n' ' '"
  G "(nohup python3 /tmp/guest-animate.py 0000:01:00.0 120 bar 60 raw > /dev/null 2>&1 &); echo bg"
  "$PROBE" --dbus-address "unix:path=$DISP" --duration-ms 3000 --snapshot "$OUT/probe.ppm" 2>&1 | grep -E 'scanout|updates|audio|session errors'
  python3 -I "$HERE/tools/ppm2png.py" "$OUT/probe.ppm" "$OUT/probe.png" 4
  grep -E 'audio|listener' "$OUT/qdev-host.log" | tail -3
fi

step "6. second-head pointer mapping: SetAbsPosition on this display -> tablet value in the guest"
grep -E 'input forwarded' "$OUT/qdev-host.log" | tail -1
B64=$(base64 -w0 "$HERE/tools/guest-abs-read.py"); G "echo $B64 | base64 -d > /tmp/guest-abs-read.py; (nohup python3 /tmp/guest-abs-read.py /dev/input/event3 6 > /tmp/abs.log 2>&1 &); echo listening"
sleep 1; python3 "$HERE/tools/display1-abs-test.py" "unix:path=$DISP" - 100 50; sleep 0.5; python3 "$HERE/tools/display1-abs-test.py" "unix:path=$DISP" - 1920 1200
sleep 6; G "cat /tmp/abs.log"
echo "expected for (1920,1200) with VM console 640x480, head origin (640,0), desktop 4480x2400: x'=(640+1920)*640/4480=365 -> ABS_X=365*32767/639=18717; y'=1200*480/2400=240 -> ABS_Y=240*32767/479=16417"

step "7. clipboard proxy: client registers/grabs through qdev-host; dbus-monitor on the VM bus shows the relayed calls"
timeout 8 dbus-monitor --address "unix:path=$VMBUS" "interface=org.qemu.Display1.Clipboard" > "$OUT/dbus-monitor.txt" 2>&1 &
MON=$!; sleep 1
python3 "$HERE/tools/display1-clipboard-test.py" "unix:path=$DISP" - 2
sleep 1; kill $MON 2>/dev/null; grep -E 'member=|error_name|string "|uint32' "$OUT/dbus-monitor.txt" | head -20
grep -E 'clipboard' "$OUT/qdev-host.log" | tail -2
echo "VM.Interfaces as seen by a client of qdev-host:"; python3 - <<PY
import gi; gi.require_version("Gio","2.0"); from gi.repository import Gio, GLib
c=Gio.DBusConnection.new_for_address_sync("unix:path=$DISP", Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
print(c.call_sync(None, "/org/qemu/Display1/VM", "org.freedesktop.DBus.Properties", "Get", GLib.Variant("(ss)", ("org.qemu.Display1.VM","Interfaces")), None, Gio.DBusCallFlags.NONE, 5000, None).unpack())
PY
echo "=== DONE ==="
