// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <errno.h>
#include <dm/pinctrl.h>
#include <power/regulator.h>
#include <sdhci.h>
#include <linux/delay.h>

#define BCM2153_SDIO_RATE	52000000
#define BCM2153_SDIO_RESET	BIT(1)

struct bcm2153_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

struct bcm2153_sdhci_priv {
	struct sdhci_host host;
	void __iomem *clock;
	u16 transfer_mode;
	u16 block_size;
};

static u16 bcm2153_sdhci_readw(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + (reg & ~3)) >> ((reg & 3) * 8);
}

static u8 bcm2153_sdhci_readb(struct sdhci_host *host, int reg)
{
	return readl(host->ioaddr + (reg & ~3)) >> ((reg & 3) * 8);
}

static void bcm2153_sdhci_writew(struct sdhci_host *host, u16 val, int reg)
{
	struct bcm2153_sdhci_priv *priv = container_of(host,
					struct bcm2153_sdhci_priv, host);
	u32 shift = (reg & 3) * 8;
	u32 old;

	if (reg == SDHCI_BLOCK_SIZE) {
		priv->block_size = val;
		return;
	}
	if (reg == SDHCI_BLOCK_COUNT) {
		writel((u32)val << 16 | priv->block_size,
		       host->ioaddr + SDHCI_BLOCK_SIZE);
		return;
	}
	if (reg == SDHCI_TRANSFER_MODE) {
		priv->transfer_mode = val;
		return;
	}
	if (reg == SDHCI_COMMAND) {
		writel((u32)val << 16 | priv->transfer_mode,
		       host->ioaddr + SDHCI_TRANSFER_MODE);
		return;
	}
	if (reg == SDHCI_CLOCK_CONTROL) {
		val &= ~(SDHCI_DIV_MASK << SDHCI_DIVIDER_SHIFT);
		val &= ~(SDHCI_DIV_HI_MASK >> SDHCI_DIV_MASK_LEN
			 << SDHCI_DIVIDER_HI_SHIFT);
		if (!(val & SDHCI_CLOCK_INT_EN))
			writel(0, priv->clock);
	}
	if ((reg & ~3) == SDHCI_INT_STATUS) {
		writel((u32)val << shift, host->ioaddr + SDHCI_INT_STATUS);
		return;
	}
	old = readl(host->ioaddr + (reg & ~3));
	writel((old & ~(0xffffU << shift)) | (u32)val << shift,
	       host->ioaddr + (reg & ~3));
}

static void bcm2153_sdhci_writeb(struct sdhci_host *host, u8 val, int reg)
{
	u32 shift = (reg & 3) * 8;
	u32 old = readl(host->ioaddr + (reg & ~3));

	writel((old & ~(0xffU << shift)) | (u32)val << shift,
	       host->ioaddr + (reg & ~3));
}

static void bcm2153_sdhci_set_clock(struct sdhci_host *host, u32 div)
{
	struct bcm2153_sdhci_priv *priv = container_of(host,
					struct bcm2153_sdhci_priv, host);

	writel(0, priv->clock);
	writel(div ? 2 * div - 1 : 0, priv->clock + 4);
	writel(1, priv->clock);
}

static const struct sdhci_ops bcm2153_sdhci_ops = {
	.read_w = bcm2153_sdhci_readw,
	.read_b = bcm2153_sdhci_readb,
	.write_w = bcm2153_sdhci_writew,
	.write_b = bcm2153_sdhci_writeb,
	.set_clock = bcm2153_sdhci_set_clock,
};

static int bcm2153_sdhci_prepare(struct udevice *dev)
{
	struct udevice *supply;
	int ret;

	ret = device_get_supply_regulator(dev, "vmmc-supply", &supply);
	if (ret)
		return ret;
	ret = regulator_set_enable(supply, true);
	if (ret)
		return ret;
	return pinctrl_select_state(dev, "powered");
}

static int bcm2153_sdhci_probe(struct udevice *dev)
{
	struct bcm2153_sdhci_plat *plat = dev_get_plat(dev);
	struct bcm2153_sdhci_priv *priv = dev_get_priv(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct sdhci_host *host = &priv->host;
	void __iomem *control;
	int ret;

	host->ioaddr = dev_read_addr_name_ptr(dev, "host");
	control = dev_read_addr_name_ptr(dev, "control");
	priv->clock = dev_read_addr_name_ptr(dev, "clock");
	if (!host->ioaddr || !control || !priv->clock)
		return -EINVAL;

	host->max_clk = dev_read_u32_default(dev, "clock-frequency", 0);
	if (host->max_clk != BCM2153_SDIO_RATE)
		return -EINVAL;

	ret = bcm2153_sdhci_prepare(dev);
	if (ret)
		return ret;

	if (readl(host->ioaddr + SDHCI_CLOCK_CONTROL) & BIT(24))
		return -EBUSY;
	setbits_le32(control, BCM2153_SDIO_RESET);
	mdelay(10);
	clrbits_le32(control, BCM2153_SDIO_RESET);

	host->name = dev->name;
	host->ops = &bcm2153_sdhci_ops;
	host->quirks = SDHCI_QUIRK_REG32_RW | SDHCI_QUIRK_NO_1_8_V;
	host->mmc = &plat->mmc;
	plat->mmc.priv = host;
	plat->mmc.dev = dev;
	upriv->mmc = &plat->mmc;

	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;
	ret = sdhci_setup_cfg(&plat->cfg, host, 25000000, 400000);
	if (ret)
		return ret;
	if (host->clk_mul)
		return -EOPNOTSUPP;
	host->flags &= ~USE_DMA;

	return sdhci_probe(dev);
}

static int bcm2153_sdhci_bind(struct udevice *dev)
{
	struct bcm2153_sdhci_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id bcm2153_sdhci_ids[] = {
	{ .compatible = "brcm,bcm2153-sdhci" },
	{ }
};

U_BOOT_DRIVER(bcm2153_sdhci) = {
	.name = "bcm2153_sdhci",
	.id = UCLASS_MMC,
	.of_match = bcm2153_sdhci_ids,
	.ops = &sdhci_ops,
	.bind = bcm2153_sdhci_bind,
	.probe = bcm2153_sdhci_probe,
	.priv_auto = sizeof(struct bcm2153_sdhci_priv),
	.plat_auto = sizeof(struct bcm2153_sdhci_plat),
};
