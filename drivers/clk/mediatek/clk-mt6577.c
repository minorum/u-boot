// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <clk-uclass.h>
#include <div64.h>
#include <dm.h>
#include <malloc.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dt-bindings/clock/mediatek,mt6577-clk.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>

enum mt6577_clock_type {
	MT6577_MAINPLL,
	MT6577_AXI,
	MT6577_MSDC_SEL,
	MT6577_MSDC3_SEL,
	MT6577_USB_SEL,
	MT6577_WPLL197,
	MT6577_WPLL61,
	MT6577_UPLL,
};

struct mt6577_clock {
	struct clk clk;
	void __iomem *base;
	enum mt6577_clock_type type;
	u32 source;
};

struct mt6577_clk_priv {
	void __iomem *base;
	struct clk *clocks[8];
	unsigned int count;
};

static const struct {
	u16 offset;
	u16 mask;
	u16 enabled;
	u32 rate;
	u8 delay;
} mt6577_analog[] = {
	{ 0x1c0, 3, 3, 197000000, 30 },
	{ 0x1c0, BIT(14) | BIT(0), BIT(14) | BIT(0), 61000000, 30 },
	{ 0x160, BIT(0), 0, 0, 20 },
};

static u32 mt6577_clock_source(struct mt6577_clock *clock)
{
	u32 value;

	if (clock->type == MT6577_AXI) {
		value = readl(clock->base) & 3;
		return value == 3 ? 1 : value;
	}
	value = readl(clock->base + 0x0c);
	if (clock->type == MT6577_MSDC3_SEL)
		return (value >> 7) & 3;
	return clock->type == MT6577_MSDC_SEL ? value & 3 : !!(value & BIT(2));
}

static ulong mt6577_clock_rate(struct clk *clk)
{
	struct mt6577_clock *clock = container_of(dev_get_clk_ptr(clk->dev),
						struct mt6577_clock, clk);
	static const unsigned int divisors[] = { 1, 4, 5, 6 };
	u32 con0, con1, prediv, postdiv, fbsel;
	ulong parent;
	u64 rate;

	if (clock->type >= MT6577_WPLL197)
		return mt6577_analog[clock->type - MT6577_WPLL197].rate;
	if (clock->type != MT6577_MAINPLL &&
	    mt6577_clock_source(clock) != clock->source)
		return 0;
	if (((clock->type == MT6577_MSDC_SEL || clock->type == MT6577_MSDC3_SEL) &&
	     clock->source > 1) ||
	    (clock->type == MT6577_USB_SEL && clock->source))
		return 0;
	parent = clk_get_parent_rate(clk);
	if (IS_ERR_VALUE(parent))
		return parent;
	if (clock->type == MT6577_MAINPLL) {
		con0 = readl(clock->base + 0x120);
		con1 = readl(clock->base + 0x124);
		fbsel = 1U << min_t(u32, FIELD_GET(GENMASK(5, 4), con0), 2);
		prediv = 1U << min_t(u32, FIELD_GET(GENMASK(7, 6), con0), 2);
		postdiv = 1U << min_t(u32, FIELD_GET(GENMASK(5, 4), con1), 2);
		rate = (u64)parent * (FIELD_GET(GENMASK(14, 8), con0) + 1) * fbsel;
		do_div(rate, prediv * postdiv);
		return rate <= ULONG_MAX ? rate : 0;
	}
	if (clock->type == MT6577_AXI)
		return parent / divisors[FIELD_GET(GENMASK(4, 3), readl(clock->base + 4))];
	return parent;
}

static int mt6577_clock_enable(struct clk *clk)
{
	struct mt6577_clock *clock = container_of(dev_get_clk_ptr(clk->dev),
						struct mt6577_clock, clk);
	u16 old, value;
	unsigned int index;
	ulong rate;

	if (clock->type < MT6577_WPLL197) {
		rate = mt6577_clock_rate(clk);
		return !rate || IS_ERR_VALUE(rate) ? -EINVAL : 0;
	}
	index = clock->type - MT6577_WPLL197;
	old = readw(clock->base + mt6577_analog[index].offset);
	value = (old & ~mt6577_analog[index].mask) | mt6577_analog[index].enabled;
	writew(value, clock->base + mt6577_analog[index].offset);
	if ((old ^ value) & BIT(0))
		udelay(mt6577_analog[index].delay);
	return 0;
}

static const struct clk_ops mt6577_clock_ops = {
	.get_rate = mt6577_clock_rate,
	.enable = mt6577_clock_enable,
};

U_BOOT_DRIVER(mt6577_clock) = {
	.name = "mt6577_clock",
	.id = UCLASS_CLK,
	.ops = &mt6577_clock_ops,
};

static struct clk *mt6577_register_clock(struct udevice *dev, const char *name,
					 const char *parent, enum mt6577_clock_type type,
					 ulong flags)
{
	struct mt6577_clk_priv *priv = dev_get_priv(dev);
	struct mt6577_clock *clock;
	int ret;

	clock = kzalloc(sizeof(*clock), GFP_KERNEL);
	if (!clock)
		return ERR_PTR(-ENOMEM);
	clock->base = priv->base;
	clock->type = type;
	clock->clk.flags = flags | CLK_GET_RATE_NOCACHE;
	if (type == MT6577_AXI || type == MT6577_MSDC_SEL ||
	    type == MT6577_MSDC3_SEL || type == MT6577_USB_SEL)
		clock->source = mt6577_clock_source(clock);
	ret = clk_register(&clock->clk, "mt6577_clock", name, parent);
	if (ret) {
		kfree(clock);
		return ERR_PTR(ret);
	}
	return &clock->clk;
}

static int mt6577_keep_clock(struct udevice *dev, struct clk *clock, int id)
{
	struct mt6577_clk_priv *priv = dev_get_priv(dev);

	if (IS_ERR(clock))
		return PTR_ERR(clock);
	priv->clocks[priv->count++] = clock;
	if (id < 0)
		clk_dm(0, clock);
	else
		dev_clk_dm(dev, id, clock);
	return 0;
}

static int mt6577_clock_request(struct clk *clk)
{
	ulong id = clk_get_id(clk);
	ulong block = dev_get_driver_data(clk->dev);

	if ((!block && id < CLK_MT6577_APMIXED_NR_CLK) ||
	    (block == 1 && id == CLK_MT6577_AXI) ||
	    (block == 2 && id >= CLK_MT6577_MSDC0 && id < CLK_MT6577_PERI_NR_CLK))
		return 0;
	return -EINVAL;
}

static int mt6577_parent_name(struct udevice *dev, unsigned int index, const char **name)
{
	struct clk input, *parent;
	int ret;

	ret = clk_get_by_index(dev, index, &input);
	if (ret)
		return ret;
	if (!input.id) {
		parent = dev_get_clk_ptr(input.dev);
		if (!parent)
			return -ENOENT;
	} else {
		ret = clk_get_by_id(input.id, &parent);
		if (ret)
			return ret;
	}
	*name = parent->dev->name;
	return 0;
}

static const struct clk_ops mt6577_provider_ops = {
	.request = mt6577_clock_request,
	.get_rate = ccf_clk_get_rate,
	.enable = ccf_clk_enable,
	.disable = ccf_clk_disable,
};

static int mt6577_clock_probe(struct udevice *dev)
{
	static const char * const names[] = {
		"mt6577_mainpll", "mt6577_wpll197", "mt6577_wpll61", "mt6577_upll",
	};
	static const enum mt6577_clock_type types[] = {
		MT6577_MAINPLL, MT6577_WPLL197, MT6577_WPLL61, MT6577_UPLL,
	};
	struct mt6577_clk_priv *priv = dev_get_priv(dev);
	const char *parents[4];
	struct clk *clock;
	ulong block = dev_get_driver_data(dev);
	u32 source;
	unsigned int i, count = block == 0 ? 1 : block == 1 ? 3 : 4;
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		ret = mt6577_parent_name(dev, i, &parents[i]);
		if (ret)
			return ret;
	}
	if (block == 0) {
		for (i = 0; i < ARRAY_SIZE(names); i++) {
			clock = mt6577_register_clock(dev, names[i], parents[0],
						      types[i], CLK_IS_CRITICAL);
			ret = mt6577_keep_clock(dev, clock, i);
			if (ret)
				goto fail;
		}
	} else if (block == 1) {
		source = readl(priv->base) & 3;
		source = source == 3 ? 1 : source;
		clock = mt6577_register_clock(dev, "mt6577_axi", parents[source],
					      MT6577_AXI, CLK_IS_CRITICAL);
		ret = mt6577_keep_clock(dev, clock, CLK_MT6577_AXI);
		if (ret)
			goto fail;
	} else {
		source = readl(priv->base + 0x0c) & 3;
		clock = mt6577_register_clock(dev, "mt6577_msdc_sel",
					      source < 2 ? parents[source + 1] : NULL,
					      MT6577_MSDC_SEL, 0);
		ret = mt6577_keep_clock(dev, clock, -1);
		if (ret)
			goto fail;
		clock = clk_register_gate(dev, "mt6577_msdc0", "mt6577_msdc_sel", 0,
					  priv->base + 0x10, 15, CLK_GATE_SET_TO_DISABLE, NULL);
		ret = mt6577_keep_clock(dev, clock, CLK_MT6577_MSDC0);
		if (ret)
			goto fail;
		source = (readl(priv->base + 0x0c) >> 7) & 3;
		clock = mt6577_register_clock(dev, "mt6577_msdc3_sel",
					      source < 2 ? parents[source + 1] : NULL,
					      MT6577_MSDC3_SEL, 0);
		ret = mt6577_keep_clock(dev, clock, -1);
		if (ret)
			goto fail;
		clock = clk_register_gate(dev, "mt6577_msdc3", "mt6577_msdc3_sel", 0,
					  priv->base + 0x10, 18, CLK_GATE_SET_TO_DISABLE, NULL);
		ret = mt6577_keep_clock(dev, clock, CLK_MT6577_MSDC3);
		if (ret)
			goto fail;
		source = readl(priv->base + 0x0c) & BIT(2);
		clock = mt6577_register_clock(dev, "mt6577_usb_sel",
					      source ? NULL : parents[3], MT6577_USB_SEL, 0);
		ret = mt6577_keep_clock(dev, clock, -1);
		if (ret)
			goto fail;
		clock = clk_register_gate(dev, "mt6577_usb", "mt6577_usb_sel", 0,
					  priv->base + 0x10, 9, CLK_GATE_SET_TO_DISABLE, NULL);
		ret = mt6577_keep_clock(dev, clock, CLK_MT6577_USB);
		if (ret)
			goto fail;
		clock = clk_register_gate(dev, "mt6577_i2c_dual", parents[0], CLK_IS_CRITICAL,
					  priv->base + 0x10, 30, CLK_GATE_SET_TO_DISABLE, NULL);
		ret = mt6577_keep_clock(dev, clock, CLK_MT6577_I2C_DUAL);
		if (ret)
			goto fail;
		clock = clk_register_gate(dev, "mt6577_i2c0", parents[0], 0,
					  priv->base + 0x10, 28, CLK_GATE_SET_TO_DISABLE, NULL);
		ret = mt6577_keep_clock(dev, clock, CLK_MT6577_I2C0);
		if (ret)
			goto fail;
	}
	return 0;
fail:
	while (priv->count) {
		struct clk *clock = priv->clocks[--priv->count];

		if (!device_remove(clock->dev, DM_REMOVE_NORMAL) && !device_unbind(clock->dev))
			kfree(clock);
	}
	return ret;
}

static const struct udevice_id mt6577_clk_ids[] = {
	{ .compatible = "mediatek,mt6577-apmixedsys", .data = 0 },
	{ .compatible = "mediatek,mt6577-infracfg", .data = 1 },
	{ .compatible = "mediatek,mt6577-pericfg", .data = 2 },
	{ }
};

U_BOOT_DRIVER(mt6577_clock_provider) = {
	.name = "mt6577_clock_provider",
	.id = UCLASS_CLK,
	.of_match = mt6577_clk_ids,
	.probe = mt6577_clock_probe,
	.ops = &mt6577_provider_ops,
	.priv_auto = sizeof(struct mt6577_clk_priv),
};
