// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <dm/lists.h>
#include <errno.h>
#include <i2c.h>
#include <power/pmic.h>

static int bcm59035_read(struct udevice *dev, uint reg, u8 *buf, int len)
{
	struct dm_i2c_chip *chip = dev_get_parent_plat(dev);
	u8 offset = reg;
	struct i2c_msg msgs[] = {
		{ .addr = chip->chip_addr, .flags = I2C_M_STOP,
		  .len = 1, .buf = &offset },
		{ .addr = chip->chip_addr, .flags = I2C_M_RD,
		  .len = 1, .buf = buf },
	};

	if (reg > 0xff || len != 1 || !buf)
		return -EINVAL;
	return dm_i2c_xfer(dev, msgs, ARRAY_SIZE(msgs));
}

static int bcm59035_write(struct udevice *dev, uint reg, const u8 *buf, int len)
{
	struct dm_i2c_chip *chip = dev_get_parent_plat(dev);
	u8 data[2];
	struct i2c_msg msg = {
		.addr = chip->chip_addr, .len = sizeof(data), .buf = data,
	};

	if (reg > 0xff || len != 1 || !buf)
		return -EINVAL;
	data[0] = reg;
	data[1] = *buf;
	return dm_i2c_xfer(dev, &msg, 1);
}

static int bcm59035_bind(struct udevice *dev)
{
	ofnode regulators = dev_read_subnode(dev, "regulators");
	ofnode rail;

	if (!ofnode_valid(regulators))
		return 0;
	rail = ofnode_find_subnode(regulators, "hcldo2");
	if (!ofnode_valid(rail) || !ofnode_is_enabled(rail))
		return 0;
	return device_bind_driver_to_node(dev, "bcm59035_hcldo2", "hcldo2",
					 rail, NULL);
}

static const struct dm_pmic_ops bcm59035_ops = {
	.read = bcm59035_read,
	.write = bcm59035_write,
};

static const struct udevice_id bcm59035_ids[] = {
	{ .compatible = "brcm,bcm59035" },
	{ }
};

U_BOOT_DRIVER(bcm59035) = {
	.name = "bcm59035",
	.id = UCLASS_PMIC,
	.of_match = bcm59035_ids,
	.bind = bcm59035_bind,
	.ops = &bcm59035_ops,
};
