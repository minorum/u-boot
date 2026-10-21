// SPDX-License-Identifier: GPL-2.0+

#include <clk.h>
#include <dm.h>
#include <wdt.h>
#include <asm/io.h>
#include <dm/read.h>
#include <linux/errno.h>

#define BCM2153_WDT_ENABLE	BIT(15)
#define BCM2153_WDT_MODE		(BIT(14) | BIT(12))

struct bcm2153_wdt {
	void *base;
	u16 control;
};

static int bcm2153_wdt_start(struct udevice *dev, u64 ms, ulong flags)
{
	struct bcm2153_wdt *wdt = dev_get_priv(dev);

	if (!ms || ms > 255000 || flags)
		return -EINVAL;

	wdt->control = BCM2153_WDT_ENABLE | BCM2153_WDT_MODE |
		       (((unsigned int)ms + 999) / 1000);
	writew(wdt->control, wdt->base);
	return 0;
}

static int bcm2153_wdt_stop(struct udevice *dev)
{
	struct bcm2153_wdt *wdt = dev_get_priv(dev);

	writew(readw(wdt->base) & ~BCM2153_WDT_ENABLE, wdt->base);
	wdt->control = 0;
	return 0;
}

static int bcm2153_wdt_reset(struct udevice *dev)
{
	struct bcm2153_wdt *wdt = dev_get_priv(dev);

	if (!wdt->control)
		return -EINVAL;

	writew(wdt->control, wdt->base);
	return 0;
}

static int bcm2153_wdt_probe(struct udevice *dev)
{
	struct bcm2153_wdt *wdt = dev_get_priv(dev);
	struct wdt_uc_plat *plat = dev_get_uclass_plat(dev);
	struct clk clk;
	u16 control;
	int ret;

	wdt->base = dev_read_addr_ptr(dev);
	if (!wdt->base)
		return -EINVAL;

	ret = clk_get_by_name(dev, "slow", &clk);
	if (ret)
		return ret;
	if (clk_get_rate(&clk) != 32768)
		return -EINVAL;
	ret = clk_enable(&clk);
	if (ret)
		return ret;

	control = readw(wdt->base);
	if (control & BCM2153_WDT_ENABLE) {
		if ((control & 0x7f00) != BCM2153_WDT_MODE || !(control & 0xff))
			return -EINVAL;
		wdt->control = control;
	}
	plat->max_timeout_ms = 255000;
	return 0;
}

static const struct wdt_ops bcm2153_wdt_ops = {
	.start = bcm2153_wdt_start,
	.stop = bcm2153_wdt_stop,
	.reset = bcm2153_wdt_reset,
};

static const struct udevice_id bcm2153_wdt_ids[] = {
	{ .compatible = "brcm,bcm2153-wdt" },
	{ }
};

U_BOOT_DRIVER(bcm2153_wdt) = {
	.name = "bcm2153_wdt",
	.id = UCLASS_WDT,
	.of_match = bcm2153_wdt_ids,
	.probe = bcm2153_wdt_probe,
	.priv_auto = sizeof(struct bcm2153_wdt),
	.ops = &bcm2153_wdt_ops,
};
