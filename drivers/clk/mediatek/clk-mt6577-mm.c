// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <clk-uclass.h>
#include <dm.h>
#include <malloc.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dt-bindings/clock/mediatek,mt6577-mmsys.h>
#include <linux/clk-provider.h>
#include <linux/iopoll.h>

struct mt6577_mm_gate {
	struct clk clk;
	void __iomem *base;
	u32 reg;
	u32 mask;
};

static const struct {
	const char *name;
	u32 reg;
	u32 mask;
} mt6577_mm_data[] = {
	{ "mt6577_mm_dpi", 0x08, BIT(4) },
	{ "mt6577_mm_larb1", 0x08, BIT(7) },
	{ "mt6577_mm_larb1_emi", 0x00, BIT(7) },
};

struct mt6577_mm {
	struct mt6577_mm_gate *gates[CLK_MT6577_MM_NR_CLK];
	unsigned int count;
};

static int mt6577_mm_request(struct clk *clk)
{
	return clk_get_id(clk) < ARRAY_SIZE(mt6577_mm_data) ? 0 : -EINVAL;
}

static ulong mt6577_mm_get_rate(struct clk *clk)
{
	(void)clk;
	return 0;
}

static int mt6577_mm_enable(struct clk *clk)
{
	struct mt6577_mm_gate *gate = container_of(dev_get_clk_ptr(clk->dev),
						struct mt6577_mm_gate, clk);
	u32 val;

	writel(gate->mask, gate->base + gate->reg + 0x20);
	return readl_poll_timeout(gate->base + gate->reg, val,
				 !(val & gate->mask), 1000);
}

static int mt6577_mm_disable(struct clk *clk)
{
	struct mt6577_mm_gate *gate = container_of(dev_get_clk_ptr(clk->dev),
						struct mt6577_mm_gate, clk);
	u32 val;

	writel(gate->mask, gate->base + gate->reg + 0x10);
	return readl_poll_timeout(gate->base + gate->reg, val,
				 val & gate->mask, 1000);
}

static const struct clk_ops mt6577_mm_gate_ops = {
	.get_rate = mt6577_mm_get_rate,
	.enable = mt6577_mm_enable,
	.disable = mt6577_mm_disable,
};

U_BOOT_DRIVER(mt6577_mm_gate) = {
	.name = "mt6577_mm_gate",
	.id = UCLASS_CLK,
	.ops = &mt6577_mm_gate_ops,
};

static int mt6577_mm_probe(struct udevice *dev)
{
	struct mt6577_mm *mm = dev_get_priv(dev);
	struct mt6577_mm_gate *gate;
	void __iomem *base = dev_read_addr_ptr(dev);
	unsigned int i;
	int ret;

	if (!base)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(mt6577_mm_data); i++) {
		gate = kzalloc(sizeof(*gate), GFP_KERNEL);
		if (!gate) {
			ret = -ENOMEM;
			goto free_clocks;
		}
		gate->base = base;
		gate->reg = mt6577_mm_data[i].reg;
		gate->mask = mt6577_mm_data[i].mask;
		gate->clk.flags = CLK_GET_RATE_NOCACHE;
		ret = clk_register(&gate->clk, "mt6577_mm_gate", mt6577_mm_data[i].name, NULL);
		if (ret) {
			kfree(gate);
			goto free_clocks;
		}
		mm->gates[mm->count++] = gate;
		dev_clk_dm(dev, i, &gate->clk);
	}
	return 0;
free_clocks:
	while (mm->count) {
		gate = mm->gates[--mm->count];
		if (!device_remove(gate->clk.dev, DM_REMOVE_NORMAL) &&
		    !device_unbind(gate->clk.dev))
			kfree(gate);
	}
	return ret;
}

static const struct clk_ops mt6577_mm_ops = {
	.request = mt6577_mm_request,
	.get_rate = ccf_clk_get_rate,
	.enable = ccf_clk_enable,
	.disable = ccf_clk_disable,
};

static const struct udevice_id mt6577_mm_ids[] = {
	{ .compatible = "mediatek,mt6577-mmsys" },
	{ }
};

U_BOOT_DRIVER(mt6577_mm_clock) = {
	.name = "mt6577_mm_clock",
	.id = UCLASS_CLK,
	.of_match = mt6577_mm_ids,
	.probe = mt6577_mm_probe,
	.ops = &mt6577_mm_ops,
	.priv_auto = sizeof(struct mt6577_mm),
};
