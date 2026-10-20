// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <errno.h>
#include <linux/delay.h>
#include <power/pmic.h>
#include <power/regulator.h>

#define BCM59035_HCLDO2	0x1b
#define BCM59035_HOSTACT	0x51
#define BCM59035_ON	0x00
#define BCM59035_LOWPOWER	0x11
#define BCM59035_OFF	0xaa

struct bcm59035_hcldo2_priv {
	bool failed;
};

static int bcm59035_hcldo2_get_enable(struct udevice *dev)
{
	struct bcm59035_hcldo2_priv *priv = dev_get_priv(dev);
	int value;

	if (priv->failed)
		return -ESHUTDOWN;
	value = pmic_reg_read(dev->parent, BCM59035_HOSTACT);
	if (value < 0)
		return value;
	if (value)
		return -EOPNOTSUPP;
	value = pmic_reg_read(dev->parent, BCM59035_HCLDO2);
	if (value < 0)
		return value;
	switch (value) {
	case BCM59035_ON:
	case BCM59035_LOWPOWER:
		return 1;
	case BCM59035_OFF:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int bcm59035_hcldo2_set_enable(struct udevice *dev, bool enable)
{
	struct bcm59035_hcldo2_priv *priv = dev_get_priv(dev);
	int ret;

	if (!enable)
		return -EACCES;
	ret = bcm59035_hcldo2_get_enable(dev);
	if (ret)
		return ret < 0 ? ret : 0;
	ret = pmic_reg_write(dev->parent, BCM59035_HCLDO2, BCM59035_LOWPOWER);
	if (ret)
		return ret;
	mdelay(1000);
	ret = pmic_reg_read(dev->parent, BCM59035_HCLDO2);
	if (ret < 0)
		return ret;
	if (ret != BCM59035_LOWPOWER) {
		priv->failed = true;
		return -EIO;
	}
	return 0;
}

static int bcm59035_hcldo2_probe(struct udevice *dev)
{
	struct dm_regulator_uclass_plat *plat = dev_get_uclass_plat(dev);

	plat->type = REGULATOR_TYPE_LDO;
	return 0;
}

static const struct dm_regulator_ops bcm59035_hcldo2_ops = {
	.get_enable = bcm59035_hcldo2_get_enable,
	.set_enable = bcm59035_hcldo2_set_enable,
};

U_BOOT_DRIVER(bcm59035_hcldo2) = {
	.name = "bcm59035_hcldo2",
	.id = UCLASS_REGULATOR,
	.ops = &bcm59035_hcldo2_ops,
	.probe = bcm59035_hcldo2_probe,
	.priv_auto = sizeof(struct bcm59035_hcldo2_priv),
};
