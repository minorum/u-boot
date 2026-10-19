// SPDX-License-Identifier: GPL-2.0-or-later
#include <dm.h>
#include <timer.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/errno.h>

#define TIMER_LOAD	0x00
#define TIMER_CTL	0x08
#define TIMER_INT	0x0c
#define TIMER_SHADOW	0x10
#define TIMER_PERIODIC	BIT(6)
#define TIMER_ENABLE	BIT(7)
#define TIMER_NEW	BIT(8)
#define TIMER_IRQ_CLEAR	BIT(3)

struct sc9830_timer_priv {
	void __iomem *base;
};

static u64 sc9830_timer_count(struct udevice *dev)
{
	struct sc9830_timer_priv *priv = dev_get_priv(dev);

	return timer_conv_64(~readl(priv->base + TIMER_SHADOW));
}

static int sc9830_timer_probe(struct udevice *dev)
{
	struct sc9830_timer_priv *priv = dev_get_priv(dev);
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base || uc_priv->clock_rate != 32768)
		return -EINVAL;
	setbits_le32((void *)0x402e0000, BIT(11));
	writel(0, priv->base + TIMER_CTL);
	writel(TIMER_IRQ_CLEAR, priv->base + TIMER_INT);
	writel(~0U, priv->base + TIMER_LOAD);
	writel(TIMER_PERIODIC | TIMER_ENABLE | TIMER_NEW,
	       priv->base + TIMER_CTL);
	return 0;
}

static const struct timer_ops sc9830_timer_ops = {
	.get_count = sc9830_timer_count,
};

static const struct udevice_id sc9830_timer_ids[] = {
	{ .compatible = "sprd,sc9830-uboot-timer" },
	{ }
};

U_BOOT_DRIVER(sc9830_timer) = {
	.name = "sc9830_timer",
	.id = UCLASS_TIMER,
	.of_match = sc9830_timer_ids,
	.priv_auto = sizeof(struct sc9830_timer_priv),
	.probe = sc9830_timer_probe,
	.ops = &sc9830_timer_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
