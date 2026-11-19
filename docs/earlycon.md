# GDM7243 UART0–2 early console

Evidence from live Polish P4 kernel disassembly:

- `gdm_uart_console_write`, address `0xd01a6c0c`: for UART0–2,
  wait until register **offset 0x14 bit 2 = 0**, then transmit a
  character via `xmit_char` at `0xd01a56bc`.
- `xmit_char` writes a 32-bit character to register **offset 0x00**.
  CRLF conversion is handled in modern Linux by `uart_console_write`.
- `gdm_uart_tx_empty` at `0xd01a510c` reads bit 4 of offset 0x14;
  this is *not* the same indication as TX FIFO space.
- `gdm_uart_init_port` at `0xd01a6da8` and the OEM boot log
  confirm UART0 MMIO `0xfc007500`, UART1 `0xfc007540`,
  UART2 `0xfc007580`. UART3 at `0xfc007600` has a **different**
  register layout (its FIFO test uses offset 0x0c), and must not
  match the UART0–2 compatible string.
- UART0 has separate hardware IRQs RX=2 and TX=3. UART1 RX=21
  TX=22; UART2 data-plane IRQ wiring must be recovered separately.
- Bootloader config is 921600 baud, and the Linux 3.10 kernel command
  line contains `console=ttyS0,921600`.

The `OF_EARLYCON_DECLARE` driver implements only polling output and
inherits the hardware configuration from U-Boot. It can be used with
`earlycon` and an enabled `chosen.stdout-path` in a suitable
board device tree; it does not implement ttyS0, input, termios, IRQ
handling, pinmux, clocks or normal console handoff.

Before physical boot: validate UART0 pin routing, effective MMIO
endianness when changing the application CPU to LE, and 921600 baud.
