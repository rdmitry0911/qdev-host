#!/bin/bash
# No-VM test of the qdev-host reload guard (debt RELOAD-LIVE-REINIT): `reload` is refused while a
# vfio-user client (guest) is attached unless --allow-guest-gpu-loss is given, and a refused reload
# never reaches the plugin. Plugin: plugins/test-v2/test-v2-reload.so (test-v2 built as ABI 4 with
# a counting reload() and a backend_report() of "reloads=N"). The checks themselves run natively
# in tools/vfu-test-client (`ctl SOCK PREFIX CMD`, socket "-" = no client attached); this script
# only orchestrates.
#   tools/test-reload-guard.sh OUTDIR
# Env: QDEV_HOST (host binary, default ./qdev-host). Runs from the repo root after "make".
set -u
OUT=${1:?outdir}
HOST=${QDEV_HOST:-./qdev-host}
CL=./tools/vfu-test-client
RUN=${RUN:-/tmp/qdev-rg-$$}      # short: unix socket paths are limited to 108 bytes
C=$RUN/ctl.sock
mkdir -p "$OUT" "$RUN"
rc=0
step() { echo "== $*" | tee -a "$OUT/summary.txt"; }
result() { local r=$1 what=$2; echo "   $what: $([ "$r" = 0 ] && echo PASS || echo "FAIL ($r)")" | tee -a "$OUT/summary.txt"; [ "$r" = 0 ] || rc=1; }
: > "$OUT/summary.txt"

timeout 60 "$HOST" --plugin plugins/test-v2/test-v2-reload.so --socket "$RUN/dev.sock" --control "$C" -v \
    > "$OUT/host.log" 2>&1 &
PID=$!
for _ in $(seq 50); do [ -S "$RUN/dev.sock" ] && [ -S "$C" ] && break; sleep 0.1; done

step "no client attached yet: reload proceeds"
timeout 20 $CL - ctl "$C" "ok reloaded /x/backend-1.so" "reload /x/backend-1.so" \
    ctl "$C" "ok reloads=1" backend > "$OUT/detached-1.client.log" 2>&1
result $? "detached: reload proceeds and reaches the plugin (reloads=1)"

step "client attached: reload refused unless --allow-guest-gpu-loss"
timeout 30 $CL "$RUN/dev.sock" info dma \
    ctl "$C" "err reload refused: a vfio-user client (guest) is attached" "reload /x/backend-2.so" \
    ctl "$C" "err reload refused: a vfio-user client (guest) is attached" "reload /x/backend-2.so --reset" \
    ctl "$C" "ok reloads=1" backend \
    ctl "$C" "ok reloaded /x/backend-3.so (reset)" "reload /x/backend-3.so --reset --allow-guest-gpu-loss" \
    ctl "$C" "ok reloaded /x/backend-4.so" "reload --allow-guest-gpu-loss /x/backend-4.so" \
    ctl "$C" "ok reloads=3" backend \
    ctl "$C" "err usage: reload PATH" "reload /x/a.so /x/b.so" \
    ctl "$C" "err usage: reload PATH" "reload /x/a.so --bogus" \
    > "$OUT/attached.client.log" 2>&1
result $? "attached: 2 refusals never reach the plugin; --allow-guest-gpu-loss proceeds (2); bad arguments rejected"
grep -q "reload /x/backend-2.so refused: a vfio-user client (guest) is attached" "$OUT/host.log"
result $? "host log names the refusal reason"

step "client detached again: reload proceeds"
for _ in $(seq 50); do grep -q "client detached" "$OUT/host.log" && break; sleep 0.1; done
timeout 20 $CL - ctl "$C" "ok reloaded /x/backend-5.so" "reload /x/backend-5.so" \
    ctl "$C" "ok reloads=4" backend > "$OUT/detached-2.client.log" 2>&1
result $? "after the client detached: reload proceeds (reloads=4)"

kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null

# State hand-over reported by the backend (T2_HANDOVER, see plugins/test-v2): without the
# suspend-time flush a plain reload under an attached guest stays refused unless --allow-unflushed;
# with the flush it proceeds.
for mode in 1 2; do
  rm -f "$RUN/dev.sock" "$C"
  T2_HANDOVER=$mode timeout 60 "$HOST" --plugin plugins/test-v2/test-v2-reload.so --socket "$RUN/dev.sock" --control "$C" -v \
      > "$OUT/host-handover$mode.log" 2>&1 &
  PID=$!
  for _ in $(seq 50); do [ -S "$RUN/dev.sock" ] && [ -S "$C" ] && break; sleep 0.1; done
  if [ $mode = 1 ]; then
    step "attached + hand-over WITHOUT flush: plain reload refused, --allow-unflushed proceeds"
    timeout 30 $CL "$RUN/dev.sock" info dma \
        ctl "$C" "err reload refused: a vfio-user client (guest) is attached; the backend state hand-over does not flush" "reload /x/h-1.so" \
        ctl "$C" "ok reloads=0" backend \
        ctl "$C" "ok reloaded /x/h-2.so" "reload /x/h-2.so --allow-unflushed" \
        ctl "$C" "ok reloads=1" backend \
        > "$OUT/handover1.client.log" 2>&1
    result $? "attached, unflushed hand-over: refused without the flag (never reaches the plugin), --allow-unflushed proceeds"
    grep -q "reload /x/h-1.so refused: a vfio-user client (guest) is attached and the backend hands the device state over without flushing" "$OUT/host-handover1.log"
    result $? "host log names the unflushed refusal reason"
  else
    step "attached + hand-over WITH flush: plain reload proceeds"
    timeout 30 $CL "$RUN/dev.sock" info dma \
        ctl "$C" "ok reloaded /x/h-3.so" "reload /x/h-3.so" \
        ctl "$C" "ok reloads=1" backend \
        > "$OUT/handover2.client.log" 2>&1
    result $? "attached, flushed hand-over: plain reload proceeds"
  fi
  kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
done
rm -rf "$RUN"
echo "overall: $([ $rc = 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $rc
