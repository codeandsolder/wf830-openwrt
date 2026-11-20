# GDM7243 UART0-2 Linux tty driver

Implemented against Linux 6.12 using normal serial-core, not the OEM
custom character-device/line-discipline interface. Separate from the
polling GDM7243 early console.

## Source and OEM evidence

Actual Polish P4 BE8 Linux kernel:
 /home/jan/research/wf830-202610/linux-mainline-20261118/live-p4-vmlinux-be8.symbols.elf

| OEM routine | Virtual address | Register behavior |
| --- | --- | --- |
| gdm_uart_console_write | 0xd01a6c0c | TX data +0x00, status +0x14 bit2 TX full |
| gdm_uart_tx_empty | 0xd01a510c | Status +0x14 bit4 reports empty |
| gdm_rx_isr | 0xd01a6340 | Status +0x14 bit1 RX empty; max 64 bytes; error +0x04, data +0x00 |
| gdm_uart_stop_tx | 0xd01a515c | Control +0x10 bit4 TX interrupt enable |
| gdm_uart_stop_rx | 0xd01a518c | Control +0x10 bits3,5 RX IRQ enable |
| gdm_uart_startup | 0xd01a60a4 | RX/TX IRQs, errors +0x04 = 0xf, format +0x08 bit4, control +0x10 OR 0x2b |
| gdm_uart_set_termios | 0xd01a5c30 | Data bits +0x08 bits5–6; stop bit3; parity bits1–2; divisor +0x0c from actual UART clock |
| gdm_uart_init_port | 0xd01a6da8 | MMIO, RX IRQ and clock |
| gdm_uart_console_setup | 0xd04308d8 | Console defaults to 921600 baud |
| gdm_uart3_tx_empty | 0xd01a511c | UART3 different status layout, not compatible |

The original RX handler reads error register +0x04, writes the same
word back to acknowledge it and consumes the data at +0x00. Error
flags are bit0 frame, bit1 parity, bit2 break, bit3 overrun.

Actual OEM platform-device memory/IRQ resources (extracted from
big-endian ARMv7 P4 kernel data, not inferred from UART instance IDs):

| Device | Resource descriptor | Physical MMIO | RX IRQ | TX IRQ |
| --- | --- | --- | --- | --- |
| UART0 | 0xd04c9930 | 0xfc007500–0xfc00753f | 2 | 3 |
| UART1 | 0xd04c98f8 | 0xfc007540–0xfc00757f | 21 | 22 |
| UART2 | 0xd04c98c0 | 0xfc007580–0xfc0075bf | 33 | 34 |
| UART3 (incompatible) | 0xd04c9888 | 0xfc007600–0xfc00765f | 12 | same IRQ12 |

The source exports `gdm_uart0_device`.. `gdm_uart3_device`
through kernel symbol-table entries starting at 0xd03e9628,
with device pointers at 0xd04c9348, 0xd04c9258,
0xd04c9168 and 0xd04c9078. Each platform device's resource
array starts at the pointer in its +0xe4 field. Resource
records use 28-byte strides, with the MMIO or IRQ start/end
and flags. UART2's RX33/TX34 are therefore independently
verified; the generic serial driver now has complete hardware
resources for its three supported ports. UART3 uses a
different register architecture and is not compatible.

## Linux implementation

- Source: target/linux/gdm7243/files/drivers/tty/serial/gdm7243-uart.c
- Kernel integration: 0006-serial-gdm7243-uart-integration.patch
- Binding compatible: gct,gdm7243-uart, shared with earlycon
- One UART serial-core driver handling three hardware ports; stable
  serial0..2 DT aliases mandatory, named RX and TX interrupts.
- Devices are ttyGDM0..2; console=ttyGDM0,921600 supported when
  SERIAL_GDM7243_CONSOLE=y, and early boot remains handled by
  SERIAL_GDM7243_EARLYCON=y.
- Interrupt-driven transmit through Linux serial-core FIFO helpers,
  up to 64 RX or TX chars per interrupt, with proper IRQ request/
  free on tty open/close.
- RX reports break, parity, framing and overrun through tty flip
  buffering, including sysrq and termios input error masks.
- Real baud divisor from DT supplied clock, 5/6/7/8 data bits,
  optional second stop bit and odd/even parity. Serial-core B0
  hangup remains encoded as B0 rather than silently becoming 9600;
  invalid baud requests avoid a zero-divisor programming path.
- Hardware status/control registers preserved outside verified
  termios-configurable bits. Early UART clock is kept enabled
  through the normal platform driver's managed clock.
- Vendor modem-control does not set hardware outputs. CTS reads
  control bit2; DSR/CAR are synthesized high for these data-only
  serial lines. CTS/RTS hardware flow control requires board GPIO
  and pinmux mapping and is not enabled until those exist.

## Hardware validation prerequisites

The eventual board DTS must select correct clock, UART pinmux,
MMIO range, RX/TX interrupts, serial0 alias and chosen stdout-path.
Use kernel command line console=ttyGDM0,921600 and earlycon.

The original bootloader uses 921600 baud. Validate line speed,
hardware register endianness, clock gating, RX errors, bidirectional
sustained IRQ traffic, UART console handoff, panic behavior and
sleep/resume with a RAM-only boot before any NAND update.
