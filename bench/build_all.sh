#!/bin/bash
# Build stock QEMU 11.1.3 (x86_64 only), libvfio-user, kvm-unit-tests into scratchpad. No system changes.
S=/tmp/claude-1000/-home-dima-Projects-qemu-mod/5f872527-d74a-40ad-b9d7-ea8eea4e541e/scratchpad/build
D=$S/deps; mkdir -p $D $S/log
export PKG_CONFIG_PATH=$D/lib/pkgconfig:$D/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
export LD_LIBRARY_PATH=$D/lib:$D/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
cd $S
echo "=== $(date) json-c ==="
if ! pkg-config --exists json-c; then
  curl -sfL -o jsonc.tgz https://github.com/json-c/json-c/archive/refs/tags/json-c-0.17-20230812.tar.gz && tar xzf jsonc.tgz && \
  cmake -S json-c-json-c-0.17-20230812 -B jsonc-build -DCMAKE_INSTALL_PREFIX=$D -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null && \
  nice -n 5 cmake --build jsonc-build -j8 >/dev/null && cmake --install jsonc-build >/dev/null && echo "json-c built"
fi
pkg-config --modversion json-c
echo "=== $(date) libvfio-user ==="
( git clone --depth 1 https://github.com/nutanix/libvfio-user.git lvu-src 2>&1 | tail -1
  cd lvu-src && meson setup build -Dtests=disabled -Dprefix=$D 2>&1 | tail -3 && nice -n 5 ninja -C build -j8 2>&1 | tail -2 && ninja -C build install 2>&1 | tail -1 ) > $S/log/lvu.log 2>&1 && echo "libvfio-user ok" || echo "libvfio-user FAILED (see log/lvu.log)"
echo "=== $(date) kvm-unit-tests ==="
( git clone --depth 1 https://gitlab.com/kvm-unit-tests/kvm-unit-tests.git kut-src 2>&1 | tail -1
  cd kut-src && ./configure 2>&1 | tail -2 && nice -n 5 make -j8 2>&1 | tail -3 ) > $S/log/kut.log 2>&1 && echo "kvm-unit-tests ok" || echo "kvm-unit-tests FAILED (see log/kut.log)"
echo "=== $(date) qemu clone ==="
( git clone --depth 1 --branch v11.1.3 https://gitlab.com/qemu-project/qemu.git qemu-src || git clone --depth 1 --branch stable-11.1 https://gitlab.com/qemu-project/qemu.git qemu-src ) 2>&1 | tail -1
cd qemu-src && git describe --tags --always; mkdir -p build && cd build
echo "=== $(date) qemu configure ==="
../configure --target-list=x86_64-softmmu --enable-kvm --disable-docs --disable-gtk --disable-sdl --disable-werror \
  --disable-guest-agent --disable-user --disable-xen --disable-spice --disable-opengl --disable-virglrenderer \
  --enable-vfio-user-server --enable-multiprocess > $S/log/qemu-configure.log 2>&1 || {
  echo "configure with vfio-user-server failed, retrying without"; 
  ../configure --target-list=x86_64-softmmu --enable-kvm --disable-docs --disable-gtk --disable-sdl --disable-werror \
  --disable-guest-agent --disable-user --disable-xen --disable-spice --disable-opengl --disable-virglrenderer > $S/log/qemu-configure2.log 2>&1; }
grep -iE 'vfio|multiprocess|kvm' $S/log/qemu-configure*.log | head
echo "=== $(date) qemu ninja ==="
nice -n 5 ninja -j24 > $S/log/qemu-ninja.log 2>&1 && echo "qemu ok" || { echo "qemu FAILED"; tail -30 $S/log/qemu-ninja.log; }
ls -la $S/qemu-src/build/qemu-system-x86_64 2>&1
echo "=== $(date) DONE ==="
