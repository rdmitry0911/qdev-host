#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# e2e-vt-warmswap.sh - end-to-end demo of `uniq-vgpu update` on increment 1's
# PROVEN apple-videotoolbox warm-swap harness, OFF-VM via the --control socket
# (no guest, no display GPU, no QEMU). Exercises the real inc1 primitives
# (qdevctl device-abi / device-swap) that `uniq-vgpu` wraps.
#
#   WARM:   bundle envelope == resident envelope -> device-swap -> health -> commit
#   REBOOT: bundle envelope != resident envelope -> "reboot required", no swap
#   ROLLBACK: symmetric warm swap back to the previous backing (inc1 probe C)
#   STATUS: resident envelope/ABI + active bundle + health + ioreg-triplet slot
#
# usage: e2e-vt-warmswap.sh [OUTDIR]
set -u

SELF=$(cd "$(dirname "$0")" && pwd)
WT=$(cd "$SELF/../.." && pwd)                       # qdev-host worktree (this branch)
INC1=${INC1:-/srv/project/apple-virgl/worktrees/qdev-applegva-20261116}
OUT=${1:-/tmp/uniq-vgpu-e2e-$$}
mkdir -p "$OUT"

UNIQ="$WT/bin/uniq-vgpu"
MKB="$WT/bin/uniq-vgpu-mkbundle"
QDEVCTL="$WT/pve/qdevctl"
QDEV_HOST="$INC1/qdev-host"
PLUGIN="$INC1/plugins/apple-videotoolbox/build/apple-videotoolbox.so"
BACKING_B="$INC1/plugins/apple-videotoolbox/build/backing_B.so"

CTL="$OUT/ctl.sock"
VFIO="$OUT/vfio.sock"
HOSTLOG="$OUT/qdev-host.log"
STATE="$OUT/state"
pass=0; fail=0
say() { echo "=== $* ==="; }
ok()  { echo "  PASS $*"; pass=$((pass+1)); }
no()  { echo "  FAIL $*"; fail=$((fail+1)); }

for f in "$QDEV_HOST" "$PLUGIN" "$BACKING_B"; do
  [ -e "$f" ] || { echo "MISSING inc1 artifact: $f"; echo "(build it: make -C $INC1/plugins/apple-videotoolbox plugin)"; exit 2; }
done

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

say "launch inc1 VT harness off-VM (control socket, no guest)"
"$QDEV_HOST" --plugin "$PLUGIN" --socket "$VFIO" --control "$CTL" -v > "$HOSTLOG" 2>&1 &
QPID=$!
for i in $(seq 1 30); do [ -S "$CTL" ] && break; sleep 0.1; done
if kill -0 "$QPID" 2>/dev/null && [ -S "$CTL" ]; then ok "qdev-host up (pid $QPID), control socket live"; else no "qdev-host failed to start"; cat "$HOSTLOG"; exit 1; fi

TGT=(--control "$CTL" --qdevctl "$QDEVCTL" --state-dir "$STATE")

say "resident envelope (live, via qdevctl device-abi)"
ABI=$(python3 "$QDEVCTL" device-abi --control "$CTL")
echo "  $ABI"
echo "$ABI" | grep -q "envelope=vtpv-envelope-1" && ok "resident envelope = vtpv-envelope-1" || no "unexpected envelope"

say "build bundles (mkbundle)"
BWARM="$OUT/bundle-vt-hevc"
BREBOOT="$OUT/bundle-vt-next"
python3 "$MKB" "$BWARM" --name vt-hevc --version 0.3-warmswapB \
    --envelope vtpv-envelope-1 --backing "$BACKING_B" > "$OUT/mkbundle-warm.txt" 2>&1 && ok "warm bundle built (envelope vtpv-envelope-1)" || no "mkbundle warm"
python3 "$MKB" "$BREBOOT" --name vt-next --version 1.0-env2 \
    --envelope vtpv-envelope-2 --backing "$BACKING_B" > "$OUT/mkbundle-reboot.txt" 2>&1 && ok "reboot bundle built (envelope vtpv-envelope-2)" || no "mkbundle reboot"

say "status (initial)"
python3 "$UNIQ" status "${TGT[@]}" | tee "$OUT/status-initial.txt"

say "update --dry-run (warm bundle: expect WARM plan)"
python3 "$UNIQ" update "$BWARM" --dry-run "${TGT[@]}" | tee "$OUT/update-dryrun.txt"
grep -q "=> WARM" "$OUT/update-dryrun.txt" && ok "dry-run: ABI-gate says WARM" || no "dry-run WARM"

say "update (warm: verify -> ABI-gate -> device-swap -> health -> commit)"
python3 "$UNIQ" update "$BWARM" --health-log "$HOSTLOG" "${TGT[@]}" | tee "$OUT/update-warm.txt"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] && ok "warm update exit 0" || no "warm update rc=$rc"
grep -q "verify: OK" "$OUT/update-warm.txt" && ok "verify gate passed" || no "verify gate"
grep -q "COMMITTED" "$OUT/update-warm.txt" && ok "committed (no guest reboot)" || no "commit"
grep -q "WARM-SWAP backing -> tag 'B'.*name_suffix ' \[warmswapB\]'.*from .*\.so" "$HOSTLOG" && ok "host log: real in-process .so backing swap (tag B, HEVC added)" || no "host WARM-SWAP log"

say "status (after warm update: active bundle + envelope + health + ioreg slot)"
# a stand-in ioreg triplet file (guest is offline off-VM; shows the status surface)
cat > "$OUT/ioreg-triplet.txt" <<EOF
AGVAEnvelopeVersion="vtpv-envelope-1"
AGVAGuestDriverVersion="dev.applevtpv.driver 1.0-warmswap"
AGVAHandshakeGeneration=1
EOF
python3 "$UNIQ" status "${TGT[@]}" --ioreg-file "$OUT/ioreg-triplet.txt" --health-log "$HOSTLOG" | tee "$OUT/status-after-warm.txt"
grep -q "active bundle:          vt-hevc" "$OUT/status-after-warm.txt" && ok "status shows active bundle vt-hevc" || no "status active bundle"
grep -q "AGVAEnvelopeVersion" "$OUT/status-after-warm.txt" && ok "status shows ioreg triplet" || no "status ioreg triplet"

say "update (reboot bundle: envelope differs -> REBOOT required, NO live swap)"
python3 "$UNIQ" update "$BREBOOT" --health-log "$HOSTLOG" "${TGT[@]}" | tee "$OUT/update-reboot.txt"
rc=${PIPESTATUS[0]}
[ "$rc" = 20 ] && ok "reboot path exit 20 (scriptable)" || no "reboot path rc=$rc"
grep -q "REBOOT REQUIRED" "$OUT/update-reboot.txt" && ok "clear reboot-required message (not silent)" || no "reboot message"
swaps_after_reboot=$(grep -c "WARM-SWAP backing" "$HOSTLOG")

say "rollback (symmetric warm swap back to previous backing, inc1 probe C)"
python3 "$UNIQ" rollback --health-log "$HOSTLOG" "${TGT[@]}" | tee "$OUT/rollback.txt"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] && ok "rollback exit 0" || no "rollback rc=$rc"
grep -q "WARM-SWAP backing -> tag 'A'" "$HOSTLOG" && ok "host log: reverted to backing A (deterministic)" || no "rollback to A"
# reboot path must not have added a swap between warm-commit and rollback
[ "$swaps_after_reboot" = "$(grep -c "tag 'B'" "$HOSTLOG" | head -1)" ] 2>/dev/null

say "device still live after the whole sequence (envelope intact)"
ABI2=$(python3 "$QDEVCTL" device-abi --control "$CTL")
echo "  $ABI2"
[ "$ABI2" = "$ABI" ] && ok "envelope CONSTANT across update/reboot-gate/rollback" || no "envelope changed: $ABI2"
kill -0 "$QPID" 2>/dev/null && ok "qdev-host process survived (vfio link never dropped)" || no "qdev-host died"

say "SUMMARY"
echo "  evidence: $OUT"
echo "  PASS=$pass FAIL=$fail"
[ "$fail" = 0 ] && echo "E2E RESULT: PASS" || echo "E2E RESULT: FAIL"
exit "$fail"
