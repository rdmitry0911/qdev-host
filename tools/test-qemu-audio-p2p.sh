#!/bin/bash
# Self-test (no guest OS): QEMU's Display1 Audio exported by qdev-host when the input goes through QMP.
#   case audio : QEMU -audiodev dbus + ich9-intel-hda/hda-output + -display dbus,p2p=yes,audiodev=  (the external-display
#                profile with sound); qdev-host --display-bus + --qemu-qmp attaches through QMP getfd/add_client and exports
#                /org/qemu/Display1/Audio; a client registers an out listener through qdev-host and QEMU calls Init on it.
#   case plain : QEMU without a dbus display: no Audio, VM.Interfaces without Audio (unchanged behaviour).
# usage: tools/test-qemu-audio-p2p.sh [OUT_DIR]   env: QEMU (qemu-system-x86_64), QEMU_L (-L dir), QDEV (./qdev-host)
set -u
cd "$(dirname "$0")/.."
QEMU=${QEMU:-qemu-system-x86_64}; QDEV=${QDEV:-./qdev-host}; PLUGIN=${PLUGIN:-plugins/tb-vga/tb-vga.so}
OUT=${1:-$(mktemp -d)}; mkdir -p "$OUT"; T=$(mktemp -d); LARG=(); [ -n "${QEMU_L:-}" ] && LARG=(-L "$QEMU_L")
PIDS=(); cleanup(){ for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$T"; }; trap cleanup EXIT
cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$T/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
EOF
fails=0
run_case() {  # name expect qemu-display-args...
  local name=$1 expect=$2; shift 2
  local d="$OUT/$name"; mkdir -p "$d"; rm -f "$T/bus" "$T/qmp.sock" "$T/vfio.sock"
  dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile >"$d/dbus.log" 2>&1 & PIDS+=($!)
  for _ in $(seq 50); do [ -S "$T/bus" ] && break; sleep 0.1; done
  "$QDEV" --plugin "$PLUGIN" --socket "$T/vfio.sock" --display-bus "unix:path=$T/bus" --qemu-qmp "$T/qmp.sock" \
    --vm-name "selftest-$name" >"$d/qdev-host.log" 2>&1 & local qd=$!; PIDS+=($qd)
  "$QEMU" "${LARG[@]}" -machine q35 -accel tcg -m 64 -nodefaults -nographic -S "$@" \
    -qmp "unix:$T/qmp.sock,server=on,wait=off" >"$d/qemu.log" 2>&1 & local qm=$!; PIDS+=($qm)
  for _ in $(seq 100); do
    grep -q "input forwarded to QEMU QMP" "$d/qdev-host.log" && { [ "$expect" = absent ] || grep -q "audio proxied" "$d/qdev-host.log"; } && break
    sleep 0.1; done
  sleep 0.5
  local a=(); [ "$expect" = absent ] && a=(--expect-absent)
  python3 tools/display1-audio-test.py "unix:path=$T/bus" org.qemu 1.5 "${a[@]}" >"$d/client.log" 2>&1; local rc=$?
  echo "case $name (expect $expect): $(grep RESULT "$d/client.log") rc=$rc"
  [ $rc = 0 ] || fails=$((fails + 1))
  kill $qm $qd 2>/dev/null; wait $qm $qd 2>/dev/null
  kill "${PIDS[0]}" 2>/dev/null; wait "${PIDS[0]}" 2>/dev/null; PIDS=()
}
run_case audio present -audiodev dbus,id=snd0,out.frequency=48000,out.channels=2 -device ich9-intel-hda,id=hda \
  -device hda-output,bus=hda.0,audiodev=snd0 -display dbus,p2p=yes,audiodev=snd0
run_case plain absent -display none
echo "test-qemu-audio-p2p: $([ $fails = 0 ] && echo PASS || echo "FAIL ($fails)")"
exit $fails
