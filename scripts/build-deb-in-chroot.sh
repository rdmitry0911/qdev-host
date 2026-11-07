#!/bin/bash
# Build the qdev-host debs inside a Debian trixie chroot (created by scripts/mkchroot.sh
# or any minbase chroot with the Build-Depends installed). Output: dist/*.deb
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
C="${QDEV_CHROOT:-/var/tmp/qdev-trixie.chroot}"
OUT="${QDEV_DEB_OUT:-$ROOT/dist}"
[ -f "$C/.ready" ] || { echo "chroot $C not ready" >&2; exit 1; }
sudo -n install -d "$C/work"
sudo -n rm -rf "$C/work/qdev-host"
sudo -n rsync -a --exclude .git --exclude results --exclude dist --exclude 'qdev-host' --exclude '*.so' \
  --exclude 'dbus/dbus-display1.[ch]' --exclude 'tools/display1-input-bench' --exclude 'debian/vfu-*' --exclude 'debian/qdev-host' --exclude 'debian/qdev-host-pve' \
  "$ROOT/" "$C/work/qdev-host/"
sudo -n chroot "$C" sh -c 'cd /work/qdev-host && dpkg-buildpackage -us -uc -b -j4' 2>&1 | tail -25
mkdir -p "$OUT"
cp "$C"/work/*.deb "$OUT"/
ls -la "$OUT"/*.deb
