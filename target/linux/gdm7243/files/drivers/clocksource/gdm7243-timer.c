// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCT GDM7243QT system clockevent timer.
 *
 * Reconstructed from the Polish P4 BE8 vendor kernel. Hardware IRQ 18
 * is a periodic timer; the vendor's one-shot callback never programs
 * a timer register, so no one-shot functionality is advertised here.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#define GDM7243_TIMER_RELOAD		0x00
#define GDM7243_TIMER_CONTROL		0x10
#define GDM7243_TIMER_ENABLE		BIT(3)
#define GDM7243_TIMER_PERIODIC_CONTROL	0x2c

struct gdm7243_timer {
	void __iomem *base;
	struct clk *clk;
	u32 reload;
	unsigned long rate;
	struct clock_event_device clockevent;
};

static struct gdm7243_timer *gdm7243_to_timer(struct clock_event_device *evt)
{
	return container_of(evt, struct gdm7243_timer, clockevent);
}

static int gdm7243_timer_shutdown(struct clock_event_device *evt)
{
	struct gdm7243_timer *timer = gdm7243_to_timer(evt);
	void __iomem *control = timer->base + GDM7243_TIMER_CONTROL;
	u32 value = readl_relaxed(control);

	/* Match the OEM disable sequence, which clears bit 3 only. */
	writel(value & ~GDM7243_TIMER_ENABLE, control);
	return 0;
}

static int gdm7243_timer_set_periodic(struct clock_event_device *evt)
{
	struct gdm7243_timer *timer = gdm7243_to_timer(evt);

	gdm7243_timer_shutdown(evt);
	writel(timer->reload, timer->base + GDM7243_TIMER_RELOAD);
	writel(GDM7243_TIMER_PERIODIC_CONTROL,
	       timer->base + GDM7243_TIMER_CONTROL);
	return 0;
}

static irqreturn_t gdm7243_timer_interrupt(int irq, void *dev_id)
{
	struct gdm7243_timer *timer = dev_id;

	/* The GDM7243 root irqchip performs the mask/ack pulse. */
	if (likely(timer->clockevent.event_handler))
		timer->clockevent.event_handler(&timer->clockevent);
	return IRQ_HANDLED;
}

static int __init gdm7243_timer_init(struct device_node *node)
{
	struct gdm7243_timer *timer;
	u64 cycles;
	int irq, ret;

	timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!timer)
		return -ENOMEM;

	timer->base = of_iomap(node, 0);
	if (!timer->base) {
		ret = -ENOMEM;
		goto free_timer;
	}

	timer->clk = of_clk_get(node, 0);
	if (IS_ERR(timer->clk)) {
		ret = PTR_ERR(timer->clk);
		goto unmap;
	}

	ret = clk_prepare_enable(timer->clk);
	if (ret)
		goto put_clk;

	timer->rate = clk_get_rate(timer->clk);
	cycles = DIV_ROUND_CLOSEST_ULL((u64)timer->rate, HZ);
	if (!cycles || cycles > (u64)U32_MAX + 1) {
		ret = -EINVAL;
		goto disable_clk;
	}
	timer->reload = cycles - 1;

	timer->clockevent.name = "gdm7243-tick";
	timer->clockevent.rating = 300;
	timer->clockevent.features = CLOCK_EVT_FEAT_PERIODIC;
	timer->clockevent.cpumask = cpumask_of(0);
	timer->clockevent.set_state_shutdown = gdm7243_timer_shutdown;
	timer->clockevent.set_state_periodic = gdm7243_timer_set_periodic;

	/* Halt any timer inherited from the bootloader before enabling IRQs. */
	gdm7243_timer_shutdown(&timer->clockevent);

	irq = irq_of_parse_and_map(node, 0);
	if (!irq) {
		ret = -EINVAL;
		goto disable_clk;
	}

	ret = request_irq(irq, gdm7243_timer_interrupt, IRQF_TIMER,
			  "gdm7243-tick", timer);
	if (ret)
		goto dispose_irq;

	clockevents_config_and_register(&timer->clockevent, timer->rate,
					0, 0);
	pr_info("GDM7243 periodic clockevent: %lu Hz, reload %u, IRQ %d\n",
		timer->rate, timer->reload, irq);
	return 0;

dispose_irq:
	irq_dispose_mapping(irq);
disable_clk:
	clk_disable_unprepare(timer->clk);
put_clk:
	clk_put(timer->clk);
unmap:
	iounmap(timer->base);
free_timer:
	kfree(timer);
	return ret;
}
TIMER_OF_DECLARE(gdm7243_timer, "gct,gdm7243-timer", gdm7243_timer_init);
