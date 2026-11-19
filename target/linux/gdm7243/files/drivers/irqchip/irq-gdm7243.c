// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCT GDM7243QT root interrupt controller.
 *
 * Register layout and priority are recovered from the Polish P4 WF830
 * ARMv7 BE8 Linux 3.10 kernel. This is a modern implementation, not
 * the OEM kernel code.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/mach/irq.h>

#define GDM7243_IRQS              64
#define GDM7243_BANK_STRIDE       0x40
#define GDM7243_IRQ_MASK          0x04
#define GDM7243_IRQ_PENDING       0x0c
#define GDM7243_IRQ_ACK           0x1c
#define GDM7243_RESERVED_IRQ      19

struct gdm7243_intc {
	void __iomem *base;
	raw_spinlock_t lock;
	struct irq_domain *domain;
	struct irq_chip chip;
};

static struct gdm7243_intc *gdm7243_root_intc;

static void __iomem *gdm7243_reg(struct gdm7243_intc *intc,
				 irq_hw_number_t hwirq, unsigned int offset)
{
	return intc->base + (hwirq / 32) * GDM7243_BANK_STRIDE + offset;
}

static void gdm7243_mask(struct irq_data *data)
{
	struct gdm7243_intc *intc = irq_data_get_irq_chip_data(data);
	void __iomem *reg = gdm7243_reg(intc, data->hwirq, GDM7243_IRQ_MASK);
	unsigned long flags;
	u32 mask = BIT(data->hwirq % 32);

	raw_spin_lock_irqsave(&intc->lock, flags);
	writel_relaxed(readl_relaxed(reg) & ~mask, reg);
	raw_spin_unlock_irqrestore(&intc->lock, flags);
}

static void gdm7243_unmask(struct irq_data *data)
{
	struct gdm7243_intc *intc = irq_data_get_irq_chip_data(data);
	void __iomem *reg = gdm7243_reg(intc, data->hwirq, GDM7243_IRQ_MASK);
	unsigned long flags;
	u32 mask = BIT(data->hwirq % 32);

	raw_spin_lock_irqsave(&intc->lock, flags);
	writel_relaxed(readl_relaxed(reg) | mask, reg);
	raw_spin_unlock_irqrestore(&intc->lock, flags);
}

static void gdm7243_mask_ack(struct irq_data *data)
{
	struct gdm7243_intc *intc = irq_data_get_irq_chip_data(data);
	void __iomem *mask_reg = gdm7243_reg(intc, data->hwirq, GDM7243_IRQ_MASK);
	void __iomem *ack_reg = gdm7243_reg(intc, data->hwirq, GDM7243_IRQ_ACK);
	unsigned long flags;
	u32 bit = BIT(data->hwirq % 32);

	raw_spin_lock_irqsave(&intc->lock, flags);
	writel_relaxed(readl_relaxed(mask_reg) & ~bit, mask_reg);
	/* The original controller pulses one bit, then zero. */
	writel_relaxed(bit, ack_reg);
	writel_relaxed(0, ack_reg);
	/* Drain posted writes before the IRQ handler executes. */
	readl(mask_reg);
	raw_spin_unlock_irqrestore(&intc->lock, flags);
}

/*
 * The P4 exception handler always selects the highest set bit of the
 * lowest byte, if nonempty, before the rest of each bank. Bank 0 wins
 * over bank 1. This unusual order is intentional.
 */
static int gdm7243_next_hwirq(struct gdm7243_intc *intc)
{
	u32 pending, selected;
	unsigned int bank;

	for (bank = 0; bank < 2; bank++) {
		pending = readl_relaxed(intc->base +
					bank * GDM7243_BANK_STRIDE +
					GDM7243_IRQ_PENDING);
		if (!pending)
			continue;

		selected = pending & 0xff;
		if (!selected)
			selected = pending;

		return bank * 32 + fls(selected) - 1;
	}
	return -1;
}

static void gdm7243_handle_irq(struct pt_regs *regs)
{
	struct gdm7243_intc *intc = gdm7243_root_intc;
	int hwirq = gdm7243_next_hwirq(intc);

	if (hwirq >= 0)
		generic_handle_domain_irq(intc->domain, hwirq);
}

static int gdm7243_domain_map(struct irq_domain *domain,
			      unsigned int virq, irq_hw_number_t hwirq)
{
	struct gdm7243_intc *intc = domain->host_data;

	/* Legacy gdm_init_irq deliberately excludes hardware IRQ 19. */
	if (hwirq >= GDM7243_IRQS || hwirq == GDM7243_RESERVED_IRQ)
		return -EINVAL;

	irq_set_chip_data(virq, intc);
	irq_set_chip_and_handler(virq, &intc->chip, handle_level_irq);
	irq_set_noprobe(virq);
	return 0;
}

static const struct irq_domain_ops gdm7243_domain_ops = {
	.map = gdm7243_domain_map,
	.xlate = irq_domain_xlate_onecell,
};

static int __init gdm7243_irq_of_init(struct device_node *node,
				     struct device_node *parent)
{
	struct gdm7243_intc *intc;

	if (parent)
		return -EINVAL;

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	intc->base = of_iomap(node, 0);
	if (!intc->base) {
		kfree(intc);
		return -ENOMEM;
	}

	raw_spin_lock_init(&intc->lock);
	intc->chip.name = "gdm7243";
	intc->chip.irq_mask = gdm7243_mask;
	intc->chip.irq_unmask = gdm7243_unmask;
	intc->chip.irq_mask_ack = gdm7243_mask_ack;

	/* Clear bootloader enables before any device drivers request IRQs. */
	writel(0, intc->base + GDM7243_IRQ_MASK);
	writel(0, intc->base + GDM7243_BANK_STRIDE + GDM7243_IRQ_MASK);

	intc->domain = irq_domain_add_linear(node, GDM7243_IRQS,
					    &gdm7243_domain_ops, intc);
	if (!intc->domain) {
		iounmap(intc->base);
		kfree(intc);
		return -ENOMEM;
	}

	gdm7243_root_intc = intc;
	set_handle_irq(gdm7243_handle_irq);
	pr_info("GDM7243 interrupt controller: 64 IRQs, reserved hwirq 19\n");
	return 0;
}
IRQCHIP_DECLARE(gdm7243_intc, "gct,gdm7243-intc", gdm7243_irq_of_init);
