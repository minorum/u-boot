// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <dm.h>
#include <dm/read.h>
#include <dm/lists.h>
#include <generic-phy.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <power/regulator.h>
#include <power/charger.h>
#include <power/pmic.h>

struct mt8377_usb_phy {
	void __iomem *base;
	struct clk *upll;
	struct clk *bus;
	struct udevice *vusb;
	struct udevice *pmic;
	enum charger_usb_type usb_type;
	unsigned int generation;
	int usb_current;
	int (*event_poll)(void *data);
	void *event_data;
	bool failed, powered, detecting, detected, data_seen, source_busy;
	bool wait_detach;
};

enum { MT8377_QUALIFIED_SLEW_MAX = 7 };

static void mt8377_usb_update(struct mt8377_usb_phy *usb, unsigned int reg,
			      u8 mask, u8 value)
{
	writeb((readb(usb->base + reg) & ~mask) | value, usb->base + reg);
}

static int mt8377_bc11_update(struct mt8377_usb_phy *usb, unsigned int reg,
			     unsigned int mask, unsigned int value)
{
	int old = pmic_reg_read(usb->pmic, reg);
	int ret;

	if (old < 0)
		return old;
	value = (old & ~mask) | (value & mask);
	ret = pmic_reg_write(usb->pmic, reg, value);
	if (ret)
		return ret;
	ret = pmic_reg_read(usb->pmic, reg);
	if (ret < 0)
		return ret;
	return (ret & mask) == (value & mask) ? 0 : -EIO;
}

static int mt8377_bc11_cleanup(struct mt8377_usb_phy *usb)
{
	int ret, cleanup;

	ret = mt8377_bc11_update(usb, 0x34, 0xff, 0);
	cleanup = mt8377_bc11_update(usb, 0x33, 0x0f, 0);
	mt8377_usb_update(usb, 0x1c, BIT(7), 0);
	if (!ret)
		ret = cleanup;
	if (!ret && (readb(usb->base + 0x1c) & BIT(7)))
		ret = -EIO;
	return ret;
}

static void mt8377_bc11_invalidate(struct mt8377_usb_phy *usb)
{
	usb->generation++;
	usb->detected = false;
	usb->data_seen = false;
	usb->wait_detach = false;
	usb->usb_type = CHARGER_USB_UNKNOWN;
}

static int mt8377_bc11_detect(struct mt8377_usb_phy *usb)
{
	enum charger_usb_type type = CHARGER_USB_UNKNOWN;
	unsigned int generation = usb->generation;
	int ret, cleanup;

	usb->detecting = true;
	mt8377_usb_update(usb, 0x1c, BIT(7), BIT(7));
	if (!(readb(usb->base + 0x1c) & BIT(7))) {
		ret = -EIO;
		goto done;
	}
	ret = mt8377_bc11_update(usb, 0x33, 0x0f, 3);
	if (ret)
		goto done;
	ret = mt8377_bc11_update(usb, 0x34, 0xff, BIT(7));
	if (ret)
		goto done;
	ret = mt8377_bc11_update(usb, 0x33, 0x0c, 2 << 2);
	if (ret)
		goto done;
	ret = mt8377_bc11_update(usb, 0x34, 0x1e, (1 << 3) | (1 << 1));
	if (ret)
		goto done;
	mdelay(100);
	ret = pmic_reg_read(usb->pmic, 0x33);
	if (ret < 0)
		goto done;
	if (ret & BIT(7))
		type = CHARGER_USB_DCP;
	ret = mt8377_bc11_update(usb, 0x33, 0x0c, 0);
	if (ret)
		goto done;
	ret = mt8377_bc11_update(usb, 0x34, 0x1e, 0);
	if (ret)
		goto done;
	mdelay(50);
	ret = mt8377_bc11_update(usb, 0x34, 0x66,
				 type == CHARGER_USB_DCP ? 2 << 5 :
				 (1 << 5) | (1 << 1));
	if (ret)
		goto done;
	mdelay(80);
	if (type == CHARGER_USB_DCP) {
		if (!(readb(usb->base + 0x76) & BIT(7)))
			type = CHARGER_USB_CDP;
	} else {
		ret = pmic_reg_read(usb->pmic, 0x33);
		if (ret < 0)
			goto done;
		if (!(ret & BIT(7)))
			type = CHARGER_USB_SDP;
		else {
			ret = mt8377_bc11_update(usb, 0x34, 0x60, 2 << 5);
			if (ret)
				goto done;
			mdelay(80);
		}
	}
	ret = 0;
done:
	cleanup = mt8377_bc11_cleanup(usb);
	if (cleanup)
		ret = cleanup;
	if (!ret) {
		ret = pmic_reg_read(usb->pmic, 0x21);
		if (ret >= 0)
			ret = ret & BIT(5) ? 0 : -ENODEV;
	}
	if (!ret && usb->event_poll)
		ret = usb->event_poll(usb->event_data);
	if (!ret && (generation != usb->generation || !usb->powered))
		ret = -EAGAIN;
	if (!ret) {
		usb->usb_type = type;
		usb->detected = true;
	} else {
		usb->detected = false;
	}
	usb->detecting = false;
	return ret;
}

static int mt8377_bc11_refresh(struct mt8377_usb_phy *usb,
			      struct charger_usb_source *source)
{
	int ret;

	if (usb->failed)
		return -EIO;
	if (!usb->powered)
		return -ENODATA;
	if (usb->detecting)
		return -EBUSY;
	ret = pmic_reg_read(usb->pmic, 0x21);
	if (ret < 0) {
		usb->detected = false;
		return ret;
	}
	if (!(ret & BIT(5))) {
		mt8377_bc11_invalidate(usb);
		usb->usb_current = 0;
		return 0;
	}
	if (usb->wait_detach) {
		source->online = true;
		return 0;
	}
	if (!usb->detected && usb->data_seen) {
		usb->usb_type = CHARGER_USB_SDP;
		usb->detected = true;
	}
	if (!usb->detected) {
		ret = mt8377_bc11_detect(usb);
		if (ret)
			return ret;
	}
	if (usb->event_poll) {
		ret = usb->event_poll(usb->event_data);
		if (ret)
			return ret;
		if (!usb->detected)
			return -EAGAIN;
	}
	source->online = true;
	source->type = usb->usb_type;
	source->current_max_ua = usb->usb_current;
	if ((usb->usb_type == CHARGER_USB_DCP ||
	     usb->usb_type == CHARGER_USB_CDP) &&
	    (!usb->usb_current || usb->usb_current >= 100000))
		source->current_max_ua = 1500000;
	return 0;
}

static int mt8377_bc11_source(struct mt8377_usb_phy *usb,
			     struct charger_usb_source *source)
{
	int ret;

	if (usb->source_busy)
		return -EBUSY;
	usb->source_busy = true;
	ret = usb->event_poll ? usb->event_poll(usb->event_data) : 0;
	if (!ret)
		ret = mt8377_bc11_refresh(usb, source);
	usb->source_busy = false;
	return ret;
}

static int mt8377_usb_get_source(struct udevice *dev,
				struct charger_usb_source *source)
{
	return mt8377_bc11_source(dev_get_priv(dev->parent), source);
}

static int mt8377_usb_set_current(struct udevice *dev, int ua)
{
	struct mt8377_usb_phy *usb = dev_get_priv(dev->parent);

	if (ua < 0)
		return -EINVAL;
	usb->generation++;
	usb->usb_current = ua;
	if (!ua) {
		mt8377_bc11_invalidate(usb);
		usb->wait_detach = usb->powered;
	}
	if (ua >= 100000) {
		usb->wait_detach = false;
		usb->data_seen = true;
		if (usb->usb_type == CHARGER_USB_DCP)
			usb->usb_type = CHARGER_USB_SDP;
	}
	return 0;
}

static int mt8377_usb_set_event_poll(struct udevice *dev, int (*poll)(void *),
				     void *data)
{
	struct mt8377_usb_phy *usb = dev_get_priv(dev->parent);

	usb->generation++;
	usb->event_poll = poll;
	usb->event_data = data;
	return 0;
}

static int mt8377_usb_calibrate(struct mt8377_usb_phy *usb)
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
	if (!value || value > MT8377_QUALIFIED_SLEW_MAX) {
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

static void mt8377_usb_save_current(struct mt8377_usb_phy *usb)
{
	mt8377_usb_update(usb, 0x6b, 0x04, 0);
	mt8377_usb_update(usb, 0x6e, 0x01, 0);
	mt8377_usb_update(usb, 0x6a, 0x04, 0);
	mt8377_usb_update(usb, 0x68, 0xc0, 0xc0);
	mt8377_usb_update(usb, 0x68, 0x30, 0x10);
	mt8377_usb_update(usb, 0x68, 0x04, 0x04);
	mt8377_usb_update(usb, 0x69, 0x3c, 0);
	mt8377_usb_update(usb, 0x6a, 0x3a, 0x3a);
	mt8377_usb_update(usb, 0x1c, 0x80, 0);
	mt8377_usb_update(usb, 0x1b, 0x08, 0);
	udelay(800);
	mt8377_usb_update(usb, 0x63, 0x02, 0x02);
	udelay(1);
	mt8377_usb_update(usb, 0x6a, 0x04, 0x04);
	udelay(1);
}

static int mt8377_usb_disable_clocks(struct mt8377_usb_phy *usb, bool bus_on)
{
	int ret = bus_on ? clk_disable(usb->bus) : 0;

	if (!ret)
		ret = clk_disable(usb->upll);
	if (ret)
		usb->failed = true;
	return ret;
}

static int mt8377_usb_power_on(struct phy *phy)
{
	struct mt8377_usb_phy *usb = dev_get_priv(phy->dev);
	struct charger_usb_source source = {};
	bool bus_on = false;
	int ret, cleanup;

	if (usb->failed)
		return -EIO;
	if (usb->powered)
		return 0;
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
	mt8377_usb_update(usb, 0x6b, 0x04, 0);
	mt8377_usb_update(usb, 0x6e, 0x01, 0);
	mt8377_usb_update(usb, 0x1c, 0x80, 0);
	mt8377_usb_update(usb, 0x02, 0x7f, 0x09);
	mt8377_usb_update(usb, 0x22, 0x03, 0);
	mt8377_usb_update(usb, 0x6a, 0x04, 0);
	mt8377_usb_update(usb, 0x1b, 0x08, 0x08);
	udelay(800);
	mt8377_usb_update(usb, 0x68, 0xf4, 0);
	mt8377_usb_update(usb, 0x69, 0x3c, 0);
	mt8377_usb_update(usb, 0x6a, 0xbe, 0);
	udelay(800);
	ret = mt8377_usb_calibrate(usb);
	if (!ret) {
		usb->powered = true;
		if (usb->pmic)
			ret = mt8377_bc11_source(usb, &source);
	}
	if (!ret)
		return 0;
	usb->powered = false;
	mt8377_bc11_invalidate(usb);
	if (usb->pmic) {
		cleanup = mt8377_bc11_cleanup(usb);
		if (cleanup) {
			usb->failed = true;
			return cleanup;
		}
	}
	mt8377_usb_save_current(usb);
disable_supply:
	cleanup = regulator_set_enable(usb->vusb, false);
	if (cleanup) {
		usb->failed = true;
		return cleanup;
	}
disable_upll:
	cleanup = mt8377_usb_disable_clocks(usb, bus_on);
	return cleanup ? cleanup : ret;
}

static int mt8377_usb_power_off(struct phy *phy)
{
	struct mt8377_usb_phy *usb = dev_get_priv(phy->dev);
	int ret;

	mt8377_bc11_invalidate(usb);
	usb->usb_current = 0;
	if (usb->source_busy)
		return -EBUSY;
	if (usb->failed)
		return -EIO;
	if (!usb->powered)
		return 0;
	usb->powered = false;
	if (usb->pmic) {
		ret = mt8377_bc11_cleanup(usb);
		if (ret) {
			usb->failed = true;
			return ret;
		}
	}
	mt8377_usb_save_current(usb);
	ret = regulator_set_enable(usb->vusb, false);
	if (ret) {
		usb->failed = true;
		return ret;
	}
	return mt8377_usb_disable_clocks(usb, true);
}

static int mt8377_usb_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct mt8377_usb_phy *usb = dev_get_priv(phy->dev);

	if (usb->failed)
		return -EIO;
	return mode == PHY_MODE_USB_DEVICE && !submode ? 0 : -EINVAL;
}

static const struct phy_ops mt8377_usb_ops = {
	.power_on = mt8377_usb_power_on,
	.power_off = mt8377_usb_power_off,
	.set_mode = mt8377_usb_set_mode,
};

static int mt8377_usb_phy_probe(struct udevice *dev)
{
	struct mt8377_usb_phy *usb = dev_get_priv(dev);
	fdt_size_t size;
	int ret, low, high;

	usb->base = dev_read_addr_size_index_ptr(dev, 0, &size);
	if (!usb->base || size < 0x800)
		return -EINVAL;
	usb->upll = devm_clk_get(dev, "upll");
	if (IS_ERR(usb->upll))
		return PTR_ERR(usb->upll);
	usb->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(usb->bus))
		return PTR_ERR(usb->bus);
	ret = device_get_supply_regulator(dev, "vusb33-supply", &usb->vusb);
	if (ret || !dev_read_bool(dev, "mediatek,bc11"))
		return ret;
	usb->pmic = usb->vusb->parent;
	if (device_get_uclass_id(usb->pmic) != UCLASS_PMIC)
		return -EINVAL;
	low = pmic_reg_read(usb->pmic, 0);
	if (low < 0)
		return low;
	high = pmic_reg_read(usb->pmic, 1);
	if (high < 0)
		return high;
	return (high << 8 | low) == 0x1029 ? 0 : -ENODEV;
}

static const struct charger_ops mt8377_usb_source_ops = {
	.get_usb_source = mt8377_usb_get_source,
	.set_usb_current = mt8377_usb_set_current,
	.set_usb_event_poll = mt8377_usb_set_event_poll,
};

U_BOOT_DRIVER(mt8377_usb_source) = {
	.name = "mt8377_usb_source",
	.id = UCLASS_CHARGER,
	.ops = &mt8377_usb_source_ops,
};

static int mt8377_usb_phy_bind(struct udevice *dev)
{
	if (!dev_read_bool(dev, "mediatek,bc11"))
		return 0;
	return device_bind_driver_to_node(dev, "mt8377_usb_source", "usb-source",
					  dev_ofnode(dev), NULL);
}

static int mt8377_usb_phy_remove(struct udevice *dev)
{
	struct phy phy = { .dev = dev };

	return mt8377_usb_power_off(&phy);
}

static const struct udevice_id mt8377_usb_phy_ids[] = {
	{ .compatible = "mediatek,mt8377-usb-phy" },
	{ .compatible = "mediatek,mt6577-usb-phy" },
	{ }
};

U_BOOT_DRIVER(mt8377_usb_phy) = {
	.name = "mt8377_usb_phy",
	.id = UCLASS_PHY,
	.of_match = mt8377_usb_phy_ids,
	.probe = mt8377_usb_phy_probe,
	.bind = mt8377_usb_phy_bind,
	.remove = mt8377_usb_phy_remove,
	.ops = &mt8377_usb_ops,
	.priv_auto = sizeof(struct mt8377_usb_phy),
};
