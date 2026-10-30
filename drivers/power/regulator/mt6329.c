// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <linux/delay.h>
#include <power/pmic.h>
#include <power/regulator.h>

static int mt6329_vusb_value(struct udevice *dev)
{
	return 3300000;
}

static int mt6329_vusb_enabled(struct udevice *dev)
{
	int value = pmic_reg_read(dev->parent, 0x9b);

	return value < 0 ? value : !!(value & BIT(0));
}

static int mt6329_vusb_enable(struct udevice *dev, bool enable)
{
	int value, ret;

	value = pmic_reg_read(dev->parent, 0x9b);
	if (value < 0)
		return value;
	value = (value & ~BIT(0)) | enable;
	ret = pmic_reg_write(dev->parent, 0x9b, value);
	if (!ret && enable)
		mdelay(10);
	return ret;
}

static int mt6329_vusb_probe(struct udevice *dev)
{
	struct dm_regulator_uclass_plat *plat = dev_get_uclass_plat(dev);

	if (plat->min_uV != 3300000 || plat->max_uV != 3300000)
		return -EINVAL;
	plat->type = REGULATOR_TYPE_FIXED;
	return 0;
}

static const struct dm_regulator_ops mt6329_vusb_ops = {
	.get_value = mt6329_vusb_value,
	.get_enable = mt6329_vusb_enabled,
	.set_enable = mt6329_vusb_enable,
};

U_BOOT_DRIVER(mt6329_vusb) = {
	.name = "mt6329_vusb",
	.id = UCLASS_REGULATOR,
	.probe = mt6329_vusb_probe,
	.ops = &mt6329_vusb_ops,
};
