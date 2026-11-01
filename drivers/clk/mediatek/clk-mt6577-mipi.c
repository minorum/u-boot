// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <clk-uclass.h>
#include <div64.h>
#include <dm.h>
#include <regmap.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>

#define MT6577_MIPI_CON0		0x00
#define MT6577_MIPI_CON1		0x04
#define MT6577_MIPI_CON3		0x0c
#define MT6577_MIPI_CON6		0x18
#define MT6577_MIPI_CON9		0x24
#define MT6577_MIPI_PLL_EN	BIT(0)
#define MT6577_MIPI_CLKR_EN	BIT(1)
#define MT6577_MIPI_DPI_EN	BIT(5)
#define MT6577_MIPI_DIV1		GENMASK(6, 0)
#define MT6577_MIPI_DIV2		GENMASK(11, 8)
#define MT6577_MIPI_ENABLE	(MT6577_MIPI_PLL_EN | MT6577_MIPI_CLKR_EN | \
				 MT6577_MIPI_DPI_EN)

struct mt6577_mipi {
	struct clk clk;
	void __iomem *base;
	struct clk parent;
	struct regmap *mmsys;
	struct regmap *apmixed;
	struct regmap *infra;
	u32 div1;
	u32 div2;
	u32 reference;
	bool enabled;
};

static ulong mt6577_mipi_get_rate(struct clk *clk)
{
	struct mt6577_mipi *mipi = container_of(dev_get_clk_ptr(clk->dev),
					       struct mt6577_mipi, clk);
	ulong parent = clk_get_rate(&mipi->parent);
	u64 rate;

	if (IS_ERR_VALUE(parent))
		return parent;
	if (!mipi->div2 || mipi->reference)
		return 0;
	rate = (u64)parent * mipi->div1;
	do_div(rate, 2 * mipi->div2);
	return rate <= ULONG_MAX ? rate : 0;
}

static ulong mt6577_mipi_round_rate(struct clk *clk, ulong requested)
{
	struct mt6577_mipi *mipi = container_of(dev_get_clk_ptr(clk->dev),
					       struct mt6577_mipi, clk);
	ulong parent = clk_get_rate(&mipi->parent);
	u64 rate;

	(void)requested;
	if (IS_ERR_VALUE(parent))
		return parent;
	rate = (u64)parent * 32;
	do_div(rate, 6);
	if (rate > ULONG_MAX)
		return -ERANGE;
	return rate;
}

static ulong mt6577_mipi_set_rate(struct clk *clk, ulong rate)
{
	struct mt6577_mipi *mipi = container_of(dev_get_clk_ptr(clk->dev),
					       struct mt6577_mipi, clk);
	ulong fixed = mt6577_mipi_round_rate(clk, rate);

	if (mipi->enabled)
		return -EBUSY;
	if (IS_ERR_VALUE(fixed))
		return fixed;
	if (!fixed || rate != fixed)
		return -EINVAL;
	mipi->div1 = 32;
	mipi->div2 = 3;
	mipi->reference = 0;
	return rate;
}

static int mt6577_mipi_enable(struct clk *clk)
{
	struct mt6577_mipi *mipi = container_of(dev_get_clk_ptr(clk->dev),
					       struct mt6577_mipi, clk);
	u32 val, old_apmixed, old_mmsys, old_infra;
	u32 old_control, old_divider, old_reference, old_bias;
	u16 old_ldo;
	int ret;

	if (mipi->enabled)
		return 0;
	if (!mipi->div2 || mipi->reference)
		return -EINVAL;
	ret = regmap_read(mipi->infra, 0x890, &old_infra);
	if (ret)
		return ret;
	if (old_infra & BIT(12))
		return -EBUSY;
	ret = regmap_read(mipi->apmixed, 0x44, &old_apmixed);
	if (ret)
		return ret;
	ret = regmap_read(mipi->mmsys, 0x858, &old_mmsys);
	if (ret)
		return ret;
	old_control = readl(mipi->base + MT6577_MIPI_CON0);
	old_divider = readl(mipi->base + MT6577_MIPI_CON1);
	old_reference = readl(mipi->base + MT6577_MIPI_CON3);
	old_bias = readl(mipi->base + MT6577_MIPI_CON6);
	old_ldo = readw(mipi->base + MT6577_MIPI_CON9);

	ret = regmap_update_bits(mipi->apmixed, 0x44, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = regmap_update_bits(mipi->mmsys, 0x858, GENMASK(15, 0), 0x8000);
	if (ret)
		goto restore_apmixed;
	ret = regmap_update_bits(mipi->infra, 0x890, BIT(4), 0);
	if (ret)
		goto restore_infra;
	writel(old_reference & ~GENMASK(3, 2), mipi->base + MT6577_MIPI_CON3);
	val = (old_divider & ~(MT6577_MIPI_DIV1 | MT6577_MIPI_DIV2)) |
		FIELD_PREP(MT6577_MIPI_DIV1, mipi->div1) |
		FIELD_PREP(MT6577_MIPI_DIV2, mipi->div2);
	writel(val, mipi->base + MT6577_MIPI_CON1);
	writel(old_control | MT6577_MIPI_ENABLE, mipi->base + MT6577_MIPI_CON0);
	writel(old_bias | GENMASK(1, 0), mipi->base + MT6577_MIPI_CON6);
	writew(0x4008, mipi->base + MT6577_MIPI_CON9);
	ret = regmap_update_bits(mipi->infra, 0x890, BIT(8), BIT(8));
	if (ret)
		goto restore_phy;
	mdelay(1);
	mipi->enabled = true;
	return 0;

restore_phy:
	writel(old_control, mipi->base + MT6577_MIPI_CON0);
	writel(old_divider, mipi->base + MT6577_MIPI_CON1);
	writel(old_reference, mipi->base + MT6577_MIPI_CON3);
	writel(old_bias, mipi->base + MT6577_MIPI_CON6);
	writew(old_ldo, mipi->base + MT6577_MIPI_CON9);
restore_infra:
	regmap_update_bits(mipi->infra, 0x890, BIT(4) | BIT(8), old_infra);
	regmap_update_bits(mipi->mmsys, 0x858, GENMASK(15, 0), old_mmsys);
restore_apmixed:
	regmap_update_bits(mipi->apmixed, 0x44, BIT(4), old_apmixed);
	return ret;
}

static int mt6577_mipi_disable(struct clk *clk)
{
	struct mt6577_mipi *mipi = container_of(dev_get_clk_ptr(clk->dev),
					       struct mt6577_mipi, clk);
	u32 val, infra, mask = MT6577_MIPI_PLL_EN | MT6577_MIPI_DPI_EN;
	bool rx_active;
	int ret;

	if (!mipi->enabled)
		return 0;
	ret = regmap_read(mipi->infra, 0x890, &infra);
	if (ret)
		return ret;
	rx_active = infra & BIT(12);
	if (!rx_active)
		mask |= MT6577_MIPI_CLKR_EN;
	val = readl(mipi->base + MT6577_MIPI_CON0);
	writel(val & ~mask, mipi->base + MT6577_MIPI_CON0);
	writew(readw(mipi->base + MT6577_MIPI_CON9) & ~BIT(3),
	       mipi->base + MT6577_MIPI_CON9);
	ret = regmap_update_bits(mipi->infra, 0x890, BIT(8), 0);
	if (ret)
		return ret;
	if (!rx_active) {
		val = readl(mipi->base + MT6577_MIPI_CON6);
		writel(val & ~GENMASK(1, 0), mipi->base + MT6577_MIPI_CON6);
		ret = regmap_update_bits(mipi->infra, 0x890, BIT(4), BIT(4));
		if (ret)
			return ret;
		ret = regmap_update_bits(mipi->apmixed, 0x44, BIT(4), 0);
		if (ret)
			return ret;
	}
	mipi->enabled = false;
	return 0;
}

static const struct clk_ops mt6577_mipi_pll_ops = {
	.get_rate = mt6577_mipi_get_rate,
	.round_rate = mt6577_mipi_round_rate,
	.set_rate = mt6577_mipi_set_rate,
	.enable = mt6577_mipi_enable,
	.disable = mt6577_mipi_disable,
};

U_BOOT_DRIVER(mt6577_mipi_pll) = {
	.name = "mt6577_mipi_pll",
	.id = UCLASS_CLK,
	.ops = &mt6577_mipi_pll_ops,
};

static int mt6577_mipi_probe(struct udevice *dev)
{
	struct mt6577_mipi *mipi = dev_get_priv(dev);
	const char * const names[] = {
		"mediatek,mmsys", "mediatek,apmixedsys", "mediatek,infracfg",
	};
	struct regmap **maps[] = { &mipi->mmsys, &mipi->apmixed, &mipi->infra };
	struct clk *parent = &mipi->parent;
	ofnode node;
	u32 val;
	unsigned int i;
	int ret;

	mipi->base = dev_read_addr_ptr(dev);
	if (!mipi->base)
		return -EINVAL;
	ret = clk_get_by_index(dev, 0, &mipi->parent);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(maps); i++) {
		node = ofnode_parse_phandle(dev_ofnode(dev), names[i], 0);
		if (!ofnode_valid(node)) {
			ret = -EINVAL;
			goto free_maps;
		}
		ret = regmap_init_mem(node, maps[i]);
		if (ret)
			goto free_maps;
	}
	val = readl(mipi->base + MT6577_MIPI_CON1);
	mipi->div1 = FIELD_GET(MT6577_MIPI_DIV1, val);
	mipi->div2 = FIELD_GET(MT6577_MIPI_DIV2, val);
	mipi->reference = readl(mipi->base + MT6577_MIPI_CON3) & GENMASK(3, 2);
	if (parent->id) {
		ret = clk_get_by_id(parent->id, &parent);
		if (ret)
			goto free_maps;
	}
	mipi->clk.flags = CLK_GET_RATE_NOCACHE;
	ret = clk_register(&mipi->clk, "mt6577_mipi_pll", "mt6577_mipi_dpi", parent->dev->name);
	if (ret)
		goto free_maps;
	dev_clk_dm(dev, 0, &mipi->clk);
	return 0;
free_maps:
	while (i) {
		--i;
		regmap_uninit(*maps[i]);
		*maps[i] = NULL;
	}
	return ret;
}

static int mt6577_mipi_remove(struct udevice *dev)
{
	struct mt6577_mipi *mipi = dev_get_priv(dev);

	int ret;

	if (mipi->enabled || mipi->clk.enable_count)
		return -EBUSY;
	ret = device_remove(mipi->clk.dev, DM_REMOVE_NORMAL);
	if (ret)
		return ret;
	ret = device_unbind(mipi->clk.dev);
	if (ret)
		return ret;
	regmap_uninit(mipi->infra);
	regmap_uninit(mipi->apmixed);
	regmap_uninit(mipi->mmsys);
	return 0;
}

static int mt6577_mipi_of_xlate(struct clk *clk, struct ofnode_phandle_args *args)
{
	if (args->args_count)
		return -EINVAL;
	clk->id = CLK_ID(clk->dev, 0);
	clk->data = 0;
	return 0;
}

static ulong mt6577_mipi_provider_round(struct clk *clk, ulong rate)
{
	struct clk *pll;
	int ret;

	ret = clk_get_by_id(clk->id, &pll);
	if (ret)
		return ret;
	return clk_round_rate(pll, rate);
}

static const struct clk_ops mt6577_mipi_ops = {
	.of_xlate = mt6577_mipi_of_xlate,
	.get_rate = ccf_clk_get_rate,
	.round_rate = mt6577_mipi_provider_round,
	.set_rate = ccf_clk_set_rate,
	.enable = ccf_clk_enable,
	.disable = ccf_clk_disable,
};

static const struct udevice_id mt6577_mipi_ids[] = {
	{ .compatible = "mediatek,mt6577-mipi-tx" },
	{ }
};

U_BOOT_DRIVER(mt6577_mipi_clock) = {
	.name = "mt6577_mipi_clock",
	.id = UCLASS_CLK,
	.of_match = mt6577_mipi_ids,
	.probe = mt6577_mipi_probe,
	.remove = mt6577_mipi_remove,
	.ops = &mt6577_mipi_ops,
	.priv_auto = sizeof(struct mt6577_mipi),
};
