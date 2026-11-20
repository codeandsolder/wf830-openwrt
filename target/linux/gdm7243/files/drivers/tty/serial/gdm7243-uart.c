// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCT GDM7243QT UART0-2 serial controller.
 *
 * Register operations are reconstructed from the Polish P4 WF830
 * ARMv7 BE8 kernel. UART3 uses a different block and is not covered.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty_flip.h>

#define GDM7243_UART_NR		3
#define GDM7243_UART_FIFO_SIZE	64

#define GDM7243_UART_DATA	0x00
#define GDM7243_UART_ERROR	0x04
#define GDM7243_UART_FORMAT	0x08
#define GDM7243_UART_DIVISOR	0x0c
#define GDM7243_UART_CONTROL	0x10
#define GDM7243_UART_STATUS	0x14

#define GDM7243_RX_EMPTY	BIT(1)
#define GDM7243_TX_FULL		BIT(2)
#define GDM7243_TX_IDLE		BIT(4)

#define GDM7243_RX_FRAME	BIT(0)
#define GDM7243_RX_PARITY	BIT(1)
#define GDM7243_RX_BREAK	BIT(2)
#define GDM7243_RX_OVERRUN	BIT(3)
#define GDM7243_RX_DISABLED	BIT(16)
#define GDM7243_RX_ERROR_MASK	GENMASK(3, 0)

#define GDM7243_FMT_DATA_MASK	GENMASK(6, 5)
#define GDM7243_FMT_TWO_STOP	BIT(3)
#define GDM7243_FMT_EVEN	BIT(2)
#define GDM7243_FMT_PARITY	BIT(1)
#define GDM7243_FMT_FIFO	BIT(4)
#define GDM7243_FMT_TERMIOS_MASK \
	(GDM7243_FMT_DATA_MASK | GDM7243_FMT_TWO_STOP | \
	 GDM7243_FMT_EVEN | GDM7243_FMT_PARITY)

#define GDM7243_CONTROL_TX_IRQ	BIT(4)
#define GDM7243_CONTROL_RX_IRQ	(BIT(3) | BIT(5))
#define GDM7243_CONTROL_RX_START	0x2b

struct gdm7243_uart {
	struct uart_port port;
	struct clk *clock;
	int rx_irq;
	int tx_irq;
};

static struct uart_port *gdm7243_ports[GDM7243_UART_NR];
static struct uart_driver gdm7243_uart_driver;

static inline u32 gdm7243_read(struct uart_port *port, unsigned int reg)
{
	return readl_relaxed(port->membase + reg);
}

static inline void gdm7243_write(struct uart_port *port,
				  unsigned int reg, u32 value)
{
	writel_relaxed(value, port->membase + reg);
}

static unsigned int gdm7243_tx_empty(struct uart_port *port)
{
	return (gdm7243_read(port, GDM7243_UART_STATUS) &
		GDM7243_TX_IDLE) ? TIOCSER_TEMT : 0;
}

static void gdm7243_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* The OEM set_mctrl operation is a no-op for UART0-2. */
}

static unsigned int gdm7243_get_mctrl(struct uart_port *port)
{
	/*
	 * The original get_mctrl reads bit 2 of the control register
	 * as CTS. These SoC UARTs have no DCD/DSR state registers.
	 */
	u32 control = gdm7243_read(port, GDM7243_UART_CONTROL);

	return TIOCM_CAR | TIOCM_DSR |
	       ((control & BIT(2)) ? TIOCM_CTS : 0);
}

static void gdm7243_stop_tx(struct uart_port *port)
{
	u32 value = gdm7243_read(port, GDM7243_UART_CONTROL);

	gdm7243_write(port, GDM7243_UART_CONTROL,
		      value & ~GDM7243_CONTROL_TX_IRQ);
}

static void gdm7243_tx_chars(struct uart_port *port)
{
	unsigned char ch;

	uart_port_tx_limited(port, ch, GDM7243_UART_FIFO_SIZE,
		!(gdm7243_read(port, GDM7243_UART_STATUS) &
		  GDM7243_TX_FULL),
		gdm7243_write(port, GDM7243_UART_DATA, ch),
		({}));
}

static void gdm7243_start_tx(struct uart_port *port)
{
	u32 value = gdm7243_read(port, GDM7243_UART_CONTROL);

	gdm7243_write(port, GDM7243_UART_CONTROL,
		      value | GDM7243_CONTROL_TX_IRQ);
	gdm7243_tx_chars(port);
}

static void gdm7243_stop_rx(struct uart_port *port)
{
	u32 value = gdm7243_read(port, GDM7243_UART_CONTROL);

	gdm7243_write(port, GDM7243_UART_CONTROL,
		      value & ~GDM7243_CONTROL_RX_IRQ);
}

static irqreturn_t gdm7243_tx_irq(int irq, void *data)
{
	struct uart_port *port = data;

	uart_port_lock(port);
	gdm7243_tx_chars(port);
	uart_port_unlock(port);
	return IRQ_HANDLED;
}

static irqreturn_t gdm7243_rx_irq(int irq, void *arg)
{
	struct uart_port *port = arg;
	struct tty_port *ttyport = &port->state->port;
	unsigned int count = GDM7243_UART_FIFO_SIZE;
	u32 error, data;
	char flag;

	uart_port_lock(port);
	while (count-- && !(gdm7243_read(port, GDM7243_UART_STATUS) &
			     GDM7243_RX_EMPTY)) {
		error = gdm7243_read(port, GDM7243_UART_ERROR) &
			GDM7243_RX_ERROR_MASK;
		/* Reading the error register does not clear it. */
		gdm7243_write(port, GDM7243_UART_ERROR, error);
		data = gdm7243_read(port, GDM7243_UART_DATA);
		port->icount.rx++;

		if (error & GDM7243_RX_BREAK) {
			port->icount.brk++;
			if (uart_handle_break(port))
				continue;
		}
		if (error & GDM7243_RX_PARITY)
			port->icount.parity++;
		if (error & GDM7243_RX_FRAME)
			port->icount.frame++;
		if (error & GDM7243_RX_OVERRUN)
			port->icount.overrun++;
		if (uart_handle_sysrq_char(port, data & 0xff))
			continue;

		/* Ignore flags must survive even when INPCK is disabled. */
		error &= port->read_status_mask | port->ignore_status_mask;
		flag = TTY_NORMAL;
		if (error & GDM7243_RX_BREAK)
			flag = TTY_BREAK;
		else if (error & GDM7243_RX_PARITY)
			flag = TTY_PARITY;
		else if (error & GDM7243_RX_FRAME)
			flag = TTY_FRAME;

		uart_insert_char(port, error | GDM7243_RX_DISABLED,
				 GDM7243_RX_OVERRUN, data & 0xff, flag);
	}
	uart_port_unlock(port);
	tty_flip_buffer_push(ttyport);
	return IRQ_HANDLED;
}

static int gdm7243_startup(struct uart_port *port)
{
	struct gdm7243_uart *uart = container_of(port, struct gdm7243_uart,
						 port);
	u32 value;
	int ret;

	ret = request_irq(uart->rx_irq, gdm7243_rx_irq, 0,
			  "gdm7243-rx", port);
	if (ret)
		return ret;
	ret = request_irq(uart->tx_irq, gdm7243_tx_irq, 0,
			  "gdm7243-tx", port);
	if (ret) {
		free_irq(uart->rx_irq, port);
		return ret;
	}

	/* A write of 0xf to the OEM error register resets error flags. */
	gdm7243_write(port, GDM7243_UART_ERROR, GDM7243_RX_ERROR_MASK);
	value = gdm7243_read(port, GDM7243_UART_FORMAT);
	gdm7243_write(port, GDM7243_UART_FORMAT,
		      value | GDM7243_FMT_FIFO);
	value = gdm7243_read(port, GDM7243_UART_CONTROL);
	gdm7243_write(port, GDM7243_UART_CONTROL,
		      (value | GDM7243_CONTROL_RX_START) &
		      ~GDM7243_CONTROL_TX_IRQ);
	return 0;
}

static void gdm7243_shutdown(struct uart_port *port)
{
	struct gdm7243_uart *uart = container_of(port, struct gdm7243_uart,
						 port);
	u32 value = gdm7243_read(port, GDM7243_UART_CONTROL);

	gdm7243_write(port, GDM7243_UART_CONTROL,
		      value & ~(GDM7243_CONTROL_RX_START |
				GDM7243_CONTROL_TX_IRQ));
	free_irq(uart->tx_irq, port);
	free_irq(uart->rx_irq, port);
}

static void gdm7243_set_termios(struct uart_port *port,
				struct ktermios *termios,
				const struct ktermios *old)
{
	unsigned int baud, divisor;
	unsigned long flags;
	u32 format, cfg = 0;

	/* CTS/RTS wiring is board-specific and cannot use generic modem IO. */
	termios->c_cflag &= ~(CRTSCTS | CMSPAR);

	switch (termios->c_cflag & CSIZE) {
	case CS6:
		cfg |= BIT(5);
		break;
	case CS7:
		cfg |= BIT(6);
		break;
	case CS8:
		cfg |= BIT(5) | BIT(6);
		break;
	default:
		break;
	}

	if (termios->c_cflag & CSTOPB)
		cfg |= GDM7243_FMT_TWO_STOP;
	if (termios->c_cflag & PARENB) {
		cfg |= GDM7243_FMT_PARITY;
		if (!(termios->c_cflag & PARODD))
			cfg |= GDM7243_FMT_EVEN;
	}

	baud = uart_get_baud_rate(port, termios, old, 1, port->uartclk);
	if (!baud)
		return;
	divisor = DIV_ROUND_CLOSEST_ULL((u64)port->uartclk, baud);
	divisor = max(divisor, 1U);

	uart_port_lock_irqsave(port, &flags);
	uart_update_timeout(port, termios->c_cflag, baud);
	port->read_status_mask = GDM7243_RX_OVERRUN;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= GDM7243_RX_FRAME |
					  GDM7243_RX_PARITY;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= GDM7243_RX_BREAK;

	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= GDM7243_RX_FRAME |
					    GDM7243_RX_PARITY;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= GDM7243_RX_BREAK;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= GDM7243_RX_OVERRUN;
	}
	if (!(termios->c_cflag & CREAD))
		port->ignore_status_mask |= GDM7243_RX_DISABLED;

	format = gdm7243_read(port, GDM7243_UART_FORMAT);
	format = (format & ~GDM7243_FMT_TERMIOS_MASK) | cfg;
	gdm7243_write(port, GDM7243_UART_FORMAT, format);
	gdm7243_write(port, GDM7243_UART_DIVISOR, divisor);
	uart_port_unlock_irqrestore(port, flags);

	/* A B0 hangup must remain B0 in termios, not become 9600 baud. */
	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);
}

static const char *gdm7243_type(struct uart_port *port)
{
	return "GCT GDM7243 UART0-2";
}

static void gdm7243_release_port(struct uart_port *port)
{
	/* The resource mapping is managed by the platform device. */
}

static int gdm7243_request_port(struct uart_port *port)
{
	return 0;
}

static void gdm7243_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_GENERIC;
}

static int gdm7243_verify_port(struct uart_port *port,
			      struct serial_struct *serial)
{
	return -EINVAL;
}

static const struct uart_ops gdm7243_ops = {
	.tx_empty = gdm7243_tx_empty,
	.set_mctrl = gdm7243_set_mctrl,
	.get_mctrl = gdm7243_get_mctrl,
	.stop_tx = gdm7243_stop_tx,
	.start_tx = gdm7243_start_tx,
	.stop_rx = gdm7243_stop_rx,
	.startup = gdm7243_startup,
	.shutdown = gdm7243_shutdown,
	.set_termios = gdm7243_set_termios,
	.type = gdm7243_type,
	.release_port = gdm7243_release_port,
	.request_port = gdm7243_request_port,
	.config_port = gdm7243_config_port,
	.verify_port = gdm7243_verify_port,
};

#ifdef CONFIG_SERIAL_GDM7243_CONSOLE
static void gdm7243_console_putchar(struct uart_port *port, unsigned char ch)
{
	while (gdm7243_read(port, GDM7243_UART_STATUS) & GDM7243_TX_FULL)
		cpu_relax();

	gdm7243_write(port, GDM7243_UART_DATA, ch);
}

static void gdm7243_console_write(struct console *con, const char *s,
				   unsigned int count)
{
	struct uart_port *port;
	unsigned long flags = 0;
	unsigned int control;
	bool locked;

	if (con->index < 0 || con->index >= GDM7243_UART_NR)
		return;

	port = READ_ONCE(gdm7243_ports[con->index]);
	if (!port)
		return;

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(port, &flags);
	else {
		uart_port_lock_irqsave(port, &flags);
		locked = true;
	}

	control = gdm7243_read(port, GDM7243_UART_CONTROL);
	gdm7243_write(port, GDM7243_UART_CONTROL,
		      control & ~GDM7243_CONTROL_TX_IRQ);
	uart_console_write(port, s, count, gdm7243_console_putchar);
	gdm7243_write(port, GDM7243_UART_CONTROL, control);

	if (locked)
		uart_port_unlock_irqrestore(port, flags);
}

static int __init gdm7243_console_setup(struct console *con, char *options)
{
	struct uart_port *port;
	int baud = 921600, bits = 8, parity = 'n', flow = 'n';

	if (con->index < 0)
		con->index = 0;
	if (con->index >= GDM7243_UART_NR)
		return -ENODEV;

	port = READ_ONCE(gdm7243_ports[con->index]);
	if (!port)
		return -ENODEV;
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, con, baud, parity, bits, flow);
}

static struct console gdm7243_console = {
	.name = "ttyGDM",
	.device = uart_console_device,
	.write = gdm7243_console_write,
	.setup = gdm7243_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,
	.data = &gdm7243_uart_driver,
};
#define GDM7243_CONSOLE	(&gdm7243_console)
#else
#define GDM7243_CONSOLE	NULL
#endif

static struct uart_driver gdm7243_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = "gdm7243-uart",
	.dev_name = "ttyGDM",
	.nr = GDM7243_UART_NR,
	.cons = GDM7243_CONSOLE,
};

static int gdm7243_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gdm7243_uart *uart;
	struct resource *mem;
	int line, ret;

	line = of_alias_get_id(dev->of_node, "serial");
	if (line < 0 || line >= GDM7243_UART_NR)
		return dev_err_probe(dev, -EINVAL,
				     "requires serial0..2 DT alias\n");
	if (READ_ONCE(gdm7243_ports[line]))
		return -EBUSY;

	uart = devm_kzalloc(dev, sizeof(*uart), GFP_KERNEL);
	if (!uart)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -EINVAL;
	uart->port.membase = devm_ioremap_resource(dev, mem);
	if (IS_ERR(uart->port.membase))
		return PTR_ERR(uart->port.membase);

	uart->clock = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(uart->clock))
		return dev_err_probe(dev, PTR_ERR(uart->clock),
				     "cannot enable UART clock\n");
	uart->port.uartclk = clk_get_rate(uart->clock);
	if (!uart->port.uartclk)
		return dev_err_probe(dev, -EINVAL, "UART clock rate is zero\n");

	uart->rx_irq = platform_get_irq_byname(pdev, "rx");
	if (uart->rx_irq < 0)
		return uart->rx_irq;
	uart->tx_irq = platform_get_irq_byname(pdev, "tx");
	if (uart->tx_irq < 0)
		return uart->tx_irq;

	uart->port.dev = dev;
	uart->port.mapbase = mem->start;
	uart->port.line = line;
	uart->port.irq = uart->rx_irq;
	uart->port.iotype = UPIO_MEM32;
	uart->port.fifosize = GDM7243_UART_FIFO_SIZE;
	uart->port.flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT;
	uart->port.type = PORT_GENERIC;
	uart->port.ops = &gdm7243_ops;

	platform_set_drvdata(pdev, uart);
	WRITE_ONCE(gdm7243_ports[line], &uart->port);
	ret = uart_add_one_port(&gdm7243_uart_driver, &uart->port);
	if (ret)
		WRITE_ONCE(gdm7243_ports[line], NULL);
	return ret;
}

static void gdm7243_remove(struct platform_device *pdev)
{
	struct gdm7243_uart *uart = platform_get_drvdata(pdev);

	uart_remove_one_port(&gdm7243_uart_driver, &uart->port);
	WRITE_ONCE(gdm7243_ports[uart->port.line], NULL);
}

static const struct of_device_id gdm7243_uart_of_match[] = {
	{ .compatible = "gct,gdm7243-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, gdm7243_uart_of_match);

static struct platform_driver gdm7243_platform_driver = {
	.probe = gdm7243_probe,
	.remove = gdm7243_remove,
	.driver = {
		.name = "gdm7243-uart",
		.of_match_table = gdm7243_uart_of_match,
	},
};

static int __init gdm7243_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&gdm7243_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&gdm7243_platform_driver);
	if (ret)
		uart_unregister_driver(&gdm7243_uart_driver);
	return ret;
}
module_init(gdm7243_uart_init);

static void __exit gdm7243_uart_exit(void)
{
	platform_driver_unregister(&gdm7243_platform_driver);
	uart_unregister_driver(&gdm7243_uart_driver);
}
module_exit(gdm7243_uart_exit);

MODULE_DESCRIPTION("GCT GDM7243 UART0-2 serial driver");
MODULE_LICENSE("GPL");
