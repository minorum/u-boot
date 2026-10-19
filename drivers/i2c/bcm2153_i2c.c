// SPDX-License-Identifier: GPL-2.0+

#include <clk.h>
#include <dm.h>
#include <errno.h>
#include <i2c.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/iopoll.h>

#define BSC_CS		0x00
#define BSC_TIM		0x04
#define BSC_DAT		0x08
#define BSC_ISR		0x28
#define BSC_CLKEN	0x2c
#define BSC_TIMEOUT_US	10000
#define BSC_BUSY		0x80
#define BSC_COMPLETE	0x0c
#define BSC_ERROR	0x04
#define BSC_NACK		0x01

struct bcm2153_i2c_priv {
	void __iomem *base;
	struct clk clk;
	ulong input_rate;
	uint rate;
	uint turnaround_us;
};

static int bcm2153_i2c_idle(struct bcm2153_i2c_priv *priv)
{
	u8 status;

	return readb_poll_timeout(priv->base + BSC_ISR, status,
				 !(status & BSC_BUSY), BSC_TIMEOUT_US);
}

static int bcm2153_i2c_complete(struct bcm2153_i2c_priv *priv)
{
	u8 status;
	int ret;

	ret = readb_poll_timeout(priv->base + BSC_ISR, status,
				status & BSC_COMPLETE, BSC_TIMEOUT_US);
	if (ret)
		return ret;
	writeb(0x8f, priv->base + BSC_ISR);
	writeb(readb(priv->base + BSC_CS) & ~6, priv->base + BSC_CS);
	ret = bcm2153_i2c_idle(priv);
	if (ret)
		return ret;
	udelay(priv->turnaround_us);
	if (status & BSC_ERROR)
		return -EIO;
	return status & BSC_NACK ? -EREMOTEIO : 0;
}

static int bcm2153_i2c_command(struct bcm2153_i2c_priv *priv, u8 command)
{
	int ret = bcm2153_i2c_idle(priv);

	if (ret)
		return ret;
	writeb(0x8f, priv->base + BSC_ISR);
	writeb((readb(priv->base + BSC_CS) & ~0x0e) | command,
	       priv->base + BSC_CS);
	return bcm2153_i2c_complete(priv);
}

static int bcm2153_i2c_send(struct bcm2153_i2c_priv *priv, u8 value)
{
	int ret = bcm2153_i2c_idle(priv);

	if (ret)
		return ret;
	writeb(0x8f, priv->base + BSC_ISR);
	writeb(value, priv->base + BSC_DAT);
	return bcm2153_i2c_complete(priv);
}

static int bcm2153_i2c_xfer(struct udevice *bus, struct i2c_msg *msgs, int count)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);
	bool started = false;
	int i, ret, stop;
	uint j;

	if (count < 0 || (count && !msgs))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (msgs[i].addr > 0x7f ||
		    (msgs[i].flags & ~(I2C_M_RD | I2C_M_STOP)))
			return -EOPNOTSUPP;
		if (msgs[i].len && !msgs[i].buf)
			return -EINVAL;
	}
	for (i = 0; i < count; i++) {
		ret = bcm2153_i2c_command(priv, started ? 0x0a : 2);
		if (ret)
			goto out;
		started = true;
		ret = bcm2153_i2c_send(priv, msgs[i].addr << 1 |
				     !!(msgs[i].flags & I2C_M_RD));
		if (ret)
			goto out;
		for (j = 0; j < msgs[i].len; j++) {
			if (msgs[i].flags & I2C_M_RD) {
				ret = bcm2153_i2c_command(priv,
					j + 1 == msgs[i].len ? 0x0e : 6);
				if (!ret)
					msgs[i].buf[j] = readb(priv->base + BSC_DAT);
			} else {
				ret = bcm2153_i2c_send(priv, msgs[i].buf[j]);
			}
			if (ret)
				goto out;
		}
		if (msgs[i].flags & I2C_M_STOP) {
			ret = bcm2153_i2c_command(priv, 4);
			started = false;
			if (ret)
				return ret;
		}
	}
	return started ? bcm2153_i2c_command(priv, 4) : 0;

out:
	if (ret != -ETIMEDOUT) {
		stop = bcm2153_i2c_command(priv, 4);
		if (stop)
			return stop;
	}
	return ret;
}

static int bcm2153_i2c_set_speed(struct udevice *bus, uint speed)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);
	uint best = 0, rate, p, div, divisor, timing = 0;
	int ret;

	if (!speed || speed > 400000)
		return -EINVAL;
	for (div = 0; div < 4; div++) {
		for (p = 0; p < 8; p++) {
			divisor = (1 << (4 - div)) * (4 * (p + 1) + 3);
			rate = priv->input_rate / divisor;
			if (rate && priv->input_rate <= (ulong)speed * divisor &&
			    rate > best) {
				best = rate;
				timing = p << 3 | div;
			}
		}
	}
	if (!best)
		return -ERANGE;
	ret = bcm2153_i2c_idle(priv);
	if (ret)
		return ret;
	if (readb(priv->base + BSC_TIM) & 0xc4)
		return -EOPNOTSUPP;
	writeb(0x13, priv->base + BSC_CLKEN);
	writeb(timing, priv->base + BSC_TIM);
	priv->rate = best;
	priv->turnaround_us = DIV_ROUND_UP(2000000, best);
	return 0;
}

static int bcm2153_i2c_get_speed(struct udevice *bus)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);

	return priv->rate;
}

static int bcm2153_i2c_probe(struct udevice *bus)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);
	int ret;

	priv->base = dev_read_addr_ptr(bus);
	if (!priv->base)
		return -EINVAL;
	ret = clk_get_by_index(bus, 0, &priv->clk);
	if (ret)
		return ret;
	priv->input_rate = clk_get_rate(&priv->clk);
	if (IS_ERR_VALUE(priv->input_rate) || !priv->input_rate)
		return -EINVAL;
	ret = clk_enable(&priv->clk);
	if (ret)
		return ret;
	ret = bcm2153_i2c_idle(priv);
	if (!ret && (readb(priv->base + BSC_TIM) & 0xc4))
		ret = -EOPNOTSUPP;
	if (ret) {
		clk_disable(&priv->clk);
		return ret;
	}
	priv->turnaround_us = 20;
	writeb(readb(priv->base + BSC_CLKEN) | 1, priv->base + BSC_CLKEN);
	writeb(readb(priv->base + BSC_CS) | 0xc0, priv->base + BSC_CS);
	udelay(80);
	writeb(0x8f, priv->base + BSC_ISR);
	writeb(readb(priv->base + 0x10) & ~3, priv->base + 0x10);
	writeb(0x89, priv->base + 0x14);
	writeb(readb(priv->base + 0x0c) & 0x7f, priv->base + 0x0c);
	writeb(readb(priv->base + 0x1c) & 0xbf, priv->base + 0x1c);
	writeb(readb(priv->base + BSC_CS) & ~6, priv->base + BSC_CS);
	udelay(80);
	writeb((readb(priv->base + BSC_CS) & 0xf0) | 1, priv->base + BSC_CS);
	udelay(80);
	writeb(0, priv->base + 0x24);
	return 0;
}

static int bcm2153_i2c_remove(struct udevice *bus)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);

	return clk_disable(&priv->clk);
}

static const struct dm_i2c_ops bcm2153_i2c_ops = {
	.xfer = bcm2153_i2c_xfer,
	.set_bus_speed = bcm2153_i2c_set_speed,
	.get_bus_speed = bcm2153_i2c_get_speed,
};

static const struct udevice_id bcm2153_i2c_ids[] = {
	{ .compatible = "brcm,bcm2153-i2c" },
	{ }
};

U_BOOT_DRIVER(bcm2153_i2c) = {
	.name = "bcm2153-i2c",
	.id = UCLASS_I2C,
	.of_match = bcm2153_i2c_ids,
	.priv_auto = sizeof(struct bcm2153_i2c_priv),
	.probe = bcm2153_i2c_probe,
	.remove = bcm2153_i2c_remove,
	.ops = &bcm2153_i2c_ops,
};
