# GCT GDM7243QT OpenWrt board support

Clean-room Linux/OpenWrt port for the **Polish P4 ZTE WF830 ODU**,
GCT GDM7243QT Cortex-A7 ARMv7 SoC.

**Target distribution:** OpenWrt 25.12 / Linux 6.12.
**Current stage:** eight Linux 6.12 GDM7243 kernel drivers plus
a physical-register-based SoC DTS and Polish P4 board DTS. The DTB
compiles through Linux Kbuild and has a complete schema validator.
A separate RAM-only kernel+initramfs builder is available for
reversible boot preparation, pending the proven BE8-to-LE handoff.

The `target/linux/gdm7243/files/` tree is a kernel source overlay,
with the OpenWrt kernel integration patch in `patches-6.12/`.
The board DT is compiler-validated; the proper OpenWrt target
packaging and U-Boot entry handoff still require device validation.

The driver work is independent from
[wf830-redox](https://github.com/codeandsolder/wf830-redox),
which owns the Rust modem control/userland stack.

## Hardware evidence

Real device: `/home/jan/projects/wf830/our-device/`.
Corrected BE8 kernel symbol ELF:
`/home/jan/research/wf830-202610/linux-mainline-20261118/live-p4-vmlinux-be8.symbols.elf`.

All proprietary firmware, calibration, dumps and decompiler databases
are deliberately excluded. Only source, declarative bindings and
sanitized hardware notes belong in this repository.

## Drivers

- `drivers/irqchip/irq-gdm7243.c`: root IRQ controller,
  64 lines/two banks, level handler, mask/ack and OEM priority.
  Static recovery complete, hardware test pending.
- `drivers/clocksource/gdm7243-rtccs.c`: free-running 32.768 kHz
  RTC clocksource and scheduler clock, recovered from the live P4 kernel.
  Static recovery complete, hardware test pending.
- `drivers/tty/serial/gdm7243-earlycon.c`: polling boot-console
  output using the recovered UART0-2 register layout.
- `drivers/tty/serial/gdm7243-uart.c`: UART0-2 serial-core
  driver, separate RX/TX IRQs, error reporting, baud/divisor and
  framing, ttyGDM devices and optional normal console.
- `drivers/gpio/gpio-gdm7243.c`: banked gpiolib direction/input/output
  with dual-parent IRQ10/29 gpio-irqchip and board-defined IRQ routing.
- `drivers/clocksource/gdm7243-timer.c`: IRQ 18 periodic Linux
  clockevent, with OEM-observed control and clock-derived reload.
- `drivers/clk/clk-gdm7243.c`: early common-clock provider for five
  OEM root clocks plus all 27 peripheral entries, strap/PLL rate
  selection and the original gate/pulse transactions.
- `drivers/pinctrl/pinctrl-gdm7243.c`: 63 pad mux groups with all
  177 recovered mode options, 42 functions, shared-field arbitration
  and normal GPIO pinctrl ownership/ranges.
- `arch/arm/boot/dts/gct/gct-gdm7243.dtsi` and
  `gct-wf830-p4.dts`: physical SoC register nodes, confirmed P4
  UART0/1 wiring, 80 MiB DDR and an 8 MiB modem-shared exclusion.
- Next: verified BE8-to-LE RAM-only boot handoff, then GIPC/GLIF,
  NAND/ECC and Ethernet.

See [IRQ controller recovery](docs/irqchip.md) and
[RTC clocksource recovery](docs/clocksource.md), and
[UART early-console recovery](docs/earlycon.md),
[full UART driver recovery](docs/uart.md),
[GPIO recovery](docs/gpio.md),
[periodic timer recovery](docs/clockevent.md), and
[clock tree recovery](docs/clocks.md), and
[pinmux register recovery](docs/pinctrl.md),
[physical board model](docs/board.md), and
[RAM-only boot preparation](docs/ram-boot.md).

## Repeatable compiler validation

Build just the ported drivers from a *clean* Linux 6.12 Git checkout
(in an automatically removed temporary worktree), with the existing
shared sccache instance and an external output directory:

```sh
tools/build-drivers.sh \
  /home/jan/research/wf830-kernel-v6.12.87 \
  /home/jan/build/wf830-linux-6.12/validate
```

The same compiler harness now also builds `gct-wf830-p4.dtb`.
Check its bindings, physical memory map and IRQ wiring with:

```sh
tools/validate-board-dtb.sh     /home/jan/build/wf830-linux-6.12/validate/arch/arm/boot/dts/gct/gct-wf830-p4.dtb     /home/jan/research/wf830-kernel-v6.12.87
```

Build a static ARM32 EABI bring-up initramfs, and optionally a
full kernel and appended-DTB image, using
`tools/build-ram-initramfs.sh` and `tools/build-ram-kernel.sh`.

No kernel/build artifacts are written into the Git repository.
The original OEM CPU runs BE8; bootloader endian handoff and physical
testing must be resolved before executing a little-endian zImage.
