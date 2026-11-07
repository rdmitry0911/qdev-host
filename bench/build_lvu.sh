#!/bin/bash
S=/tmp/claude-1000/-home-dima-Projects-qemu-mod/5f872527-d74a-40ad-b9d7-ea8eea4e541e/scratchpad/build
D=$S/deps
export PKG_CONFIG_PATH=$D/lib/pkgconfig:$D/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
export LD_LIBRARY_PATH=$D/lib:$D/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
cd $S
echo "=== $(date) cmocka ==="
if ! pkg-config --exists cmocka; then
  curl -sfL -o cmocka.tgz https://cmocka.org/files/1.1/cmocka-1.1.7.tar.xz 2>/dev/null || curl -sfL -o cmocka.tgz https://github.com/clibs/cmocka/archive/refs/tags/cmocka-1.1.7.tar.gz
  mkdir -p cmocka-src && tar xf cmocka.tgz -C cmocka-src --strip-components=1 && \
  cmake -S cmocka-src -B cmocka-build -DCMAKE_INSTALL_PREFIX=$D -DCMAKE_BUILD_TYPE=Release -DUNIT_TESTING=OFF >/dev/null && \
  nice -n 5 cmake --build cmocka-build -j8 >/dev/null && cmake --install cmocka-build >/dev/null && echo "cmocka built"
fi
pkg-config --modversion cmocka
echo "=== $(date) libvfio-user ==="
cd $S/lvu-src && rm -rf build && meson setup build -Dprefix=$D 2>&1 | tail -3 && nice -n 5 ninja -C build -j8 2>&1 | tail -2 && ninja -C build install 2>&1 | tail -1 && ls $D/lib/x86_64-linux-gnu/libvfio-user* $D/lib/libvfio-user* 2>/dev/null
ls $S/lvu-src/build/samples/ 2>/dev/null | head -20
echo "=== $(date) qemu reconfigure with vfio-user-server ==="
cd $S/qemu-src/build && ../configure --target-list=x86_64-softmmu --enable-kvm --disable-docs --disable-gtk --disable-sdl --disable-werror \
  --disable-guest-agent --disable-user --disable-xen --disable-spice --disable-opengl --disable-virglrenderer \
  --enable-vfio-user-server --enable-multiprocess > $S/log/qemu-configure3.log 2>&1 && echo "configure ok" || { echo "configure FAILED"; tail -20 $S/log/qemu-configure3.log; }
nice -n 5 ninja -j24 > $S/log/qemu-ninja2.log 2>&1 && echo "qemu ok" || { echo "qemu FAILED"; tail -20 $S/log/qemu-ninja2.log; }
$S/qemu-src/build/qemu-system-x86_64 -object x-vfio-user-server,help 2>&1 | head -5
$S/qemu-src/build/qemu-system-x86_64 -machine help 2>&1 | grep -i remote
echo "=== $(date) DONE ==="
