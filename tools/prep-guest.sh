#!/bin/bash
# Prepare a Debian 13 test guest without root privileges on the host:
#  1. download the nocloud image, 2. extract kernel + initrd (GPT parse + debugfs),
#  3. inject a serial autologin override for root, 4. create a qcow2 overlay.
# Usage: QEMU_BUILD=/path/to/qemu/build prep-guest.sh <workdir>
set -e
W=${1:?workdir}; mkdir -p "$W"; cd "$W"
QI=${QEMU_BUILD:?set QEMU_BUILD}/qemu-img
IMG=debian-13-nocloud-amd64.qcow2
[ -f $IMG ] || curl -fL -o $IMG https://cloud.debian.org/images/cloud/trixie/latest/$IMG
$QI convert -O raw $IMG base.raw
read START CNT < <(python3 - <<'PY'
import struct
f=open('base.raw','rb'); f.seek(512); hdr=f.read(92)
_,_,_,_,_,_,_,_,_,_,pe_lba,npe,pesz,_=struct.unpack('<8sIIIIQQQQ16sQIII',hdr)
f.seek(pe_lba*512); best=None
for i in range(npe):
    e=f.read(pesz); tg,pg,fl,ll=struct.unpack('<16s16sQQ',e[:48])
    if tg==b'\0'*16: continue
    if best is None or (ll-fl)>best[1]: best=(fl,ll-fl+1)
print(best[0],best[1])
PY
)
dd if=base.raw of=root.ext4 bs=512 skip=$START count=$CNT status=none
K=$(debugfs -R "ls /boot" root.ext4 2>/dev/null | tr ' ' '\n' | grep '^vmlinuz-' | head -1)
I=$(debugfs -R "ls /boot" root.ext4 2>/dev/null | tr ' ' '\n' | grep '^initrd.img-' | head -1)
debugfs -R "dump /boot/$K vmlinuz" root.ext4 2>/dev/null
debugfs -R "dump /boot/$I initrd.img" root.ext4 2>/dev/null
cat > override.conf <<'CONF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear --keep-baud 115200,57600,38400,9600 - $TERM
CONF
debugfs -w -R "mkdir /etc/systemd/system/serial-getty@ttyS0.service.d" root.ext4 >/dev/null 2>&1 || true
debugfs -w -R "write override.conf /etc/systemd/system/serial-getty@ttyS0.service.d/override.conf" root.ext4 >/dev/null 2>&1
dd if=root.ext4 of=base.raw bs=512 seek=$START conv=notrunc status=none
$QI convert -O qcow2 base.raw guest-autologin.qcow2
rm -f base.raw root.ext4 override.conf
$QI create -q -f qcow2 -b guest-autologin.qcow2 -F qcow2 test2.qcow2 20G
echo "kernel: $W/vmlinuz  initrd: $W/initrd.img  disk: $W/test2.qcow2"
echo 'boot: KERNEL=.../vmlinuz INITRD=.../initrd.img DISK=.../test2.qcow2 APPEND="console=ttyS0,115200n8 root=/dev/vda1 rw net.ifnames=0 systemd.firstboot=0" run-guest.sh'
