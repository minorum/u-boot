// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <errno.h>
#include <i2c.h>
#include <asm/barriers.h>
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
	bool failed;
};

static void bcm2153_write(struct bcm2153_i2c_priv *bsc, u8 value,
			 unsigned int offset)
{
	writeb(value, bsc->base + offset);
	dsb();
}

static int bcm2153_i2c_idle(struct bcm2153_i2c_priv *bsc)
{
	u8 status;

	return readb_poll_timeout(bsc->base + BSC_ISR, status,
				 !(status & BSC_BUSY), BSC_TIMEOUT_US);
}

static int bcm2153_operation(struct bcm2153_i2c_priv *bsc, u8 value, bool data)
{
	u8 status;
	int ret;

	bcm2153_write(bsc, 0x8f, BSC_ISR);
	ret = bcm2153_i2c_idle(bsc);
	if (ret)
		return ret;
	if (data)
		bcm2153_write(bsc, value, BSC_DAT);
	else
		bcm2153_write(bsc, (readb(bsc->base + BSC_CS) &
			      (value == 4 ? 0xf9 : 0xf1)) | value, BSC_CS);
	ret = readb_poll_timeout(bsc->base + BSC_ISR, status,
				 status & BSC_COMPLETE, BSC_TIMEOUT_US);
	if (ret)
		return ret;
	bcm2153_write(bsc, 0x8f, BSC_ISR);
	bcm2153_write(bsc, readb(bsc->base + BSC_CS) & 0xf9, BSC_CS);
	ret = bcm2153_i2c_idle(bsc);
	if (ret)
		return ret;
	if (status & BSC_ERROR)
		return -EIO;
	if (status & BSC_NACK)
		return -EREMOTEIO;
	udelay(31);
	return 0;
}

static int bcm2153_i2c_xfer(struct udevice *bus, struct i2c_msg *msgs, int count)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);
	int i, ret;
	uint j;

	if (count < 0 || (count && !msgs))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (msgs[i].addr > 0x7f ||
		    (msgs[i].flags & ~(I2C_M_RD | I2C_M_STOP)) ||
		    (i + 1 < count && !(msgs[i].flags & I2C_M_STOP)))
			return -EOPNOTSUPP;
		if (msgs[i].len && !msgs[i].buf)
			return -EINVAL;
	}
	if (priv->failed)
		return -ESHUTDOWN;
	for (i = 0; i < count; i++) {
		ret = bcm2153_operation(priv, 2, false);
		if (ret)
			goto out;
		ret = bcm2153_operation(priv, msgs[i].addr << 1 |
				     !!(msgs[i].flags & I2C_M_RD), true);
		if (ret)
			goto out;
		for (j = 0; j < msgs[i].len; j++) {
			if (msgs[i].flags & I2C_M_RD) {
				ret = bcm2153_operation(priv,
					j + 1 == msgs[i].len ? 0x0e : 6, false);
				if (!ret)
					msgs[i].buf[j] = readb(priv->base + BSC_DAT);
			} else {
				ret = bcm2153_operation(priv, msgs[i].buf[j], true);
			}
			if (ret)
				goto out;
		}
		ret = bcm2153_operation(priv, 4, false);
		if (ret)
			goto out;
	}
	return 0;

out:
	priv->failed = true;
	return ret;
}

static int bcm2153_i2c_probe(struct udevice *bus)
{
	struct bcm2153_i2c_priv *priv = dev_get_priv(bus);

	priv->base = dev_read_addr_ptr(bus);
	if (!priv->base)
		return -EINVAL;
	if (readb(priv->base + BSC_TIM) != 0x11 ||
	    readb(priv->base + BSC_CLKEN) != 0x13 ||
	    !(readb(priv->base + BSC_CS) & 1))
		return -EOPNOTSUPP;
	return bcm2153_i2c_idle(priv);
}

static const struct dm_i2c_ops bcm2153_i2c_ops = {
	.xfer = bcm2153_i2c_xfer,
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
	.ops = &bcm2153_i2c_ops,
};
