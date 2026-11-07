# Микробенчмарк: vfio-user-pci (стоковый QEMU) против устройства в процессе QEMU

Гость: kvm-unit-tests `x86/vmexit.flat` (bare-metal, без ОС), устройство `pci-testdev`.
Сравниваются три варианта одного и того же устройства:

1. `-device pci-testdev` внутри QEMU (нативное устройство);
2. `src/vfu-testdev.c`, сервер на libvfio-user с протоколом pci-testdev, подключён через `-device vfio-user-pci`;
3. второй QEMU в роли сервера: `-machine x-remote,vfio-user=on -object x-vfio-user-server`.

Измеряемые операции (тики TSC на операцию, печатает гость):
`pci-mem` запись в BAR0 (MMIO), `pci-io` запись в BAR1 (PIO), `pci-mem-read`, `pci-io-read`,
`pci-bar2-write/read` (RAM/mmap BAR), `pci-dma-kick` (сервер меряет DMA сам, печатает `DMA_BENCH`),
`pci-dma-sync` (синхронный DMA внутри обработчика чтения: демонстрация взаимоблокировки).

## Сборка (всё в scratch-каталоге, без прав root)

```bash
src/build_all.sh      # json-c, libvfio-user, kvm-unit-tests, QEMU v11.1.3 (x86_64-softmmu)
src/build_lvu.sh      # cmocka, libvfio-user с установкой, QEMU с --enable-vfio-user-server
cd kut-src && git apply .../src/kvm-unit-tests-vmexit.patch && make x86/vmexit.flat
gcc -O2 -I$DEPS/include/vfio-user -o vfu-testdev src/vfu-testdev.c -L$DEPS/lib/x86_64-linux-gnu -lvfio-user
```
Пути внутри скриптов указывают на scratch-каталог сессии 2026-11-07; при переносе поправить переменную `S`.

## Запуск

```bash
# нативно
qemu-system-x86_64 -enable-kvm -cpu host -m 512 -smp 1 -nodefaults -display none -serial stdio \
  -device pc-testdev -device isa-debug-exit,iobase=0xf4,iosize=0x4 \
  -device pci-testdev,membar=4096,membar-backed=on -kernel x86/vmexit.flat \
  -append "vmcall pci-mem-read pci-io-read pci-bar2-write pci-bar2-read pci-mem pci-io"

# через vfio-user: run_vfu.sh <tag> "<аргументы сервера>" "<доп. JSON-свойства клиента>" "<тесты>"
src/run_vfu.sh A  ""   ""                          "vmcall pci-mem-read pci-io-read pci-mem pci-io"
src/run_vfu.sh B  ""   ',"x-no-posted-writes":true' "pci-mem"
src/run_vfu.sh C  "-m" ""                          "pci-bar2-write pci-bar2-read pci-mem"   # mmap BAR2
src/run_vfu.sh P  "-p" ""                          "pci-mem-read pci-mem"                   # polling-сервер
src/run_vfu.sh D1 ""   ""                          "pci-dma-kick"                            # DMA через сообщения
src/run_vfu_shm.sh D2 "" ""                        "pci-dma-kick"                            # DMA через memfd share=on
```

Сырые результаты: `results/` (гость `*.guest.txt`, сервер `*.server.txt`, окружение `ENV.txt`).
Перевод в микросекунды: тики / 3593.2 (TSC этого хоста).
