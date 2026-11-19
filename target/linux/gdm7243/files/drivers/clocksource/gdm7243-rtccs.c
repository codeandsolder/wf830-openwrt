// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCT GDM7243QT 32.768 kHz free-running RTC counter.
 *
 * The Polish P4 vendor kernel uses the 32-bit register at 0xfc000604
 * for both sched_clock and clocksource ("rtccs").
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/sched_clock.h>

#define GDM7243_RTC_COUNT	0x04
#define GDM7243_RTC_HZ		32768

static void __iomem *gdm7243_rtc_count;

static u64 notrace gdm7243_read_sched_clock(void)
{
	return readl_relaxed(gdm7243_rtc_count);
}

static int __init gdm7243_rtccs_init(struct device_node *node)
{
	void __iomem *base;
	u32 frequency;
	int ret;

	ret = of_property_read_u32(node, "clock-frequency", &frequency);
	if (ret)
		return ret;

	/* Frequency is a property of the 32.768 kHz oscillator, not the CPU PLL. */
	if (frequency != GDM7243_RTC_HZ)
		return -EINVAL;

	base = of_iomap(node, 0);
	if (!base)
		return -ENOMEM;

	ret = clocksource_mmio_init(base + GDM7243_RTC_COUNT,
				    "gdm7243-rtccs", frequency, 300, 32,
				    clocksource_mmio_readl_up);
	if (ret) {
		iounmap(base);
		return ret;
	}

	gdm7243_rtc_count = base + GDM7243_RTC_COUNT;
	sched_clock_register(gdm7243_read_sched_clock, 32, frequency);
	pr_info("GDM7243 32.768 kHz RTC counter registered\n");
	return 0;
}
TIMER_OF_DECLARE(gdm7243_rtccs, "gct,gdm7243-rtccs", gdm7243_rtccs_init);
