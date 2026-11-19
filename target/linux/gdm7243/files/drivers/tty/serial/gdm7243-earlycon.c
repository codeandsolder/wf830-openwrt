// SPDX-License-Identifier: GPL-2.0-only
/*
 * Early serial output for the GCT GDM7243 application CPU UART0-2.
 *
 * This is a boot-console-only implementation: the bootloader must have
 * programmed the port's baud rate and clock before Linux starts.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/serial_core.h>

#define GDM7243_UART_DATA       0x00
#define GDM7243_UART_STATUS     0x14
#define GDM7243_UART_TX_FULL    BIT(2)

static void gdm7243_early_putc(struct uart_port *port, unsigned char ch)
{
	while (readl_relaxed(port->membase + GDM7243_UART_STATUS) &
	       GDM7243_UART_TX_FULL)
		cpu_relax();

	writel_relaxed(ch, port->membase + GDM7243_UART_DATA);
}

static void gdm7243_early_write(struct console *con, const char *s,
				 unsigned int count)
{
	struct earlycon_device *device = con->data;

	uart_console_write(&device->port, s, count, gdm7243_early_putc);
}

static int __init gdm7243_early_setup(struct earlycon_device *device,
				      const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = gdm7243_early_write;
	return 0;
}
OF_EARLYCON_DECLARE(gdm7243, "gct,gdm7243-uart", gdm7243_early_setup);
