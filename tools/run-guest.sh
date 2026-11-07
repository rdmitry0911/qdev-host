#!/bin/bash
# Stock QEMU q35 guest with a "Thunderbolt" root port (pcie-root-port + SSDT _DSD),
# a boot VGA, shared guest memory, serial console and QMP on unix sockets.
# Env: QEMU (binary), DISK (qcow2 overlay), KERNEL/INITRD (optional direct boot), RUN (socket dir)
set -e
QEMU=${QEMU:-qemu-system-x86_64}; RUN=${RUN:-/tmp/claude-1000/vfu}; mkdir -p "$RUN"
HERE=$(cd "$(dirname "$0")"/.. && pwd)
ARGS=()
if [ -n "$KERNEL" ]; then
  ARGS+=(-kernel "$KERNEL" -initrd "$INITRD" -append "${APPEND:-console=ttyS0,115200n8 root=/dev/vda1 rw net.ifnames=0}")
fi
exec "$QEMU" -machine q35,accel=kvm -cpu host -m "${MEM:-2G}" -smp "${SMP:-2}" -nodefaults -display none \
  -object memory-backend-memfd,id=m0,size="${MEM:-2G}",share=on -machine memory-backend=m0 \
  -drive file="${DISK:?set DISK}",if=virtio,format=qcow2 \
  -device VGA,id=bootvga,bus=pcie.0,addr=0x06,vgamem_mb=4 \
  -device pcie-root-port,id=tbrp0,bus=pcie.0,chassis=1,slot=1,addr=0x05,pref64-reserve=${PREF64:-256M},mem-reserve=8M \
  -device pcie-root-port,id=tbrp1,bus=pcie.0,chassis=2,slot=2,addr=0x07,pref64-reserve=64M,mem-reserve=8M \
  -device pcie-pci-bridge,id=tbbr0,bus=tbrp1 \
  -acpitable file="$HERE/acpi/ssdt-tb.aml" \
  -serial unix:"$RUN/serial0.sock",server,nowait \
  -qmp unix:"$RUN/qmp0.sock",server,nowait \
  "${ARGS[@]}" "$@"
