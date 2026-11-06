// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <cpu_func.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/ofnode_graph.h>
#include <hang.h>
#include <panel.h>
#include <regmap.h>
#include <syscon.h>
#include <video.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <linux/clk-provider.h>

#define DPI_EN			0x00
#define DPI_CNTL		0x04
#define DPI_INT_ENABLE		0x08
#define DPI_INT_STATUS		0x0c
#define DPI_SIZE		0x10
#define DPI_CLK_CNTL		0x14
#define DPI_FB_ADDR		0x30
#define DPI_FB_PITCH		0x34
#define DPI_FIFO_TH		0x60
#define DPI_FIFO_STEP		0x64
#define DPI_REQ_TH		0x68
#define DPI_HCNTL		0x70
#define DPI_VCNTL		0x74
#define DPI_CURSOR_CNTL		0x80
#define DPI_GMC_CNTL		0x90
#define DPI_TMODE		0x94

struct mt8377_dpi {
	void __iomem *regs;
	struct regmap *mmsys;
	struct clk clocks[3];
	struct clk pll;
	struct udevice *panel;
	unsigned int gates;
	bool pll_enabled;
};

static void mt8377_dpi_configure(struct mt8377_dpi *dpi,
				 const struct display_timing *t,
				 unsigned int div, u32 fb)
{
	u32 clock = (div - 1) | ((div / 2) << 8);
	u32 hsync = (t->hsync_len.typ - 1) |
		    ((t->hback_porch.typ - 1) << 8) |
		    ((t->hfront_porch.typ - 1) << 16);
	u32 vsync = (t->vsync_len.typ - 1) |
		    ((t->vback_porch.typ - 1) << 8) |
		    ((t->vfront_porch.typ - 1) << 16);

	if (t->flags & DISPLAY_FLAGS_PIXDATA_NEGEDGE)
		clock |= BIT(31);
	if (t->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		hsync |= BIT(24);
	if (t->flags & DISPLAY_FLAGS_DE_LOW)
		hsync |= BIT(25);
	if (t->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		vsync |= BIT(24);

	writel(0, dpi->regs + DPI_INT_ENABLE);
	writel(0, dpi->regs + DPI_INT_STATUS);
	writel(BIT(12) | BIT(13) | BIT(14), dpi->regs + DPI_CNTL);
	writel(t->hactive.typ | (t->vactive.typ << 16), dpi->regs + DPI_SIZE);
	writel(clock, dpi->regs + DPI_CLK_CNTL);
	writel(hsync, dpi->regs + DPI_HCNTL);
	writel(vsync, dpi->regs + DPI_VCNTL);
	writel(400 | (496 << 16), dpi->regs + DPI_FIFO_TH);
	writel(8 | (8 << 16), dpi->regs + DPI_FIFO_STEP);
	writel(512, dpi->regs + DPI_REQ_TH);
	writel(0, dpi->regs + DPI_CURSOR_CNTL);
	writel(0, dpi->regs + DPI_TMODE);
	writel(0x41, dpi->regs + DPI_GMC_CNTL);
	writel(fb, dpi->regs + DPI_FB_ADDR);
	writel(t->hactive.typ * 2, dpi->regs + DPI_FB_PITCH);
}

static int mt8377_dpi_stop(struct mt8377_dpi *dpi)
{
	struct clk *engine;
	int ret;

	if (dpi->gates == ARRAY_SIZE(dpi->clocks)) {
		writel(0, dpi->regs + DPI_INT_ENABLE);
		writel(0, dpi->regs + DPI_EN);
		readl(dpi->regs + DPI_EN);
		ret = clk_get_by_id(dpi->clocks[2].id, &engine);
		if (ret || engine->enable_count != 1 ||
		    (engine->flags & CLK_IS_CRITICAL))
			return -EBUSY;
	}
	while (dpi->gates) {
		ret = clk_disable(&dpi->clocks[dpi->gates - 1]);
		if (ret)
			return ret;
		dpi->gates--;
	}
	if (dpi->pll_enabled) {
		ret = clk_disable(&dpi->pll);
		if (ret)
			return ret;
		dpi->pll_enabled = false;
	}
	return 0;
}

static int mt8377_dpi_clock(struct mt8377_dpi *dpi, unsigned long rate,
			    unsigned long *pll_rate)
{
	unsigned long best_error = ULONG_MAX;
	unsigned int div, best_div = 0;
	long rounded;

	if (!rate || rate > ULONG_MAX / 32)
		return -EINVAL;
	for (div = 2; div <= 32; div++) {
		unsigned long actual, error;

		rounded = clk_round_rate(&dpi->pll, rate * div);
		if (rounded <= 0)
			continue;
		actual = rounded / div;
		error = actual > rate ? actual - rate : rate - actual;
		if (error < best_error) {
			best_error = error;
			best_div = div;
			*pll_rate = rounded;
		}
	}
	if (!best_div || best_error > rate / 100)
		return -EINVAL;
	return best_div;
}

static int mt8377_dpi_validate(const struct display_timing *t)
{
	const unsigned int porches[] = {
		t->hfront_porch.typ, t->hsync_len.typ, t->hback_porch.typ,
		t->vfront_porch.typ, t->vsync_len.typ, t->vback_porch.typ,
	};
	unsigned int i;

	if (!t->hactive.typ || t->hactive.typ > 1024 ||
	    !t->vactive.typ || t->vactive.typ > 768 || t->hactive.typ % 4 ||
	    t->flags & ~(DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_HSYNC_HIGH |
			 DISPLAY_FLAGS_VSYNC_LOW | DISPLAY_FLAGS_VSYNC_HIGH |
			 DISPLAY_FLAGS_DE_LOW | DISPLAY_FLAGS_DE_HIGH |
			 DISPLAY_FLAGS_PIXDATA_POSEDGE | DISPLAY_FLAGS_PIXDATA_NEGEDGE))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(porches); i++)
		if (!porches[i] || porches[i] > 256)
			return -EINVAL;
	return 0;
}

static int mt8377_dpi_probe(struct udevice *dev)
{
	static const char * const names[] = { "larb-emi", "larb", "engine" };
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *video = dev_get_uclass_priv(dev);
	struct mt8377_dpi *dpi = dev_get_priv(dev);
	struct display_timing timing;
	unsigned long pll_rate;
	ofnode remote;
	int div, ret, cleanup;

	dpi->regs = dev_read_addr_ptr(dev);
	if (!dpi->regs || plat->base > U32_MAX || plat->base % 8)
		return -EINVAL;
	dpi->mmsys = syscon_regmap_lookup_by_phandle(dev, "mediatek,mmsys");
	if (IS_ERR(dpi->mmsys))
		return PTR_ERR(dpi->mmsys);
	ret = clk_get_by_name(dev, "pll", &dpi->pll);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) {
		ret = clk_get_by_name(dev, names[i], &dpi->clocks[i]);
		if (ret)
			return ret;
	}
	remote = ofnode_graph_get_remote_node(dev_ofnode(dev), -1, -1);
	ret = uclass_get_device_by_ofnode(UCLASS_PANEL, remote, &dpi->panel);
	if (ret)
		return ret;
	ret = panel_get_display_timing(dpi->panel, &timing);
	if (ret)
		goto fail;
	ret = mt8377_dpi_validate(&timing);
	if (ret)
		goto fail;
	div = mt8377_dpi_clock(dpi, timing.pixelclock.typ, &pll_rate);
	if (div < 0) {
		ret = div;
		goto fail;
	}
	ret = clk_set_rate(&dpi->pll, pll_rate);
	if (ret < 0)
		goto fail;
	ret = clk_enable(&dpi->pll);
	if (ret)
		goto fail;
	dpi->pll_enabled = true;
	while (dpi->gates < ARRAY_SIZE(dpi->clocks)) {
		ret = clk_enable(&dpi->clocks[dpi->gates]);
		if (ret)
			goto fail;
		dpi->gates++;
	}
	writel(0, dpi->regs + DPI_EN);
	ret = regmap_update_bits(dpi->mmsys, 0x900, GENMASK(2, 0), BIT(0));
	if (ret)
		goto fail;
	memset((void *)plat->base, 0, plat->size);
	flush_dcache_range(plat->base, plat->base + ALIGN(plat->size, ARCH_DMA_MINALIGN));
	mt8377_dpi_configure(dpi, &timing, div, plat->base);
	writel(1, dpi->regs + DPI_EN);
	ret = panel_enable_backlight(dpi->panel);
	if (ret)
		goto fail;
	video->xsize = timing.hactive.typ;
	video->ysize = timing.vactive.typ;
	video->bpix = VIDEO_BPP16;
	video_set_flush_dcache(dev, true);
	return 0;

fail:
	cleanup = mt8377_dpi_stop(dpi);
	if (cleanup)
		hang();
	cleanup = device_remove(dpi->panel, DM_REMOVE_NORMAL);
	return cleanup ? cleanup : ret;
}

static int mt8377_dpi_remove(struct udevice *dev)
{
	struct mt8377_dpi *dpi = dev_get_priv(dev);
	int ret;

	ret = panel_set_backlight(dpi->panel, 0);
	if (mt8377_dpi_stop(dpi))
		hang();
	if (ret)
		return ret;
	return device_remove(dpi->panel, DM_REMOVE_NORMAL);
}

static int mt8377_dpi_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = 1024 * 768 * 2;
	return 0;
}

static const struct udevice_id mt8377_dpi_ids[] = {
	{ .compatible = "mediatek,mt8377-dpi" },
	{ .compatible = "mediatek,mt6577-dpi" },
	{ }
};

U_BOOT_DRIVER(mt8377_dpi) = {
	.name = "mt8377_dpi",
	.id = UCLASS_VIDEO,
	.of_match = mt8377_dpi_ids,
	.bind = mt8377_dpi_bind,
	.probe = mt8377_dpi_probe,
	.remove = mt8377_dpi_remove,
	.flags = DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
	.priv_auto = sizeof(struct mt8377_dpi),
};
