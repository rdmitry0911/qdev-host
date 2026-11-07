#!/bin/bash
# Create a Debian trixie chroot with the Build-Depends of qdev-host (needs passwordless sudo and debootstrap).
set -e
C=/var/tmp/qdev-trixie.chroot
if [ ! -f $C/.ready ]; then
  sudo -n rm -rf $C.partial; sudo -n debootstrap --variant=minbase --arch=amd64 trixie $C.partial https://deb.debian.org/debian > /var/tmp/qdev-debootstrap.log 2>&1
  sudo -n mv $C.partial $C
  printf '#!/bin/sh\nexit 101\n' | sudo -n tee $C/usr/sbin/policy-rc.d >/dev/null; sudo -n chmod 755 $C/usr/sbin/policy-rc.d
  sudo -n chroot $C sh -c 'apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends build-essential debhelper dh-python pkgconf meson ninja-build libglib2.0-dev libglib2.0-dev-bin libdrm-dev libjson-c-dev libcmocka-dev python3 ca-certificates' > /var/tmp/qdev-chroot-apt.log 2>&1
  sudo -n touch $C/.ready
fi
echo "chroot ready: $C"; sudo -n chroot $C dpkg -l debhelper meson libglib2.0-dev libdrm-dev libjson-c-dev | awk '/^ii/ {print $2, $3}'
