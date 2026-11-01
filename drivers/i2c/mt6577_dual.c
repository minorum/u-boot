// SPDX-License-Identifier: GPL-2.0-only
#include <clk.h>
#include <div64.h>
#include <dm.h>
#include <i2c.h>
#include <asm/io.h>
#include <linux/iopoll.h>

#define DUAL_DATA	0x00
#define DUAL_ADDR	0x04
#define DUAL_MASK	0x08
#define DUAL_STATUS	0x0c
#define DUAL_CONTROL	0x10
#define DUAL_LENGTH	0x14
#define DUAL_COUNT	0x18
#define DUAL_TIMING	0x20
#define DUAL_START	0x24
#define DUAL_FIFO_STATUS 0x30
#define DUAL_FIFO_CLEAR	0x38
#define DUAL_IO		0x40
#define DUAL_HS		0x48

struct mt6577_dual {
	void __iomem *base;
	struct clk clock;
	bool push_pull;
};

static int mt6577_dual_timing(ulong rate)
{
	u64 step;
	unsigned int sample;

	if (!rate || IS_ERR_VALUE(rate))
		return -EINVAL;
	for (sample = 1; sample <= 8; sample++) {
		step = DIV_ROUND_UP_ULL(rate, 3200000ULL * sample);
		if (step <= 64)
			return ((sample - 1) << 8) | (step - 1);
	}
	return -ERANGE;
}

static int mt6577_dual_xfer(struct udevice *dev, struct i2c_msg *msg, int count)
{
	struct mt6577_dual *dual = dev_get_priv(dev);
	void __iomem *base = dual->base;
	bool reading = count == 2;
	u16 status, value;
	int timing, ret, cleanup;

	if (!msg || (count != 1 && count != 2) || !msg[0].buf || msg[0].flags ||
	    msg[0].len != (reading ? 1 : 2))
		return -EOPNOTSUPP;
	if (reading && (!msg[1].buf || msg[1].flags != I2C_M_RD ||
			msg[1].len != 1 || msg[1].addr != msg[0].addr))
		return -EOPNOTSUPP;
	if (msg[0].addr < 0x08 || msg[0].addr > 0x77)
		return -EINVAL;
	ret = clk_enable(&dual->clock);
	if (ret)
		return ret;
	if ((readw(base + DUAL_MASK) & 7) || (readw(base + DUAL_STATUS) & 7) ||
	    (readl(base + DUAL_START) & 1) || !(readw(base + DUAL_FIFO_STATUS) & 1)) {
		ret = -EBUSY;
		goto disable;
	}
	timing = mt6577_dual_timing(clk_get_rate(&dual->clock));
	if (timing < 0) {
		ret = timing;
		goto disable;
	}
	value = readw(base + DUAL_TIMING);
	writew((value & ~0x73f) | timing, base + DUAL_TIMING);
	writew(dual->push_pull ? 0 : 3, base + DUAL_IO);
	writew(readw(base + DUAL_HS) & ~1, base + DUAL_HS);
	value = readw(base + DUAL_CONTROL) & ~0x7e;
	writew(value | (reading ? 0x3a : 0x28), base + DUAL_CONTROL);
	writew(msg[0].addr << 1, base + DUAL_ADDR);
	value = readw(base + DUAL_LENGTH) & ~0x1fff;
	writew(value | (reading ? 0x101 : 2), base + DUAL_LENGTH);
	writew(reading ? 2 : 1, base + DUAL_COUNT);
	writew(1, base + DUAL_FIFO_CLEAR);
	writeb(msg[0].buf[0], base + DUAL_DATA);
	if (!reading)
		writeb(msg[0].buf[1], base + DUAL_DATA);
	writel(1, base + DUAL_START);
	ret = read_poll_timeout(readw, status, status & 7, 20, 100000, base + DUAL_STATUS);
	if (!ret && (status & 6))
		ret = -ENXIO;
	if (!ret && reading) {
		if (readw(base + DUAL_FIFO_STATUS) & 1)
			ret = -EIO;
		else
			msg[1].buf[0] = readb(base + DUAL_DATA);
	}
	writew(status & 7, base + DUAL_STATUS);
disable:
	cleanup = clk_disable(&dual->clock);
	return cleanup ? cleanup : ret;
}

static int mt6577_dual_speed(struct udevice *dev, uint speed)
{
	return speed == 100000 ? 0 : -EINVAL;
}

static int mt6577_dual_probe(struct udevice *dev)
{
	struct mt6577_dual *dual = dev_get_priv(dev);

	dual->base = dev_read_addr_ptr(dev);
	if (!dual->base)
		return -EINVAL;
	dual->push_pull = dev_read_bool(dev, "mediatek,use-push-pull");
	return clk_get_by_name(dev, "main", &dual->clock);
}

static const struct dm_i2c_ops mt6577_dual_ops = {
	.xfer = mt6577_dual_xfer,
	.set_bus_speed = mt6577_dual_speed,
};

static const struct udevice_id mt6577_dual_ids[] = {
	{ .compatible = "mediatek,mt6577-i2c" },
	{ .compatible = "mediatek,mt6577-i2c-dual" },
	{ }
};

U_BOOT_DRIVER(mt6577_dual_i2c) = {
	.name = "mt6577_dual_i2c",
	.id = UCLASS_I2C,
	.of_match = mt6577_dual_ids,
	.probe = mt6577_dual_probe,
	.ops = &mt6577_dual_ops,
	.priv_auto = sizeof(struct mt6577_dual),
};
