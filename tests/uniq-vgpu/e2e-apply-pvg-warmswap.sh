#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# e2e-apply-pvg-warmswap.sh - end-to-end demo of `uniq-vgpu apply` + the guest-sudo
# `uniq-vgpu-guest-install` against a STANDING warm-swap-capable eee1 guest, via its
# --control socket (device-swap; NO fresh boot). It is the live repro of the proof
# recorded in evidence/runs/uniq-vgpu-update-20261117/{apply-live-4080,guestinstall-live-4080}.
#
# It is NON-DISRUPTIVE: the host backend is swapped B then returned to A, and the
# guest kext install runs with --no-reboot and is rolled back (AuxKC restored
# byte-for-byte), so the standing guest is left exactly as found.
#
# Run it ON the stand that owns the control socket (the unix socket is local), e.g.:
#   WARM_DIR=~/warm-4080 \
#   CTL=/dev/shm/uniqvgpu-warm-4080/ctl.sock \
#   GUEST_SSH='-p 3342 devops@127.0.0.1' \
#   KEXT_TAR=~/UniqVGPU.kext.tar \
#   tests/uniq-vgpu/e2e-apply-pvg-warmswap.sh
#
# Env (all have 4080-stand defaults):
#   WARM_DIR   the standing-guest package dir (has pkg/qdev/{tools/qdevctl,plugins/apple-pvg/backing_*.so})
#   CTL        the qdev-host --control socket of the standing guest
#   GUEST_SSH  ssh args to reach the guest's macOS (for WindowServer-pid / optional remote install)
#   KEXT_TAR   the resident UniqVGPU.kext (.tar); binary sha 43118cc0..., uuid AACD3BB1...
#   BIN        dir holding uniq-vgpu / uniq-vgpu-mkbundle / uniq-vgpu-guest-install (default: this repo's bin)
#   DO_GUEST   1 to also push+install the kext over GUEST_SSH (then roll it back); 0 = host-only (default 1)
set -u
SELF=$(cd "$(dirname "$0")" && pwd)
WT=$(cd "$SELF/../.." && pwd)
BIN=${BIN:-$WT/bin}
WARM_DIR=${WARM_DIR:-$HOME/warm-4080}
CTL=${CTL:-/dev/shm/uniqvgpu-warm-4080/ctl.sock}
GUEST_SSH=${GUEST_SSH:--p 3342 devops@127.0.0.1}
KEXT_TAR=${KEXT_TAR:-$HOME/uniqvgpu-apply-test/UniqVGPU.kext.tar}
DO_GUEST=${DO_GUEST:-1}
QDEVCTL="$WARM_DIR/pkg/qdev/tools/qdevctl"
BACKING_B="$WARM_DIR/pkg/qdev/plugins/apple-pvg/backing_B.so"
BACKING_A="$WARM_DIR/pkg/qdev/plugins/apple-pvg/backing_A.so"
OUT=${OUT:-/tmp/uniq-vgpu-apply-e2e-$$}; mkdir -p "$OUT"
KSHA=43118cc09d51951e5c28c04c518a59f655234d2c99b0f4332ae6fbc3852c1c93
KUUID=AACD3BB1-145E-3246-81CD-C70B581D32E5
SSHO="-o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new"
pass=0; fail=0
ok(){ echo "  PASS $*"; pass=$((pass+1)); }
no(){ echo "  FAIL $*"; fail=$((fail+1)); }
say(){ echo "=== $* ==="; }

for f in "$QDEVCTL" "$BACKING_B" "$BACKING_A" "$KEXT_TAR"; do
  [ -e "$f" ] || { echo "MISSING: $f (set WARM_DIR / KEXT_TAR)"; exit 2; }
done

# small, fast health probe over the standing guest's (large, live) qdev-host.log
cat > "$OUT/hp.sh" <<HP
#!/bin/bash
L="$WARM_DIR/run/qdev-host.log"
W="\$(tail -c 60000000 "\$L" 2>/dev/null)"
c(){ printf '%s' "\$W" | grep -ciE "\$1"; }
printf 'qmu_error=%s panics=%s gpu_hang=%s gpuRestart=%s\n' \
  "\$(c 'qmu error')" "\$(c 'kernel panic|panic\\(')" "\$(c 'gpu_hang|gpu hang')" "\$(c 'gpurestart')"
HP
chmod +x "$OUT/hp.sh"

ABI0=$(python3 "$QDEVCTL" device-abi --control "$CTL"); echo "PRE abi: $ABI0"
echo "$ABI0" | grep -q "envelope=pvg-envelope-1" && ok "resident envelope pvg-envelope-1" || no "envelope"
WS0=$(timeout 15 ssh $SSHO $GUEST_SSH 'pgrep -x WindowServer' 2>/dev/null | head -1)

say "mkbundle: host backing_B + resident kext (envelope pvg-envelope-1)"
python3 "$BIN/uniq-vgpu-mkbundle" "$OUT/b-warm" --name pvg-gpu --version 0.2-warmswapB \
  --envelope pvg-envelope-1 --backing "$BACKING_B" --kext "$KEXT_TAR" \
  --kext-sha "$KSHA" --kext-uuid "$KUUID" --kext-build 25G342 --kext-build 25C56 >/dev/null \
  && ok "warm bundle built" || no "mkbundle warm"
python3 "$BIN/uniq-vgpu-mkbundle" "$OUT/b-reboot" --name pvg-next --version 1.0-env2 \
  --envelope pvg-envelope-2 --backing "$BACKING_B" --kext "$KEXT_TAR" --kext-uuid "$KUUID" >/dev/null \
  && ok "reboot bundle built (pvg-envelope-2)" || no "mkbundle reboot"

T=(--control "$CTL" --qdevctl "$QDEVCTL" --state-dir "$OUT/state" --health-cmd "$OUT/hp.sh")

say "apply --dry-run (warm): WARM plan + guest install required"
python3 "$BIN/uniq-vgpu" apply "$OUT/b-warm" --dry-run "${T[@]}" > "$OUT/dryrun.txt" 2>&1
grep -q "host: WARM (no reboot)" "$OUT/dryrun.txt" && ok "dry-run WARM" || no "dry-run WARM"
grep -q "guest kext: install required" "$OUT/dryrun.txt" && ok "dry-run flags guest kext" || no "dry-run guest"

if [ "$DO_GUEST" = "1" ]; then
  say "apply --guest-ssh (host warm swap + push+install kext over the channel, --no-reboot)"
  python3 "$BIN/uniq-vgpu" apply "$OUT/b-warm" "${T[@]}" --guest-ssh "${GUEST_SSH##* }" > "$OUT/apply-guest.txt" 2>&1
  rc=$?
  grep -q "exec channel: ssh" "$OUT/apply-guest.txt" && ok "apply auto-detected the SSH channel" || no "channel"
  grep -q "present in AuxKC" "$OUT/apply-guest.txt" && ok "remote guest-install put the kext in the AuxKC" || no "remote install"
  POST=$(timeout 20 ssh $SSHO $GUEST_SSH "sudo /usr/bin/kmutil inspect --show-kext-uuids -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -i uniqvgpu" 2>/dev/null)
  echo "$POST" | grep -qi dev.uniqvgpu.driver && ok "AuxKC carries dev.uniqvgpu.driver" || no "AuxKC missing uniqvgpu"
  say "guest rollback (restore AuxKC byte-for-byte, no reboot)"
  timeout 60 ssh $SSHO $GUEST_SSH "sudo /tmp/uniq-vgpu-guest-install --rollback --no-reboot --yes" > "$OUT/guest-rollback.txt" 2>&1
  POST2=$(timeout 20 ssh $SSHO $GUEST_SSH "sudo /usr/bin/kmutil inspect --show-kext-uuids -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -i uniqvgpu || echo NONE" 2>/dev/null)
  [ "$POST2" = "NONE" ] && ok "guest rollback removed uniqvgpu from AuxKC" || no "rollback left uniqvgpu"
else
  say "apply (warm, no channel): host swap + print guest one-liner (exit 21)"
  python3 "$BIN/uniq-vgpu" apply "$OUT/b-warm" "${T[@]}" > "$OUT/apply.txt" 2>&1; rc=$?
  [ "$rc" = 21 ] && ok "exit 21 (host warm, guest kext required)" || no "rc=$rc"
  grep -q "sudo uniq-vgpu-guest-install" "$OUT/apply.txt" && ok "prints guest one-liner" || no "one-liner"
fi

say "apply (reboot bundle, pvg-envelope-2): REBOOT required, NO live swap"
python3 "$BIN/uniq-vgpu" apply "$OUT/b-reboot" "${T[@]}" --state-dir "$OUT/state-rb" > "$OUT/reboot.txt" 2>&1
[ "$?" = 20 ] && ok "reboot path exit 20" || no "reboot exit"
grep -q "REBOOT REQUIRED" "$OUT/reboot.txt" && ok "clear REBOOT REQUIRED" || no "reboot msg"

say "leave host as found: device back on backing A"
python3 "$QDEVCTL" device-swap --control "$CTL" "$BACKING_A" >/dev/null 2>&1 && ok "device reset to A" || no "reset A"
WS1=$(timeout 15 ssh $SSHO $GUEST_SSH 'pgrep -x WindowServer' 2>/dev/null | head -1)
[ -n "$WS0" ] && [ "$WS1" = "$WS0" ] && ok "WindowServer pid unchanged ($WS1) = no guest reboot" || echo "  INFO WindowServer $WS0->$WS1"

say "SUMMARY PASS=$pass FAIL=$fail  (evidence: $OUT)"
[ "$fail" = 0 ] && echo "E2E-APPLY: PASS" || echo "E2E-APPLY: FAIL"
exit "$fail"
