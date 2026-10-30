// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <dm.h>
#include <dm/read.h>
#include <generic-phy.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <power/regulator.h>

struct mt6577_usb_phy {
	void __iomem *base;
	struct clk *upll;
	struct clk *bus;
	struct udevice *vusb;
	bool failed;
};

enum { MT6577_QUALIFIED_SLEW_MAX = 7 };

static void mt6577_usb_update(struct mt6577_usb_phy *usb, unsigned int reg,
			      u8 mask, u8 value)
{
	writeb((readb(usb->base + reg) & ~mask) | value, usb->base + reg);
}

static int mt6577_usb_calibrate(struct mt6577_usb_phy *usb)
{
	u8 saved = readb(usb->base + 0x12);
	u8 control = readb(usb->base + 0x703);
	u8 cycles = readb(usb->base + 0x701);
	u8 free_run = readb(usb->base + 0x711);
	u8 ready;
	u32 result, value;
	int ret;

	if ((saved & BIT(7)) || (control & 1) || (free_run & 1))
		return -EBUSY;
	writeb(0x81, usb->base + 0x12);
	writeb(1, usb->base + 0x711);
	writeb(4, usb->base + 0x701);
	writeb(control | 1, usb->base + 0x703);
	if (readb(usb->base + 0x710) & 1) {
		ret = -EBUSY;
		goto restore;
	}
	ret = read_poll_timeout(readb, ready, ready & 1, 10, 3000000, usb->base + 0x710);
	if (ret)
		goto restore;
	result = readl(usb->base + 0x70c);
	if (!result) {
		ret = -ERANGE;
		goto restore;
	}
	value = (1024 * 48 * 28) / result;
	value = value / 1000 + (value % 1000 >= 500);
	if (!value || value > MT6577_QUALIFIED_SLEW_MAX) {
		ret = -ERANGE;
		goto restore;
	}
restore:
	writeb(control, usb->base + 0x703);
	writeb(cycles, usb->base + 0x701);
	writeb(free_run, usb->base + 0x711);
	writeb(ret ? saved : value, usb->base + 0x12);
	return ret;
}

static void mt6577_usb_save_current(struct mt6577_usb_phy *usb)
{
	mt6577_usb_update(usb, 0x6b, 0x04, 0);
	mt6577_usb_update(usb, 0x6e, 0x01, 0);
	mt6577_usb_update(usb, 0x6a, 0x04, 0);
	mt6577_usb_update(usb, 0x68, 0xc0, 0xc0);
	mt6577_usb_update(usb, 0x68, 0x30, 0x10);
	mt6577_usb_update(usb, 0x68, 0x04, 0x04);
	mt6577_usb_update(usb, 0x69, 0x3c, 0);
	mt6577_usb_update(usb, 0x6a, 0x3a, 0x3a);
	mt6577_usb_update(usb, 0x1c, 0x80, 0);
	mt6577_usb_update(usb, 0x1b, 0x08, 0);
	udelay(800);
	mt6577_usb_update(usb, 0x63, 0x02, 0x02);
	udelay(1);
	mt6577_usb_update(usb, 0x6a, 0x04, 0x04);
	udelay(1);
}

static int mt6577_usb_disable_clocks(struct mt6577_usb_phy *usb, bool bus_on)
{
	int ret = bus_on ? clk_disable(usb->bus) : 0;

	if (!ret)
		ret = clk_disable(usb->upll);
	if (ret)
		usb->failed = true;
	return ret;
}

static int mt6577_usb_power_on(struct phy *phy)
{
	struct mt6577_usb_phy *usb = dev_get_priv(phy->dev);
	bool bus_on = false;
	int ret, cleanup;

	if (usb->failed)
		return -EIO;
	if (clk_get_rate(usb->bus) != 61000000)
		return -EINVAL;
	ret = clk_enable(usb->upll);
	if (ret)
		return ret;
	ret = regulator_set_enable(usb->vusb, true);
	if (ret)
		goto disable_upll;
	mdelay(10);
	ret = clk_enable(usb->bus);
	if (ret)
		goto disable_supply;
	bus_on = true;
	udelay(50);
	mt6577_usb_update(usb, 0x6b, 0x04, 0);
	mt6577_usb_update(usb, 0x6e, 0x01, 0);
	mt6577_usb_update(usb, 0x1c, 0x80, 0);
	mt6577_usb_update(usb, 0x02, 0x7f, 0x09);
	mt6577_usb_update(usb, 0x22, 0x03, 0);
	mt6577_usb_update(usb, 0x6a, 0x04, 0);
	mt6577_usb_update(usb, 0x1b, 0x08, 0x08);
	udelay(800);
	mt6577_usb_update(usb, 0x68, 0xf4, 0);
	mt6577_usb_update(usb, 0x69, 0x3c, 0);
	mt6577_usb_update(usb, 0x6a, 0xbe, 0);
	udelay(800);
	ret = mt6577_usb_calibrate(usb);
	if (!ret)
		return 0;
	mt6577_usb_save_current(usb);
disable_supply:
	cleanup = regulator_set_enable(usb->vusb, false);
	if (cleanup) {
		usb->failed = true;
		return cleanup;
	}
disable_upll:
	cleanup = mt6577_usb_disable_clocks(usb, bus_on);
	return cleanup ? cleanup : ret;
}

static int mt6577_usb_power_off(struct phy *phy)
{
	struct mt6577_usb_phy *usb = dev_get_priv(phy->dev);
	int ret;

	if (usb->failed)
		return -EIO;
	mt6577_usb_save_current(usb);
	ret = regulator_set_enable(usb->vusb, false);
	if (ret) {
		usb->failed = true;
		return ret;
	}
	return mt6577_usb_disable_clocks(usb, true);
}

static int mt6577_usb_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct mt6577_usb_phy *usb = dev_get_priv(phy->dev);

	if (usb->failed)
		return -EIO;
	return mode == PHY_MODE_USB_DEVICE && !submode ? 0 : -EINVAL;
}

static const struct phy_ops mt6577_usb_ops = {
	.power_on = mt6577_usb_power_on,
	.power_off = mt6577_usb_power_off,
	.set_mode = mt6577_usb_set_mode,
};

static int mt6577_usb_phy_probe(struct udevice *dev)
{
	struct mt6577_usb_phy *usb = dev_get_priv(dev);
	fdt_size_t size;

	usb->base = dev_read_addr_size_index_ptr(dev, 0, &size);
	if (!usb->base || size < 0x800)
		return -EINVAL;
	usb->upll = devm_clk_get(dev, "upll");
	if (IS_ERR(usb->upll))
		return PTR_ERR(usb->upll);
	usb->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(usb->bus))
		return PTR_ERR(usb->bus);
	return device_get_supply_regulator(dev, "vusb33-supply", &usb->vusb);
}

static const struct udevice_id mt6577_usb_phy_ids[] = {
	{ .compatible = "mediatek,mt6577-usb-phy" },
	{ }
};

U_BOOT_DRIVER(mt6577_usb_phy) = {
	.name = "mt6577_usb_phy",
	.id = UCLASS_PHY,
	.of_match = mt6577_usb_phy_ids,
	.probe = mt6577_usb_phy_probe,
	.ops = &mt6577_usb_ops,
	.priv_auto = sizeof(struct mt6577_usb_phy),
};
