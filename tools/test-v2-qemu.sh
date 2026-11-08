#!/bin/bash
# qdev-host + test-v2 driven by a stock QEMU >= 11.0 (qtest accelerator: no guest code runs, no image).
# usage: tools/test-v2-qemu.sh OUTDIR   (repo root, after make). Env: QEMU (binary),
# QEMU_DATA (its pc-bios directory; default: ../pc-bios from the binary, as in a build tree).
set -u
OUT=${1:?outdir}; mkdir -p "$OUT"
Q=${QEMU:-qemu-system-x86_64}
DATA=${QEMU_DATA:-$(dirname "$(command -v "$Q")")/../pc-bios}
R=${RUN:-/tmp/qdev-qt-$$}; mkdir -p "$R"; rm -f "$R"/*.sock
DRIVE=$(dirname "$0")/test-v2-qemu-drive.py
timeout 90 ./qdev-host --plugin plugins/test-v2/test-v2.so --plugin-args fps=10 --socket "$R/t2.sock" \
    --display-p2p "$R/d1.sock" --control "$R/ctl.sock" --qemu-qmp "$R/qmp.sock" -v > "$OUT/qemu-qtest.host.log" 2>&1 &
HPID=$!
for _ in $(seq 50); do [ -S "$R/t2.sock" ] && break; sleep 0.1; done
timeout 80 "$Q" -machine q35,accel=qtest -nodefaults -display none -m 256M -L "$DATA" \
    -object memory-backend-memfd,id=m0,size=256M,share=on -machine memory-backend=m0 \
    -device '{"driver":"vfio-user-pci","socket":{"path":"'"$R/t2.sock"'","type":"unix"},"id":"t2","addr":"0x5"}' \
    -device qemu-xhci,id=xhci,addr=0x6 -device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
    -qmp unix:"$R/qmp.sock",server=on,wait=off -qmp unix:"$R/qmp2.sock",server=on,wait=off \
    -qtest unix:"$R/qtest.sock",server=on,wait=off -serial none -monitor none \
    -trace 'vfio_user_dma_*' -trace vfio_intx_interrupt -trace vfio_msi_interrupt -trace vfio_msi_enable \
    -trace 'input_event_*' -D "$OUT/qemu-qtest.qemu-trace.log" > "$OUT/qemu-qtest.qemu.log" 2>&1 &
QPID=$!
for _ in $(seq 50); do [ -S "$R/qtest.sock" ] && [ -S "$R/qmp2.sock" ] && break; sleep 0.1; done
sleep 0.5
timeout 40 python3 "$DRIVE" "$R" > "$OUT/qemu-qtest.drive.log" 2>&1; rc=$?
wait "$QPID" 2>/dev/null
sleep 0.3
kill -TERM "$HPID" 2>/dev/null; wait "$HPID" 2>/dev/null
rm -rf "$R"
echo "drive exit $rc"
exit $rc
