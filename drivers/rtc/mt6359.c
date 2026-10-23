// SPDX-License-Identifier: GPL-2.0+
#include <dm.h>
#include <rtc.h>
#include <power/pmic.h>
#include <linux/delay.h>
#include <time.h>

#define MT6359_RTC_BBPU		0x588
#define MT6359_RTC_PDN1		0x5b4
#define MT6359_RTC_WRTGR		0x5c2
#define MT6359_RTC_CBUSY		BIT(6)

static int mt6359_rtc_trigger(struct udevice *dev)
{
	ulong start;
	int ret;

	ret = pmic_reg_write(dev->parent, MT6359_RTC_WRTGR, 1);
	if (ret)
		return ret;
	start = get_timer(0);
	do {
		ret = pmic_reg_read(dev->parent, MT6359_RTC_BBPU);
		if (ret < 0)
			return ret;
		if (!(ret & MT6359_RTC_CBUSY))
			return 0;
		udelay(10);
	} while (get_timer(start) < 1000);
	return -ETIMEDOUT;
}

static int mt6359_rtc_read(struct udevice *dev, unsigned int reg,
			   u8 *buf, unsigned int len)
{
	u8 data[2];
	int value;

	if (reg >= sizeof(data) || !len || len > sizeof(data) - reg)
		return -EINVAL;
	value = pmic_reg_read(dev->parent, MT6359_RTC_PDN1);
	if (value < 0)
		return value;
	data[0] = value;
	data[1] = value >> 8;
	memcpy(buf, data + reg, len);
	return 0;
}

static int mt6359_rtc_write(struct udevice *dev, unsigned int reg,
			    const u8 *buf, unsigned int len)
{
	u32 mask, value;
	int old, ret;

	if (reg >= 2 || !len || len > 2 - reg)
		return -EINVAL;
	old = pmic_reg_read(dev->parent, MT6359_RTC_PDN1);
	if (old < 0)
		return old;
	mask = ((1U << (len * 8)) - 1) << (reg * 8);
	value = buf[0];
	if (len == 2)
		value |= buf[1] << 8;
	value = (old & ~mask) | (value << (reg * 8));
	ret = pmic_reg_write(dev->parent, MT6359_RTC_PDN1, value);
	if (ret)
		return ret;
	return mt6359_rtc_trigger(dev);
}

static const struct rtc_ops mt6359_rtc_ops = {
	.read = mt6359_rtc_read,
	.write = mt6359_rtc_write,
};

static const struct udevice_id mt6359_rtc_ids[] = {
	{ .compatible = "mediatek,mt6359-rtc" },
	{ }
};

U_BOOT_DRIVER(mt6359_rtc) = {
	.name = "mt6359_rtc",
	.id = UCLASS_RTC,
	.of_match = mt6359_rtc_ids,
	.ops = &mt6359_rtc_ops,
};
