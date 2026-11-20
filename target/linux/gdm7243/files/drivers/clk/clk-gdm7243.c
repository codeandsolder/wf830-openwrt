// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCT GDM7243QT application CPU clocks.
 *
 * The original P4 BE8 firmware exposes five clock roots plus 27
 * peripherals. Its early hardware strap/PLL selection and gate
 * transactions are reconstructed here using the common clock framework.
 */
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/gct,gdm7243.h>

#define GDM7243_PLL_STRAP	0x408
#define GDM7243_PLL_CONFIG	0x41c
#define GDM7243_GATE_A		0x40c
#define GDM7243_GATE_B		0x438
#define GDM7243_GATE_C		0x43c
#define GDM7243_GATE_B_PULSE	0x440
#define GDM7243_GATE_C_PULSE	0x444
#define GDM7243_GATE_A_PULSE	0x48c
#define GDM7243_LOCK_A		0x010
#define GDM7243_LOCK_BC	0x018

#define GDM7243_ROOTS		5
#define GDM7243_FIRST_GATE	GDM7243_CLK_UDC

struct gdm7243_clk_ctrl;

struct gdm7243_gate {
	struct clk_hw hw;
	struct gdm7243_clk_ctrl *controller;
	const char *parent;
	u32 offset;
	u32 mask;
};

struct gdm7243_clk_ctrl {
	void __iomem *base;
	raw_spinlock_t lock;
	struct clk_hw_onecell_data *clocks;
	struct gdm7243_gate gates[GDM7243_CLK_COUNT - GDM7243_FIRST_GATE];
};

struct gdm7243_gate_desc {
	const char *name;
	const char *parent;
	u16 reg;
	u32 mask;
};

static const char * const gdm7243_root_names[GDM7243_ROOTS] = {
	"gdm7243-xtal",
	"gdm7243-pll",
	"gdm7243-arm",
	"gdm7243-1x",
	"gdm7243-2x",
};

/*
 * Exact ordering, MMIO register addresses, bits, and supported parents
 * recovered from the OEM 27-entry gdm_setup_clocks table.
 *
 * NULL parent means the OEM entry contains neither a parent pointer nor
 * a fixed rate: enable/disable works, but rate remains unknown (zero)
 * until the actual clock source is established for that peripheral.
 */
static const struct gdm7243_gate_desc gdm7243_gates[] = {
	/* index 5: USB device */
	{ "gdm7243-udc",       "gdm7243-2x", GDM7243_GATE_B, BIT(4) },
	{ "gdm7243-udc-40m",   NULL,         GDM7243_GATE_A, BIT(9) },
	/* index 7: timers is a fixed 1x alias without a gate */
	{ "gdm7243-timers",    "gdm7243-1x", 0,              0 },
	{ "gdm7243-uart0",     "gdm7243-1x", GDM7243_GATE_B, BIT(3) },
	{ "gdm7243-uart1",     "gdm7243-1x", GDM7243_GATE_B, BIT(7) },
	{ "gdm7243-uart2",     "gdm7243-1x", GDM7243_GATE_B, BIT(23) },
	{ "gdm7243-uart3",     "gdm7243-1x", GDM7243_GATE_B, BIT(18) },
	{ "gdm7243-xtal-32k",  NULL,         GDM7243_GATE_C, BIT(10) },
	{ "gdm7243-wlan",      "gdm7243-1x", GDM7243_GATE_B, BIT(2) },
	{ "gdm7243-i2s",       NULL,         GDM7243_GATE_B, BIT(5) },
	{ "gdm7243-pcm",       NULL,         GDM7243_GATE_B, BIT(6) },
	{ "gdm7243-eth0",      NULL,         GDM7243_GATE_B, BIT(20) },
	{ "gdm7243-eth1",      NULL,         GDM7243_GATE_C, BIT(2) },
	{ "gdm7243-usim",      NULL,         GDM7243_GATE_B, BIT(8) },
	{ "gdm7243-sddev",     NULL,         GDM7243_GATE_B, BIT(10) },
	{ "gdm7243-sdhost0",   NULL,         GDM7243_GATE_B, BIT(21) },
	{ "gdm7243-sdhost1",   NULL,         GDM7243_GATE_B, BIT(28) },
	{ "gdm7243-gsh",       NULL,         GDM7243_GATE_A, BIT(8) },
	{ "gdm7243-nand",      NULL,         GDM7243_GATE_B, BIT(13) },
	{ "gdm7243-pd-slv",    NULL,         GDM7243_GATE_C, BIT(22) },
	{ "gdm7243-pu",        NULL,         GDM7243_GATE_A, BIT(26) },
	{ "gdm7243-pur",       NULL,         GDM7243_GATE_A, BIT(27) },
	{ "gdm7243-pd-usb",    NULL,         GDM7243_GATE_C, BIT(20) },
	{ "gdm7243-i2c",       "gdm7243-1x", GDM7243_GATE_B, BIT(11) },
	{ "gdm7243-uh",        NULL,         GDM7243_GATE_A, BIT(11) },
	{ "gdm7243-uhb",       NULL,         GDM7243_GATE_C, BIT(4) },
	{ "gdm7243-spi",       NULL,         GDM7243_GATE_C, BIT(0) },
};

static unsigned long gdm7243_pll_rate(u32 pll_config)
{
	switch (pll_config & ~BIT(0)) {
	case 0x3fa28206:
		return 832000000;
	case 0x3fa2d90a:
		return 833280000;
	case 0x3fa2d48c:
		return 680000000;
	case 0x3fa29c06:
		return 998000000;
	case 0x3fa47d06:
		return 400000000;
	default:
		return 800000000;
	}
}

static void gdm7243_read_root_rates(void __iomem *base,
				     unsigned long rates[GDM7243_ROOTS])
{
	unsigned long pll;

	rates[GDM7243_CLK_XTAL] = 19200000;

	/* Direct 80/20/10 MHz strap bypasses the PLL in the OEM kernel. */
	if ((readl(base + GDM7243_PLL_STRAP) & 0xf000) == 0xf000) {
		rates[GDM7243_CLK_PLL] = 0;
		rates[GDM7243_CLK_ARM] = 80000000;
		rates[GDM7243_CLK_1X] = 10000000;
		rates[GDM7243_CLK_2X] = 20000000;
		return;
	}

	pll = gdm7243_pll_rate(readl(base + GDM7243_PLL_CONFIG));
	rates[GDM7243_CLK_PLL] = pll;
	rates[GDM7243_CLK_ARM] = pll;
	rates[GDM7243_CLK_1X] = pll / 8;
	rates[GDM7243_CLK_2X] = pll / 4;
}

static int gdm7243_gate_update(struct gdm7243_gate *gate, bool enable)
{
	struct gdm7243_clk_ctrl *ctrl = gate->controller;
	void __iomem *reg = ctrl->base + gate->offset;
	void __iomem *mutex = ctrl->base +
		(gate->offset == GDM7243_GATE_A ?
		 GDM7243_LOCK_A : GDM7243_LOCK_BC);
	void __iomem *pulse = NULL;
	unsigned long flags;
	u32 value, other, busy;
	int ret;

	raw_spin_lock_irqsave(&ctrl->lock, flags);

	/*
	 * The original kernel waits for the hardware gate transaction lock
	 * to clear, then writes zero on completion. Time out instead of
	 * clobbering a gate transaction owned by the other processor.
	 */
	ret = readl_poll_timeout_atomic(mutex, busy, !busy, 1, 1000);
	if (ret)
		goto unlock;

	value = readl(reg) & ~gate->mask;
	if (enable) {
		if (gate->offset == GDM7243_GATE_A)
			pulse = ctrl->base + GDM7243_GATE_A_PULSE;
		else if (gate->offset == GDM7243_GATE_B)
			pulse = ctrl->base + GDM7243_GATE_B_PULSE;
		else if (gate->offset == GDM7243_GATE_C)
			pulse = ctrl->base + GDM7243_GATE_C_PULSE;

		if (pulse) {
			writel(value, reg);
			other = readl(pulse) & ~gate->mask;
			writel(other, pulse);
			writel(other | gate->mask, pulse);
		}
		value |= gate->mask;
	}
	writel(value, reg);
	writel(0, mutex);

unlock:
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
	return ret;
}

static int gdm7243_gate_enable(struct clk_hw *hw)
{
	struct gdm7243_gate *gate =
		container_of(hw, struct gdm7243_gate, hw);

	return gdm7243_gate_update(gate, true);
}

static void gdm7243_gate_disable(struct clk_hw *hw)
{
	struct gdm7243_gate *gate =
		container_of(hw, struct gdm7243_gate, hw);

	if (gdm7243_gate_update(gate, false))
		pr_warn_ratelimited("%s: hardware gate transaction busy\n",
				    clk_hw_get_name(hw));
}

static int gdm7243_gate_is_enabled(struct clk_hw *hw)
{
	struct gdm7243_gate *gate =
		container_of(hw, struct gdm7243_gate, hw);

	return !!(readl(gate->controller->base + gate->offset) & gate->mask);
}

static const struct clk_ops gdm7243_gate_ops = {
	.enable = gdm7243_gate_enable,
	.disable = gdm7243_gate_disable,
	.is_enabled = gdm7243_gate_is_enabled,
};

static int __init gdm7243_register_roots(struct gdm7243_clk_ctrl *ctrl)
{
	unsigned long rates[GDM7243_ROOTS];
	unsigned int i;

	gdm7243_read_root_rates(ctrl->base, rates);
	for (i = 0; i < GDM7243_ROOTS; i++) {
		ctrl->clocks->hws[i] =
			clk_hw_register_fixed_rate(NULL, gdm7243_root_names[i],
						   NULL, 0, rates[i]);
		if (IS_ERR(ctrl->clocks->hws[i]))
			return PTR_ERR(ctrl->clocks->hws[i]);
	}
	pr_info("GDM7243 clocks: ARM %lu Hz, 1x %lu Hz, 2x %lu Hz\n",
		rates[GDM7243_CLK_ARM], rates[GDM7243_CLK_1X],
		rates[GDM7243_CLK_2X]);
	return 0;
}

static int __init gdm7243_register_gates(struct gdm7243_clk_ctrl *ctrl)
{
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(gdm7243_gates) !=
		     GDM7243_CLK_COUNT - GDM7243_FIRST_GATE);

	for (i = 0; i < ARRAY_SIZE(gdm7243_gates); i++) {
		const struct gdm7243_gate_desc *desc = &gdm7243_gates[i];
		unsigned int index = i + GDM7243_FIRST_GATE;
		struct gdm7243_gate *gate = &ctrl->gates[i];
		struct clk_init_data init = {
			.name = desc->name,
			.ops = &gdm7243_gate_ops,
			.flags = CLK_IGNORE_UNUSED,
		};
		int ret;

		if (!desc->mask) {
			ctrl->clocks->hws[index] =
				clk_hw_register_fixed_factor(NULL, desc->name,
							     desc->parent, 0,
							     1, 1);
			if (IS_ERR(ctrl->clocks->hws[index]))
				return PTR_ERR(ctrl->clocks->hws[index]);
			continue;
		}

		gate->controller = ctrl;
		gate->offset = desc->reg;
		gate->mask = desc->mask;
		gate->parent = desc->parent;
		if (gate->parent) {
			init.parent_names = &gate->parent;
			init.num_parents = 1;
		}

		gate->hw.init = &init;
		ret = clk_hw_register(NULL, &gate->hw);
		if (ret)
			return ret;
		ctrl->clocks->hws[index] = &gate->hw;
	}
	return 0;
}

static void __init gdm7243_unregister_clocks(struct gdm7243_clk_ctrl *ctrl)
{
	int i;

	for (i = GDM7243_CLK_COUNT - 1; i >= 0; i--) {
		struct clk_hw *hw = ctrl->clocks->hws[i];

		if (!hw || IS_ERR(hw))
			continue;
		if (i < GDM7243_ROOTS)
			clk_hw_unregister_fixed_rate(hw);
		else if (i == GDM7243_CLK_TIMERS)
			clk_hw_unregister_fixed_factor(hw);
		else
			clk_hw_unregister(hw);
	}
}

static void __init gdm7243_clk_init(struct device_node *node)
{
	struct gdm7243_clk_ctrl *ctrl;
	int ret;

	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return;

	ctrl->clocks = kzalloc(struct_size(ctrl->clocks, hws,
					  GDM7243_CLK_COUNT), GFP_KERNEL);
	if (!ctrl->clocks)
		goto free_ctrl;
	ctrl->clocks->num = GDM7243_CLK_COUNT;

	ctrl->base = of_iomap(node, 0);
	if (!ctrl->base)
		goto free_data;
	raw_spin_lock_init(&ctrl->lock);

	ret = gdm7243_register_roots(ctrl);
	if (ret)
		goto unreg;
	ret = gdm7243_register_gates(ctrl);
	if (ret)
		goto unreg;
	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get,
				     ctrl->clocks);
	if (ret)
		goto unreg;
	return;

unreg:
	pr_err("%pOF: clock registration failed: %d\n", node, ret);
	gdm7243_unregister_clocks(ctrl);
	iounmap(ctrl->base);
free_data:
	kfree(ctrl->clocks);
free_ctrl:
	kfree(ctrl);
}
CLK_OF_DECLARE(gdm7243, "gct,gdm7243-clocks", gdm7243_clk_init);
