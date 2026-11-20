# GDM7243QT clock-controller recovery

This driver is an early common-clock-framework (CCF) provider for
the Polish P4 GDM7243QT application processor. It replaces the original
3.10 private clock structures; it does not copy firmware code.

## Original hardware evidence

- Actual corrected P4 ARMv7 BE8 symbol ELF: `/home/jan/research/wf830-202610/linux-mainline-20261118/live-p4-vmlinux-be8.symbols.elf`
- Clock root and gate registration: `gdm7243_init_clock` at 0xd0425f58 and `gdm_setup_clocks` at 0xd04267a0.
- 27 peripheral descriptors begin at original kernel address 0xd04c70d4, each 0x3c bytes. Data words are big-endian; BE8 instruction decoding is separate. This table is the source of the full register list below.
- OEM MMIO gate transaction routine: `gdm_clk_enable` at 0xd0017d94.
- Clock controller base physical 0xfc000000, gate registers 0xfc00040c, 0xfc000438 and 0xfc00043c; hardware lock registers 0xfc000010 and 0xfc000018.

### Five root clocks

| ID | Name | Reconstructed rate |
| --- | --- | --- |
| 0 | xtal | always 19.2 MHz in OEM table |
| 1 | pll | boot-register decoded PLL, zero in bypass mode |
| 2 | arm | PLL frequency or 80 MHz bypass |
| 3 | 1x | PLL / 8 or 10 MHz bypass |
| 4 | 2x | PLL / 4 or 20 MHz bypass |

The boot strap at 0xfc000408 selects the direct bypass path when
`(value & 0xf000) == 0xf000`. Otherwise, the PLL at
`0xfc00041c` (ignoring bit0) is mapped by the stock kernel as follows:

| PLL register | OEM PLL rate |
| --- | --- |
| 0x3fa28206 | 832,000,000 Hz |
| 0x3fa2d90a | 833,280,000 Hz |
| 0x3fa2d48c | 680,000,000 Hz |
| 0x3fa29c06 | 998,000,000 Hz |
| 0x3fa47d06 | 400,000,000 Hz |
| any other value | 800,000,000 Hz |

The live P4 833.28 MHz selection thus produces 1x at
104.16 MHz and 2x at 208.32 MHz; neither is hardcoded.
Rates are latched once at early boot, just as in the OEM kernel.
No PLL reprogramming is attempted.

### All 27 OEM peripheral clock descriptors

The table is generated from the original clean-room CCF descriptors
to keep this documentation synchronized with the source.

| ID | Gate clock | Parent | Gate register | Bit |
| --- | --- | --- | --- | --- |
| 5 | gdm7243-udc | gdm7243-2x | 0xfc000438 | 4 |
| 6 | gdm7243-udc-40m | unknown | 0xfc00040c | 9 |
| 7 | gdm7243-timers | gdm7243-1x | none | — |
| 8 | gdm7243-uart0 | gdm7243-1x | 0xfc000438 | 3 |
| 9 | gdm7243-uart1 | gdm7243-1x | 0xfc000438 | 7 |
| 10 | gdm7243-uart2 | gdm7243-1x | 0xfc000438 | 23 |
| 11 | gdm7243-uart3 | gdm7243-1x | 0xfc000438 | 18 |
| 12 | gdm7243-xtal-32k | unknown | 0xfc00043c | 10 |
| 13 | gdm7243-wlan | gdm7243-1x | 0xfc000438 | 2 |
| 14 | gdm7243-i2s | unknown | 0xfc000438 | 5 |
| 15 | gdm7243-pcm | unknown | 0xfc000438 | 6 |
| 16 | gdm7243-eth0 | unknown | 0xfc000438 | 20 |
| 17 | gdm7243-eth1 | unknown | 0xfc00043c | 2 |
| 18 | gdm7243-usim | unknown | 0xfc000438 | 8 |
| 19 | gdm7243-sddev | unknown | 0xfc000438 | 10 |
| 20 | gdm7243-sdhost0 | unknown | 0xfc000438 | 21 |
| 21 | gdm7243-sdhost1 | unknown | 0xfc000438 | 28 |
| 22 | gdm7243-gsh | unknown | 0xfc00040c | 8 |
| 23 | gdm7243-nand | unknown | 0xfc000438 | 13 |
| 24 | gdm7243-pd-slv | unknown | 0xfc00043c | 22 |
| 25 | gdm7243-pu | unknown | 0xfc00040c | 26 |
| 26 | gdm7243-pur | unknown | 0xfc00040c | 27 |
| 27 | gdm7243-pd-usb | unknown | 0xfc00043c | 20 |
| 28 | gdm7243-i2c | gdm7243-1x | 0xfc000438 | 11 |
| 29 | gdm7243-uh | unknown | 0xfc00040c | 11 |
| 30 | gdm7243-uhb | unknown | 0xfc00043c | 4 |
| 31 | gdm7243-spi | unknown | 0xfc00043c | 0 |

Unknown means the original descriptor supplied neither a parent
nor a clock rate. The provider exports these gates without inventing
a source frequency; they support enable/disable but report unknown
rates until the underlying tree is reconstructed. `timers` is a
no-gate, unity-ratio child of `1x`; the UART0–3 clocks have separate
gates and inherit the actual 1x frequency.

## OEM gate transaction semantics

- The source waits for lock word 0xfc000010 for gates in the
  0xfc00040c register, otherwise 0xfc000018.
- Disabling clears the selected enable bit (read-modify-write).
- Enabling first clears that bit, then performs a clear/set pulse
  at 0xfc00048c (0x40c gates), 0xfc000440 (0x438 gates) or
  0xfc000444 (0x43c gates), then sets the original gate bit.
- Finally it writes zero to the hardware transaction-lock word.

The new CCF implementation uses one raw spinlock for all local
read-modify-write transactions and bounded polling of the OEM
hardware lock rather than modifying registers while another
processor holds them. It sets `CLK_IGNORE_UNUSED` on all gates
to prevent automatic shutdown of firmware/baseband dependencies
during initial kernel migration.

## Source and integration

- Early driver: `target/linux/gdm7243/files/drivers/clk/clk-gdm7243.c`
- Constants: `target/linux/gdm7243/files/include/dt-bindings/clock/gct,gdm7243.h`
- Clock binding: `Documentation/devicetree/bindings/clock/gct,gdm7243-clocks.yaml`
- Kconfig/Makefile patch: `patches-6.12/0007-clk-gdm7243-build-integration.patch`
- Clock provider DT: `clock-controller@fc000000` with
  `compatible = "gct,gdm7243-clocks"`, `reg = <0xfc000000 0x500>` and
  `#clock-cells = <1>`. Consumer examples use
  `<&clocks GDM7243_CLK_TIMERS>` and `<&clocks GDM7243_CLK_UART0>`.
- Registered with `CLK_OF_DECLARE` so the clocksource/timer OF
  startup can acquire 1x before the ordinary platform device probe.

## Hardware validation gates

Check the actual strap/PLL value in stock captures, UART baud/divisor
and timer reload after early-clock registration, both GPIO parent
interrupts and panic output. Keep the modem/baseband running during
RAM-only validation to detect accidental gate shutdown. The
clock tree and gate writes are source-recovered but not yet exercised
on the physical WF830. Additional peripheral parents and data rates
are deliberately left unresolved where the OEM table lists zero.
