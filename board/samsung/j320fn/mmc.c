// SPDX-License-Identifier: GPL-2.0-or-later
#include <dm.h>
#include <mmc.h>
#include <sdhci.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>

struct sc9830_sdhci_priv {
	struct sdhci_host host;
	bool reset_pending;
};

struct sc9830_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

static void sc9830_sdhci_writeb(struct sdhci_host *host, u8 value, int reg)
{
	struct sc9830_sdhci_priv *priv = (void *)host;

	if (reg == SDHCI_POWER_CONTROL)
		return;
	if (reg == SDHCI_SOFTWARE_RESET) {
		if (value & SDHCI_RESET_ALL)
			priv->reset_pending = true;
		value |= BIT(3);
	}
	writeb(value, host->ioaddr + reg);
}

static u8 sc9830_sdhci_readb(struct sdhci_host *host, int reg)
{
	struct sc9830_sdhci_priv *priv = (void *)host;
	u8 value = readb(host->ioaddr + reg);

	if (reg == SDHCI_SOFTWARE_RESET && priv->reset_pending &&
	    !(value & SDHCI_RESET_ALL)) {
		writel(0x34, host->ioaddr + 0x80);
		writel(8, host->ioaddr + 0x84);
		writel(8, host->ioaddr + 0x88);
		priv->reset_pending = false;
	}
	return value;
}

static void sc9830_sdhci_writew(struct sdhci_host *host, u16 value, int reg)
{
	u32 div;

	if (reg == SDHCI_CLOCK_CONTROL && (value & SDHCI_CLOCK_INT_EN) &&
	    host->mmc->clock) {
		div = min_t(u32, DIV_ROUND_UP(host->max_clk,
					    2 * host->mmc->clock) - 1, 1023);
		value &= ~((SDHCI_DIV_MASK << SDHCI_DIVIDER_SHIFT) |
			   (3 << SDHCI_DIVIDER_HI_SHIFT));
		value |= ((div & 0xff) << 8) | ((div & 0x300) >> 2);
	}
	if (reg == SDHCI_COMMAND && !(value & SDHCI_CMD_DATA))
		writew(0, host->ioaddr + SDHCI_TRANSFER_MODE);
	writew(value, host->ioaddr + reg);
}

static u32 sc9830_sdhci_readl(struct sdhci_host *host, int reg)
{
	if (reg == SDHCI_CAPABILITIES_1)
		return 0;
	return readl(host->ioaddr + reg);
}

static const struct sdhci_ops sc9830_sdhci_ops = {
	.read_l = sc9830_sdhci_readl,
	.read_b = sc9830_sdhci_readb,
	.write_b = sc9830_sdhci_writeb,
	.write_w = sc9830_sdhci_writew,
};

static void sc9830_sdhci_setup(void)
{
	static const u16 pins[] = {
		0x1b0, 0x1c4, 0x1a0, 0x1a4, 0x1a8, 0x1ac,
		0x1b4, 0x1b8, 0x1c0, 0x1c8, 0x1cc,
	};
	void __iomem *ahb = (void *)0x20e00000;
	void __iomem *clock = (void *)0x21500048;
	void __iomem *pin_base = (void *)0x402a0000;
	u32 reset;
	int i;

	setbits_le32(ahb, BIT(11));
	readl(ahb);
	for (i = 0; i < ARRAY_SIZE(pins); i++)
		writel(pins[i] == 0x1b0 ? 0x100010 : 0x80090,
		       pin_base + pins[i]);
	clrsetbits_le32(clock, 3, 3);
	readl(clock);
	reset = readl(ahb + 4);
	writel(reset | BIT(14), ahb + 4);
	readl(ahb + 4);
	udelay(10);
	writel(reset & ~BIT(14), ahb + 4);
	readl(ahb + 4);
}

static int sc9830_sdhci_probe(struct udevice *dev)
{
	struct sc9830_sdhci_priv *priv = dev_get_priv(dev);
	struct sc9830_sdhci_plat *plat = dev_get_plat(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct sdhci_host *host = &priv->host;
	int ret;

	host->ioaddr = dev_read_addr_ptr(dev);
	if (!host->ioaddr)
		return -EINVAL;
	host->name = "SC9830 eMMC";
	host->max_clk = 384000000;
	host->ops = &sc9830_sdhci_ops;
	host->quirks = SDHCI_QUIRK_SUPPORT_SINGLE | SDHCI_QUIRK_NO_1_8_V |
		       SDHCI_QUIRK_NO_HISPD_BIT | SDHCI_QUIRK_32BIT_DMA_ADDR;
	host->mmc = &plat->mmc;
	host->mmc->dev = dev;
	host->mmc->cfg = &plat->cfg;
	host->mmc->priv = host;
	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;
	sc9830_sdhci_setup();
	ret = sdhci_setup_cfg(&plat->cfg, host, 12000000, 400000);
	if (ret)
		return ret;
	if (!(host->flags & USE_SDMA))
		return -EOPNOTSUPP;
	plat->cfg.host_caps = MMC_MODE_1BIT | MMC_CAP_NONREMOVABLE |
			      MMC_CAP(MMC_LEGACY);
	plat->cfg.voltages = MMC_VDD_30_31;
	plat->cfg.b_max = min(plat->cfg.b_max, 1024U);
	host->force_align_buffer = true;
	upriv->mmc = host->mmc;
	ret = sdhci_probe(dev);
	if (ret)
		return ret;
	if (priv->reset_pending)
		return -ETIMEDOUT;
	return 0;
}

static int sc9830_sdhci_bind(struct udevice *dev)
{
	struct sc9830_sdhci_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id sc9830_sdhci_ids[] = {
	{ .compatible = "sprd,sc9830-sdhci" },
	{ }
};

U_BOOT_DRIVER(sc9830_sdhci) = {
	.name = "sc9830_sdhci",
	.id = UCLASS_MMC,
	.of_match = sc9830_sdhci_ids,
	.bind = sc9830_sdhci_bind,
	.probe = sc9830_sdhci_probe,
	.ops = &sdhci_ops,
	.priv_auto = sizeof(struct sc9830_sdhci_priv),
	.plat_auto = sizeof(struct sc9830_sdhci_plat),
};
