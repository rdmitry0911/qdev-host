# qemu-thunderbolt на стоковом QEMU: PoC через vfio-user

Дата: 2026-11-07. Стенд: стоковый QEMU v11.1.3 (сборка upstream-тега без патчей), libvfio-user master,
гость Debian 13 (ядро 6.12, pciehp, bochs-drm), KVM, хост Threadripper PRO 5975WX в LXC.

## 1. Что в форке и куда это ложится на стоке

Форк `rdmitry0911/qemu-thunderbolt` добавляет три типа устройств (1271 строка поверх upstream):

| Устройство форка | Что добавляет сверх стока | Как это сделано на стоке |
|---|---|---|
| `thunderbolt-root-port` | `_DSD` {PCI-Thunderbolt, pci-supports-link-change}; QOM‑свойство `connected` (PDS/DLLLA + attention button) | обычный `pcie-root-port` + SSDT через `-acpitable` (`acpi/ssdt-tb.dsl`); `connected=false/true` = `device_del`/`device_add` внешнего устройства (`scripts/tbctl.py disconnect/connect`) |
| `thunderbolt-pcie-pci-bridge` | `_DSD` {IOPCITunnelled} и описание подчинённых устройств | обычный `pcie-pci-bridge` + тот же SSDT |
| `thunderbolt-vga` | stdvga с `hotpluggable=true` и EDID 1280x800 | внешний процесс `tb-vga/tb-vga.c` на libvfio-user, подключается как `vfio-user-pci` |

Мосты и root port из процесса QEMU вынести нельзя: `vfio-user-pci` в стоке отдаёт гостю только
endpoint (type‑0 header). Но всё «thunderbolt‑ное» в них это ACPI‑свойства и переключатель
присутствия, и обе вещи делаются штатными средствами. Единственное устройство, которое нужно было
вынести, это VGA, и оно вынесено.

## 2. Что проверено (лог `results/test-flow.clean.log`)

1. `device_add vfio-user-pci,bus=tbrp0` → в госте `01:00.0 VGA compatible controller [1234:1111]`,
   BAR0 16 МиБ 64‑bit prefetchable (mmap в memfd сервера), BAR2 4 КиБ MMIO, PCIe endpoint, 8 GT/s x4.
2. Стоковый драйвер Linux `bochs-drm` привязывается без изменений: `Found bochs VGA, ID 0xb0c5`,
   `Found EDID data blob`, появляется `/dev/fb1`.
3. Гость делает modeset 1280x800x32 и пишет кадр; сервер по SIGUSR1 снимает VRAM: `results/fb-after-draw.png`
   (градиент + шахматка, как нарисовано в госте).
4. `device_del` → `DEVICE_DELETED`, `card1` исчезает из гостя; сервер остаётся жить, состояние сохранено.
5. Повторный `device_add` к тому же серверу: драйвер привязывается снова, VRAM не обнулялся
   (`results/fb-after-replug.png`).
6. `tbctl disconnect tbrp0` / `tbctl connect tbrp0 tbvga0=<sock>` — аналог `connected=false/true` форка.
7. SSDT загружен: в госте `grep -a PCI-Thunderbolt /sys/firmware/acpi/tables/SSDT` и `IOPCITunnelled`
   находятся; пути устройств `\_SB.PCI0.S28` (root port на addr 0x05) и `\_SB.PCI0.S38.S00` (мост).

## 3. Запуск

```bash
# подготовка гостя (без root на хосте): образ, ядро, initrd, автологин на ttyS0
QEMU_BUILD=/path/to/qemu/build scripts/prep-guest.sh /path/to/guestdir

# гость: q35, boot VGA, два root port с резервом окон под hotplug, SSDT, serial и QMP на unix-сокетах
QEMU=/path/to/qemu-system-x86_64 DISK=.../test2.qcow2 KERNEL=.../vmlinuz INITRD=.../initrd.img \
  APPEND="console=ttyS0,115200n8 root=/dev/vda1 rw net.ifnames=0 systemd.firstboot=0" scripts/run-guest.sh

# сквозной тест: сервер, plug, проверка в госте, рисование, дамп, unplug, replug, disconnect/connect
scripts/test-flow.sh
```

Сборка сервера: `gcc -O2 -I<libvfio-user>/include -o tb-vga tb-vga/tb-vga.c -lvfio-user`.
Запуск вручную: `tb-vga/tb-vga -e tb-vga/edid-1280x800.bin /run/tb/tbvga0.sock`, затем
`scripts/tbctl.py <qmp.sock> plug tbvga0 /run/tb/tbvga0.sock tbrp0`.

## 4. Подводные камни, найденные по дороге

- Окна моста. Под hotplug 16 МиБ prefetchable BAR у root port должен быть резерв:
  `pcie-root-port,...,pref64-reserve=64M,mem-reserve=8M`. Без него Linux пишет
  `BAR 0: can't assign; no space`, и драйвер не стартует. У форка та же механика (свойства `*-reserve`
  скопированы из gen_pcie_root_port), просто там тестировали под OVMF/macOS.
- 32‑битный prefetchable BAR не попадает в 64‑битное окно; BAR0 сделан 64‑битным.
- Путь unix‑сокета не длиннее 107 байт.
- У libvfio-user `vfu_run_ctx` не прерывается сигналом, поэтому сервер работает через `poll()` на
  `vfu_get_poll_fd()` с `LIBVFIO_USER_FLAG_ATTACH_NB`.
- Имя устройства в ACPI у QEMU: `S<devfn hex>`, devfn = slot<<3|func. Проверять через
  `/sys/bus/pci/devices/<bdf>/firmware_node/path`.
- Один mmap‑регион (BAR0) отключает posted‑записи для всего устройства (см. отчёт по оверхеду в
  корне проекта): запись регистра dispi стоит ~23 мкс вместо ~5. Для дисплея это неважно.

## 5. Что не переносится и что осталось

- Вывод изображения в консоль QEMU (VNC/SPICE) у внешнего устройства невозможен: протокол vfio-user не
  знает о дисплее. Кадр живёт в memfd сервера; показывать его должен сам сервер (свой VNC, SDL,
  запись в файл). Для сценария macOS «устройство видно и исчезает» это не требуется.
- Legacy VGA (порты 0x3c0, окно 0xa0000) и option ROM у внешнего устройства не реализованы; загрузочным
  остаётся стоковый `VGA`, как и в примерах форка.
- macOS не проверялся (нет образа). На macOS SSDT удобнее подкладывать через OpenCore (ACPI/Add), а не
  `-acpitable`; свойства оставлены в `_DSD`, как в форке. Если macOS их не подхватит, тот же SSDT
  переписывается на `_DSM` с Apple UUID без изменений в QEMU.
- Следующий шаг по плану: `qdev-host` с plugin‑ABI, в который `tb-vga` войдёт первым плагином, и
  перенос второго устройства (USB/NVMe можно не переносить, они в стоке hotpluggable и так).
