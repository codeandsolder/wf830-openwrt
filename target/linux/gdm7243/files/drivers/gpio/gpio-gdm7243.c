// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO controller for the GCT GDM7243QT application processor.
 *
 * Clean-room reconstruction from the Polish P4 WF830 BE8 kernel.
 * The vendor pin encoding is (bank << 8) | bit; gpiolib uses
 * contiguous offsets with 32 pins per bank instead.
 */
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define GDM7243_GPIO_DIRECTION	0x00
#define GDM7243_GPIO_DATA	0x40
#define GDM7243_GPIO_BANKS	16
#define GDM7243_GPIO_BANK_WIDTH	32
#define GDM7243_GPIO_MAX_LINES	(GDM7243_GPIO_BANKS * GDM7243_GPIO_BANK_WIDTH)
#define GDM7243_GPIO_IRQ_BANKS	7
#define GDM7243_GPIO_MAX_IRQ_LINES (GDM7243_GPIO_IRQ_BANKS * GDM7243_GPIO_BANK_WIDTH)

#define GDM7243_GPIO_IRQ_MODE	0x80
#define GDM7243_GPIO_IRQ_POLARITY	0xc0
#define GDM7243_GPIO_IRQ_SUMMARY	0x104
#define GDM7243_GPIO_IRQ_ENABLE	0x1c0
#define GDM7243_GPIO_IRQ_PENDING	0x2c0
#define GDM7243_GPIO_IRQ_CHANNEL_STRIDE	0x40
#define GDM7243_GPIO_IRQ_SUMMARY_STRIDE	0x04
#define GDM7243_GPIO_IRQ_BANK_MASK	GENMASK(GDM7243_GPIO_IRQ_BANKS - 1, 0)

struct gdm7243_gpio;

struct gdm7243_gpio_parent {
	struct gdm7243_gpio *gpio;
	unsigned int channel;
};

struct gdm7243_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	raw_spinlock_t lock;
	u32 irq1_mask[GDM7243_GPIO_IRQ_BANKS];
	unsigned int parent_irqs[2];
	struct gdm7243_gpio_parent parent[2];
	void *parent_data[2];
};

static void __iomem *gdm7243_gpio_reg(struct gdm7243_gpio *gpio,
				      unsigned int offset, unsigned int reg)
{
	return gpio->base + reg +
		(offset / GDM7243_GPIO_BANK_WIDTH) * sizeof(u32);
}

static void gdm7243_gpio_update(struct gdm7243_gpio *gpio,
				unsigned int offset, unsigned int reg,
				bool value)
{
	void __iomem *addr = gdm7243_gpio_reg(gpio, offset, reg);
	unsigned long flags;
	u32 bits = BIT(offset % GDM7243_GPIO_BANK_WIDTH);
	u32 word;

	raw_spin_lock_irqsave(&gpio->lock, flags);
	word = readl_relaxed(addr);
	if (value)
		word |= bits;
	else
		word &= ~bits;
	writel(word, addr);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
}

static int gdm7243_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	u32 word = readl(gdm7243_gpio_reg(gpio, offset,
					  GDM7243_GPIO_DIRECTION));

	return !(word & BIT(offset % GDM7243_GPIO_BANK_WIDTH));
}

static int gdm7243_gpio_direction_input(struct gpio_chip *gc,
					unsigned int offset)
{
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);

	gdm7243_gpio_update(gpio, offset, GDM7243_GPIO_DIRECTION, false);
	return 0;
}

static int gdm7243_gpio_direction_output(struct gpio_chip *gc,
					 unsigned int offset, int value)
{
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	void __iomem *data = gdm7243_gpio_reg(gpio, offset, GDM7243_GPIO_DATA);
	void __iomem *dir =
		gdm7243_gpio_reg(gpio, offset, GDM7243_GPIO_DIRECTION);
	unsigned long flags;
	u32 bit = BIT(offset % GDM7243_GPIO_BANK_WIDTH);
	u32 word;

	/* Keep the latch write and direction change atomic against set(). */
	raw_spin_lock_irqsave(&gpio->lock, flags);
	word = readl_relaxed(data);
	writel(value ? word | bit : word & ~bit, data);
	word = readl_relaxed(dir);
	writel(word | bit, dir);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static int gdm7243_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	u32 word = readl(gdm7243_gpio_reg(gpio, offset,
					  GDM7243_GPIO_DATA));

	return !!(word & BIT(offset % GDM7243_GPIO_BANK_WIDTH));
}

static void gdm7243_gpio_set(struct gpio_chip *gc, unsigned int offset,
			     int value)
{
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);

	gdm7243_gpio_update(gpio, offset, GDM7243_GPIO_DATA, value);
}

/*
 * GPIO0 (parent IRQ 10) and GPIO1 (parent IRQ 29) have independently
 * masked/pending banks but share the mode/polarity register arrays.
 * The board-defined route mask gives each Linux GPIO IRQ one parent.
 */
static unsigned int gdm7243_gpio_irq_channel(struct gdm7243_gpio *gpio,
					     unsigned int offset)
{
	unsigned int bank = offset / GDM7243_GPIO_BANK_WIDTH;
	unsigned int bit = offset % GDM7243_GPIO_BANK_WIDTH;

	return !!(gpio->irq1_mask[bank] & BIT(bit));
}

static void __iomem *gdm7243_gpio_irq_reg(struct gdm7243_gpio *gpio,
					  unsigned int channel,
					  unsigned int offset,
					  unsigned int reg)
{
	return gpio->base + reg +
		channel * GDM7243_GPIO_IRQ_CHANNEL_STRIDE +
		(offset / GDM7243_GPIO_BANK_WIDTH) * sizeof(u32);
}

static void gdm7243_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	unsigned int pin = irqd_to_hwirq(data);
	unsigned int channel = gdm7243_gpio_irq_channel(gpio, pin);
	void __iomem *reg =
		gdm7243_gpio_irq_reg(gpio, channel, pin,
					  GDM7243_GPIO_IRQ_ENABLE);
	unsigned long flags;
	u32 value;

	raw_spin_lock_irqsave(&gpio->lock, flags);
	value = readl_relaxed(reg) & ~BIT(pin % GDM7243_GPIO_BANK_WIDTH);
	writel(value, reg);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
	gpiochip_disable_irq(gc, pin);
}

static void gdm7243_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	unsigned int pin = irqd_to_hwirq(data);
	unsigned int channel = gdm7243_gpio_irq_channel(gpio, pin);
	void __iomem *reg =
		gdm7243_gpio_irq_reg(gpio, channel, pin,
					  GDM7243_GPIO_IRQ_ENABLE);
	unsigned long flags;
	u32 value;

	gpiochip_enable_irq(gc, pin);
	raw_spin_lock_irqsave(&gpio->lock, flags);
	value = readl_relaxed(reg) | BIT(pin % GDM7243_GPIO_BANK_WIDTH);
	writel(value, reg);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
}

static int gdm7243_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	unsigned int pin = irqd_to_hwirq(data);
	void __iomem *mode, *polarity;
	unsigned long flags;
	u32 bit, mode_value, polarity_value;
	bool level, active_high;

	/*
	 * OEM request_gpio{0,1}_irq encodes level mode as low nibble 0,
	 * edge mode as nonzero, and high polarity as high nibble 0x10.
	 * GPIO0 supports a custom both-edge method by sampling the pin,
	 * but the two channels do not expose identical both-edge paths.
	 */
	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		level = false;
		active_high = true;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		level = false;
		active_high = false;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		level = true;
		active_high = true;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		level = true;
		active_high = false;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * The mode/polarity bits are shared between GPIO0 and GPIO1.
	 * One routing choice per pin avoids conflicting configurations.
	 */
	mode = gdm7243_gpio_reg(gpio, pin, GDM7243_GPIO_IRQ_MODE);
	polarity = gdm7243_gpio_reg(gpio, pin, GDM7243_GPIO_IRQ_POLARITY);
	bit = BIT(pin % GDM7243_GPIO_BANK_WIDTH);

	raw_spin_lock_irqsave(&gpio->lock, flags);
	mode_value = readl_relaxed(mode);
	polarity_value = readl_relaxed(polarity);
	writel(level ? mode_value | bit : mode_value & ~bit, mode);
	writel(active_high ? polarity_value | bit :
	       polarity_value & ~bit, polarity);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static const struct irq_chip gdm7243_gpio_irqchip = {
	.name = "gdm7243-gpio",
	.irq_mask = gdm7243_gpio_irq_mask,
	.irq_unmask = gdm7243_gpio_irq_unmask,
	.irq_set_type = gdm7243_gpio_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void gdm7243_gpio_irq_valid_mask(struct gpio_chip *gc,
					unsigned long *valid_mask,
					unsigned int ngpios)
{
	if (ngpios > GDM7243_GPIO_MAX_IRQ_LINES)
		bitmap_clear(valid_mask, GDM7243_GPIO_MAX_IRQ_LINES,
			     ngpios - GDM7243_GPIO_MAX_IRQ_LINES);
}

static int gdm7243_gpio_irq_init_hw(struct gpio_chip *gc)
{
	struct gdm7243_gpio *gpio = gpiochip_get_data(gc);
	unsigned int channel, bank;
	void __iomem *summary;

	for (channel = 0; channel < 2; channel++) {
		for (bank = 0; bank < GDM7243_GPIO_IRQ_BANKS; bank++) {
			void __iomem *mask = gpio->base +
				GDM7243_GPIO_IRQ_ENABLE +
				channel * GDM7243_GPIO_IRQ_CHANNEL_STRIDE +
				bank * sizeof(u32);
			void __iomem *status = gpio->base +
				GDM7243_GPIO_IRQ_PENDING +
				channel * GDM7243_GPIO_IRQ_CHANNEL_STRIDE +
				bank * sizeof(u32);

			/* Kernel takes ownership of both IRQ channels. */
			writel(0, mask);
			writel(readl(status), status);
		}
		summary = gpio->base + GDM7243_GPIO_IRQ_SUMMARY +
			  channel * GDM7243_GPIO_IRQ_SUMMARY_STRIDE;
		writel(readl(summary), summary);
	}
	return 0;
}

static void gdm7243_gpio_parent_irq(struct irq_desc *desc)
{
	struct gdm7243_gpio_parent *parent =
		irq_desc_get_handler_data(desc);
	struct gdm7243_gpio *gpio = parent->gpio;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int channel = parent->channel;
	unsigned int bank, bit, pin;
	void __iomem *summary = gpio->base + GDM7243_GPIO_IRQ_SUMMARY +
		channel * GDM7243_GPIO_IRQ_SUMMARY_STRIDE;
	u32 banks, pending[GDM7243_GPIO_IRQ_BANKS] = {};
	u32 enabled[GDM7243_GPIO_IRQ_BANKS] = {};

	chained_irq_enter(chip, desc);
	banks = readl(summary) & GDM7243_GPIO_IRQ_BANK_MASK;
	for (bank = 0; bank < GDM7243_GPIO_IRQ_BANKS; bank++) {
		void __iomem *status = gpio->base +
			GDM7243_GPIO_IRQ_PENDING +
			channel * GDM7243_GPIO_IRQ_CHANNEL_STRIDE +
			bank * sizeof(u32);

		if (!(banks & BIT(bank)))
			continue;

		/*
		 * OEM gpio_clear_interrupt acknowledges all seven bank status
		 * words first, followed by the per-channel bank summary.
		 * Read, clear and dispatch the snapshot exactly once.
		 */
		pending[bank] = readl(status);
		enabled[bank] = readl(gpio->base + GDM7243_GPIO_IRQ_ENABLE +
			channel * GDM7243_GPIO_IRQ_CHANNEL_STRIDE +
			bank * sizeof(u32));
		writel(pending[bank], status);
	}
	writel(banks, summary);

	for (bank = 0; bank < GDM7243_GPIO_IRQ_BANKS; bank++) {
		unsigned long active = pending[bank] & enabled[bank];
		u32 route = gpio->irq1_mask[bank];

		if (channel == 0)
			active &= ~route;
		else
			active &= route;

		for_each_set_bit(bit, &active, GDM7243_GPIO_BANK_WIDTH) {
			pin = bank * GDM7243_GPIO_BANK_WIDTH + bit;
			if (pin < gpio->gc.ngpio)
				generic_handle_domain_irq(gpio->gc.irq.domain, pin);
		}
	}
	chained_irq_exit(chip, desc);
}

static int gdm7243_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gdm7243_gpio *gpio;
	struct gpio_irq_chip *girq;
	u32 lines;
	int ret, channel;

	ret = device_property_read_u32(dev, "ngpios", &lines);
	if (ret)
		return ret;
	if (!lines || lines > GDM7243_GPIO_MAX_LINES)
		return -EINVAL;

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	raw_spin_lock_init(&gpio->lock);

	/*
	 * Each GPIO may be routed to either parent IRQ. Select GPIO1
	 * when the corresponding bit is set in gct,irq1-mask; other pins
	 * use GPIO0 by default. No runtime routing change is permitted.
	 */
	if (device_property_present(dev, "gct,irq1-mask")) {
		if (device_property_count_u32(dev, "gct,irq1-mask") !=
		    GDM7243_GPIO_IRQ_BANKS)
			return dev_err_probe(dev, -EINVAL,
					     "expected seven GPIO1 routing masks\n");
		ret = device_property_read_u32_array(dev, "gct,irq1-mask",
					gpio->irq1_mask,
					GDM7243_GPIO_IRQ_BANKS);
		if (ret)
			return ret;
	}

	for (channel = 0; channel < GDM7243_GPIO_IRQ_BANKS; channel++) {
		unsigned int first = channel * GDM7243_GPIO_BANK_WIDTH;
		unsigned int remaining;

		if (first >= lines) {
			if (gpio->irq1_mask[channel])
				return dev_err_probe(dev, -EINVAL,
					"IRQ routing outside populated GPIOs\n");
			continue;
		}
		remaining = min_t(unsigned int, lines - first,
				    GDM7243_GPIO_BANK_WIDTH);
		if (remaining < GDM7243_GPIO_BANK_WIDTH &&
		    (gpio->irq1_mask[channel] &
		     ~GENMASK(remaining - 1, 0)))
			return dev_err_probe(dev, -EINVAL,
				"IRQ routing outside populated GPIOs\n");
	}

	for (channel = 0; channel < 2; channel++) {
		int irq = platform_get_irq_byname(pdev,
					  channel ? "gpio1" : "gpio0");

		if (irq < 0)
			return irq;
		gpio->parent_irqs[channel] = irq;
		gpio->parent[channel].gpio = gpio;
		gpio->parent[channel].channel = channel;
		gpio->parent_data[channel] = &gpio->parent[channel];
	}

	gpio->gc.label = dev_name(dev);
	gpio->gc.parent = dev;
	gpio->gc.owner = THIS_MODULE;
	gpio->gc.base = -1;
	gpio->gc.ngpio = lines;
	gpio->gc.get_direction = gdm7243_gpio_get_direction;
	gpio->gc.direction_input = gdm7243_gpio_direction_input;
	gpio->gc.direction_output = gdm7243_gpio_direction_output;
	gpio->gc.get = gdm7243_gpio_get;
	gpio->gc.set = gdm7243_gpio_set;

	girq = &gpio->gc.irq;
	gpio_irq_chip_set_chip(girq, &gdm7243_gpio_irqchip);
	girq->handler = handle_simple_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->init_hw = gdm7243_gpio_irq_init_hw;
	girq->init_valid_mask = gdm7243_gpio_irq_valid_mask;
	girq->parent_handler = gdm7243_gpio_parent_irq;
	girq->num_parents = ARRAY_SIZE(gpio->parent_irqs);
	girq->parents = gpio->parent_irqs;
	girq->per_parent_data = true;
	girq->parent_handler_data_array = gpio->parent_data;

	return devm_gpiochip_add_data(dev, &gpio->gc, gpio);
}

static const struct of_device_id gdm7243_gpio_of_match[] = {
	{ .compatible = "gct,gdm7243-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, gdm7243_gpio_of_match);

static struct platform_driver gdm7243_gpio_driver = {
	.probe = gdm7243_gpio_probe,
	.driver = {
		.name = "gdm7243-gpio",
		.of_match_table = gdm7243_gpio_of_match,
	},
};
module_platform_driver(gdm7243_gpio_driver);

MODULE_DESCRIPTION("GCT GDM7243 banked GPIO driver");
MODULE_LICENSE("GPL");
