#!/bin/bash
# No-VM test: a backend reload under an attached guest never leaves guest MMIO waiting for it
# (debt D-GUEST-ISR-SIGNALDISPLAYS, apple-virgl evidence/runs/reload-debts-20261119: a 6 s
# hand-over under host load made stock QEMU's vfio-user client time out BAR0 reads after 5 s
# (x-msg-timeout) and hand the guest 0xffffffff -> guest kernel panic).
# Reference: -[_PGDevice willSuspend/finishSuspend] .. willResume/didResume run while the VM's
# vCPUs are stopped (Virtualization.framework saves/restores a PAUSED VM); -[_PGDevice
# mmioReadAtOffset:] / mmioWriteAtOffset:value: never wait for the suspend. qdev-host therefore
# stops the vCPUs over QMP (--vm-qmp) while it keeps serving BAR0, runs the reload, then continues.
# Plugin: test-v2-reload.so with T2_HANDOVER=2 (full hand-over) and T2_RELOAD_MS (slow reload).
# The checks run natively in tools/vfu-test-client; this script only orchestrates.
#   tools/test-reload-vm-pause.sh OUTDIR        Env: QDEV_HOST (default ./qdev-host), RELOAD_MS (6000)
set -u
OUT=${1:?outdir}
HOST=${QDEV_HOST:-./qdev-host}
CL=./tools/vfu-test-client
MS=${RELOAD_MS:-6000}
RUN=${RUN:-/tmp/qdev-vp-$$}
C=$RUN/ctl.sock Q=$RUN/qmp.sock
mkdir -p "$OUT" "$RUN"
rc=0
step() { echo "== $*" | tee -a "$OUT/summary.txt"; }
result() { local r=$1 what=$2; echo "   $what: $([ "$r" = 0 ] && echo PASS || echo "FAIL ($r)")" | tee -a "$OUT/summary.txt"; [ "$r" = 0 ] || rc=1; }
start_host() {   # $1 = log name, rest = extra host args
  local log=$1; shift
  rm -f "$RUN/dev.sock" "$C"
  T2_HANDOVER=2 T2_RELOAD_MS=$MS timeout 120 "$HOST" --plugin plugins/test-v2/test-v2-reload.so \
      --socket "$RUN/dev.sock" --control "$C" -v "$@" > "$OUT/$log" 2>&1 &
  PID=$!
  for _ in $(seq 50); do [ -S "$RUN/dev.sock" ] && [ -S "$C" ] && break; sleep 0.1; done
}
: > "$OUT/summary.txt"
echo "host=$HOST reload_ms=$MS" >> "$OUT/summary.txt"

step "A: guest attached, reload ($MS ms hand-over) with the VM pausable over QMP: BAR0 answered in time"
start_host host-A.log --vm-qmp "$Q"
# The guest's read is issued while the reload request is pending (stop answered 300 ms late).
timeout 60 $CL "$RUN/dev.sock" info qmpfake "$Q" 300 sleep 150 r32max bar0 0x0 1000 \
    sleep $((MS + 1500)) qmpcheck "$C" 1 r32max bar0 0x0 1000 > "$OUT/A-guest.client.log" 2>&1 &
G=$!
sleep 0.1
timeout 60 $CL - ctlhc "$C" "ok reloaded /x/vp-1.so" "reload /x/vp-1.so" > "$OUT/A-ctl.client.log" 2>&1
result $? "A: reload accepted and completed; the deferred reply reaches a half-closed control client"
wait $G
result $? "A: in-flight BAR0 read answered within 1 s; reload ran inside QMP stop..cont; VM running after"
kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null

step "B: guest attached, no QMP: a reload that would leave the running guest's MMIO unanswered is refused"
start_host host-B.log
timeout 60 $CL "$RUN/dev.sock" info sleep 150 r32max bar0 0x0 1000 \
    ctl "$C" "ok reloads=0" backend > "$OUT/B-guest.client.log" 2>&1 &
G=$!
sleep 0.1
timeout 60 $CL - ctl "$C" "err reload refused: a vfio-user client (guest) is attached and its vCPUs" "reload /x/vp-2.so" \
    > "$OUT/B-ctl.client.log" 2>&1
result $? "B: refused before reaching the plugin"
wait $G
result $? "B: BAR0 answered in time, the plugin never reloaded"
kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null

step "C: guest attached, no QMP, caller paused the VM (--vm-paused): reload proceeds"
start_host host-C.log
timeout 60 $CL "$RUN/dev.sock" info ctl "$C" "ok reloaded /x/vp-3.so" "reload /x/vp-3.so --vm-paused" \
    ctl "$C" "ok reloads=1" backend > "$OUT/C.client.log" 2>&1
result $? "C: --vm-paused reload reaches the plugin"
kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null

rm -rf "$RUN"
echo "RESULT $([ $rc = 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $rc
