// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO controller for the GCT GDM7243QT application processor.
 *
 * Clean-room reconstruction from the Polish P4 WF830 BE8 kernel.
 * The vendor pin encoding is (bank << 8) | bit; gpiolib uses
 * contiguous offsets with 32 pins per bank instead.
 */
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
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

struct gdm7243_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	raw_spinlock_t lock;
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

static int gdm7243_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gdm7243_gpio *gpio;
	u32 lines;
	int ret;

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
