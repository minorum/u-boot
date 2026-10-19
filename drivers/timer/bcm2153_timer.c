// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <errno.h>
#include <timer.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define BCM2153_GPT_CTRL		0x04
#define BCM2153_GPT_RELOAD	0x08
#define BCM2153_GPT_COUNTER	0x0c
#define BCM2153_GPT_ENABLE	BIT(31)
#define BCM2153_GPT_POWER		BIT(24)
#define BCM2153_GPT_PENDING	BIT(0)
#define BCM2153_GPT_SLOW_RATE	32768

struct bcm2153_timer_priv {
	void __iomem *base;
};

static u64 bcm2153_timer_get_count(struct udevice *dev)
{
	struct bcm2153_timer_priv *priv = dev_get_priv(dev);
	u32 count, next;

	count = readl(priv->base + BCM2153_GPT_COUNTER);
	do {
		next = count;
		count = readl(priv->base + BCM2153_GPT_COUNTER);
	} while (next != count);

	return timer_conv_64(~count);
}

static int bcm2153_timer_probe(struct udevice *dev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct bcm2153_timer_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base || uc_priv->clock_rate != BCM2153_GPT_SLOW_RATE)
		return -EINVAL;

	writel(0, priv->base + BCM2153_GPT_CTRL);
	writel(BCM2153_GPT_ENABLE | BCM2153_GPT_POWER | BCM2153_GPT_PENDING,
	       priv->base + BCM2153_GPT_CTRL);
	writel(~0U, priv->base + BCM2153_GPT_RELOAD);

	return 0;
}

static const struct timer_ops bcm2153_timer_ops = {
	.get_count = bcm2153_timer_get_count,
};

static const struct udevice_id bcm2153_timer_ids[] = {
	{ .compatible = "brcm,bcm2153-gpt" },
	{ }
};

U_BOOT_DRIVER(bcm2153_timer) = {
	.name = "bcm2153_timer",
	.id = UCLASS_TIMER,
	.of_match = bcm2153_timer_ids,
	.priv_auto = sizeof(struct bcm2153_timer_priv),
	.probe = bcm2153_timer_probe,
	.ops = &bcm2153_timer_ops,
};
