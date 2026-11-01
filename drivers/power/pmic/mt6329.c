// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <dm/lists.h>
#include <i2c.h>
#include <power/pmic.h>

static int mt6329_reg_count(struct udevice *dev)
{
	return 0x194;
}

static int mt6329_read(struct udevice *dev, uint reg, u8 *data, int len)
{
	struct udevice *bank;
	int ret;

	if (len != 1)
		return -EACCES;
	if (reg == 0x192 || reg == 0x193) {
		ret = i2c_get_chip(dev->parent, 0x61, 1, &bank);
		if (ret)
			return ret;
		return dm_i2c_read(bank, reg & 0xff, data, len);
	}
	if (reg != 0 && reg != 1 && reg != 0x9b && reg != 0xa7 &&
	    reg != 0x38 && reg != 0xe8 && reg != 0xe9 &&
	    reg != 0xdc && reg != 0xdd && reg != 0xe0 && reg != 0xe1)
		return -EACCES;
	return dm_i2c_read(dev, reg, data, len);
}

static int mt6329_write(struct udevice *dev, uint reg, const u8 *data, int len)
{
	u8 old;
	u8 mask;
	int ret;

	if (len != 1)
		return -EACCES;
	switch (reg) {
	case 0x9b:
		mask = BIT(0);
		break;
	case 0x38:
		mask = BIT(5);
		break;
	case 0xe8:
	case 0xe9:
		mask = 0xf3;
		break;
	default:
		return -EACCES;
	}
	ret = dm_i2c_read(dev, reg, &old, 1);
	if (ret)
		return ret;
	if ((reg == 0x9b && (old & BIT(1))) || ((old ^ *data) & ~mask))
		return -EACCES;
	return dm_i2c_write(dev, reg, data, len);
}

static int mt6329_probe(struct udevice *dev)
{
	static const uint registers[] = { 0, 1, 0x9b, 0xa7 };
	u8 data[4];
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = mt6329_read(dev, registers[i], &data[i], 1);
		if (ret)
			return ret;
	}
	if ((data[1] << 8 | data[0]) != 0x1029)
		return -ENODEV;
	if ((data[2] | data[3]) & BIT(1))
		return -EOPNOTSUPP;
	return 0;
}

static int mt6329_bind(struct udevice *dev)
{
	ofnode regulators = dev_read_subnode(dev, "regulators");
	ofnode node;
	int ret;

	if (IS_ENABLED(CONFIG_REGULATOR_MT6329)) {
		if (!ofnode_valid(regulators))
			return -EINVAL;
		node = ofnode_find_subnode(regulators, "vusb");
		if (ofnode_valid(node) && ofnode_is_enabled(node)) {
			ret = device_bind_driver_to_node(dev, "mt6329_vusb", "vusb", node, NULL);
			if (ret)
				return ret;
		}
	}
	if (IS_ENABLED(CONFIG_ADC_MT6329)) {
		node = dev_read_subnode(dev, "adc");
		if (ofnode_valid(node) && ofnode_is_enabled(node))
			return device_bind_driver_to_node(dev, "mt6329_adc", "adc", node, NULL);
	}

	return 0;
}

static const struct dm_pmic_ops mt6329_ops = {
	.reg_count = mt6329_reg_count,
	.read = mt6329_read,
	.write = mt6329_write,
};

static const struct udevice_id mt6329_ids[] = {
	{ .compatible = "mediatek,mt6329" },
	{ }
};

U_BOOT_DRIVER(mt6329_pmic) = {
	.name = "mt6329_pmic",
	.id = UCLASS_PMIC,
	.of_match = mt6329_ids,
	.bind = mt6329_bind,
	.probe = mt6329_probe,
	.ops = &mt6329_ops,
};
