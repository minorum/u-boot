// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <errno.h>
#include <video.h>
#include <asm/io.h>
#include <linux/iopoll.h>

#define LCD_WIDTH	240
#define LCD_HEIGHT	400
#define LCD_BUSY	BIT(30)

struct gt_s5620_lcd_priv {
	void __iomem *base;
	int error;
};

static int gt_s5620_lcd_write(struct gt_s5620_lcd_priv *priv, int offset,
			     u32 value)
{
	u32 status;

	if (readl(priv->base + 0x1c) & LCD_BUSY) {
		priv->error = readl_poll_timeout(priv->base + 0x1c, status,
						!(status & LCD_BUSY), 10000);
		if (priv->error)
			return priv->error;
	}
	writel(value, priv->base + offset);
	return 0;
}

static int gt_s5620_lcd_axis(struct gt_s5620_lcd_priv *priv, u8 command,
			    unsigned int start, unsigned int end)
{
	u8 data[] = { start >> 8, start, end >> 8, end };
	unsigned int i;
	int ret;

	ret = gt_s5620_lcd_write(priv, 0, command);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(data); i++) {
		ret = gt_s5620_lcd_write(priv, 4, data[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int gt_s5620_lcd_sync(struct udevice *dev)
{
	struct gt_s5620_lcd_priv *priv = dev_get_priv(dev);
	struct video_priv *video = dev_get_uclass_priv(dev);
	int xs = video->damage.xstart, xe = video->damage.xend;
	int ys = video->damage.ystart, ye = video->damage.yend;
	int x, y, ret;
	u16 *line;

	if (priv->error)
		return priv->error;
	if (xs < 0 || ys < 0 || xe > LCD_WIDTH || ye > LCD_HEIGHT)
		return -EINVAL;
	if (xs >= xe || ys >= ye)
		return 0;
	ret = gt_s5620_lcd_axis(priv, 0x2a, xs, xe - 1);
	if (ret)
		return ret;
	ret = gt_s5620_lcd_axis(priv, 0x2b, ys, ye - 1);
	if (ret)
		return ret;
	ret = gt_s5620_lcd_write(priv, 0, 0x2c);
	if (ret)
		return ret;
	for (y = ys; y < ye; y++) {
		line = video->fb + y * video->line_length;
		for (x = xs; x < xe; x++) {
			ret = gt_s5620_lcd_write(priv, 4, line[x]);
			if (ret)
				return ret;
		}
	}
	return gt_s5620_lcd_write(priv, 0, 0x29);
}

static int gt_s5620_lcd_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = LCD_WIDTH * LCD_HEIGHT * sizeof(u16);
	plat->align = 32;
	return 0;
}

static int gt_s5620_lcd_probe(struct udevice *dev)
{
	struct gt_s5620_lcd_priv *priv = dev_get_priv(dev);
	struct video_priv *video = dev_get_uclass_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	video->xsize = LCD_WIDTH;
	video->ysize = LCD_HEIGHT;
	video->bpix = VIDEO_BPP16;
	return 0;
}

static const struct video_ops gt_s5620_lcd_ops = {
	.video_sync = gt_s5620_lcd_sync,
};

static const struct udevice_id gt_s5620_lcd_ids[] = {
	{ .compatible = "samsung,gt-s5620-stock-lcd" },
	{ }
};

U_BOOT_DRIVER(gt_s5620_lcd) = {
	.name = "gt_s5620_lcd",
	.id = UCLASS_VIDEO,
	.of_match = gt_s5620_lcd_ids,
	.bind = gt_s5620_lcd_bind,
	.probe = gt_s5620_lcd_probe,
	.priv_auto = sizeof(struct gt_s5620_lcd_priv),
	.ops = &gt_s5620_lcd_ops,
};
