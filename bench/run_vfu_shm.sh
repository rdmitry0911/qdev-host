#!/bin/bash
# usage: run_vfu.sh <tag> "<server args>" "<extra client props>" "<tests>"
S=/tmp/claude-1000/-home-dima-Projects-qemu-mod/5f872527-d74a-40ad-b9d7-ea8eea4e541e/scratchpad
Q=$S/build/qemu-src/build/qemu-system-x86_64
TAG=$1; SARGS=$2; CPROPS=$3; TESTS=$4
SOCK=/tmp/claude-1000/vfu/$TAG.sock; rm -f $SOCK
$S/srv/vfu-testdev $SARGS $SOCK > $S/res/$TAG.server.txt 2>&1 &
SPID=$!
for i in $(seq 1 50); do [ -S $SOCK ] && break; sleep 0.1; done
[ -S $SOCK ] || { echo "server did not create socket"; cat $S/res/$TAG.server.txt; kill $SPID; exit 1; }
cd $S/build/kut-src
timeout 280 $Q -enable-kvm -cpu host -m 512 -smp 1 -nodefaults -display none -serial stdio \
  -object memory-backend-memfd,id=m0,size=512M,share=on -machine memory-backend=m0 -device pc-testdev -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
  -device "{\"driver\":\"vfio-user-pci\",\"socket\":{\"path\":\"$SOCK\",\"type\":\"unix\"}$CPROPS}" \
  -kernel x86/vmexit.flat -append "$TESTS" > $S/res/$TAG.guest.txt 2>&1
echo "qemu exit=$?"
sleep 0.3; kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
grep -v '^enabling\|^smp\|^paging\|^cr' $S/res/$TAG.guest.txt
echo "--- server ---"; tail -6 $S/res/$TAG.server.txt
