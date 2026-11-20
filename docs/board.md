# GDM7243QT SoC and Polish P4 ZTE WF830 board model

The authoritative declarations live in:

- `target/linux/gdm7243/files/arch/arm/boot/dts/gct/gct-gdm7243.dtsi`
- `target/linux/gdm7243/files/arch/arm/boot/dts/gct/gct-wf830-p4.dts`

The SoC file deliberately describes only the subsystems with known
physical registers and interrupt routing. The board file applies
P4-specific RAM, IPC reservations, serial aliases and pad settings.

## Physical RAM and modem carveout

The live P4 2018 kernel confirms:

| Evidence | Value |
| --- | --- |
| `Booting Linux on physical CPU 0x0` | Single Cortex-A7 CPU, MIDR 0x410fc074 |
| `CPU: GDM7243QT R0B` | SoC version 0x72430100 |
| `boot mem size` / `mem_base` | 80 MiB from 0xd0000000 |
| `mem_size` | 0x05000000, ending 0xd5000000 |
| GIPC shared protocol | Physical 0xd4822800 |
| GIPC channel 28 ring | Physical 0xd480b000 |
| Serial-flash staging | Physical 0xd4a82380 |
| Modem loader staging | Begins 0xd5100000, outside the 80-MiB application range |

Source: `/home/jan/projects/wf830/our-device/runtime/dmesg.txt`
and the preserved original kernel ELF on Acer:
`/home/jan/research/wf830-202610/linux-mainline-20261118/live-p4-vmlinux-be8.symbols.elf`.

The first board DT exposes the full 80 MiB physical region but
**reserves 0xd4800000..0xd4ffffff (8 MiB) using no-map**,
so Linux allocates at most 72 MiB. This is an explicit
conservative allocation fence for the currently observed GIPC and
modem/flash staging allocations. The exact carveout layout can
be refined after memory ownership is reconstructed from both CPUs.

## Modeled MMIO blocks

| SoC component | Physical range | Root IRQ | Source |
| --- | --- | --- | --- |
| CCF clock controller | 0xfc000000, 0x500 bytes | none | PLL/gates OEM table |
| Seven-bank pinctrl | 0xfc000500, 0x1c bytes | none | 63 OEM mux descriptors |
| 32.768 kHz RTC clocksource | 0xfc000600, 0x20 bytes | none | `gdm_read_sched_clock` |
| GPIO + both IRQ channels | 0xfc002080, 0x320 bytes | 10, 29 | OEM GPIO irq/mask/ack |
| 64-bit root IRQ lines | 0xfc002c00, 0x60 bytes | CPU exception | `gdm_init_irq` |
| Periodic system tick | 0xfc006c40, 0x20 bytes | 18 | OEM `gdm_init_time` |
| UART0 | 0xfc007500, 0x40 bytes | RX 2 / TX 3 | live P4 serial |
| UART1 | 0xfc007540, 0x40 bytes | RX 21 / TX 22 | live P4 serial |
| UART2 | 0xfc007580, 0x40 bytes | RX 33 / TX 34 | OEM platform-device resource table |

The root interrupt controller has 64 sources and excludes IRQ19.
The timer is **periodic only** in the observed OEM implementation
and expects `CONFIG_HZ_PERIODIC=y`.
UART0, UART1 and UART2 have independent RX/TX IRQs; UART1/2 are disabled
in the first P4 board device tree until its use is explicitly needed.

The actual OEM BE8 kernel exports `gdm_uart2_device` via its
`__ksymtab` entry at 0xd03e9638, resolving the platform device
at 0xd04c9168. Its resource pointer at 0xd04c98c0 contains
physical memory 0xfc007580..0xfc0075bf and two interrupts
33 (RX) and 34 (TX). UART2 is represented in the SoC DTS
with both root IRQs and remains disabled on the P4 board.
UART3 (MMIO 0xfc007600, IRQ12) has a different register
layout and is not assigned the UART0–2 compatible string.

The original P4 Ethernet controller was logged at
**0xec0c0000 / IRQ15** with RTL8211 PHY, and SDHCI at
**0xec0b0000 / IRQ16**. No new Ethernet, storage or firmware
node is enabled until their drivers exist.

## Board-specific pinmux and GPIO

The pinctrl driver recognizes 63 groups, 177 options and 42
functions. The initial P4 DT selects only UART0's **GPG9–10**
pad group. UART1 has a prepared **GPG11–12** state (the OEM
boot log confirms `gdm-uart.1` selected this group), while
RGMII `GPE0–10` plus `GPE12–15` is defined but never
selected until the actual Ethernet MAC is ported.

The GPIO IRQ routing defaults to parent 10 (GPIO0) until
board-specific GPIO1 ownership is proven. The GPIO data/IP
controller has normal Linux offsets 0–223 for the known mux
banks; 93 undocumented pins are marked reserved:

```dts
gpio-reserved-ranges = <8 24>, <46 18>, <81 15>,
                       <120 8>, <145 15>, <184 8>,
                       <219 5>;
```

## DT/Kconfig integration

The board DTB target is
`gct/gct-wf830-p4.dtb`, with `gct/Makefile` and
the `0009-arm-dts-gdm7243-build-integration.patch` adding
the new kernel vendor directory. The Kconfig condition is
`CONFIG_ARCH_MULTI_V7`. All eight platform drivers and the
DTB are validated against a clean Linux 6.12.87 source tree
using the source-overlay harness `tools/build-drivers.sh`.
See [RAM-only boot preparation](ram-boot.md) for the separate
image-builder and bootloader checks.

No OEM firmware blobs, NAND images, DTB build artifacts or
memory dumps are committed to this repository.
